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

#include "internal/platform/implementation/linux/sharing_context.h"

#include <cstdint>
#include <memory>

#include "internal/platform/task_runner.h"
#include "internal/platform/task_runner_impl.h"

namespace nearby {
namespace linux_platform {

std::unique_ptr<TaskRunner> SharingContext::CreateSequencedTaskRunner()
    const {
  return std::make_unique<TaskRunnerImpl>(/*runner_count=*/1);
}

std::unique_ptr<TaskRunner> SharingContext::CreateConcurrentTaskRunner(
    uint32_t concurrent_count) const {
  return std::make_unique<TaskRunnerImpl>(concurrent_count);
}

}  // namespace linux_platform
}  // namespace nearby
