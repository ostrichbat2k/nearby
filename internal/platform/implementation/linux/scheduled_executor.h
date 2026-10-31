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

#ifndef PLATFORM_IMPL_LINUX_SCHEDULED_EXECUTOR_H_
#define PLATFORM_IMPL_LINUX_SCHEDULED_EXECUTOR_H_

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "absl/time/time.h"
#include "internal/platform/implementation/cancelable.h"
#include "internal/platform/implementation/linux/thread_pool_executor.h"
#include "internal/platform/implementation/scheduled_executor.h"
#include "internal/platform/runnable.h"

namespace nearby {
namespace linux_platform {

// A Cancelable whose Cancel() just flips a shared atomic flag the scheduler
// thread checks before actually running the task.
class ScheduledCancelable : public api::Cancelable {
 public:
  bool Cancel() override {
    return !cancelled_.exchange(true);
  }
  bool IsCancelled() const { return cancelled_.load(); }

 private:
  std::atomic_bool cancelled_{false};
};

// A ScheduledExecutor backed by one dedicated timer thread (which sleeps
// until the next due task) and a small worker pool that actually runs due
// tasks, so a slow task can't delay other timers.
class ScheduledExecutor : public api::ScheduledExecutor {
 public:
  ScheduledExecutor();
  ~ScheduledExecutor() override;

  void Execute(Runnable&& runnable) override;
  void Shutdown() override;
  std::shared_ptr<api::Cancelable> Schedule(Runnable&& runnable,
                                            absl::Duration duration) override;

 private:
  struct Task {
    absl::Time due_time;
    Runnable runnable;
    std::shared_ptr<ScheduledCancelable> cancelable;
  };

  void TimerLoop();

  std::mutex mutex_;
  std::condition_variable cond_;
  std::vector<Task> tasks_;
  std::thread timer_thread_;
  ThreadPoolExecutor worker_pool_;
  bool shutdown_ = false;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_SCHEDULED_EXECUTOR_H_
