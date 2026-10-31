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

#include "internal/platform/implementation/linux/sharing_context.h"

#include <net/if.h>
#include <sys/socket.h>

#include <cstdint>
#include <memory>

#include <ifaddrs.h>

#include "internal/platform/task_runner.h"
#include "internal/platform/task_runner_impl.h"

namespace nearby {
namespace linux_platform {

bool NoOpConnectivityManager::IsLanConnected() {
  // Same check as linux_platform::WifiLanMedium::IsNetworkConnected (not
  // shared via a helper -- it's a handful of lines, see wifi_lan.cc).
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) != 0) return false;
  bool connected = false;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) continue;
    if ((ifa->ifa_flags & IFF_UP) == 0 ||
        (ifa->ifa_flags & IFF_RUNNING) == 0) {
      continue;
    }
    if (ifa->ifa_addr->sa_family == AF_INET ||
        ifa->ifa_addr->sa_family == AF_INET6) {
      connected = true;
      break;
    }
  }
  freeifaddrs(ifaddr);
  return connected;
}

std::unique_ptr<TaskRunner> SharingContext::CreateSequencedTaskRunner()
    const {
  return std::make_unique<TaskRunnerImpl>(/*runner_count=*/1);
}

std::unique_ptr<TaskRunner> SharingContext::CreateConcurrentTaskRunner(
    uint32_t concurrent_count) const {
  return std::make_unique<TaskRunnerImpl>(concurrent_count);
}

}  // namespace linux_platform
}  // namespace nearby
