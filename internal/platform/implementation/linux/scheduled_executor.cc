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

#include "internal/platform/implementation/linux/scheduled_executor.h"

#include <algorithm>
#include <utility>

#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace nearby {
namespace linux_platform {

ScheduledExecutor::ScheduledExecutor() : worker_pool_(/*thread_count=*/2) {
  timer_thread_ = std::thread([this] { TimerLoop(); });
}

ScheduledExecutor::~ScheduledExecutor() { Shutdown(); }

void ScheduledExecutor::Execute(Runnable&& runnable) {
  worker_pool_.Execute(std::move(runnable));
}

void ScheduledExecutor::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
  }
  cond_.notify_all();
  if (timer_thread_.joinable()) timer_thread_.join();
  worker_pool_.Shutdown();
}

std::shared_ptr<api::Cancelable> ScheduledExecutor::Schedule(
    Runnable&& runnable, absl::Duration duration) {
  auto cancelable = std::make_shared<ScheduledCancelable>();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return cancelable;
    tasks_.push_back(
        Task{absl::Now() + duration, std::move(runnable), cancelable});
  }
  cond_.notify_all();
  return cancelable;
}

void ScheduledExecutor::TimerLoop() {
  std::unique_lock<std::mutex> lock(mutex_);
  for (;;) {
    if (shutdown_) return;

    if (tasks_.empty()) {
      cond_.wait(lock);
      continue;
    }

    auto next_it = std::min_element(
        tasks_.begin(), tasks_.end(),
        [](const Task& a, const Task& b) { return a.due_time < b.due_time; });

    absl::Time now = absl::Now();
    if (next_it->due_time > now) {
      cond_.wait_for(lock, absl::ToChronoMilliseconds(next_it->due_time - now));
      continue;
    }

    Task task = std::move(*next_it);
    tasks_.erase(next_it);
    lock.unlock();
    if (!task.cancelable->IsCancelled()) {
      worker_pool_.Execute(std::move(task.runnable));
    }
    lock.lock();
  }
}

}  // namespace linux_platform
}  // namespace nearby
