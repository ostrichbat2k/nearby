load("@rules_cc//cc:cc_library.bzl", "cc_library")

# Copyright 2026 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     https://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
licenses(["notice"])

# Wraps the system-installed sdbus-c++ (pacman package "sdbus-cpp"), used by
# the linux platform backend to talk to BlueZ over D-Bus. Headers resolve via
# the compiler's default system include path (/usr/include/sdbus-c++/...);
# this target only needs to add the link flags (`pkg-config --libs
# sdbus-c++`).
cc_library(
    name = "sdbus_cpp",
    linkopts = [
        "-lsdbus-c++",
        "-lsystemd",
    ],
    visibility = ["//visibility:public"],
)
