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
// daemon and expects a real external BLE central to connect) for the
// BlueZ-backed GattServer. Run with `bazel run`. Starts BLE advertising for
// the Nearby/Copresence service UUID (0000fef3-...) plus a GATT server
// exposing one read-only characteristic under that service with a known
// test payload, then waits -- use any BLE scanner (e.g. a phone with a
// generic BLE debug app, or `bluetoothctl`'s own `menu gatt` from a second
// machine) to discover this PC, connect, and read the characteristic back.

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

  // Same UUID as the BLE advertisement service AND the Nearby Connections
  // "Copresence" GATT service (connections/implementation/mediums/ble/
  // ble_utils.cc's kCopresenceServiceUuid) -- both roles share this UUID.
  nearby::Uuid copresence_service_uuid =
      *nearby::Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb");
  nearby::Uuid characteristic_uuid =
      *nearby::Uuid::FromString("8fc1b45c-2a55-4f32-8d8e-0000000000aa");

  nearby::api::ble::ServerGattConnectionCallback gatt_callback;
  gatt_callback.characteristic_subscription_cb =
      [](const nearby::api::ble::GattCharacteristic& characteristic) {
        printf("Remote device subscribed to characteristic %s\n",
               std::string(characteristic.uuid).c_str());
      };
  gatt_callback.characteristic_unsubscription_cb =
      [](const nearby::api::ble::GattCharacteristic& characteristic) {
        printf("Remote device unsubscribed from characteristic %s\n",
               std::string(characteristic.uuid).c_str());
      };

  std::unique_ptr<nearby::api::ble::GattServer> gatt_server =
      ble.StartGattServer(std::move(gatt_callback));
  if (gatt_server == nullptr) {
    printf("StartGattServer returned nullptr\n");
    return 1;
  }
  printf("StartGattServer succeeded\n");

  nearby::ByteArray test_value("hello from linux gatt server");
  std::optional<nearby::api::ble::GattCharacteristic> characteristic =
      gatt_server->CreateCharacteristic(
          copresence_service_uuid, characteristic_uuid,
          nearby::api::ble::GattCharacteristic::Permission::kRead,
          nearby::api::ble::GattCharacteristic::Property::kRead, test_value);
  if (!characteristic.has_value()) {
    printf("CreateCharacteristic failed\n");
    gatt_server->Stop();
    return 1;
  }
  printf("CreateCharacteristic + RegisterApplication succeeded\n");

  nearby::api::ble::BleAdvertisementData advertising_data;
  advertising_data.is_extended_advertisement = false;
  advertising_data.service_data[copresence_service_uuid] =
      nearby::ByteArray(std::string("\x11\x01hi", 4));
  nearby::api::ble::AdvertiseParameters params;
  params.tx_power_level = nearby::api::ble::TxPowerLevel::kHigh;
  params.is_connectable = true;

  bool advertising = ble.StartAdvertising(advertising_data, params);
  printf("StartAdvertising: %d\n", advertising);

  printf("Waiting 60s -- connect from a real BLE central now and read "
         "characteristic %s under service %s.\n",
         std::string(characteristic_uuid).c_str(),
         std::string(copresence_service_uuid).c_str());
  std::this_thread::sleep_for(std::chrono::seconds(60));

  printf("StopAdvertising: %d\n", ble.StopAdvertising());
  gatt_server->Stop();
  printf("GATT server stopped\n");

  return 0;
}
