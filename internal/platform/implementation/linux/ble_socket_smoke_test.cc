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
// daemon and a real phone) for the Weave-based BleSocket client path
// (BleMedium::Connect()). Run with `bazel run`, with the target phone's
// Quick Share "ready to receive" screen open so it is advertising
// 0000fef3-... and running its GATT server.
//
// This is a *discovery* test first and foremost: it is not yet known
// whether a real phone's Quick Share GATT server actually hosts the Weave
// ToPeripheralChar/FromPeripheralChar characteristics (UUIDs copied from
// the google/nearby Apple backend's own GNSUtils.m/ble_medium.mm -- see
// ble_socket.h) while just sitting on that screen, as opposed to only
// registering them dynamically once it has actually decided BLE is the
// medium for an in-progress Nearby Connections handshake. So this test:
//   1. Scans for and connects to the phone's GATT server (as the existing
//      ble_gatt_smoke_test does).
//   2. Dumps the raw GATT tree and explicitly reports whether the two Weave
//      characteristic UUIDs are present under the Copresence service.
//   3. If the GattClient's own DiscoverServiceAndCharacteristics() for those
//      two characteristics succeeds, calls the real BleMedium::Connect()
//      (the production code path) and, if it returns a socket, writes a
//      short message and waits briefly for anything the phone sends back.
// Step 3 only runs if step 2's discovery succeeds -- if the phone doesn't
// expose these characteristics at rest, that's an expected, logged outcome,
// not a crash.

#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "absl/strings/string_view.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/exception.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/ble.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/input_stream.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/output_stream.h"
#include "internal/platform/uuid.h"

namespace {

using nearby::linux_platform::BluezSystemBus;
using nearby::linux_platform::kGattCharacteristic1Interface;

// Returns {to_peripheral_found, from_peripheral_found}.
std::pair<bool, bool> DumpGattTreeAndFindWeaveCharacteristics(
    const std::string& device_path, const nearby::Uuid& to_peripheral_uuid,
    const nearby::Uuid& from_peripheral_uuid) {
  bool found_to_peripheral = false;
  bool found_from_peripheral = false;
  std::string prefix = device_path + "/";
  printf("-- Raw GATT tree under %s --\n", device_path.c_str());
  for (const auto& [path, interfaces] :
       BluezSystemBus::Get().GetManagedObjects()) {
    if (std::string(path).rfind(prefix, 0) != 0) continue;
    auto char_it = interfaces.find(kGattCharacteristic1Interface);
    if (char_it == interfaces.end()) continue;
    auto uuid_it = char_it->second.find("UUID");
    if (uuid_it == char_it->second.end()) continue;
    std::string uuid_str = uuid_it->second.get<std::string>();
    printf("  Characteristic %s uuid=%s\n", std::string(path).c_str(),
           uuid_str.c_str());
    std::optional<nearby::Uuid> uuid = nearby::Uuid::FromString(uuid_str);
    if (!uuid.has_value()) continue;
    if (*uuid == to_peripheral_uuid) found_to_peripheral = true;
    if (*uuid == from_peripheral_uuid) found_from_peripheral = true;
  }
  printf("-- end GATT tree --\n");
  return {found_to_peripheral, found_from_peripheral};
}

}  // namespace

int main() {
  nearby::linux_platform::BluetoothAdapter adapter;
  printf("Adapter enabled: %d\n", adapter.IsEnabled());

  nearby::linux_platform::BleMedium ble;

  nearby::Uuid copresence_service_uuid =
      *nearby::Uuid::FromString("0000fef3-0000-1000-8000-00805f9b34fb");
  nearby::Uuid to_peripheral_uuid =
      *nearby::Uuid::FromString("00000100-0004-1000-8000-001a11000101");
  nearby::Uuid from_peripheral_uuid =
      *nearby::Uuid::FromString("00000100-0004-1000-8000-001a11000102");

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
  gatt_callback.on_characteristic_changed_cb = [](absl::string_view value) {
    printf("Characteristic changed, %zu bytes\n", value.size());
  };
  gatt_callback.disconnected_cb = []() { printf("GATT disconnected\n"); };

  std::unique_ptr<nearby::api::ble::GattClient> probe_gatt_client =
      ble.ConnectToGattServer(*found_peripheral,
                              nearby::api::ble::TxPowerLevel::kHigh,
                              std::move(gatt_callback));
  if (probe_gatt_client == nullptr) {
    printf("ConnectToGattServer returned nullptr\n");
    return 1;
  }
  printf("ConnectToGattServer succeeded (probe connection)\n");

  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  bool found_to_peripheral = false;
  bool found_from_peripheral = false;
  if (adapter_path.has_value()) {
    nearby::MacAddress mac;
    if (nearby::MacAddress::FromUint64(*found_peripheral, mac)) {
      std::tie(found_to_peripheral, found_from_peripheral) =
          DumpGattTreeAndFindWeaveCharacteristics(
              nearby::linux_platform::DevicePathForMacAddress(*adapter_path,
                                                              mac),
              to_peripheral_uuid, from_peripheral_uuid);
    }
  }
  printf("Weave ToPeripheralChar present: %d\n", found_to_peripheral);
  printf("Weave FromPeripheralChar present: %d\n", found_from_peripheral);

  bool discovered = probe_gatt_client->DiscoverServiceAndCharacteristics(
      copresence_service_uuid, {to_peripheral_uuid, from_peripheral_uuid});
  printf("DiscoverServiceAndCharacteristics(Weave chars): %d\n", discovered);
  probe_gatt_client->Disconnect();

  if (!discovered) {
    printf(
        "Phone's GATT server does not expose the Weave characteristics at "
        "rest -- BleMedium::Connect() would fail here too, skipping.\n");
    return 0;
  }

  printf("Attempting the real BleMedium::Connect() path...\n");
  nearby::CancellationFlag cancellation_flag;
  std::unique_ptr<nearby::api::ble::BleSocket> socket = ble.Connect(
      "quickshare-linux-smoke-test", nearby::api::ble::TxPowerLevel::kHigh,
      *found_peripheral, &cancellation_flag);
  if (socket == nullptr) {
    printf("BleMedium::Connect() returned nullptr (Weave handshake failed "
           "or timed out) -- see WARNING/ERROR logs above.\n");
    return 1;
  }
  printf("BleMedium::Connect() succeeded! Weave handshake completed.\n");

  std::string outgoing = "hello from quickshare-linux";
  nearby::Exception write_result =
      socket->GetOutputStream().Write(outgoing);
  printf("Write(%zu bytes): exception=%d\n", outgoing.size(),
         write_result.value);

  printf("Waiting up to 5s for any data the phone sends back...\n");
  // Read() has no built-in timeout, so run it on a background thread and
  // enforce the deadline by closing the socket (which unblocks it with
  // EOF) from the main thread; join before the socket is destroyed so
  // there's no race between the reader thread and ~BleSocket().
  std::thread reader([&socket]() {
    nearby::ExceptionOr<nearby::ByteArray> read_result =
        socket->GetInputStream().Read(512);
    if (read_result.ok()) {
      printf("Read %zu bytes back: \"%s\"\n", read_result.result().size(),
             std::string(read_result.result()).c_str());
    } else {
      printf("Read failed/EOF: exception=%d\n",
             static_cast<int>(read_result.exception()));
    }
  });
  std::this_thread::sleep_for(std::chrono::seconds(5));
  socket->Close();
  reader.join();
  printf("Closed.\n");
  return 0;
}
