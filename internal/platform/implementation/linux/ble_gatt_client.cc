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

#include "internal/platform/implementation/linux/ble_gatt_client.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/logging.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

namespace {

using Permission = api::ble::GattCharacteristic::Permission;
using Property = api::ble::GattCharacteristic::Property;
using WriteType = api::ble::GattClient::WriteType;

constexpr std::chrono::seconds kServicesResolvedTimeout{10};

// Maps BlueZ's GattCharacteristic1.Flags strings
// (https://github.com/bluez/bluez/blob/master/doc/org.bluez.GattCharacteristic.rst)
// to the API's Permission/Property bitflags.
void FlagsToPermissionAndProperty(const std::vector<std::string>& flags,
                                   Permission& permission,
                                   Property& property) {
  permission = Permission::kNone;
  property = Property::kNone;
  for (const auto& flag : flags) {
    if (flag == "read") {
      permission |= Permission::kRead;
      property |= Property::kRead;
    } else if (flag == "write" || flag == "write-without-response") {
      permission |= Permission::kWrite;
      property |= Property::kWrite;
    } else if (flag == "notify") {
      property |= Property::kNotify;
    } else if (flag == "indicate") {
      property |= Property::kIndicate;
    }
  }
}

bool HasNotifyOrIndicate(Property property) {
  return (property & Property::kNotify) != Property::kNone ||
         (property & Property::kIndicate) != Property::kNone;
}

}  // namespace

GattClient::GattClient(std::string device_path,
                        api::ble::ClientGattConnectionCallback callback)
    : device_path_(std::move(device_path)), callback_(std::move(callback)) {}

GattClient::~GattClient() { Disconnect(); }

bool GattClient::EnsureConnected() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (connected_) return true;

  device_proxy_ = BluezSystemBus::Get().CreateProxy(device_path_);

  // Best-effort: force the LE bearer via BlueZ's experimental PreferredBearer
  // property before connecting. Without this, a device that's BR/EDR-bonded
  // but not LE-bonded (e.g. a phone also classic-paired with this machine
  // for audio) always gets connected over BR/EDR -- that's BlueZ's
  // documented Connect() policy ("connect first the bonded bearer"), and it
  // silently yields zero GATT services since no real LE link is ever
  // established. PreferredBearer only exists when bluetoothd was started
  // with --experimental, so this is a harmless no-op otherwise; the normal
  // Connect() call below still runs either way.
  try {
    device_proxy_->setProperty("PreferredBearer")
        .onInterface(kDevice1Interface)
        .toValue<std::string>("le");
  } catch (const sdbus::Error&) {
  }

  try {
    device_proxy_->callMethod("Connect").onInterface(kDevice1Interface);
  } catch (const sdbus::Error& e) {
    if (e.getName() != "org.bluez.Error.AlreadyConnected") {
      LOG(ERROR) << "Failed to connect to GATT server at " << device_path_
                 << ": " << e.what();
      device_proxy_.reset();
      return false;
    }
  }
  connected_ = true;
  return true;
}

bool GattClient::WaitForServicesResolved() {
  std::mutex wait_mutex;
  std::condition_variable wait_cv;
  bool resolved = false;

  // Subscribe before checking the current property value below, so a
  // transition that happens between the check and the subscription can't be
  // missed (the subscription itself doesn't race on this connection's single
  // event-loop thread, so subscribe-then-check is race-free).
  auto signal_proxy = BluezSystemBus::Get().CreateProxy(device_path_);
  signal_proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([&](const std::string& interface,
                const std::map<std::string, sdbus::Variant>& changed,
                const std::vector<std::string>& /*invalidated*/) {
        if (interface != kDevice1Interface) return;
        auto it = changed.find("ServicesResolved");
        if (it == changed.end() || !it->second.get<bool>()) return;
        std::lock_guard<std::mutex> lock(wait_mutex);
        resolved = true;
        wait_cv.notify_all();
      });

  try {
    sdbus::Variant already_resolved =
        device_proxy_->getProperty("ServicesResolved")
            .onInterface(kDevice1Interface);
    if (already_resolved.get<bool>()) return true;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read ServicesResolved property on "
               << device_path_ << ": " << e.what();
    return false;
  }

  std::unique_lock<std::mutex> lock(wait_mutex);
  return wait_cv.wait_for(lock, kServicesResolvedTimeout,
                          [&] { return resolved; });
}

