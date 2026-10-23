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
// Shared D-Bus plumbing for talking to BlueZ (the Linux Bluetooth daemon),
// used by both bluetooth_adapter.cc and ble.cc. BlueZ exposes Bluetooth
// adapters, devices, and GATT objects entirely over D-Bus -- there is no
// ioctl/socket-level API for adapter management or BLE advertising/scanning,
// so this is the foundation everything else in the BlueZ backend builds on.
//
// See https://github.com/bluez/bluez/tree/master/doc for the interface
// definitions this code depends on (org.bluez.Adapter1, org.bluez.Device1,
// org.bluez.LEAdvertisingManager1, org.bluez.GattManager1, and the standard
// org.freedesktop.DBus.ObjectManager/Properties interfaces every BlueZ
// object supports).

#ifndef PLATFORM_IMPL_LINUX_BLUEZ_H_
#define PLATFORM_IMPL_LINUX_BLUEZ_H_

#include <map>
#include <memory>
#include <optional>
#include <string>

#include <sdbus-c++/sdbus-c++.h>

namespace nearby {
namespace linux_platform {

inline constexpr const char* kBluezService = "org.bluez";
inline constexpr const char* kBluezRootPath = "/";
inline constexpr const char* kAdapter1Interface = "org.bluez.Adapter1";
inline constexpr const char* kDevice1Interface = "org.bluez.Device1";
inline constexpr const char* kGattManager1Interface = "org.bluez.GattManager1";
inline constexpr const char* kGattService1Interface = "org.bluez.GattService1";
inline constexpr const char* kGattCharacteristic1Interface =
    "org.bluez.GattCharacteristic1";
inline constexpr const char* kLEAdvertisingManager1Interface =
    "org.bluez.LEAdvertisingManager1";
inline constexpr const char* kLEAdvertisement1Interface =
    "org.bluez.LEAdvertisement1";
inline constexpr const char* kObjectManagerInterface =
    "org.freedesktop.DBus.ObjectManager";
inline constexpr const char* kPropertiesInterface =
    "org.freedesktop.DBus.Properties";

// Return type of org.freedesktop.DBus.ObjectManager.GetManagedObjects:
// object path -> interface name -> property name -> value.
using ManagedObjects = std::map<
    sdbus::ObjectPath,
    std::map<std::string, std::map<std::string, sdbus::Variant>>>;

// Process-wide handle to the D-Bus system bus connection used to talk to
// BlueZ. There is exactly one of these: sdbus-c++ dispatches incoming
// signals (device discovered, property changed) and incoming method calls
// (on objects we expose, like our LEAdvertisement1) on the event-loop thread
// this class starts, so every proxy/object in the process should share one
// connection and one such thread rather than each spinning up their own.
class BluezSystemBus {
 public:
  static BluezSystemBus& Get();

  BluezSystemBus(const BluezSystemBus&) = delete;
  BluezSystemBus& operator=(const BluezSystemBus&) = delete;

  sdbus::IConnection& connection() { return *connection_; }

  // Creates a proxy for a BlueZ object (service is always "org.bluez").
  std::unique_ptr<sdbus::IProxy> CreateProxy(const std::string& object_path);

  // Equivalent to calling GetManagedObjects on BlueZ's root ObjectManager:
  // every object BlueZ currently exposes, with its interfaces/properties.
  ManagedObjects GetManagedObjects();

  // Object path of the first object implementing org.bluez.Adapter1 (e.g.
  // "/org/bluez/hci0"), or nullopt if no Bluetooth controller is present.
  std::optional<std::string> FindAdapterPath();

 private:
  BluezSystemBus();

  std::unique_ptr<sdbus::IConnection> connection_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_BLUEZ_H_
