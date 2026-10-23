load("@rules_cc//cc:cc_binary.bzl", "cc_binary")
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

# Linux desktop backend for the platform abstraction layer. BLE/Bluetooth
# classic/WiFi LAN/WiFi Direct/WiFi Hotspot/WiFi Aware/AWDL mediums are not
# implemented yet (platform.cc returns nullptr for them) -- this target only
# covers the primitives (mutex/condvar/file/executors/device info/
# preferences) needed to get //internal/platform and //connections building
# on Linux.
cc_library(
    name = "linux",
    srcs = [
        "ble.cc",
        "ble_gatt_client.cc",
        "bluetooth_adapter.cc",
        "bluez.cc",
        "device_info.cc",
        "platform.cc",
        "preferences_manager.cc",
        "preferences_repository.cc",
        "scheduled_executor.cc",
        "system_clock.cc",
        "thread_pool_executor.cc",
        "timer.cc",
    ],
    hdrs = [
        "atomic_boolean.h",
        "atomic_uint32.h",
        "ble.h",
        "ble_gatt_client.h",
        "bluetooth_adapter.h",
        "bluez.h",
        "device_info.h",
        "preferences_manager.h",
        "preferences_repository.h",
        "scheduled_executor.h",
        "thread_pool_executor.h",
        "timer.h",
        "wifi_medium.h",
    ],
    tags = ["linux"],
    visibility = [
        "//connections:__subpackages__",
        "//internal/platform:__subpackages__",
        "//sharing:__subpackages__",
    ],
    deps = [
        "//internal/base:file_path",
        "//internal/base:files",
        "//internal/platform:base",
        "//internal/platform:logging",
        "//internal/platform:mac_address",
        "//internal/platform/implementation:comm",
        "//internal/platform/implementation:platform",
        "//internal/platform/implementation:types",
        "//internal/platform/implementation/shared:count_down_latch",
        "//internal/platform/implementation/shared:file",
        "//internal/platform/implementation/shared:posix_condition_variable",
        "//internal/platform/implementation/shared:posix_mutex",
        "//third_party/sdbus_cpp",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_absl//absl/types:span",
        "@com_google_protobuf//:protobuf",
        "@com_google_protobuf//src/google/protobuf/json:json",
        "@nlohmann_json//:json",
    ],
    alwayslink = True,
)

# Manual smoke test against the real system BlueZ daemon; see
# ble_smoke_test.cc. Not part of any automated test suite.
cc_binary(
    name = "ble_smoke_test",
    srcs = ["ble_smoke_test.cc"],
    tags = ["linux"],
    deps = [
        ":linux",
        "//internal/platform:base",
        "//internal/platform/implementation:comm",
    ],
)

# Throwaway diagnostic; see ble_scan_dump.cc.
cc_binary(
    name = "ble_scan_dump",
    srcs = ["ble_scan_dump.cc"],
    tags = ["linux"],
    deps = [
        ":linux",
    ],
)
