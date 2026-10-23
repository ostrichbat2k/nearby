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

#include "internal/platform/implementation/linux/device_info.h"

#include <climits>
#include <cstdlib>
#include <optional>
#include <string>

#include <unistd.h>

#include "absl/strings/string_view.h"
#include "internal/base/file_path.h"

namespace nearby {
namespace linux_platform {

FilePath DeviceInfo::GetHomeDirectory() {
  const char* home = std::getenv("HOME");
  if (home != nullptr && home[0] != '\0') {
    return FilePath(absl::string_view(home));
  }
  return FilePath("/tmp");
}

std::optional<std::string> DeviceInfo::GetOsDeviceName() const {
  char hostname[HOST_NAME_MAX + 1] = {0};
  if (gethostname(hostname, sizeof(hostname) - 1) != 0) {
    return std::nullopt;
  }
  return std::string(hostname);
}

api::DeviceInfo::DeviceType DeviceInfo::GetDeviceType() const {
  return api::DeviceInfo::DeviceType::kLaptop;
}

api::DeviceInfo::OsType DeviceInfo::GetOsType() const {
  // api::DeviceInfo::OsType has no Linux entry upstream yet.
  return api::DeviceInfo::OsType::kUnknown;
}

FilePath DeviceInfo::GetDownloadPath() const {
  // TODO: query XDG_DOWNLOAD_DIR via `xdg-user-dir DOWNLOAD` instead of
  // assuming the common default.
  FilePath path = GetHomeDirectory();
  path.append(FilePath("Downloads"));
  return path;
}

FilePath DeviceInfo::GetLocalAppDataPath(FilePath sub_path) const {
  const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
  FilePath path = (xdg_data_home != nullptr && xdg_data_home[0] != '\0')
                      ? FilePath(absl::string_view(xdg_data_home))
                      : [this] {
                          FilePath p = GetHomeDirectory();
                          p.append(FilePath(".local"));
                          p.append(FilePath("share"));
                          return p;
                        }();
  path.append(sub_path);
  return path;
}

FilePath DeviceInfo::GetTemporaryPath() const {
  const char* tmpdir = std::getenv("TMPDIR");
  if (tmpdir != nullptr && tmpdir[0] != '\0') {
    return FilePath(absl::string_view(tmpdir));
  }
  return FilePath("/tmp");
}

FilePath DeviceInfo::GetLogPath() const {
  const char* xdg_state_home = std::getenv("XDG_STATE_HOME");
  FilePath path = (xdg_state_home != nullptr && xdg_state_home[0] != '\0')
                      ? FilePath(absl::string_view(xdg_state_home))
                      : [this] {
                          FilePath p = GetHomeDirectory();
                          p.append(FilePath(".local"));
                          p.append(FilePath("state"));
                          return p;
                        }();
  path.append(FilePath("nearby"));
  return path;
}

// TODO: implement screen-lock state/notifications via the
// org.freedesktop.ScreenSaver D-Bus interface. Not needed for the initial
// discovery/handshake milestone.
bool DeviceInfo::IsScreenLocked() const { return false; }

void DeviceInfo::RegisterScreenLockedListener(
    absl::string_view listener_name,
    std::function<void(api::DeviceInfo::ScreenStatus)> callback) {}

void DeviceInfo::UnregisterScreenLockedListener(
    absl::string_view listener_name) {}

// TODO: implement via org.freedesktop.login1 (systemd-inhibit) D-Bus calls.
bool DeviceInfo::PreventSleep() { return true; }

bool DeviceInfo::AllowSleep() { return true; }

}  // namespace linux_platform
}  // namespace nearby
