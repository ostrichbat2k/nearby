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

#include "internal/platform/implementation/linux/ble_gatt_server.h"

#include <atomic>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "internal/platform/byte_array.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/logging.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

namespace {

using Permission = api::ble::GattCharacteristic::Permission;
using Property = api::ble::GattCharacteristic::Property;

std::atomic<int> g_next_app_id{0};

std::vector<std::string> PropertyToFlags(Property property) {
  std::vector<std::string> flags;
  if ((property & Property::kRead) != Property::kNone) flags.push_back("read");
  if ((property & Property::kWrite) != Property::kNone) {
    flags.push_back("write");
  }
  if ((property & Property::kNotify) != Property::kNone) {
    flags.push_back("notify");
  }
  if ((property & Property::kIndicate) != Property::kNone) {
    flags.push_back("indicate");
  }
  return flags;
}

}  // namespace

GattServer::GattServer(api::ble::ServerGattConnectionCallback callback)
    : callback_(std::move(callback)) {
  app_path_ =
      "/com/github/nearby/linux/gatt_server" + std::to_string(g_next_app_id++);
  app_object_ = sdbus::createObject(BluezSystemBus::Get().connection(),
                                    sdbus::ObjectPath{app_path_});
  app_object_->addObjectManager();

  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  if (adapter_path.has_value()) {
    gatt_manager_proxy_ = BluezSystemBus::Get().CreateProxy(*adapter_path);
  } else {
    LOG(WARNING) << "No Bluetooth adapter found via BlueZ; GATT server will "
                    "be unavailable.";
  }
}

GattServer::~GattServer() { Stop(); }

bool GattServer::RegisterWithBluez() {
  // Must be called without mutex_ held: RegisterApplication causes BlueZ to
  // call GetManagedObjects on our own ObjectManager (handled transparently
  // by addObjectManager()) while handling this request, including our
  // "Value" property getter below, which itself locks mutex_. That's a call
  // back into an object we expose, arriving on this same shared connection
  // on BluezSystemBus's event-loop thread while the calling thread would
  // otherwise be blocked waiting for the reply -- holding mutex_ across
  // this call deadlocks against our own property getter. callMethodAsync +
  // a future (same reentrancy hazard/fix as RegisterAdvertisement in
  // ble.cc) only avoids the *connection* half of that; not holding mutex_
  // here avoids the rest.
  if (!gatt_manager_proxy_) return false;
  try {
    std::map<std::string, sdbus::Variant> options;
    auto future = gatt_manager_proxy_->callMethodAsync("RegisterApplication")
                      .onInterface(kGattManager1Interface)
                      .withArguments(sdbus::ObjectPath{app_path_}, options)
                      .getResultAsFuture<>();
    future.get();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to register GATT application " << app_path_ << ": "
               << e.what();
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  registered_ = true;
  return true;
}

void GattServer::UnregisterFromBluez() {
  // Must be called without mutex_ held; see RegisterWithBluez.
  bool was_registered;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_registered = registered_;
  }
  if (!was_registered || !gatt_manager_proxy_) return;
  try {
    auto future = gatt_manager_proxy_->callMethodAsync("UnregisterApplication")
                      .onInterface(kGattManager1Interface)
                      .withArguments(sdbus::ObjectPath{app_path_})
                      .getResultAsFuture<>();
    future.get();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to unregister GATT application " << app_path_
               << ": " << e.what();
  }
  std::lock_guard<std::mutex> lock(mutex_);
  registered_ = false;
}

std::string GattServer::CharacteristicKey(
    const Uuid& service_uuid, const Uuid& characteristic_uuid) const {
  return ToLowerUuidString(service_uuid) + "|" +
         ToLowerUuidString(characteristic_uuid);
}

