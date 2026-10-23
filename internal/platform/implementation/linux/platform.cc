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

#include "internal/platform/implementation/platform.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"
#include "internal/base/file_path.h"
#include "internal/base/files.h"
#include "internal/platform/implementation/app_lifecycle_monitor.h"
#include "internal/platform/implementation/atomic_boolean.h"
#include "internal/platform/implementation/atomic_reference.h"
#include "internal/platform/implementation/awdl.h"
#include "internal/platform/implementation/ble.h"
#include "internal/platform/implementation/bluetooth_adapter.h"
#include "internal/platform/implementation/bluetooth_classic.h"
#include "internal/platform/implementation/condition_variable.h"
#include "internal/platform/implementation/count_down_latch.h"
#include "internal/platform/implementation/credential_storage.h"
#include "internal/platform/implementation/device_info.h"
#include "internal/platform/implementation/http_loader.h"
#include "internal/platform/implementation/input_file.h"
#include "internal/platform/implementation/linux/atomic_boolean.h"
#include "internal/platform/implementation/linux/atomic_uint32.h"
#include "internal/platform/implementation/linux/bluetooth_adapter.h"
#include "internal/platform/implementation/linux/device_info.h"
#include "internal/platform/implementation/linux/preferences_manager.h"
#include "internal/platform/implementation/linux/scheduled_executor.h"
#include "internal/platform/implementation/linux/thread_pool_executor.h"
#include "internal/platform/implementation/linux/timer.h"
#include "internal/platform/implementation/linux/wifi_medium.h"
#include "internal/platform/implementation/log_message.h"
#include "internal/platform/implementation/mutex.h"
#include "internal/platform/implementation/output_file.h"
#include "internal/platform/implementation/preferences_manager.h"
#include "internal/platform/implementation/shared/file.h"
#include "internal/platform/implementation/shared/posix_condition_variable.h"
#include "internal/platform/implementation/shared/posix_mutex.h"
#include "internal/platform/implementation/shared/count_down_latch.h"
#include "internal/platform/implementation/submittable_executor.h"
#include "internal/platform/implementation/timer.h"
#include "internal/platform/implementation/wifi.h"
#include "internal/platform/implementation/wifi_aware.h"
#include "internal/platform/implementation/wifi_direct.h"
#include "internal/platform/implementation/wifi_hotspot.h"
#include "internal/platform/implementation/wifi_lan.h"
#include "internal/platform/os_name.h"

