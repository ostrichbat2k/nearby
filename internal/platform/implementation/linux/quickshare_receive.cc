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
// Manual driver (not an automated test): the receiver-side mirror of
// quickshare_send.cc. Advertises a real, Quick-Share-identifiable BLE+WiFi
// LAN endpoint (service ID "NearbySharing") via the real production
// NearbyConnectionsManagerImpl, and -- unlike quickshare_advertise.cc, which
// stops at logging OnIncomingConnection -- actually drives the real
// paired-key verification handshake, auto-accepts the IntroductionFrame, and
// saves the incoming file(s) to disk. This is the direction that was still
// unverified as of the last session: a real phone discovering and sending
// *to* this machine.
//
// Paired-key verification: see quickshare_send.cc's file-level comment for
// why this side also settles on PairedKeyResultFrame::UNABLE (no
// NearbyShareCertificateManager on either end of this port).

#include <openssl/rand.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "connections/implementation/flags/nearby_connections_feature_flags.h"
#include "internal/flags/nearby_flags.h"
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
using ::nearby::sharing::service::proto::ConnectionResponseFrame;
using ::nearby::sharing::service::proto::Frame;
using ::nearby::sharing::service::proto::PairedKeyEncryptionFrame;
using ::nearby::sharing::service::proto::PairedKeyResultFrame;
using ::nearby::sharing::service::proto::V1Frame;

// Mirrors quickshare_send.cc's constants -- see that file's comments for the
// real-source pointers (sharing/certificates/constants.h,
// sharing/paired_key_verification_runner.cc).
constexpr size_t kNearbyShareNumBytesAuthenticationTokenHash = 6;
constexpr size_t kNearbyShareNumBytesRandomSignature = 72;

constexpr absl::Duration kReadFrameTimeout = absl::Seconds(15);

std::vector<uint8_t> GenerateRandomBytes(size_t num_bytes) {
  std::vector<uint8_t> bytes(num_bytes);
  RAND_bytes(bytes.data(), static_cast<int>(bytes.size()));
  return bytes;
}

