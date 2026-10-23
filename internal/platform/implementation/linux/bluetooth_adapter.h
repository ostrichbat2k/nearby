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

#ifndef PLATFORM_IMPL_LINUX_BLUETOOTH_ADAPTER_H_
#define PLATFORM_IMPL_LINUX_BLUETOOTH_ADAPTER_H_

#include <string>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/bluetooth_adapter.h"
#include "internal/platform/mac_address.h"

namespace nearby {
namespace linux_platform {

// TODO: back this with BlueZ's org.bluez.Adapter1 D-Bus interface (hci0's
// Powered/DiscoverableTimeout/Alias/Address properties) instead of the
// in-memory placeholder below. Tracked as part of the BlueZ BLE medium work.
class BluetoothAdapter : public api::BluetoothAdapter {
 public:
  BluetoothAdapter() = default;
  ~BluetoothAdapter() override = default;

  bool SetStatus(Status status) override {
    enabled_ = (status == Status::kEnabled);
    return true;
  }
  bool IsEnabled() const override { return enabled_; }

  ScanMode GetScanMode() const override { return scan_mode_; }
  bool SetScanMode(ScanMode scan_mode) override {
    scan_mode_ = scan_mode;
    return true;
  }

  std::string GetName() const override { return name_; }
  bool SetName(absl::string_view name, bool persist) override {
    name_ = std::string(name);
    return true;
  }

  MacAddress GetMacAddress() const override { return MacAddress(); }

 private:
  bool enabled_ = true;
  ScanMode scan_mode_ = ScanMode::kNone;
  std::string name_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLUETOOTH_ADAPTER_H_
