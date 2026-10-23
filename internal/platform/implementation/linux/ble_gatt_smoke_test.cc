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
// daemon and a real phone) for the BlueZ-backed GattClient. Run with
// `bazel run`, with the target phone's Quick Share "ready to receive" screen
// open so it is advertising 0000fef3-... and running its GATT server.
//
// 0000fef3-0000-1000-8000-00805f9b34fb is both Google's "Nearby" BLE
// advertisement service UUID *and* the Nearby Connections "Copresence" GATT
// service UUID (see connections/implementation/mediums/ble/ble_utils.cc) --
// the same UUID serves both roles. This test first scans for that
// advertisement to find the phone, then connects to its GATT server and
// tries to discover that same service via GattClient, and separately dumps
// every GATT service/characteristic BlueZ sees on the device (raw, bypassing
// GattClient) so the real UUIDs/flags Quick Share's GATT server exposes are
// visible for future protocol-level work. As a secondary, simpler check it
// also tries to read the standard Generic Access/Device Name characteristic,
// which should be present on any connectable BLE peripheral.

#include <chrono>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/ble.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"

namespace {

using nearby::linux_platform::BluezSystemBus;
using nearby::linux_platform::kGattCharacteristic1Interface;
using nearby::linux_platform::kGattService1Interface;
using nearby::linux_platform::ToLowerUuidString;

void DumpGattTree(const std::string& device_path) {
  printf("-- Raw GATT tree under %s --\n", device_path.c_str());
  std::string prefix = device_path + "/";
  for (const auto& [path, interfaces] : BluezSystemBus::Get().GetManagedObjects()) {
    if (std::string(path).rfind(prefix, 0) != 0) continue;

    auto service_it = interfaces.find(kGattService1Interface);
    if (service_it != interfaces.end()) {
      auto uuid_it = service_it->second.find("UUID");
      printf("  Service %s uuid=%s\n", std::string(path).c_str(),
             uuid_it != service_it->second.end()
                 ? uuid_it->second.get<std::string>().c_str()
                 : "?");
      continue;
    }

    auto char_it = interfaces.find(kGattCharacteristic1Interface);
    if (char_it != interfaces.end()) {
      auto uuid_it = char_it->second.find("UUID");
      auto flags_it = char_it->second.find("Flags");
      std::string flags_str;
      if (flags_it != char_it->second.end()) {
        for (const auto& f : flags_it->second.get<std::vector<std::string>>()) {
          flags_str += f + ",";
        }
      }
      printf("  Characteristic %s uuid=%s flags=%s\n", std::string(path).c_str(),
             uuid_it != char_it->second.end()
                 ? uuid_it->second.get<std::string>().c_str()
                 : "?",
             flags_str.c_str());
    }
  }
  printf("-- end GATT tree --\n");
}

}  // namespace

int main() {
  nearby::linux_platform::BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());

  nearby::linux_platform::BleMedium ble;

  nearby::Uuid copresence_service_uuid =
      *nearby::Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb");

  std::optional<nearby::api::ble::BlePeripheral::UniqueId> found_peripheral;

  nearby::api::ble::BleMedium::ScanCallback scan_callback;
  scan_callback.advertisement_found_cb =
      [&found_peripheral](nearby::api::ble::BlePeripheral::UniqueId id,
                           nearby::api::ble::BleAdvertisementData /*data*/) {
        if (!found_peripheral.has_value()) {
          printf("Found Quick Share peripheral id=%llu\n",
                 static_cast<unsigned long long>(id));
        }
        found_peripheral = id;
      };

  bool started = ble.StartScanning(copresence_service_uuid,
                                   nearby::api::ble::TxPowerLevel::kHigh,
                                   std::move(scan_callback));
  printf("StartScanning: %d\n", started);

  for (int i = 0; i < 20 && !found_peripheral.has_value(); ++i) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  ble.StopScanning();

  if (!found_peripheral.has_value()) {
    printf("Never found a Quick Share peripheral -- is the phone's \"ready "
           "to receive\" screen open?\n");
    return 1;
  }

  nearby::api::ble::ClientGattConnectionCallback gatt_callback;
  gatt_callback.on_characteristic_changed_cb =
      [](absl::string_view value) {
        printf("Characteristic changed, %zu bytes\n", value.size());
      };
  gatt_callback.disconnected_cb = []() { printf("GATT disconnected\n"); };

  std::unique_ptr<nearby::api::ble::GattClient> gatt_client =
      ble.ConnectToGattServer(*found_peripheral,
                              nearby::api::ble::TxPowerLevel::kHigh,
                              std::move(gatt_callback));
  if (gatt_client == nullptr) {
    printf("ConnectToGattServer returned nullptr\n");
    return 1;
  }
  printf("ConnectToGattServer succeeded\n");

  // Raw dump of whatever BlueZ resolved, independent of GattClient's own
  // matching logic -- shows the real Copresence service/characteristic
  // UUIDs this phone's Quick Share actually exposes.
  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  if (adapter_path.has_value()) {
    nearby::MacAddress mac;
    if (nearby::MacAddress::FromUint64(*found_peripheral, mac)) {
      DumpGattTree(nearby::linux_platform::DevicePathForMacAddress(
          *adapter_path, mac));
    }
  }

  bool discovered_copresence =
      gatt_client->DiscoverServiceAndCharacteristics(copresence_service_uuid,
                                                      {});
  printf("DiscoverServiceAndCharacteristics(Copresence, no characteristics): "
         "%d\n",
         discovered_copresence);

  nearby::Uuid generic_access_service =
      *nearby::Uuid::FromString("00001800-0000-1000-8000-00805f9b34fb");
  nearby::Uuid device_name_characteristic =
      *nearby::Uuid::FromString("00002a00-0000-1000-8000-00805f9b34fb");

  bool discovered_generic_access =
      gatt_client->DiscoverServiceAndCharacteristics(
          generic_access_service, {device_name_characteristic});
  printf("DiscoverServiceAndCharacteristics(Generic Access/Device Name): %d\n",
         discovered_generic_access);

  if (discovered_generic_access) {
    std::optional<nearby::api::ble::GattCharacteristic> characteristic =
        gatt_client->GetCharacteristic(generic_access_service,
                                       device_name_characteristic);
    if (characteristic.has_value()) {
      std::optional<std::string> value =
          gatt_client->ReadCharacteristic(*characteristic);
      if (value.has_value()) {
        printf("Device Name read: \"%s\"\n", value->c_str());
      } else {
        printf("Failed to read Device Name characteristic\n");
      }
    }
  }

  gatt_client->Disconnect();
  printf("Disconnected\n");

  return 0;
}