std::optional<api::ble::GattCharacteristic> GattServer::CreateCharacteristic(
    const Uuid& service_uuid, const Uuid& characteristic_uuid,
    Permission permission, Property property,
    const ByteArray& initial_value) {
  api::ble::GattCharacteristic info;
  {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!app_object_) return std::nullopt;

  std::string char_key = CharacteristicKey(service_uuid, characteristic_uuid);
  if (characteristics_.find(char_key) != characteristics_.end()) {
    LOG(ERROR) << "Characteristic already exists: service="
               << std::string(service_uuid)
               << " characteristic=" << std::string(characteristic_uuid);
    return std::nullopt;
  }

  std::string service_key = ToLowerUuidString(service_uuid);
  auto service_it = services_.find(service_key);
  if (service_it == services_.end()) {
    ServiceState service;
    service.uuid = service_uuid;
    service.object_path =
        app_path_ + "/service" + std::to_string(services_.size());
    service.dbus_object = sdbus::createObject(
        BluezSystemBus::Get().connection(), sdbus::ObjectPath{service.object_path});
    std::string service_uuid_str = ToLowerUuidString(service_uuid);
    service.dbus_object
        ->addVTable(sdbus::registerProperty("UUID").withGetter(
                        [service_uuid_str]() { return service_uuid_str; }),
                    sdbus::registerProperty("Primary").withGetter(
                        []() { return true; }))
        .forInterface(kGattService1Interface);
    service_it = services_.emplace(service_key, std::move(service)).first;
  }
  ServiceState& service = service_it->second;

  auto [it, inserted] = characteristics_.emplace(char_key, CharacteristicState{});
  CharacteristicState* state = &it->second;
  state->info.uuid = characteristic_uuid;
  state->info.service_uuid = service_uuid;
  state->info.permission = permission;
  state->info.property = property;
  state->value = initial_value;
  state->object_path =
      service.object_path + "/char" + std::to_string(characteristics_.size());

  std::string char_uuid_str = ToLowerUuidString(characteristic_uuid);
  std::vector<std::string> flags = PropertyToFlags(property);
  std::string service_path_copy = service.object_path;

  state->dbus_object = sdbus::createObject(BluezSystemBus::Get().connection(),
                                           sdbus::ObjectPath{state->object_path});
  state->dbus_object
      ->addVTable(
          sdbus::registerProperty("UUID").withGetter(
              [char_uuid_str]() { return char_uuid_str; }),
          sdbus::registerProperty("Service").withGetter(
              [service_path_copy]() {
                return sdbus::ObjectPath{service_path_copy};
              }),
          sdbus::registerProperty("Flags").withGetter(
              [flags]() { return flags; }),
          sdbus::registerProperty("Value").withGetter([this, state]() {
            std::lock_guard<std::mutex> lock(mutex_);
            return ToBytes(state->value);
          }),
          sdbus::registerMethod("ReadValue")
              .implementedAs([this, state](
                                 const std::map<std::string, sdbus::Variant>&
                                     options) {
                return OnReadValue(state, options);
              }),
          sdbus::registerMethod("WriteValue")
              .implementedAs([this, state](
                                 const std::vector<uint8_t>& value,
                                 const std::map<std::string, sdbus::Variant>&
                                     options) {
                OnWriteValue(state, value, options);
              }),
          sdbus::registerMethod("StartNotify")
              .implementedAs([this, state]() { OnStartNotify(state); }),
          sdbus::registerMethod("StopNotify")
              .implementedAs([this, state]() { OnStopNotify(state); }))
      .forInterface(kGattCharacteristic1Interface);

  info = state->info;
  }  // mutex_ released here -- the BlueZ round-trips below must not hold it.

  // BlueZ freezes the application's object tree once RegisterApplication
  // succeeds (see doc/org.bluez.GattManager.rst) -- adding a characteristic
  // after that point requires unregistering and registering again with the
  // now-larger tree, rather than relying on InterfacesAdded (which BlueZ
  // explicitly ignores post-registration).
  UnregisterFromBluez();
  if (!RegisterWithBluez()) {
    return std::nullopt;
  }

  return info;
}

GattServer::CharacteristicState* GattServer::FindCharacteristic(
    const api::ble::GattCharacteristic& characteristic) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = characteristics_.find(CharacteristicKey(
      characteristic.service_uuid, characteristic.uuid));
  if (it == characteristics_.end()) return nullptr;
  return &it->second;
}

