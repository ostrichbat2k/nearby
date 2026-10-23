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
// This file does not exist in the public google/nearby mirror -- the real
// implementation lives in Google's internal Blaze tree at this same path
// and is pulled in transitively by sharing/incoming_share_session.cc and
// sharing/outgoing_share_session.cc (which only use GetPowerStatus() for an
// analytics-event field). This is a minimal OSS-buildable stand-in; swap in
// a real implementation (e.g. reading /sys/class/power_supply) if power
// status ever needs to be more than an analytics placeholder.

#ifndef LOCATION_NEARBY_CPP_SHARING_CLIENTS_CPP_COMMON_NEARBY_SHARING_COMMON_H_
#define LOCATION_NEARBY_CPP_SHARING_CLIENTS_CPP_COMMON_NEARBY_SHARING_COMMON_H_

#include "proto/sharing_enums.pb.h"

namespace nearby::sharing::cpp::common {

inline ::location::nearby::proto::sharing::PowerStatus GetPowerStatus() {
  return ::location::nearby::proto::sharing::POWER_STATUS_UNKNOWN;
}

}  // namespace nearby::sharing::cpp::common

#endif  // LOCATION_NEARBY_CPP_SHARING_CLIENTS_CPP_COMMON_NEARBY_SHARING_COMMON_H_
