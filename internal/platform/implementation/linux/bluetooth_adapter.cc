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

#include "internal/platform/implementation/linux/bluetooth_adapter.h"

#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/linux/bluez.h"
#include "internal/platform/logging.h"
#include "internal/platform/mac_address.h"

namespace nearby {
namespace linux_platform {

BluetoothAdapter::BluetoothAdapter() {
  auto adapter_path = BluezSystemBus::Get().FindAdapterPath();
  if (!adapter_path.has_value()) {
    LOG(WARNING) << "No Bluetooth adapter found via BlueZ; Bluetooth/BLE "
                    "will be unavailable.";
    return;
  }
  object_path_ = *adapter_path;
  proxy_ = BluezSystemBus::Get().CreateProxy(object_path_);
}

bool BluetoothAdapter::SetStatus(Status status) {
  if (!proxy_) return false;
  try {
    proxy_->setProperty("Powered")
        .onInterface(kAdapter1Interface)
        .toValue(status == Status::kEnabled);
    return true;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to set adapter Powered: " << e.what();
    return false;
  }
}

bool BluetoothAdapter::IsEnabled() const {
  if (!proxy_) return false;
  try {
    return proxy_->getProperty("Powered")
        .onInterface(kAdapter1Interface)
        .get<bool>();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read adapter Powered: " << e.what();
    return false;
  }
}

// BlueZ doesn't expose a separate "connectable" toggle independent of
// Discoverable the way Android's ScanMode does -- a powered classic radio
// always accepts incoming connections. We approximate: Discoverable=true
// maps to kConnectableDiscoverable, everything else to kConnectable.
api::BluetoothAdapter::ScanMode BluetoothAdapter::GetScanMode() const {
  if (!proxy_) return ScanMode::kUnknown;
  try {
    bool discoverable = proxy_->getProperty("Discoverable")
                             .onInterface(kAdapter1Interface)
                             .get<bool>();
    return discoverable ? ScanMode::kConnectableDiscoverable
                         : ScanMode::kConnectable;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read adapter Discoverable: " << e.what();
    return ScanMode::kUnknown;
  }
}

bool BluetoothAdapter::SetScanMode(ScanMode scan_mode) {
  if (!proxy_) return false;
  bool discoverable = scan_mode == ScanMode::kConnectableDiscoverable;
  try {
    if (discoverable) {
      // Disable BlueZ's default 180s auto-timeout; Nearby controls when
      // discoverability turns off explicitly, via SetScanMode(kConnectable).
      proxy_->setProperty("DiscoverableTimeout")
          .onInterface(kAdapter1Interface)
          .toValue(static_cast<uint32_t>(0));
    }
    proxy_->setProperty("Discoverable")
        .onInterface(kAdapter1Interface)
        .toValue(discoverable);
    return true;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to set adapter Discoverable: " << e.what();
    return false;
  }
}

std::string BluetoothAdapter::GetName() const {
  if (!proxy_) return "";
  try {
    return proxy_->getProperty("Alias")
        .onInterface(kAdapter1Interface)
        .get<std::string>();
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read adapter Alias: " << e.what();
    return "";
  }
}

bool BluetoothAdapter::SetName(absl::string_view name, bool persist) {
  if (!proxy_) return false;
  try {
    // BlueZ always persists Alias in the adapter's settings storage; there
    // is no transient-rename option, so `persist` is a no-op here.
    proxy_->setProperty("Alias")
        .onInterface(kAdapter1Interface)
        .toValue(std::string(name));
    return true;
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to set adapter Alias: " << e.what();
    return false;
  }
}

MacAddress BluetoothAdapter::GetMacAddress() const {
  MacAddress mac;
  if (!proxy_) return mac;
  try {
    std::string address = proxy_->getProperty("Address")
                               .onInterface(kAdapter1Interface)
                               .get<std::string>();
    MacAddress::FromString(address, mac);
  } catch (const sdbus::Error& e) {
    LOG(ERROR) << "Failed to read adapter Address: " << e.what();
  }
  return mac;
}

}  // namespace linux_platform
}  // namespace nearby
