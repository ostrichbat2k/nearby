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
// A minimal Linux implementation of sharing::Context and the small
// interfaces it bundles (ConnectivityManager, sharing::api::BluetoothAdapter,
// api::FastInitiationManager), for driving NearbyConnectionsManagerImpl
// directly -- deliberately NOT the upstream ContextImpl, which needs a
// SharingPlatform bundling account/certificate/sync backend integration
// that's out of scope (not present in this OSS mirror; see
// sharing/internal/api:platform_connections_core's own comment).
//
// ConnectivityManager and FastInitiationManager are no-ops here: neither is
// needed to drive a BLE-advertised connection -- ConnectivityManager only
// gates WiFi/internet-dependent upgrade paths, and FastInitiationManager is
// the separate "nearby notification" nudge feature, not core connectivity.

#ifndef THIRD_PARTY_NEARBY_INTERNAL_PLATFORM_IMPLEMENTATION_LINUX_SHARING_CONTEXT_H_
#define THIRD_PARTY_NEARBY_INTERNAL_PLATFORM_IMPLEMENTATION_LINUX_SHARING_CONTEXT_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "internal/platform/clock_impl.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/mac_address.h"
#include "internal/platform/task_runner.h"
#include "internal/platform/timer.h"
#include "internal/platform/timer_impl.h"
#include "sharing/internal/api/bluetooth_adapter.h"
#include "sharing/internal/api/fast_initiation_manager.h"
#include "sharing/internal/public/connectivity_manager.h"
#include "sharing/internal/public/context.h"

namespace nearby {
namespace linux_platform {

// Wraps the existing, real linux_platform::BluetoothAdapter (backed by
// BlueZ's org.bluez.Adapter1) to satisfy sharing::api::BluetoothAdapter.
// The BLE-capability queries are hardcoded to reasonable defaults since
// BlueZ's D-Bus API doesn't expose controller LE feature bits generically;
// this only gates informational checks, not correctness of the connection
// itself.
class SharingBluetoothAdapter : public sharing::api::BluetoothAdapter {
 public:
  // Qualified as ::nearby::linux_platform::BluetoothAdapter, not just
  // BluetoothAdapter: unqualified lookup inside this class body prefers
  // the base class's own injected-class-name (sharing::api::
  // BluetoothAdapter) over the same-named class in the enclosing
  // namespace, so the bare name resolves to the wrong type here.
  explicit SharingBluetoothAdapter(
      ::nearby::linux_platform::BluetoothAdapter& adapter)
      : adapter_(adapter) {}

  bool IsPresent() const override { return true; }
  bool IsPowered() const override { return adapter_.IsEnabled(); }
  bool IsLowEnergySupported() const override { return true; }
  bool IsScanOffloadSupported() const override { return false; }
  bool IsAdvertisementOffloadSupported() const override { return false; }
  bool IsExtendedAdvertisingSupported() const override { return false; }
  bool IsPeripheralRoleSupported() const override { return true; }
  PermissionStatus GetOsPermissionStatus() const override {
    return PermissionStatus::kAllowed;
  }
  void SetPowered(bool powered, std::function<void()> success_callback,
                  std::function<void()> error_callback) override {
    error_callback();
  }
  std::optional<std::string> GetAdapterId() const override {
    return adapter_.GetName();
  }
  MacAddress GetAddress() const override { return adapter_.GetMacAddress(); }
  void AddObserver(Observer* observer) override {}
  void RemoveObserver(Observer* observer) override {}
  bool HasObserver(Observer* observer) override { return false; }

 private:
  ::nearby::linux_platform::BluetoothAdapter& adapter_;
};

// No-op: gates WiFi/internet-dependent medium upgrades we don't use here.
class NoOpConnectivityManager : public nearby::ConnectivityManager {
 public:
  bool IsLanConnected() override { return false; }
  bool IsInternetConnected() override { return false; }
  void RegisterLanListener(absl::string_view, std::function<void(bool)>) override {}
  void UnregisterLanListener(absl::string_view) override {}
  void RegisterInternetListener(absl::string_view,
                                std::function<void(bool)>) override {}
  void UnregisterInternetListener(absl::string_view) override {}
  bool IsHPRealtekDevice() override { return false; }
};

// No-op: this is the separate "fast pair"-style proactive nudge feature,
// not required for a discoverable BLE advertisement to work.
class NoOpFastInitiationManager : public nearby::api::FastInitiationManager {
 public:
  void StartAdvertising(
      nearby::api::FastInitBleBeacon::FastInitType type,
      std::function<void()> callback,
      std::function<void(Error)> error_callback) override {
    error_callback(Error::kHardwareNotSupported);
  }
  void StopAdvertising(std::function<void()> callback) override {
    callback();
  }
  void StartScanning(
      std::function<void()> devices_discovered_callback,
      std::function<void()> devices_not_discovered_callback,
      std::function<void(Error)> error_callback) override {
    error_callback(Error::kHardwareNotSupported);
  }
  void StopScanning(std::function<void()> callback) override { callback(); }
  bool IsAdvertising() override { return false; }
  bool IsScanning() override { return false; }
};

class SharingContext : public nearby::Context {
 public:
  explicit SharingContext(::nearby::linux_platform::BluetoothAdapter& adapter)
      : bt_adapter_(adapter) {}

  Clock* GetClock() const override { return &clock_; }
  std::unique_ptr<Timer> CreateTimer() override {
    return std::make_unique<TimerImpl>();
  }
  nearby::ConnectivityManager* GetConnectivityManager() const override {
    return &connectivity_manager_;
  }
  sharing::api::BluetoothAdapter& GetBluetoothAdapter() const override {
    return bt_adapter_;
  }
  nearby::api::FastInitiationManager& GetFastInitiationManager()
      const override {
    return fast_initiation_manager_;
  }
  std::unique_ptr<TaskRunner> CreateSequencedTaskRunner() const override;
  std::unique_ptr<TaskRunner> CreateConcurrentTaskRunner(
      uint32_t concurrent_count) const override;

 private:
  mutable ClockImpl clock_;
  mutable SharingBluetoothAdapter bt_adapter_;
  mutable NoOpConnectivityManager connectivity_manager_;
  mutable NoOpFastInitiationManager fast_initiation_manager_;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // THIRD_PARTY_NEARBY_INTERNAL_PLATFORM_IMPLEMENTATION_LINUX_SHARING_CONTEXT_H_
