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

#include "internal/platform/implementation/linux/thread_pool_executor.h"

#include <utility>

namespace nearby {
namespace linux_platform {

ThreadPoolExecutor::ThreadPoolExecutor(int32_t thread_count) {
  if (thread_count < 1) thread_count = 1;
  workers_.reserve(thread_count);
  for (int32_t i = 0; i < thread_count; ++i) {
    workers_.emplace_back([this] { WorkerLoop(); });
  }
}

ThreadPoolExecutor::~ThreadPoolExecutor() { Shutdown(); }

void ThreadPoolExecutor::Execute(Runnable&& runnable) {
  DoSubmit(std::move(runnable));
}

bool ThreadPoolExecutor::DoSubmit(Runnable&& wrapped_callable) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (shutdown_) return false;
  queue_.push_back(std::move(wrapped_callable));
  cond_.notify_one();
  return true;
}

void ThreadPoolExecutor::Shutdown() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
  }
  cond_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) worker.join();
  }
  workers_.clear();
}

void ThreadPoolExecutor::WorkerLoop() {
  for (;;) {
    Runnable task;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      cond_.wait(lock, [this] { return shutdown_ || !queue_.empty(); });
      if (queue_.empty()) {
        if (shutdown_) return;
        continue;
      }
      task = std::move(queue_.front());
      queue_.pop_front();
    }
    task();
  }
}

}  // namespace linux_platform
}  // namespace nearby
