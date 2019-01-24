licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "codec_lib",
    srcs = [
        "codec_impl.cc",
        "envoy_quic_connection.cc",
        "envoy_quic_stream.cc",
    ],
    hdrs = [
        "codec_impl.h",
        "envoy_quic_connection.h",
        "envoy_quic_stream.h",
    ],
    deps = [
        "//include/envoy/http:codec_interface",
        "//include/envoy/http:header_map_interface",
        "//include/envoy/network:connection_interface",
        "//source/common/buffer:buffer_lib",
        "//source/common/buffer:watermark_buffer_lib",
        "//source/common/http:codec_helper_lib",
        "//source/common/http:header_map_lib",
    ],
)
