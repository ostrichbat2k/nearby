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

#ifndef PLATFORM_IMPL_LINUX_WIFI_MEDIUM_H_
#define PLATFORM_IMPL_LINUX_WIFI_MEDIUM_H_

#include "internal/platform/implementation/wifi.h"

namespace nearby {
namespace linux_platform {

// TODO: query real SSID/BSSID/frequency/capability via NetworkManager's
// D-Bus API instead of reporting an always-disconnected placeholder. Not
// needed for the discovery/handshake milestone -- WifiLanMedium (separate)
// is what actually carries the Nearby Connections data channel.
class WifiMedium : public api::WifiMedium {
 public:
  WifiMedium() = default;
  ~WifiMedium() override = default;

  bool IsInterfaceValid() const override { return true; }

  api::WifiCapability& GetCapability() override { return capability_; }

  api::WifiInformation& GetInformation() override { return information_; }

 private:
  api::WifiCapability capability_;
  api::WifiInformation information_{.is_connected = false,
                                    .ssid = "",
                                    .bssid = "",
                                    .ap_frequency = -1};
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_WIFI_MEDIUM_H_
