// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Manual driver (not an automated test): discovers a real phone's
// "NearbySharing" BLE advertisement, connects to it via the real production
// NearbyConnectionsManagerImpl, performs the real paired-key verification
// handshake, sends a real IntroductionFrame describing one file, waits for
// the phone to accept, then actually sends the file's bytes. If Nearby
// Connections upgrades the underlying medium, the transfer completes over
// the real WiFi LAN data channel (internal/platform/implementation/linux/
// wifi_lan.cc) instead of BLE.
//
// Paired-key verification: we have no NearbyShareCertificateManager (no
// account/certificate backend -- see sharing_context.h's own comment), so
// we deliberately take the same fallback path the real algorithm itself
// documents for this exact situation (see
// sharing/paired_key_verification_runner.cc's SendPairedKeyEncryptionFrame:
// "if a valid signature cannot be generated" -> random bytes, "consistent
// with the GmsCore implementation"). As the *sender* with no certificate,
// VerifyPairedKeyEncryptionFrame/VerifyAuthTokenHashWithPrivateCertificates
// both return kUnable, and that only escalates to kFail on the *receiver*
// side for non-"everyone" visibility (see the same file's
// OnReadPairedKeyEncryptionFrame) -- so the exchange legitimately settles on
// PairedKeyResultFrame::UNABLE, the same outcome real Quick Share reaches
// when sending to a device that isn't one of your paired contacts.

#include <openssl/rand.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "internal/base/file_path.h"
#include "internal/base/files.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/device_info.h"
#include "internal/platform/implementation/linux/sharing_context.h"
#include "internal/platform/task_runner_impl.h"
#include "proto/sharing_enums.pb.h"
#include "sharing/advertisement.h"
#include "sharing/incoming_frames_reader.h"
#include "sharing/nearby_connections_manager.h"
#include "sharing/nearby_connections_manager_impl.h"
#include "sharing/nearby_connections_service_impl.h"
#include "sharing/nearby_connections_types.h"
#include "sharing/proto/enums.pb.h"
#include "sharing/proto/wire_format.pb.h"

