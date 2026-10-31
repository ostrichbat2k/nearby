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
// GATT client (central role): connects to a remote device's GATT server
// (e.g. a phone advertising Nearby/Quick Share) via org.bluez.Device1, then
// discovers its GattService1/GattCharacteristic1 children through the
// shared ObjectManager and talks to them directly.

#ifndef PLATFORM_IMPL_LINUX_BLE_GATT_CLIENT_H_
#define PLATFORM_IMPL_LINUX_BLE_GATT_CLIENT_H_

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

class GattClient : public api::ble::GattClient {
 public:
  GattClient(std::string device_path,
             api::ble::ClientGattConnectionCallback callback);
  ~GattClient() override;

  bool DiscoverServiceAndCharacteristics(
      const Uuid& service_uuid,
      const std::vector<Uuid>& characteristic_uuids) override;

  std::optional<api::ble::GattCharacteristic> GetCharacteristic(
      const Uuid& service_uuid, const Uuid& characteristic_uuid) override;

  std::optional<std::string> ReadCharacteristic(
      const api::ble::GattCharacteristic& characteristic) override;

  bool WriteCharacteristic(const api::ble::GattCharacteristic& characteristic,
                           absl::string_view value, WriteType type) override;

  void Disconnect() override;

 private:
  struct DiscoveredCharacteristic {
    std::string object_path;
    api::ble::GattCharacteristic info;
  };

  bool EnsureConnected();
  bool WaitForServicesResolved();
  std::string CharacteristicKey(const Uuid& service_uuid,
                                const Uuid& characteristic_uuid) const;
  // Caller must hold mutex_.
  sdbus::IProxy* GetOrCreateCharacteristicProxy(const std::string& object_path);
  // Starts notifications on the given characteristic and bridges its
  // PropertiesChanged(Value) signal to callback_.on_characteristic_changed_cb.
  // Must not be called while holding mutex_.
  void SubscribeToNotifications(const std::string& object_path);

  std::string device_path_;
  api::ble::ClientGattConnectionCallback callback_;

  std::mutex mutex_;
  std::unique_ptr<sdbus::IProxy> device_proxy_;
  bool connected_ = false;
  std::map<std::string, DiscoveredCharacteristic> characteristics_;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>>
      characteristic_proxies_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLE_GATT_CLIENT_H_
