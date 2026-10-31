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
// one integrates with Google's internal account/sign-in client library.
// sharing/nearby_sharing_service_impl.{h,cc} only use the narrow surface
// declared below (checking whether a signed-in account exists, and an
// Observer pair for login/logout), inferred from their actual call sites --
// nothing there reads any field off Account itself. A concrete
// AccountManager (e.g. backed by a local GAIA/OAuth flow, or simply "no
// account, visibility is device-local only") still needs writing before any
// binary can instantiate NearbySharingServiceImpl; this header only
// provides the interface shape so sharing/ builds.

#ifndef LOCATION_NEARBY_SHARING_LIB_ACCOUNT_ACCOUNT_MANAGER_H_
#define LOCATION_NEARBY_SHARING_LIB_ACCOUNT_ACCOUNT_MANAGER_H_

#include <optional>
#include <string>

#include "absl/strings/string_view.h"

namespace nearby {

class AccountManager {
 public:
  struct Account {
    std::string id;
  };

  class Observer {
   public:
    virtual ~Observer() = default;

    virtual void OnLoginSucceeded(absl::string_view account_id) {}
    virtual void OnLogoutSucceeded(absl::string_view account_id,
                                   bool credential_error) {}
  };

  virtual ~AccountManager() = default;

  virtual std::optional<Account> GetCurrentAccount() = 0;

  virtual void AddObserver(Observer* observer) = 0;
  virtual void RemoveObserver(Observer* observer) = 0;
};

}  // namespace nearby

#endif  // LOCATION_NEARBY_SHARING_LIB_ACCOUNT_ACCOUNT_MANAGER_H_