namespace {

using ::location::nearby::proto::sharing::OSType;
using ::nearby::FilePath;
using ::nearby::Files;
using ::nearby::linux_platform::BluetoothAdapter;
using ::nearby::linux_platform::DeviceInfo;
using ::nearby::linux_platform::NoOpConnectivityManager;
using ::nearby::linux_platform::SharingContext;
using ::nearby::sharing::Advertisement;
using ::nearby::sharing::AdvertisementCapabilities;
using ::nearby::sharing::NearbyConnection;
using ::nearby::sharing::NearbyConnectionsManager;
using ::nearby::sharing::NearbyConnectionsManagerImpl;
using ::nearby::sharing::NearbyConnectionsServiceImpl;
using ::nearby::sharing::Payload;
using ::nearby::sharing::PayloadStatus;
using ::nearby::sharing::PayloadTransferUpdate;
using ::nearby::sharing::ShareTargetType;
using ::nearby::sharing::Status;
using ::nearby::sharing::TransportType;
using ::nearby::sharing::service::proto::ConnectionResponseFrame;
using ::nearby::sharing::service::proto::FileMetadata;
using ::nearby::sharing::service::proto::Frame;
using ::nearby::sharing::service::proto::IntroductionFrame;
using ::nearby::sharing::service::proto::PairedKeyEncryptionFrame;
using ::nearby::sharing::service::proto::PairedKeyResultFrame;
using ::nearby::sharing::service::proto::V1Frame;

// See sharing/certificates/constants.h:47 (not separately buildable -- see
// that file's own "bundled into a monolithic target" situation -- so the
// value is hardcoded here with this pointer back to the real source).
constexpr size_t kNearbyShareNumBytesAuthenticationTokenHash = 6;
// See sharing/paired_key_verification_runner.cc's kNearbyShareNumBytesRandomSignature.
constexpr size_t kNearbyShareNumBytesRandomSignature = 72;

constexpr absl::Duration kReadFrameTimeout = absl::Seconds(15);

std::vector<uint8_t> GenerateRandomBytes(size_t num_bytes) {
  std::vector<uint8_t> bytes(num_bytes);
  RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
  return bytes;
}

std::vector<uint8_t> BuildOwnEndpointInfo() {
  std::vector<uint8_t> salt(Advertisement::kSaltSize, 0);
  std::vector<uint8_t> encrypted_metadata_key(
      Advertisement::kMetadataEncryptionKeyHashByteSize, 0);
  std::unique_ptr<Advertisement> advertisement = Advertisement::NewInstance(
      std::move(salt), std::move(encrypted_metadata_key),
      ShareTargetType::kLaptop, std::string("quickshare-linux"),
      /*vendor_id=*/0, AdvertisementCapabilities{});
  return advertisement != nullptr ? advertisement->ToEndpointInfo()
                                  : std::vector<uint8_t>{};
}

// Drives one discover -> connect -> verify -> introduce -> transfer session
// against the first endpoint found. Lives as long as main() holds a
// shared_ptr to it; async callbacks capture shared_from_this() so the
// session survives until it is explicitly Finish()ed.
class SendSession : public NearbyConnectionsManager::DiscoveryListener,
                    public std::enable_shared_from_this<SendSession> {
 public:
  SendSession(NearbyConnectionsManager& manager, FilePath file_to_send,
             std::string file_display_name, std::string mime_type,
             uint64_t file_size)
      : manager_(manager),
        file_to_send_(std::move(file_to_send)),
        file_display_name_(std::move(file_display_name)),
        mime_type_(std::move(mime_type)),
        file_size_(file_size) {}

  void OnEndpointDiscovered(absl::string_view endpoint_id,
                            absl::Span<const uint8_t> endpoint_info) override {
    if (connecting_or_connected_) return;
    connecting_or_connected_ = true;

    std::unique_ptr<Advertisement> peer_advertisement =
        Advertisement::FromEndpointInfo(endpoint_info);
    printf("Discovered endpoint %s (%s), connecting...\n",
           std::string(endpoint_id).c_str(),
           (peer_advertisement != nullptr &&
            peer_advertisement->device_name().has_value())
               ? peer_advertisement->device_name()->c_str()
               : "unknown device name");

    manager_.StopDiscovery();
    std::shared_ptr<SendSession> self = shared_from_this();
    manager_.Connect(
        BuildOwnEndpointInfo(), endpoint_id,
        /*bluetooth_mac_address=*/std::nullopt,
        ::nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
        TransportType::kAny,
        [self](absl::string_view endpoint_id, NearbyConnection* connection,
              Status status) {
          self->OnConnectResult(std::string(endpoint_id), connection, status);
        });
  }

  void OnEndpointLost(absl::string_view endpoint_id) override {
    printf("Endpoint lost: %s\n", std::string(endpoint_id).c_str());
  }

  bool done() const { return done_; }
  bool success() const { return success_; }

 private:
  void OnConnectResult(std::string endpoint_id, NearbyConnection* connection,
                       Status status) {
    if (status != Status::kSuccess || connection == nullptr) {
      printf("Connect failed: status=%d\n", static_cast<int>(status));
      Finish(false);
      return;
    }
    printf("Connected to endpoint %s\n", endpoint_id.c_str());
    endpoint_id_ = std::move(endpoint_id);
    connection_ = connection;
    frames_reader_ =
        std::make_shared<::nearby::sharing::IncomingFramesReader>(
            frames_task_runner_, connection_);

    std::shared_ptr<SendSession> self = shared_from_this();
    connection_->SetDisconnectionListener([self]() {
      printf("Connection closed by peer.\n");
      self->Finish(false);
    });

    SendPairedKeyEncryptionFrame();
    frames_reader_->ReadFrame(
        V1Frame::PAIRED_KEY_ENCRYPTION,
        [self](bool is_timeout, std::optional<V1Frame> frame) {
          self->OnPairedKeyEncryptionFrame(is_timeout, std::move(frame));
        },
        kReadFrameTimeout);
  }

  void WriteFrame(const Frame& frame) {
    std::vector<uint8_t> data(frame.ByteSizeLong());
    frame.SerializeToArray(data.data(), data.size());
    manager_.Send(
        endpoint_id_, std::make_unique<Payload>(std::move(data)),
        std::weak_ptr<NearbyConnectionsManager::PayloadStatusListener>());
  }

  void SendPairedKeyEncryptionFrame() {
    std::vector<uint8_t> signature =
        GenerateRandomBytes(kNearbyShareNumBytesRandomSignature);
    std::vector<uint8_t> secret_id_hash =
        GenerateRandomBytes(kNearbyShareNumBytesAuthenticationTokenHash);

    Frame frame;
    frame.set_version(Frame::V1);
    V1Frame* v1_frame = frame.mutable_v1();
    v1_frame->set_type(V1Frame::PAIRED_KEY_ENCRYPTION);
    PairedKeyEncryptionFrame* encryption_frame =
        v1_frame->mutable_paired_key_encryption();
    encryption_frame->set_signed_data(
        reinterpret_cast<const char*>(signature.data()), signature.size());
    encryption_frame->set_secret_id_hash(
        reinterpret_cast<const char*>(secret_id_hash.data()),
        secret_id_hash.size());
    WriteFrame(frame);
    printf("Sent PairedKeyEncryptionFrame (random signature -- no "
           "certificate manager on this side).\n");
  }

  void OnPairedKeyEncryptionFrame(bool is_timeout,
                                  std::optional<V1Frame> frame) {
    if (!frame.has_value()) {
      printf("Failed to read peer's PairedKeyEncryptionFrame (timeout=%d)\n",
             is_timeout);
      Finish(false);
      return;
    }
    printf("Received peer's PairedKeyEncryptionFrame (%zu-byte signature)\n",
           frame->paired_key_encryption().signed_data().size());

    Frame result_frame;
    result_frame.set_version(Frame::V1);
    V1Frame* v1_frame = result_frame.mutable_v1();
    v1_frame->set_type(V1Frame::PAIRED_KEY_RESULT);
    PairedKeyResultFrame* result = v1_frame->mutable_paired_key_result();
    // UNABLE, not SUCCESS/FAIL -- see the file-level comment above.
    result->set_status(PairedKeyResultFrame::UNABLE);
    result->set_os_type(OSType::UNKNOWN_OS_TYPE);
    WriteFrame(result_frame);
    printf("Sent PairedKeyResultFrame (status=UNABLE).\n");

    std::shared_ptr<SendSession> self = shared_from_this();
    frames_reader_->ReadFrame(
        V1Frame::PAIRED_KEY_RESULT,
        [self](bool is_timeout, std::optional<V1Frame> frame) {
          self->OnPairedKeyResultFrame(is_timeout, std::move(frame));
        },
        kReadFrameTimeout);
  }

  void OnPairedKeyResultFrame(bool is_timeout, std::optional<V1Frame> frame) {
    if (!frame.has_value()) {
      printf("Failed to read peer's PairedKeyResultFrame (timeout=%d)\n",
             is_timeout);
      Finish(false);
      return;
    }
    printf("Peer's paired-key verification result: status=%d\n",
           static_cast<int>(frame->paired_key_result().status()));
    SendIntroduction();
  }

  void SendIntroduction() {
    Payload file_payload(file_to_send_);
    file_payload_id_ = file_payload.id;

    Frame frame;
    frame.set_version(Frame::V1);
    V1Frame* v1_frame = frame.mutable_v1();
    v1_frame->set_type(V1Frame::INTRODUCTION);
    IntroductionFrame* introduction = v1_frame->mutable_introduction();
    introduction->set_use_case(IntroductionFrame::NEARBY_SHARE);
    introduction->set_start_transfer(true);
    FileMetadata* file_metadata = introduction->add_file_metadata();
    file_metadata->set_id(file_payload_id_);
    file_metadata->set_name(file_display_name_);
    file_metadata->set_payload_id(file_payload_id_);
    file_metadata->set_mime_type(mime_type_);
    file_metadata->set_size(file_size_);
    WriteFrame(frame);
    printf("Sent IntroductionFrame for \"%s\" (%llu bytes, payload id %lld), "
           "waiting for the phone to accept...\n",
           file_display_name_.c_str(),
           static_cast<unsigned long long>(file_size_),
           static_cast<long long>(file_payload_id_));

    pending_file_payload_ = std::make_unique<Payload>(std::move(file_payload));

    std::shared_ptr<SendSession> self = shared_from_this();
    frames_reader_->ReadFrame(
        V1Frame::RESPONSE,
        [self](bool is_timeout, std::optional<V1Frame> frame) {
          self->OnConnectionResponse(is_timeout, std::move(frame));
        },
        absl::ZeroDuration());
  }

  void OnConnectionResponse(bool is_timeout, std::optional<V1Frame> frame) {
    if (!frame.has_value()) {
      printf("No ConnectionResponseFrame from peer (timeout=%d).\n",
             is_timeout);
      Finish(false);
      return;
    }
    ConnectionResponseFrame::Status status =
        frame->connection_response().status();
    printf("ConnectionResponseFrame: status=%d\n", static_cast<int>(status));
    if (status != ConnectionResponseFrame::ACCEPT) {
      printf("Peer did not accept the transfer.\n");
      Finish(false);
      return;
    }

    printf("Accepted! Sending file payload...\n");
    payload_listener_ = std::make_shared<PayloadListener>(shared_from_this());
    manager_.Send(endpoint_id_, std::move(pending_file_payload_),
                  payload_listener_->GetWeakPtr());
  }

  class PayloadListener : public NearbyConnectionsManager::PayloadStatusListener {
   public:
    explicit PayloadListener(std::shared_ptr<SendSession> session)
        : session_(std::move(session)) {}
    void OnStatusUpdate(std::unique_ptr<PayloadTransferUpdate> update) override {
      session_->OnPayloadUpdate(*update);
    }

   private:
    std::shared_ptr<SendSession> session_;
  };

  void OnPayloadUpdate(const PayloadTransferUpdate& update) {
    printf("Payload %lld: status=%d, %llu/%llu bytes\n",
           static_cast<long long>(update.payload_id),
           static_cast<int>(update.status),
           static_cast<unsigned long long>(update.bytes_transferred),
           static_cast<unsigned long long>(update.total_bytes));
    switch (update.status) {
      case PayloadStatus::kSuccess:
        printf("Transfer complete.\n");
        Finish(true);
        break;
      case PayloadStatus::kFailure:
      case PayloadStatus::kCanceled:
        printf("Transfer did not complete.\n");
        Finish(false);
        break;
      case PayloadStatus::kInProgress:
        break;
    }
  }

  void Finish(bool ok) {
    if (done_) return;
    success_ = ok;
    done_ = true;
  }

  NearbyConnectionsManager& manager_;
  FilePath file_to_send_;
  std::string file_display_name_;
  std::string mime_type_;
  uint64_t file_size_;

  bool connecting_or_connected_ = false;
  bool done_ = false;
  bool success_ = false;

  std::string endpoint_id_;
  NearbyConnection* connection_ = nullptr;
  ::nearby::TaskRunnerImpl frames_task_runner_{/*runner_count=*/1};
  std::shared_ptr<::nearby::sharing::IncomingFramesReader> frames_reader_;
  int64_t file_payload_id_ = 0;
  std::unique_ptr<Payload> pending_file_payload_;
  std::shared_ptr<PayloadListener> payload_listener_;
};

}  // namespace