bool GattClient::DiscoverServiceAndCharacteristics(
    const Uuid& service_uuid,
    const std::vector<Uuid>& characteristic_uuids) {
  if (!EnsureConnected()) return false;
  if (!WaitForServicesResolved()) {
    LOG(ERROR) << "GATT services did not resolve for " << device_path_;
    return false;
  }

  ManagedObjects objects = BluezSystemBus::Get().GetManagedObjects();
  std::string target_service_uuid = ToLowerUuidString(service_uuid);
  std::string device_prefix = device_path_ + "/";

  for (const auto& [service_path, service_interfaces] : objects) {
    if (std::string(service_path).rfind(device_prefix, 0) != 0) continue;
    auto service_it = service_interfaces.find(kGattService1Interface);
    if (service_it == service_interfaces.end()) continue;
    auto uuid_it = service_it->second.find("UUID");
    if (uuid_it == service_it->second.end()) continue;
    std::string service_uuid_str = uuid_it->second.get<std::string>();
    std::transform(service_uuid_str.begin(), service_uuid_str.end(),
                   service_uuid_str.begin(), ::tolower);
    if (service_uuid_str != target_service_uuid) continue;

    // BlueZ can expose the same service UUID more than once (duplicate
    // services); each instance may carry different characteristics, so a
    // non-matching instance shouldn't rule out a later one.
    std::map<std::string, DiscoveredCharacteristic> found;
    bool found_all = true;
    for (const Uuid& characteristic_uuid : characteristic_uuids) {
      std::string target_characteristic_uuid =
          ToLowerUuidString(characteristic_uuid);
      bool found_this = false;
      for (const auto& [char_path, char_interfaces] : objects) {
        auto char_it = char_interfaces.find(kGattCharacteristic1Interface);
        if (char_it == char_interfaces.end()) continue;
        auto service_prop_it = char_it->second.find("Service");
        if (service_prop_it == char_it->second.end()) continue;
        if (service_prop_it->second.get<sdbus::ObjectPath>() !=
            service_path) {
          continue;
        }
        auto char_uuid_it = char_it->second.find("UUID");
        if (char_uuid_it == char_it->second.end()) continue;
        std::string char_uuid_str = char_uuid_it->second.get<std::string>();
        std::transform(char_uuid_str.begin(), char_uuid_str.end(),
                       char_uuid_str.begin(), ::tolower);
        if (char_uuid_str != target_characteristic_uuid) continue;

        std::vector<std::string> flags;
        auto flags_it = char_it->second.find("Flags");
        if (flags_it != char_it->second.end()) {
          flags = flags_it->second.get<std::vector<std::string>>();
        }
        Permission permission;
        Property property;
        FlagsToPermissionAndProperty(flags, permission, property);

        DiscoveredCharacteristic dc;
        dc.object_path = std::string(char_path);
        dc.info.uuid = characteristic_uuid;
        dc.info.service_uuid = service_uuid;
        dc.info.permission = permission;
        dc.info.property = property;
        found[CharacteristicKey(service_uuid, characteristic_uuid)] =
            std::move(dc);
        found_this = true;
        break;
      }
      if (!found_this) {
        found_all = false;
        break;
      }
    }

    if (!found_all) continue;

    std::vector<std::string> to_subscribe;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto& [key, dc] : found) {
        if (HasNotifyOrIndicate(dc.info.property)) {
          to_subscribe.push_back(dc.object_path);
        }
        characteristics_[key] = std::move(dc);
      }
    }
    for (const std::string& object_path : to_subscribe) {
      SubscribeToNotifications(object_path);
    }
    return true;
  }

  LOG(ERROR) << "Failed to find service " << std::string(service_uuid)
             << " with all requested characteristics on " << device_path_;
  return false;
}

std::optional<api::ble::GattCharacteristic> GattClient::GetCharacteristic(
    const Uuid& service_uuid, const Uuid& characteristic_uuid) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it =
      characteristics_.find(CharacteristicKey(service_uuid, characteristic_uuid));
  if (it == characteristics_.end()) return std::nullopt;
  return it->second.info;
}

