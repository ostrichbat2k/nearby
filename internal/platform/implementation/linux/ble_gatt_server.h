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
// GATT server (peripheral role): publishes a local GATT service hierarchy
// via org.bluez.GattManager1.RegisterApplication, exposing our own
// GattService1/GattCharacteristic1 D-Bus objects for BlueZ to serve to
// remote GATT clients.
//
// BlueZ requires the whole object hierarchy under the application's
// ObjectManager to exist *before* RegisterApplication is called, and treats
// it as frozen afterward (see doc/org.bluez.GattManager.rst in the BlueZ
// source: "Once a service has been registered, the objects of a service
// should not be removed... InterfacesAdded signals will be ignored.").
// CreateCharacteristic() therefore re-registers the whole application with
// BlueZ (unregister, then register again) every time it adds a new
// characteristic, rather than trying to add to an already-registered tree.

#ifndef PLATFORM_IMPL_LINUX_BLE_GATT_SERVER_H_
#define PLATFORM_IMPL_LINUX_BLE_GATT_SERVER_H_

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/base/thread_annotations.h"
#include "absl/status/status.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

class GattServer : public api::ble::GattServer {
 public:
  explicit GattServer(api::ble::ServerGattConnectionCallback callback);
  ~GattServer() override;

  std::optional<api::ble::GattCharacteristic> CreateCharacteristic(
      const Uuid& service_uuid, const Uuid& characteristic_uuid,
      api::ble::GattCharacteristic::Permission permission,
      api::ble::GattCharacteristic::Property property,
      const ByteArray& initial_value) override;

  bool UpdateCharacteristic(const api::ble::GattCharacteristic& characteristic,
                            const ByteArray& value) override;

  absl::Status NotifyCharacteristicChanged(
      const api::ble::GattCharacteristic& characteristic, bool confirm,
      const ByteArray& new_value) override;

  void Stop() override;

 private:
  struct CharacteristicState {
    api::ble::GattCharacteristic info;
    ByteArray value;
    std::string object_path;
    std::unique_ptr<sdbus::IObject> dbus_object;
    bool notifying = false;
  };

  struct ServiceState {
    Uuid uuid;
    std::string object_path;
    std::unique_ptr<sdbus::IObject> dbus_object;
  };

  // Must be called without mutex_ held -- these block on a BlueZ round-trip
  // that can reenter our own property getters (which lock mutex_) on
  // BluezSystemBus's event-loop thread; see the .cc file for details.
  bool RegisterWithBluez() ABSL_LOCKS_EXCLUDED(mutex_);
  void UnregisterFromBluez() ABSL_LOCKS_EXCLUDED(mutex_);

  std::string CharacteristicKey(const Uuid& service_uuid,
                                const Uuid& characteristic_uuid) const;
  CharacteristicState* FindCharacteristic(
      const api::ble::GattCharacteristic& characteristic)
      ABSL_LOCKS_EXCLUDED(mutex_);

  // D-Bus method handlers for a characteristic object, dispatched on
  // BluezSystemBus's event-loop thread. `state` points into characteristics_
  // and stays valid for the server's lifetime (entries are never erased).
  std::vector<uint8_t> OnReadValue(
      CharacteristicState* state,
      const std::map<std::string, sdbus::Variant>& options);
  void OnWriteValue(CharacteristicState* state,
                    const std::vector<uint8_t>& value,
                    const std::map<std::string, sdbus::Variant>& options);
  void OnStartNotify(CharacteristicState* state);
  void OnStopNotify(CharacteristicState* state);

  api::ble::ServerGattConnectionCallback callback_;

  std::mutex mutex_;
  std::string app_path_;
  std::unique_ptr<sdbus::IObject> app_object_;
  std::unique_ptr<sdbus::IProxy> gatt_manager_proxy_;
  bool registered_ = false;
  std::map<std::string, ServiceState> services_;
  std::map<std::string, CharacteristicState> characteristics_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLE_GATT_SERVER_H_
