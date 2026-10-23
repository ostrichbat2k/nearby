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
// Throwaway diagnostic: dumps every device's ServiceData seen during a BLE
// discovery window, unfiltered, so we can find the real service UUID
// Quick Share's "ready to receive" advertisement uses instead of guessing.
// Not wired into any build target meant to stick around.

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "internal/platform/implementation/linux/bluez.h"

using nearby::linux_platform::BluezSystemBus;
using nearby::linux_platform::kAdapter1Interface;
using nearby::linux_platform::kDevice1Interface;

int main(int argc, char** argv) {
  int seconds = argc > 1 ? std::atoi(argv[1]) : 25;

  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  if (!adapter_path) {
    printf("No adapter found\n");
    return 1;
  }
  auto adapter_proxy = BluezSystemBus::Get().CreateProxy(*adapter_path);

  auto object_manager_proxy =
      BluezSystemBus::Get().CreateProxy(nearby::linux_platform::kBluezRootPath);

  auto dump = [](const std::string& path,
                 const std::map<std::string, sdbus::Variant>& props) {
    auto it = props.find("ServiceData");
    if (it == props.end()) return;
    auto service_data =
        it->second.get<std::map<std::string, sdbus::Variant>>();
    std::string name_str;
    auto name_it = props.find("Name");
    if (name_it != props.end()) {
      name_str = name_it->second.get<std::string>();
    }
    printf("[%s] %s ServiceData keys: ", path.c_str(), name_str.c_str());
    for (const auto& [k, v] : service_data) {
      printf("%s ", k.c_str());
    }
    printf("\n");
  };

  object_manager_proxy->uponSignal("InterfacesAdded")
      .onInterface(nearby::linux_platform::kObjectManagerInterface)
      .call([&](const sdbus::ObjectPath& path,
                const std::map<std::string,
                               std::map<std::string, sdbus::Variant>>&
                    interfaces) {
        auto it = interfaces.find(kDevice1Interface);
        if (it == interfaces.end()) return;
        printf("NEW %s\n", std::string(path).c_str());
        dump(path, it->second);
      });

  std::vector<std::unique_ptr<sdbus::IProxy>> watchers;
  for (const auto& [path, interfaces] : BluezSystemBus::Get().GetManagedObjects()) {
    if (interfaces.find(kDevice1Interface) == interfaces.end()) continue;
    std::string device_path(path);
    auto watcher = BluezSystemBus::Get().CreateProxy(device_path);
    watcher->uponSignal("PropertiesChanged")
        .onInterface(nearby::linux_platform::kPropertiesInterface)
        .call([device_path, &dump](
                  const std::string& interface,
                  const std::map<std::string, sdbus::Variant>& changed,
                  const std::vector<std::string>&) {
          if (interface != kDevice1Interface) return;
          dump(device_path, changed);
        });
    watchers.push_back(std::move(watcher));
  }

  std::map<std::string, sdbus::Variant> filter;
  filter["Transport"] = sdbus::Variant(std::string("le"));
  filter["DuplicateData"] = sdbus::Variant(true);
  adapter_proxy->callMethod("SetDiscoveryFilter")
      .onInterface(kAdapter1Interface)
      .withArguments(filter);
  adapter_proxy->callMethod("StartDiscovery").onInterface(kAdapter1Interface);

  printf("Scanning for %d seconds...\n", seconds);
  std::this_thread::sleep_for(std::chrono::seconds(seconds));

  adapter_proxy->callMethod("StopDiscovery").onInterface(kAdapter1Interface);
  printf("Done.\n");
  return 0;
}
