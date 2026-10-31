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

#include "internal/platform/implementation/linux/avahi.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <sdbus-c++/sdbus-c++.h>

#include "absl/container/flat_hash_map.h"
#include "internal/platform/implementation/linux/bluez.h"

namespace nearby {
namespace linux_platform {

std::unique_ptr<sdbus::IProxy> CreateAvahiProxy(
    const std::string& object_path) {
  return sdbus::createProxy(BluezSystemBus::Get().connection(),
                             sdbus::ServiceName{kAvahiService},
                             sdbus::ObjectPath{object_path});
}

std::vector<std::vector<uint8_t>> TxtRecordsToAvahi(
    const absl::flat_hash_map<std::string, std::string>& txt_records) {
  std::vector<std::vector<uint8_t>> txt;
  txt.reserve(txt_records.size());
  for (const auto& [key, value] : txt_records) {
    std::string entry = key + "=" + value;
    txt.emplace_back(entry.begin(), entry.end());
  }
  return txt;
}

absl::flat_hash_map<std::string, std::string> TxtRecordsFromAvahi(
    const std::vector<std::vector<uint8_t>>& txt) {
  absl::flat_hash_map<std::string, std::string> txt_records;
  for (const std::vector<uint8_t>& entry : txt) {
    std::string text(entry.begin(), entry.end());
    size_t pos = text.find('=');
    if (pos == std::string::npos) {
      txt_records[text] = "";
    } else {
      txt_records[text.substr(0, pos)] = text.substr(pos + 1);
    }
  }
  return txt_records;
}

std::string StripTrailingDot(const std::string& service_type) {
  if (!service_type.empty() && service_type.back() == '.') {
    return service_type.substr(0, service_type.size() - 1);
  }
  return service_type;
}

}  // namespace linux_platform
}  // namespace nearby