namespace nearby {
namespace api {

std::string ImplementationPlatform::GetCustomSavePath(
    const std::string& save_path, const std::string& parent_folder,
    const std::string& file_name) {
  FilePath base = save_path.empty() ? FilePath("/tmp") : FilePath(save_path);
  if (!parent_folder.empty()) {
    base.append(FilePath(parent_folder));
  }
  if (file_name.empty()) {
    return base.ToString();
  }

  FilePath candidate = base;
  candidate.append(FilePath(file_name));
  if (!Files::FileExists(candidate)) {
    return candidate.ToString();
  }

  FilePath stem = candidate.GetFileName();
  FilePath extension = candidate.GetExtension();
  std::string stem_str = stem.ToString();
  std::string extension_str = extension.ToString();
  if (extension_str.size() <= stem_str.size()) {
    stem_str = stem_str.substr(0, stem_str.size() - extension_str.size());
  }

  for (int index = 1;; ++index) {
    FilePath attempt = base;
    attempt.append(
        FilePath(stem_str + " " + std::to_string(index) + extension_str));
    if (!Files::FileExists(attempt)) {
      return attempt.ToString();
    }
  }
}

OSName ImplementationPlatform::GetCurrentOS() { return OSName::kLinux; }

// Atomics:
std::unique_ptr<AtomicBoolean> ImplementationPlatform::CreateAtomicBoolean(
    bool initial_value) {
  return std::make_unique<linux_platform::AtomicBoolean>(initial_value);
}

std::unique_ptr<AtomicUint32> ImplementationPlatform::CreateAtomicUint32(
    std::uint32_t value) {
  return std::make_unique<linux_platform::AtomicUint32>(value);
}

std::unique_ptr<CountDownLatch> ImplementationPlatform::CreateCountDownLatch(
    std::int32_t count) {
  return std::make_unique<shared::CountDownLatch>(count);
}

#pragma push_macro("CreateMutex")
#undef CreateMutex
std::unique_ptr<Mutex> ImplementationPlatform::CreateMutex(Mutex::Mode) {
  return std::make_unique<posix::Mutex>();
}
#pragma pop_macro("CreateMutex")

std::unique_ptr<ConditionVariable>
ImplementationPlatform::CreateConditionVariable(Mutex* mutex) {
  return std::make_unique<posix::ConditionVariable>(
      static_cast<posix::Mutex*>(mutex));
}

std::unique_ptr<InputFile> ImplementationPlatform::CreateInputFile(
    const std::string& file_path) {
  return shared::IOFile::CreateInputFile(file_path);
}

std::unique_ptr<OutputFile> ImplementationPlatform::CreateOutputFile(
    const std::string& file_path) {
  FilePath path{file_path};
  FilePath folder_path = path.GetParentPath();
  if (!Files::DirectoryExists(folder_path) &&
      !Files::CreateDirectories(folder_path)) {
    return nullptr;
  }
  return shared::IOFile::CreateOutputFile(file_path);
}

std::unique_ptr<LogMessage> ImplementationPlatform::CreateLogMessage(
    const char*, int, LogMessage::Severity) {
  return nullptr;
}

// Java-like Executors
std::unique_ptr<SubmittableExecutor>
ImplementationPlatform::CreateSingleThreadExecutor() {
  return std::make_unique<linux_platform::ThreadPoolExecutor>(1);
}

std::unique_ptr<SubmittableExecutor>
ImplementationPlatform::CreateMultiThreadExecutor(std::int32_t max_concurrency) {
  return std::make_unique<linux_platform::ThreadPoolExecutor>(max_concurrency);
}

std::unique_ptr<ScheduledExecutor>
ImplementationPlatform::CreateScheduledExecutor() {
  return std::make_unique<linux_platform::ScheduledExecutor>();
}

// Protocol implementations, domain-specific support.
// BLE/Bluetooth-classic/WiFi-LAN/WiFi-Direct/WiFi-Hotspot/WiFi-Aware/AWDL
// mediums are not implemented yet -- that is the next phase of this port
// (BlueZ over D-Bus for BLE/BT, Avahi/NetworkManager for WiFi LAN). Returning
// nullptr here matches how the upstream apple/windows backends stub out
// mediums they don't support on a given OS.
std::unique_ptr<AwdlMedium> ImplementationPlatform::CreateAwdlMedium() {
  return nullptr;
}

std::unique_ptr<BluetoothAdapter>
ImplementationPlatform::CreateBluetoothAdapter() {
  return std::make_unique<linux_platform::BluetoothAdapter>();
}

std::unique_ptr<BluetoothClassicMedium>
ImplementationPlatform::CreateBluetoothClassicMedium(BluetoothAdapter&) {
  return nullptr;
}

std::unique_ptr<api::ble::BleMedium> ImplementationPlatform::CreateBleMedium(
    api::BluetoothAdapter&) {
  return nullptr;
}

std::unique_ptr<api::CredentialStorage>
ImplementationPlatform::CreateCredentialStorage() {
  return nullptr;
}

std::unique_ptr<WifiMedium> ImplementationPlatform::CreateWifiMedium() {
  return std::make_unique<linux_platform::WifiMedium>();
}

std::unique_ptr<WifiLanMedium> ImplementationPlatform::CreateWifiLanMedium() {
  return nullptr;
}

std::unique_ptr<WifiAwareMedium> ImplementationPlatform::CreateWifiAwareMedium() {
  return nullptr;
}

std::unique_ptr<WifiHotspotMedium>
ImplementationPlatform::CreateWifiHotspotMedium() {
  return nullptr;
}

std::unique_ptr<WifiDirectMedium>
ImplementationPlatform::CreateWifiDirectMedium() {
  return nullptr;
}

std::unique_ptr<Timer> ImplementationPlatform::CreateTimer() {
  return std::make_unique<linux_platform::Timer>();
}

std::unique_ptr<AppLifecycleMonitor>
ImplementationPlatform::CreateAppLifecycleMonitor(
    std::function<void(AppLifecycleMonitor::AppLifecycleState)>
        state_updated_callback) {
  return nullptr;
}

std::unique_ptr<nearby::api::PreferencesManager>
ImplementationPlatform::CreatePreferencesManager(absl::string_view path) {
  return std::make_unique<linux_platform::PreferencesManager>(
      FilePath(path));
}

std::unique_ptr<DeviceInfo> ImplementationPlatform::CreateDeviceInfo() {
  return std::make_unique<linux_platform::DeviceInfo>();
}

absl::StatusOr<WebResponse> ImplementationPlatform::SendRequest(
    const WebRequest& request) {
  // TODO: implement via libcurl. Not needed for the discovery/handshake
  // milestone.
  return absl::UnimplementedError(
      "HTTP requests are not yet implemented on Linux.");
}

}  // namespace api
}  // namespace nearby