bool GattServer::UpdateCharacteristic(
    const api::ble::GattCharacteristic& characteristic,
    const ByteArray& value) {
  CharacteristicState* state = FindCharacteristic(characteristic);
  if (state == nullptr) {
    LOG(ERROR) << "UpdateCharacteristic: unknown characteristic "
               << std::string(characteristic.uuid);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  state->value = value;
  return true;
}

absl::Status GattServer::NotifyCharacteristicChanged(
    const api::ble::GattCharacteristic& characteristic, bool confirm,
    const ByteArray& new_value) {
  CharacteristicState* state = FindCharacteristic(characteristic);
  if (state == nullptr) {
    return absl::NotFoundError("Unknown characteristic: " +
                               std::string(characteristic.uuid));
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    state->value = new_value;
  }
  try {
    // Updating the Value property and emitting PropertiesChanged is how
    // BlueZ's server-side GATT implementation knows to push a real BLE
    // notification/indication to any remote device currently subscribed
    // (i.e. that called StartNotify()) -- a no-op if nobody is subscribed.
    state->dbus_object->emitPropertiesChangedSignal(
        kGattCharacteristic1Interface,
        std::vector<sdbus::PropertyName>{sdbus::PropertyName{"Value"}});
  } catch (const sdbus::Error& e) {
    return absl::InternalError(e.what());
  }
  return absl::OkStatus();
}

void GattServer::Stop() {
  UnregisterFromBluez();
  std::lock_guard<std::mutex> lock(mutex_);
  characteristics_.clear();
  services_.clear();
  app_object_.reset();
}

std::vector<uint8_t> GattServer::OnReadValue(
    CharacteristicState* state,
    const std::map<std::string, sdbus::Variant>& options) {
  if (callback_.on_characteristic_read_cb) {
    api::ble::BlePeripheral::UniqueId remote_id = 0;
    auto device_it = options.find("device");
    if (device_it != options.end()) {
      MacAddress mac;
      std::string device_path =
          device_it->second.get<sdbus::ObjectPath>();
      if (MacAddressFromDevicePath(device_path, mac)) {
        remote_id = mac.address();
      }
    }
    int offset = 0;
    auto offset_it = options.find("offset");
    if (offset_it != options.end()) {
      offset = offset_it->second.get<uint16_t>();
    }

    std::promise<absl::StatusOr<std::string>> promise;
    std::future<absl::StatusOr<std::string>> future = promise.get_future();
    callback_.on_characteristic_read_cb(
        remote_id, state->info, offset,
        [&promise](absl::StatusOr<absl::string_view> result) {
          if (result.ok()) {
            promise.set_value(std::string(*result));
          } else {
            promise.set_value(result.status());
          }
        });
    absl::StatusOr<std::string> result = future.get();
    if (!result.ok()) {
      throw sdbus::Error(sdbus::Error::Name{"org.bluez.Error.Failed"},
                         std::string(result.status().message()));
    }
    return ToBytes(ByteArray(*result));
  }

  std::lock_guard<std::mutex> lock(mutex_);
  return ToBytes(state->value);
}

void GattServer::OnWriteValue(
    CharacteristicState* state, const std::vector<uint8_t>& value,
    const std::map<std::string, sdbus::Variant>& options) {
  if (callback_.on_characteristic_write_cb) {
    api::ble::BlePeripheral::UniqueId remote_id = 0;
    auto device_it = options.find("device");
    if (device_it != options.end()) {
      MacAddress mac;
      std::string device_path =
          device_it->second.get<sdbus::ObjectPath>();
      if (MacAddressFromDevicePath(device_path, mac)) {
        remote_id = mac.address();
      }
    }
    int offset = 0;
    auto offset_it = options.find("offset");
    if (offset_it != options.end()) {
      offset = offset_it->second.get<uint16_t>();
    }

    std::promise<absl::Status> promise;
    std::future<absl::Status> future = promise.get_future();
    callback_.on_characteristic_write_cb(
        remote_id, state->info, offset,
        absl::string_view(reinterpret_cast<const char*>(value.data()),
                          value.size()),
        [&promise](absl::Status result) { promise.set_value(result); });
    absl::Status result = future.get();
    if (!result.ok()) {
      throw sdbus::Error(sdbus::Error::Name{"org.bluez.Error.Failed"},
                         std::string(result.message()));
    }
    return;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  state->value = ByteArray(reinterpret_cast<const char*>(value.data()),
                           value.size());
}

void GattServer::OnStartNotify(CharacteristicState* state) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state->notifying) return;
    state->notifying = true;
  }
  if (callback_.characteristic_subscription_cb) {
    callback_.characteristic_subscription_cb(state->info);
  }
}

void GattServer::OnStopNotify(CharacteristicState* state) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!state->notifying) return;
    state->notifying = false;
  }
  if (callback_.characteristic_unsubscription_cb) {
    callback_.characteristic_unsubscription_cb(state->info);
  }
}

}  // namespace linux_platform
}  // namespace nearby
