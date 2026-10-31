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

#ifndef PLATFORM_IMPL_LINUX_TIMER_H_
#define PLATFORM_IMPL_LINUX_TIMER_H_

#include <memory>

#include "absl/base/thread_annotations.h"
#include "absl/functional/any_invocable.h"
#include "absl/synchronization/mutex.h"
#include "internal/platform/implementation/cancelable.h"
#include "internal/platform/implementation/linux/scheduled_executor.h"
#include "internal/platform/implementation/timer.h"

namespace nearby {
namespace linux_platform {

class Timer : public api::Timer {
 public:
  Timer() = default;
  ~Timer() override;

  bool Create(int delay, int interval,
              absl::AnyInvocable<void()> callback) override
      ABSL_LOCKS_EXCLUDED(mutex_);
  bool Stop() override ABSL_LOCKS_EXCLUDED(mutex_);

 private:
  void FireAndMaybeReschedule(int interval);

  mutable absl::Mutex mutex_;
  absl::AnyInvocable<void()> callback_ ABSL_GUARDED_BY(mutex_);
  ScheduledExecutor scheduled_executor_ ABSL_GUARDED_BY(mutex_);
  std::shared_ptr<api::Cancelable> cancelable_task_ ABSL_GUARDED_BY(mutex_) =
      nullptr;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_TIMER_H_
