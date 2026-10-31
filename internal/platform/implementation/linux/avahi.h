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
// Shared D-Bus plumbing for talking to Avahi (the Linux mDNS/DNS-SD daemon),
// used by wifi_lan.cc to implement WifiLanMedium's advertise/discover half
// (the actual data channel is a plain POSIX TCP socket, see wifi_lan.h).
//
// Constants and method/signal signatures below are taken directly from this
// machine's installed avahi package: /usr/include/avahi-common/defs.h (enum
// values) and /usr/share/dbus-1/interfaces/org.freedesktop.Avahi.*.xml
// (D-Bus method/signal signatures) -- not guessed from memory.

#ifndef PLATFORM_IMPL_LINUX_AVAHI_H_
#define PLATFORM_IMPL_LINUX_AVAHI_H_

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/container/flat_hash_map.h"

namespace nearby {
namespace linux_platform {

inline constexpr const char* kAvahiService = "org.freedesktop.Avahi";
inline constexpr const char* kAvahiServerPath = "/";
inline constexpr const char* kAvahiServerInterface = "org.freedesktop.Avahi.Server";
inline constexpr const char* kAvahiEntryGroupInterface =
    "org.freedesktop.Avahi.EntryGroup";
inline constexpr const char* kAvahiServiceBrowserInterface =
    "org.freedesktop.Avahi.ServiceBrowser";

// AvahiIfIndex / AvahiProtocol wildcard values (avahi-common/defs.h doesn't
// actually define these two -- they're from avahi-common/address.h -- but
// they're the standard "don't care" sentinels used throughout the D-Bus API).
inline constexpr int32_t kAvahiIfUnspec = -1;
inline constexpr int32_t kAvahiProtoUnspec = -1;
inline constexpr int32_t kAvahiProtoInet = 0;
inline constexpr int32_t kAvahiProtoInet6 = 1;

// AvahiLookupResultFlags (defs.h). Only the one we need to filter out our own
// locally-registered services from discovery results.
inline constexpr uint32_t kAvahiLookupResultLocal = 8;

// AvahiEntryGroupState (defs.h).
enum class AvahiEntryGroupState : int32_t {
  kUncommitted = 0,
  kRegistering = 1,
  kEstablished = 2,
  kCollision = 3,
  kFailure = 4,
};

// Creates a proxy for an Avahi object. Reuses the single process-wide D-Bus
// system-bus connection + event-loop thread that BluezSystemBus already
// started (see bluez.h's class comment) rather than opening a second
// connection -- Avahi and BlueZ are just two different well-known names on
// the same system bus.
std::unique_ptr<sdbus::IProxy> CreateAvahiProxy(const std::string& object_path);

// Converts NsdServiceInfo's flat key->value TXT record map to Avahi's wire
// format for EntryGroup.AddService's "txt" (aay) parameter: one byte-array
// per record, each holding the raw bytes of "key=value".
std::vector<std::vector<uint8_t>> TxtRecordsToAvahi(
    const absl::flat_hash_map<std::string, std::string>& txt_records);

// Inverse of TxtRecordsToAvahi, for records read back from ResolveService.
absl::flat_hash_map<std::string, std::string> TxtRecordsFromAvahi(
    const std::vector<std::vector<uint8_t>>& txt);

// Nearby's service type strings look like "_ABCDEF._tcp." (see
// NsdServiceInfo::kNsdTypeFormat) -- Avahi's D-Bus API wants the type without
// the trailing dot. Strips it if present; otherwise returns the input
// unchanged.
std::string StripTrailingDot(const std::string& service_type);

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_AVAHI_H_
