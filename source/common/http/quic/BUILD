licenses(["notice"])  # Apache 2

load(
    "//third_party/envoy/src/bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "codec_lib",
    srcs = ["codec_impl.cc"],
    hdrs = ["codec_impl.h"],
    deps = [
        "//third_party/envoy/src/include/envoy/http:codec_interface",
        "//third_party/envoy/src/include/envoy/http:codes_interface",
        "//third_party/envoy/src/source/common/buffer:buffer_lib",
        "//third_party/envoy/src/source/common/buffer:watermark_buffer_lib",
        "//third_party/envoy/src/source/common/http:header_map_lib",
    ],
)

envoy_cc_library(
    name = "envoy_quic_session_lib",
    srcs = [
        "envoy_quic_connection.cc",
        "envoy_quic_stream.cc",
    ],
    hdrs = [
        "envoy_quic_connection.h",
        "envoy_quic_stream.h",
    ],
    deps = [
        "//third_party/envoy/src/source/common/buffer:buffer_lib",
        "//third_party/envoy/src/source/common/buffer:watermark_buffer_lib",
        "//third_party/envoy/src/source/common/http:header_map_lib",
    ],
)
