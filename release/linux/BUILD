load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")

cc_static_library(
    name = "nearby_linux",
    deps = [
        "//internal/platform/implementation/linux:sharing_context",
        "//sharing:nearby_connections_manager",
    ],
    visibility = ["//visibility:public"],
)
