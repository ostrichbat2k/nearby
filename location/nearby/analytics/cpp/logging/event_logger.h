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
// This file does not exist in the public google/nearby mirror -- it's part
// of Google's internal clearcut-logging client, which isn't published.
// sharing/nearby_connections_service_impl.h/.cc take an
// `nearby::analytics::EventLogger*` (defaulted to nullptr) purely to pass
// through to AnalyticsRecorderImpl's constructor. Our AnalyticsRecorderImpl
// (connections/implementation/analytics/analytics_recorder_impl.h) is a
// local no-op implementation that never calls anything on this pointer, by
// design: this Linux client doesn't send usage telemetry to Google. This
// type only needs to exist for that constructor signature to compile.

#ifndef LOCATION_NEARBY_ANALYTICS_CPP_LOGGING_EVENT_LOGGER_H_
#define LOCATION_NEARBY_ANALYTICS_CPP_LOGGING_EVENT_LOGGER_H_

namespace nearby::analytics {

class EventLogger {
 public:
  virtual ~EventLogger() = default;
};

}  // namespace nearby::analytics

#endif  // LOCATION_NEARBY_ANALYTICS_CPP_LOGGING_EVENT_LOGGER_H_
