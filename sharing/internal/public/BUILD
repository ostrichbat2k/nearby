# Copyright 2023 Google LLC
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

load("@rules_cc//cc:cc_library.bzl", "cc_library")
load("@rules_cc//cc:cc_test.bzl", "cc_test")

licenses(["notice"])

cc_library(
    name = "pref_names",
    hdrs = [
        "pref_names.h",
    ],
    # compatible_with referenced a Google-internal //buildenv/target
    # environment group not present in this OSS mirror; merely loading
    # this BUILD file (for any target in it) failed because of it,
    # regardless of whether the requested target depends on pref_names.
    # Removed on the Linux fork -- see sharing/proto/BUILD's rust_proto
    # removal for the same class of issue.
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "types",
    hdrs = [
        "connectivity_manager.h",
        "context.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "//internal/platform:types",
        # Linux fork: context.h/connectivity_manager.h only ever #include
        # the self-contained bluetooth_adapter.h/fast_initiation_manager.h
        # headers from :platform's bundle, not the account/certificate/sync
        # backend the rest of that target drags in -- see
        # platform_connections_core's own comment.
        "//sharing/internal/api:platform_connections_core",
        "@com_google_absl//absl/strings",
    ],
)

cc_library(
    name = "nearby_context",
    srcs = [
        "connectivity_manager_impl.cc",
        "context_impl.cc",
    ],
    hdrs = [
        "connectivity_manager_impl.h",
        "context_impl.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":types",
        "//internal/flags:nearby_flags",
        "//internal/platform:types",
        "//sharing/flags/generated:generated_flags",
        "//sharing/internal/api:platform",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_test(
    name = "connectivity_manager_impl_test",
    size = "small",
    timeout = "short",
    srcs = [
        "connectivity_manager_impl_test.cc",
    ],
    deps = [
        ":nearby_context",
        ":types",
        "//internal/platform/implementation:platform_impl",
        "//sharing/internal/api:mock_sharing_platform",
        "//sharing/internal/api:platform",
        "@com_github_protobuf_matchers//protobuf-matchers",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "logging",
    hdrs = [
        "logging.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/log:vlog_is_on",
    ],
)
