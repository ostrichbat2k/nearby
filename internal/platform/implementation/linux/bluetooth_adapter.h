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

#include <memory>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/strings/string_view.h"
#include "internal/platform/implementation/bluetooth_adapter.h"
#include "internal/platform/mac_address.h"

namespace nearby {
namespace linux_platform {

// Backed by BlueZ's org.bluez.Adapter1 D-Bus interface on the first
// Bluetooth controller BlueZ exposes (typically /org/bluez/hci0). If no
// adapter is present (no Bluetooth hardware, or bluetoothd isn't running),
// every operation is a harmless no-op/failure -- there's no USB Bluetooth
// dongle hot-plug story here, so this is only re-checked at construction.
class BluetoothAdapter : public api::BluetoothAdapter {
 public:
  BluetoothAdapter();
  ~BluetoothAdapter() override = default;

  bool SetStatus(Status status) override;
  bool IsEnabled() const override;

  ScanMode GetScanMode() const override;
  bool SetScanMode(ScanMode scan_mode) override;

  std::string GetName() const override;
  bool SetName(absl::string_view name, bool persist) override;

  MacAddress GetMacAddress() const override;

  // Object path of the underlying adapter (e.g. "/org/bluez/hci0"), used by
  // BleMedium to talk to the same controller. Empty if no adapter was found.
  const std::string& object_path() const { return object_path_; }

 private:
  std::string object_path_;
  std::unique_ptr<sdbus::IProxy> proxy_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLUETOOTH_ADAPTER_H_
