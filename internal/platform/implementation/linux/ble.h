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
// BLE medium backed by BlueZ over D-Bus. Covers advertising (via
// org.bluez.LEAdvertisingManager1, exposing our own LEAdvertisement1 object)
// and scanning (via org.bluez.Adapter1 discovery + the standard
// ObjectManager/Properties signals BlueZ fires as it learns about nearby
// devices). GATT server/client and the BLE socket abstractions built on top
// of them are not implemented yet (see the TODOs below) -- advertising and
// scanning alone are enough to validate BLE discovery against a real
// Android phone, which is the current milestone.
//
// All D-Bus I/O for a given device discovery goes through BluezSystemBus's
// single shared connection/event-loop thread (see bluez.h), so the
// ScanCallback/AdvertisingCallback callbacks below are invoked from that
// thread, not from the caller's thread.

#ifndef PLATFORM_IMPL_LINUX_BLE_H_
#define PLATFORM_IMPL_LINUX_BLE_H_

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/functional/any_invocable.h"
#include "internal/platform/cancellation_flag.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

class BleMedium : public api::ble::BleMedium {
 public:
  BleMedium();
  ~BleMedium() override;

  bool StartAdvertising(
      const api::ble::BleAdvertisementData& advertising_data,
      api::ble::AdvertiseParameters advertise_set_parameters) override;

  std::unique_ptr<AdvertisingSession> StartAdvertising(
      const api::ble::BleAdvertisementData& advertising_data,
      api::ble::AdvertiseParameters advertise_set_parameters,
      AdvertisingCallback callback) override;

  bool StopAdvertising() override;

  bool StartScanning(const Uuid& service_uuid,
                      api::ble::TxPowerLevel tx_power_level,
                      ScanCallback callback) override;

  bool StopScanning() override;

  std::unique_ptr<ScanningSession> StartScanning(
      const Uuid& service_uuid, api::ble::TxPowerLevel tx_power_level,
      ScanningCallback callback) override;

  // TODO: implement via org.bluez.GattManager1 (RegisterApplication),
  // exposing our characteristics as GattService1/GattCharacteristic1 D-Bus
  // objects for BlueZ to serve. Not needed for BLE discovery; needed before
  // the BLE medium can carry the UKEY2/OfflineFrames handshake itself.
  std::unique_ptr<api::ble::GattServer> StartGattServer(
      api::ble::ServerGattConnectionCallback callback) override {
    return nullptr;
  }

  std::unique_ptr<api::ble::GattClient> ConnectToGattServer(
      api::ble::BlePeripheral::UniqueId peripheral_id,
      api::ble::TxPowerLevel tx_power_level,
      api::ble::ClientGattConnectionCallback callback) override;

  // TODO: implement as a GATT-characteristic-emulated stream (write
  // characteristic for inbound data, notify characteristic for outbound),
  // once StartGattServer exists -- this is what Android's non-L2CAP
  // BleSocket is built on.
  std::unique_ptr<api::ble::BleServerSocket> OpenServerSocket(
      const std::string& service_id) override {
    return nullptr;
  }

  // TODO: see OpenServerSocket.
  std::unique_ptr<api::ble::BleSocket> Connect(
      const std::string& service_id, api::ble::TxPowerLevel tx_power_level,
      api::ble::BlePeripheral::UniqueId peripheral_id,
      CancellationFlag* cancellation_flag) override {
    return nullptr;
  }

  bool IsExtendedAdvertisementsAvailable() override { return false; }

 private:
  struct Advertisement {
    std::string object_path;
    std::unique_ptr<sdbus::IObject> dbus_object;
    std::string type;
    std::vector<std::string> service_uuids;
    std::map<std::string, sdbus::Variant> service_data;
  };

  // Shared by both StartScanning overloads. advertisement_found_cb is called
  // on every (re-)discovery of an advertisement carrying `service_uuid` in
  // its ServiceData; advertisement_lost_cb is only invoked by the
  // ScanningSession overload's richer callback (ScanCallback has no "lost"
  // notion), so it's optional.
  bool StartScanningInternal(
      const Uuid& service_uuid,
      absl::AnyInvocable<void(api::ble::BlePeripheral::UniqueId,
                              api::ble::BleAdvertisementData)>
          advertisement_found_cb);

  void OnDeviceInterfacesAdded(
      const sdbus::ObjectPath& path,
      const std::map<std::string, std::map<std::string, sdbus::Variant>>&
          interfaces_and_properties);
  void OnDevicePropertiesChanged(
      const std::string& device_path, const std::string& interface,
      const std::map<std::string, sdbus::Variant>& changed_properties);
  void HandleDeviceProperties(
      const std::string& device_path,
      const std::map<std::string, sdbus::Variant>& properties);

  std::string adapter_path_;
  std::unique_ptr<sdbus::IProxy> adapter_proxy_;

  std::mutex mutex_;
  bool scanning_ = false;
  Uuid scanning_service_uuid_;
  absl::AnyInvocable<void(api::ble::BlePeripheral::UniqueId,
                          api::ble::BleAdvertisementData)>
      advertisement_found_cb_;
  std::unique_ptr<sdbus::IProxy> object_manager_proxy_;
  std::vector<std::unique_ptr<sdbus::IProxy>> device_property_watchers_;

  std::atomic<int> next_advertisement_id_{0};
  std::unique_ptr<Advertisement> advertisement_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLE_H_
