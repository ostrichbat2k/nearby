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
// Manual smoke test (not a unit test -- it talks to the real system BlueZ
// daemon) for the BlueZ-backed BluetoothAdapter and BleMedium. Run with
// `bazel run`. Not wired into any test suite; exercised ad hoc while
// standing up the backend.

#include <chrono>
#include <cstdio>
#include <thread>

#include "internal/platform/implementation/linux/ble.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/uuid.h"

int main() {
  nearby::linux_platform::BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());
  printf("Adapter name: %s\n", adapter.GetName().c_str());
  printf("Adapter address: %s\n", adapter.GetMacAddress().ToString().c_str());

  nearby::linux_platform::BleMedium ble;

  nearby::Uuid test_service_uuid("0000fef3-0000-1000-8000-00805f9b34fb");

  nearby::api::ble::BleMedium::ScanCallback scan_callback;
  scan_callback.advertisement_found_cb =
      [](nearby::api::ble::BlePeripheral::UniqueId id,
         nearby::api::ble::BleAdvertisementData data) {
        printf("Found advertisement from peripheral id=%llu, %zu service "
               "data entries\n",
               static_cast<unsigned long long>(id),
               data.service_data.size());
      };

  bool started =
      ble.StartScanning(test_service_uuid,
                         nearby::api::ble::TxPowerLevel::kHigh,
                         std::move(scan_callback));
  printf("StartScanning: %d\n", started);

  std::this_thread::sleep_for(std::chrono::seconds(5));

  printf("StopScanning: %d\n", ble.StopScanning());

  nearby::api::ble::BleAdvertisementData advertising_data;
  advertising_data.is_extended_advertisement = false;
  advertising_data.service_data[test_service_uuid] =
      nearby::ByteArray(std::string("hello"));
  nearby::api::ble::AdvertiseParameters params;
  params.tx_power_level = nearby::api::ble::TxPowerLevel::kHigh;
  params.is_connectable = true;

  bool advertising = ble.StartAdvertising(advertising_data, params);
  printf("StartAdvertising: %d\n", advertising);

  std::this_thread::sleep_for(std::chrono::seconds(5));

  printf("StopAdvertising: %d\n", ble.StopAdvertising());

  return 0;
}