std::optional<std::string> GattClient::ReadCharacteristic(
    const api::ble::GattCharacteristic& characteristic) {
  std::string object_path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = characteristics_.find(CharacteristicKey(
        characteristic.service_uuid, characteristic.uuid));
    if (it == characteristics_.end()) {
      LOG(ERROR) << "ReadCharacteristic: unknown characteristic "
                 << std::string(characteristic.uuid);
      return std::nullopt;
    }
    object_path = it->second.object_path;
  }

  try {
    auto proxy = BluezSystemBus::Get().CreateProxy(object_path);
    std::map<std::string, sdbus::Variant> options;
    std::vector<uint8_t> value;
    proxy->callMethod("ReadValue")
        .onInterface(kGattCharacteristic1Interface)
        .withArguments(options)
        .storeResultsTo(value);
    return std::string(value.begin(), value.end());
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read characteristic "
               << std::string(characteristic.uuid) << " at " << object_path
               << ": " << e.what();
    return std::nullopt;
  }
}

bool GattClient::WriteCharacteristic(
    const api::ble::GattCharacteristic& characteristic,
    absl::string_view value, WriteType type) {
  std::string object_path;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = characteristics_.find(CharacteristicKey(
        characteristic.service_uuid, characteristic.uuid));
    if (it == characteristics_.end()) {
      LOG(ERROR) << "WriteCharacteristic: unknown characteristic "
                 << std::string(characteristic.uuid);
      return false;
    }
    object_path = it->second.object_path;
  }

  try {
    auto proxy = BluezSystemBus::Get().CreateProxy(object_path);
    std::map<std::string, sdbus::Variant> options;
    options["type"] = sdbus::Variant(std::string(
        type == WriteType::kWithoutResponse ? "command" : "request"));
    std::vector<uint8_t> bytes(value.begin(), value.end());
    proxy->callMethod("WriteValue")
        .onInterface(kGattCharacteristic1Interface)
        .withArguments(bytes, options);
    return true;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to write characteristic "
               << std::string(characteristic.uuid) << " at " << object_path
               << ": " << e.what();
    return false;
  }
}

void GattClient::Disconnect() {
  std::unique_ptr<sdbus::IProxy> device_proxy;
  std::map<std::string, std::unique_ptr<sdbus::IProxy>> characteristic_proxies;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connected_) return;
    connected_ = false;
    device_proxy = std::move(device_proxy_);
    characteristic_proxies = std::move(characteristic_proxies_);
    characteristics_.clear();
  }
  // Drop notify subscriptions before disconnecting the device itself.
  characteristic_proxies.clear();
  if (!device_proxy) return;

  try {
    device_proxy->callMethod("Disconnect").onInterface(kDevice1Interface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to disconnect from " << device_path_ << ": "
               << e.what();
  }
  if (callback_.disconnected_cb) callback_.disconnected_cb();
}

std::string GattClient::CharacteristicKey(
    const Uuid& service_uuid, const Uuid& characteristic_uuid) const {
  return ToLowerUuidString(service_uuid) + "|" +
         ToLowerUuidString(characteristic_uuid);
}

sdbus::IProxy* GattClient::GetOrCreateCharacteristicProxy(
    const std::string& object_path) {
  auto it = characteristic_proxies_.find(object_path);
  if (it != characteristic_proxies_.end()) return it->second.get();
  auto proxy = BluezSystemBus::Get().CreateProxy(object_path);
  sdbus::IProxy* ptr = proxy.get();
  characteristic_proxies_[object_path] = std::move(proxy);
  return ptr;
}

void GattClient::SubscribeToNotifications(const std::string& object_path) {
  sdbus::IProxy* proxy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    proxy = GetOrCreateCharacteristicProxy(object_path);
  }

  proxy->uponSignal("PropertiesChanged")
      .onInterface(kPropertiesInterface)
      .call([this](const std::string& interface,
                   const std::map<std::string, sdbus::Variant>& changed,
                   const std::vector<std::string>& /*invalidated*/) {
        if (interface != kGattCharacteristic1Interface) return;
        auto it = changed.find("Value");
        if (it == changed.end()) return;
        std::vector<uint8_t> value = it->second.get<std::vector<uint8_t>>();
        if (callback_.on_characteristic_changed_cb) {
          callback_.on_characteristic_changed_cb(absl::string_view(
              reinterpret_cast<const char*>(value.data()), value.size()));
        }
      });

  try {
    proxy->callMethod("StartNotify").onInterface(kGattCharacteristic1Interface);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to start notifications on " << object_path << ": "
               << e.what();
  }
}

}  // namespace linux_platform
}  // namespace nearby
