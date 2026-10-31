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
// This file does not exist in the public google/nearby mirror. It's part of
// Google's internal "gloop" fiber/threading library. As of this writing
// sharing/nearby_sharing_service_impl.cc includes it but doesn't appear to
// call anything from it (dead include, likely left over from an internal
// refactor) -- this empty stand-in exists only to satisfy that #include for
// an OSS build. If a future upstream sync starts actually using a
// gloop::util::time symbol, the build will fail here with a clear "no such
// member" error pointing at the missing piece to add.

#ifndef THIRD_PARTY_GLOOP_UTIL_TIME_PROTOUTIL_H_
#define THIRD_PARTY_GLOOP_UTIL_TIME_PROTOUTIL_H_

#endif  // THIRD_PARTY_GLOOP_UTIL_TIME_PROTOUTIL_H_
