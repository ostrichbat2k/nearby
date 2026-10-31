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

#include "internal/platform/implementation/linux/bluez.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "internal/platform/byte_array.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/uuid.h"

namespace nearby {
namespace linux_platform {

BluezSystemBus& BluezSystemBus::Get() {
  static BluezSystemBus* instance = new BluezSystemBus();
  return *instance;
}

BluezSystemBus::BluezSystemBus()
    : connection_(sdbus::createSystemBusConnection()) {
  connection_->enterEventLoopAsync();
}

std::unique_ptr<sdbus::IProxy> BluezSystemBus::CreateProxy(
    const std::string& object_path) {
  return sdbus::createProxy(*connection_, sdbus::ServiceName{kBluezService},
                             sdbus::ObjectPath{object_path});
}

ManagedObjects BluezSystemBus::GetManagedObjects() {
  auto proxy = CreateProxy(kBluezRootPath);
  ManagedObjects objects;
  proxy->callMethod("GetManagedObjects")
      .onInterface(kObjectManagerInterface)
      .storeResultsTo(objects);
  return objects;
}

std::optional<std::string> BluezSystemBus::FindAdapterPath() {
  for (const auto& [path, interfaces] : GetManagedObjects()) {
    if (interfaces.find(kAdapter1Interface) != interfaces.end()) {
      return std::string(path);
    }
  }
  return std::nullopt;
}

std::vector<uint8_t> ToBytes(const ByteArray& value) {
  const uint8_t* data = reinterpret_cast<const uint8_t*>(value.data());
  return std::vector<uint8_t>(data, data + value.size());
}

ByteArray FromBytes(const std::vector<uint8_t>& value) {
  return ByteArray(reinterpret_cast<const char*>(value.data()), value.size());
}

std::string ToLowerUuidString(const Uuid& uuid) {
  std::string s(uuid);
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

bool MacAddressFromDevicePath(const std::string& device_path,
                              MacAddress& out) {
  constexpr char kDevPrefix[] = "/dev_";
  size_t pos = device_path.rfind(kDevPrefix);
  if (pos == std::string::npos) return false;
  std::string address = device_path.substr(pos + sizeof(kDevPrefix) - 1);
  std::replace(address.begin(), address.end(), '_', ':');
  return MacAddress::FromString(address, out);
}

std::string DevicePathForMacAddress(const std::string& adapter_path,
                                    const MacAddress& mac) {
  std::string address = mac.ToString();
  std::replace(address.begin(), address.end(), ':', '_');
  return adapter_path + "/dev_" + address;
}

}  // namespace linux_platform
}  // namespace nearby