// Drives one incoming connection's app-level protocol: paired-key
// verification, auto-accept of the IntroductionFrame, and tracking the
// incoming file payload(s) to completion. The manager itself writes
// incoming FILE payloads to disk (SetCustomSavePath, set in main() before
// StartAdvertising) -- this class only drives the handshake and watches for
// completion. Only one connection is handled at a time, matching the BLE
// GATT server's own single-peer scoping.
class ReceiveSession : public NearbyConnectionsManager::IncomingConnectionListener,
                       public std::enable_shared_from_this<ReceiveSession> {
 public:
  explicit ReceiveSession(NearbyConnectionsManager& manager)
      : manager_(manager) {}

  void OnIncomingConnection(absl::string_view endpoint_id,
                            absl::Span<const uint8_t> endpoint_info,
                            NearbyConnection* connection) override {
    if (busy_) {
      printf("Already handling a connection, ignoring incoming connection "
             "from %s.\n",
             std::string(endpoint_id).c_str());
      return;
    }
    busy_ = true;

    printf("Incoming connection from %s, verifying...\n",
           std::string(endpoint_id).c_str());
    endpoint_id_ = std::string(endpoint_id);
    connection_ = connection;
    frames_reader_ =
        std::make_shared<::nearby::sharing::IncomingFramesReader>(
            frames_task_runner_, connection_);

    std::shared_ptr<ReceiveSession> self = shared_from_this();
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

  bool done() const { return done_; }
  bool success() const { return success_; }

 private:
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
    result->set_status(PairedKeyResultFrame::UNABLE);
    result->set_os_type(OSType::UNKNOWN_OS_TYPE);
    WriteFrame(result_frame);
    printf("Sent PairedKeyResultFrame (status=UNABLE).\n");

    std::shared_ptr<ReceiveSession> self = shared_from_this();
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

    std::shared_ptr<ReceiveSession> self = shared_from_this();
    frames_reader_->ReadFrame(
        V1Frame::INTRODUCTION,
        [self](bool is_timeout, std::optional<V1Frame> frame) {
          self->OnIntroductionFrame(is_timeout, std::move(frame));
        },
        kReadFrameTimeout);
  }

  void OnIntroductionFrame(bool is_timeout, std::optional<V1Frame> frame) {
    if (!frame.has_value()) {
      printf("No IntroductionFrame from peer (timeout=%d).\n", is_timeout);
      Finish(false);
      return;
    }

    const auto& introduction = frame->introduction();
    if (introduction.file_metadata_size() == 0) {
      printf("IntroductionFrame has no files, rejecting.\n");
      Frame response;
      response.set_version(Frame::V1);
      response.mutable_v1()->set_type(V1Frame::RESPONSE);
      response.mutable_v1()->mutable_connection_response()->set_status(
          ConnectionResponseFrame::REJECT);
      WriteFrame(response);
      Finish(false);
      return;
    }

    printf("Receiving %d file(s):\n", introduction.file_metadata_size());
    payload_listener_ = std::make_shared<PayloadListener>(shared_from_this());
    for (const auto& file_metadata : introduction.file_metadata()) {
      printf("  - \"%s\" (%lld bytes, payload id %lld)\n",
             file_metadata.name().c_str(),
             static_cast<long long>(file_metadata.size()),
             static_cast<long long>(file_metadata.payload_id()));
      pending_payload_names_[file_metadata.payload_id()] =
          file_metadata.name();
      // Must be registered before we ACCEPT: NearbyConnectionsManagerImpl
      // cancels any non-bytes payload with no registered listener as
      // "unknown" (see its OnPayloadReceived).
      manager_.RegisterPayloadStatusListener(file_metadata.payload_id(),
                                             payload_listener_->GetWeakPtr());
    }

    // Auto-accept: this driver is an unattended receiver, there's no UI to
    // ask a human. Matches the real app's own "no visible user prompt"
    // auto-accept behavior at the Nearby Connections layer for the
    // lower-level connection; this is the equivalent choice one layer up,
    // for the app-level file offer itself.
    printf("Auto-accepting.\n");
    Frame response;
    response.set_version(Frame::V1);
    response.mutable_v1()->set_type(V1Frame::RESPONSE);
    response.mutable_v1()->mutable_connection_response()->set_status(
        ConnectionResponseFrame::ACCEPT);
    WriteFrame(response);

    // The real app (IncomingShareSession::TryUpgradeBandwidth, called right
    // after writing this same ACCEPT frame) requests a bandwidth upgrade to
    // WiFi LAN/WebRTC here for large-enough transfers; auto_upgrade_bandwidth
    // is deliberately false on StartAdvertising (manual control, matching
    // production), so without this call every transfer -- any size -- stays
    // on BLE. This driver has no AttachmentContainer/size threshold, so just
    // always request it: there's no cost to asking when the transfer is
    // small, and it's the only way a real file-sized transfer ever leaves
    // BLE's ~20-bytes-per-GATT-write throughput.
    printf("Requesting bandwidth upgrade to WiFi LAN.\n");
    manager_.UpgradeBandwidth(endpoint_id_);
  }

  class PayloadListener
      : public NearbyConnectionsManager::PayloadStatusListener {
   public:
    explicit PayloadListener(std::shared_ptr<ReceiveSession> session)
        : session_(std::move(session)) {}
    void OnStatusUpdate(std::unique_ptr<PayloadTransferUpdate> update)
        override {
      session_->OnPayloadUpdate(*update);
    }

   private:
    std::shared_ptr<ReceiveSession> session_;
  };

  void OnPayloadUpdate(const PayloadTransferUpdate& update) {
    auto it = pending_payload_names_.find(update.payload_id);
    std::string name =
        it != pending_payload_names_.end() ? it->second : "(unknown)";
    printf("Payload %lld (\"%s\"): status=%d, %llu/%llu bytes\n",
           static_cast<long long>(update.payload_id), name.c_str(),
           static_cast<int>(update.status),
           static_cast<unsigned long long>(update.bytes_transferred),
           static_cast<unsigned long long>(update.total_bytes));
    switch (update.status) {
      case PayloadStatus::kSuccess:
        pending_payload_names_.erase(update.payload_id);
        if (pending_payload_names_.empty()) {
          printf("All files received.\n");
          Finish(true);
        }
        break;
      case PayloadStatus::kFailure:
      case PayloadStatus::kCanceled:
        printf("Receiving payload %lld (\"%s\") did not complete.\n",
               static_cast<long long>(update.payload_id), name.c_str());
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

  bool busy_ = false;
  bool done_ = false;
  bool success_ = false;

  std::string endpoint_id_;
  NearbyConnection* connection_ = nullptr;
  ::nearby::TaskRunnerImpl frames_task_runner_{/*runner_count=*/1};
  std::shared_ptr<::nearby::sharing::IncomingFramesReader> frames_reader_;
  // payload_id -> display name, for progress/completion logging.
  absl::flat_hash_map<int64_t, std::string> pending_payload_names_;
  std::shared_ptr<PayloadListener> payload_listener_;
};

}  // namespace

int main(int argc, char** argv) {
  // The legacy (non-refactored) BLE endpoint channel path skips
  // mediums::BleSocket's BleInputStream decorator, which is what strips the
  // BLE-specific service_id_hash envelope from each reassembled Weave
  // message before BaseEndpointChannel::Read()'s generic length-prefix
  // parsing sees it. With the flag off (its default), that envelope leaks
  // straight into the generic parser as bogus leading bytes, corrupting
  // every real message -- confirmed via hex dump against a real phone: the
  // raw ConnectionRequestFrame bytes arrive intact, but 3 extra leading
  // bytes before them make BaseEndpointChannel::Read() misinterpret the
  // length prefix and garble the parse. Forcing this on fixes it.
  ::nearby::NearbyFlags::GetInstance().OverrideBoolFlagValue(
      ::nearby::connections::config_package_nearby::nearby_connections_feature::
          kRefactorBleL2cap,
      true);

  BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());
  printf("Adapter address: %s\n", adapter.GetMacAddress().ToString().c_str());

  SharingContext context(adapter);
  DeviceInfo device_info;
  NoOpConnectivityManager connectivity_manager;
  ::nearby::TaskRunnerImpl callback_task_runner(/*runner_count=*/1);

  std::string save_dir =
      argc > 1 ? std::string(argv[1]) : device_info.GetDownloadPath().ToString();
  printf("Saving incoming files to: %s\n", save_dir.c_str());

  auto connections_service = std::make_unique<NearbyConnectionsServiceImpl>(
      &connectivity_manager, /*event_logger=*/nullptr);
  NearbyConnectionsManagerImpl manager(&callback_task_runner, &context,
                                      connectivity_manager, device_info,
                                      std::move(connections_service));
  manager.SetCustomSavePath(save_dir);

  std::vector<uint8_t> salt(Advertisement::kSaltSize, 0);
  std::vector<uint8_t> encrypted_metadata_key(
      Advertisement::kMetadataEncryptionKeyHashByteSize, 0);
  std::unique_ptr<Advertisement> advertisement = Advertisement::NewInstance(
      std::move(salt), std::move(encrypted_metadata_key),
      ShareTargetType::kLaptop, std::string("quickshare-linux"),
      /*vendor_id=*/0, AdvertisementCapabilities{});
  if (advertisement == nullptr) {
    printf("Advertisement::NewInstance failed\n");
    return 1;
  }
  std::vector<uint8_t> endpoint_info = advertisement->ToEndpointInfo();

  auto session = std::make_shared<ReceiveSession>(manager);
  bool start_result_known = false;
  bool start_ok = false;
  // TEMPORARY: explicitly exclude web_rtc/bluetooth-classic (unimplemented
  // on this platform). BLE must stay on -- Quick Share's "nearby devices"
  // discovery on the phone is BLE-scan-based, so without it we're invisible
  // to the sender (confirmed: disabling BLE made the phone's Quick Share
  // sheet not show this device at all).
  //
  // wifi_hotspot=true: this is what actually lets bandwidth-upgrade work at
  // all for a BLE-bootstrapped connection. Confirmed via a real-hardware
  // test (2026-11-09) that a real phone's own ConnectionRequestFrame, sent
  // over BLE, declares its supported upgrade mediums as { WIFI_DIRECT,
  // WIFI_AWARE, WIFI_HOTSPOT, BLE_L2CAP, BLUETOOTH, BLE, NFC } -- WIFI_LAN
  // is conspicuously absent (real Android only offers WIFI_LAN as an
  // upgrade medium when the connection was *already* over WIFI_LAN from the
  // start, which is a different, already-working case). With
  // wifi_hotspot=false here, BasePcpHandler::ComputeIntersectionOfSupported
  // Mediums had nothing in common with the phone's list at all ("Our
  // supported medium name: WIFI_LAN, BLE" vs. the phone's list above has no
  // overlap beyond BLE itself, which isn't a valid upgrade target) --
  // confirmed by the exact failure: "BwuManager cannot initiate bandwidth
  // upgrade ... because the current BandwidthUpgradeMedium cannot be
  // deduced." Since we (the file receiver) are the one calling
  // UpgradeBandwidth() below, we become the BWU *initiator*, which for
  // WIFI_HOTSPOT means *we* host the SoftAP
  // (WifiHotspotBwuHandler::HandleInitializeUpgradedMediumForEndpoint calls
  // StartWifiHotspot()) and the phone joins it -- see wifi_hotspot.h's
  // file-level comment for the full role analysis.
  ::nearby::sharing::MediumSelection wifi_lan_only(
      /*bluetooth=*/false, /*ble=*/true, /*web_rtc=*/false,
      /*wifi_lan=*/true, /*wifi_hotspot=*/true);
  manager.StartAdvertising(
      endpoint_info, session.get(), ::nearby::sharing::PowerLevel::kHighPower,
      ::nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
      /*use_stable_endpoint_id=*/false, /*force_new_endpoint_id=*/false,
      [&](Status status) {
        start_ok = status == Status::kSuccess;
        start_result_known = true;
        printf("StartAdvertising callback: status=%d\n",
               static_cast<int>(status));
      },
      wifi_lan_only);

  for (int i = 0; i < 50 && !start_result_known; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!start_result_known || !start_ok) {
    printf("StartAdvertising failed.\n");
    manager.Shutdown();
    return 1;
  }

  printf("Advertising as \"quickshare-linux\" for up to 120s -- send a file "
         "to this device from the phone's Quick Share...\n");
  for (int i = 0; i < 1200 && !session->done(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }

  bool ok = session->done() && session->success();
  printf(session->done() ? (ok ? "\nSUCCESS: file(s) received.\n"
                               : "\nFAILED: see log above.\n")
                         : "\nTimed out waiting for a sender.\n");

  manager.Shutdown();
  return ok ? 0 : 1;
}
