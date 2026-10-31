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
// Manual driver (not an automated test): advertises a real,
// Quick-Share-identifiable BLE endpoint from this machine, using the real
// production NearbyConnectionsManagerImpl + NearbyConnectionsServiceImpl
// (service ID "NearbySharing", matching what the real Android app uses) --
// not a generic connections::Core smoke test, and not a hand-rolled
// endpoint_info encoding. If the device name shows up in a real phone's
// Quick Share "send to" list, that's end-to-end proof this machine looks
// like a genuine Nearby Share receiver.
//
// Device name is sent in the clear ("everyone" visibility -- see
// sharing/advertisement.h's own comment: a device name present means
// visible-to-everyone, no certificate/account backend needed to decode it).
// Does not yet attempt to actually accept a real payload transfer.

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "sharing/advertisement.h"
#include "sharing/common/nearby_share_enums.h"
#include "sharing/nearby_connections_manager.h"
#include "sharing/nearby_connections_manager_impl.h"
#include "sharing/nearby_connections_service_impl.h"
#include "sharing/proto/enums.pb.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/device_info.h"
#include "internal/platform/implementation/linux/sharing_context.h"
#include "internal/platform/task_runner_impl.h"

namespace {

using nearby::linux_platform::BluetoothAdapter;
using nearby::linux_platform::DeviceInfo;
using nearby::linux_platform::NoOpConnectivityManager;
using nearby::linux_platform::SharingContext;
using nearby::sharing::Advertisement;
using nearby::sharing::AdvertisementCapabilities;
using nearby::sharing::NearbyConnection;
using nearby::sharing::NearbyConnectionsManager;
using nearby::sharing::NearbyConnectionsManagerImpl;
using nearby::sharing::NearbyConnectionsServiceImpl;
using nearby::sharing::ShareTargetType;

class LoggingIncomingConnectionListener
    : public NearbyConnectionsManager::IncomingConnectionListener {
 public:
  void OnIncomingConnection(absl::string_view endpoint_id,
                            absl::Span<const uint8_t> endpoint_info,
                            NearbyConnection* connection) override {
    printf("OnIncomingConnection: endpoint_id=%s, endpoint_info=%zu bytes\n",
           std::string(endpoint_id).c_str(), endpoint_info.size());
  }
};

}  // namespace

int main() {
  BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());
  printf("Adapter address: %s\n", adapter.GetMacAddress().ToString().c_str());

  SharingContext context(adapter);
  DeviceInfo device_info;
  NoOpConnectivityManager connectivity_manager;
  nearby::TaskRunnerImpl callback_task_runner(/*runner_count=*/1);

  auto connections_service = std::make_unique<NearbyConnectionsServiceImpl>(
      &connectivity_manager, /*event_logger=*/nullptr);
  NearbyConnectionsManagerImpl manager(&callback_task_runner, &context,
                                       connectivity_manager, device_info,
                                       std::move(connections_service));

  // "Everyone" visibility: a real device_name means the bytes are sent in
  // the clear (see advertisement.h) -- salt/encrypted_metadata_key are
  // still fixed-size fields in the wire format, but nothing decrypts them
  // when a device_name is already present, so arbitrary placeholder bytes
  // are fine here.
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
  printf("Built endpoint_info: %zu bytes\n", endpoint_info.size());

  LoggingIncomingConnectionListener listener;
  bool start_result_known = false;
  bool start_ok = false;
  manager.StartAdvertising(
      endpoint_info, &listener, nearby::sharing::PowerLevel::kHighPower,
      nearby::sharing::proto::DataUsage::ONLINE_DATA_USAGE,
      /*use_stable_endpoint_id=*/false, /*force_new_endpoint_id=*/false,
      [&](nearby::sharing::Status status) {
        start_ok = status == nearby::sharing::Status::kSuccess;
        start_result_known = true;
        printf("StartAdvertising callback: status=%d\n",
               static_cast<int>(status));
      });

  for (int i = 0; i < 50 && !start_result_known; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  if (!start_result_known) {
    printf("StartAdvertising callback never fired within 5s\n");
  } else if (!start_ok) {
    printf("StartAdvertising failed\n");
  } else {
    printf(
        "Advertising as \"quickshare-linux\" -- check the real phone's "
        "Quick Share send screen for up to 120s...\n");
    std::this_thread::sleep_for(std::chrono::seconds(120));
  }

  manager.Shutdown();
  printf("Done.\n");
  return 0;
}
