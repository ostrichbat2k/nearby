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

#ifndef PLATFORM_IMPL_LINUX_THREAD_POOL_EXECUTOR_H_
#define PLATFORM_IMPL_LINUX_THREAD_POOL_EXECUTOR_H_

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "internal/platform/implementation/submittable_executor.h"
#include "internal/platform/runnable.h"

namespace nearby {
namespace linux_platform {

// A simple fixed-size std::thread pool backing both the single-thread and
// multi-thread SubmittableExecutor factories.
class ThreadPoolExecutor : public api::SubmittableExecutor {
 public:
  explicit ThreadPoolExecutor(int32_t thread_count);
  ~ThreadPoolExecutor() override;

  void Execute(Runnable&& runnable) override;
  bool DoSubmit(Runnable&& wrapped_callable) override;
  void Shutdown() override;

 private:
  void WorkerLoop();

  std::mutex mutex_;
  std::condition_variable cond_;
  std::deque<Runnable> queue_;
  std::vector<std::thread> workers_;
  bool shutdown_ = false;
};

}  // namespace linux_platform
}  // namespace nearby

#endif  // PLATFORM_IMPL_LINUX_THREAD_POOL_EXECUTOR_H_
