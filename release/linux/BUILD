load("@rules_cc//cc:cc_static_library.bzl", "cc_static_library")

cc_static_library(
    name = "nearby_linux",
    deps = [
        "//internal/platform/implementation/linux:sharing_context",
        "//sharing:nearby_connections_manager",
        # Needed by any consumer driving the real protocol handshake
        # (paired-key verification, introduction, response), not just
        # calling StartDiscovery/Connect/Send -- see quickshare_send.cc.
        "//sharing:incoming_frame_reader",
        "//sharing/proto:wire_format_cc_proto",
        "//proto:sharing_enums_cc_proto",
    ],
    visibility = ["//visibility:public"],
)