int main(int argc, char** argv) {
  std::string path = argc > 1 ? argv[1]
                              : "/tmp/quickshare-linux-send-test.txt";
  if (argc <= 1) {
    FILE* f = fopen(path.c_str(), "w");
    if (f != nullptr) {
      fputs("Hello from the Linux Nearby Share port -- real end-to-end "
            "BLE-discovery + Nearby Connections file transfer test.\n",
            f);
      fclose(f);
    }
  }
  FilePath file_path(path);
  std::optional<uintmax_t> file_size = Files::GetFileSize(file_path);
  if (!file_size.has_value()) {
    printf("File not found or unreadable: %s\n", path.c_str());
    return 1;
  }
  std::string display_name = file_path.GetFileName().ToString();
  printf("Will send: %s (%ju bytes) as \"%s\"\n", path.c_str(), *file_size,
         display_name.c_str());

  BluetoothAdapter adapter;
  SharingContext context(adapter);
  DeviceInfo device_info;
  NoOpConnectivityManager connectivity_manager;
  printf("IsLanConnected: %d\n", connectivity_manager.IsLanConnected());
  ::nearby::TaskRunnerImpl callback_task_runner(/*runner_count=*/1);

  auto connections_service = std::make_unique<NearbyConnectionsServiceImpl>(
      &connectivity_manager, /*event_logger=*/nullptr);
  NearbyConnectionsManagerImpl manager(&callback_task_runner, &context,
                                      connectivity_manager, device_info,
                                      std::move(connections_service));

  auto session = std::make_shared<SendSession>(
      manager, file_path, display_name, "text/plain", *file_size);

  bool discovery_result_known = false;
  bool discovery_ok = false;
  manager.StartDiscovery(
      session.get(), ::nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
      /*alternate_service_uuid=*/std::nullopt,
      [&](Status status) {
        discovery_ok = status == Status::kSuccess;
        discovery_result_known = true;
        printf("StartDiscovery callback: status=%d\n",
               static_cast<int>(status));
      });

  for (int i = 0; i < 300 && !discovery_result_known; ++i) {
    if (i % 10 == 0) {
      printf("  ...waiting for StartDiscovery to resolve (%.1fs)\n", i / 10.0);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!discovery_result_known || !discovery_ok) {
    printf("StartDiscovery failed (result_known=%d, ok=%d).\n",
           discovery_result_known, discovery_ok);
    manager.Shutdown();
    return 1;
  }

  printf("Discovering for up to 60s -- make sure the phone's Quick Share "
         "\"ready to receive\" screen is open...\n");
  for (int i = 0; i < 600 && !session->done(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok = session->done() && session->success();
  printf(ok ? "\nSUCCESS: file transfer completed.\n"
            : "\nFAILED: see log above.\n");

  manager.Shutdown();
  return ok ? 0 : 1;
}
