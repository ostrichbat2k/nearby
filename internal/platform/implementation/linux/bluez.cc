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

#include <memory>
#include <optional>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

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

}  // namespace linux_platform
}  // namespace nearby
