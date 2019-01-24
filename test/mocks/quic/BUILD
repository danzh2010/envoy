licenses(["notice"])  # Apache 2

load(
    "//bazel:envoy_build_system.bzl",
    "envoy_cc_library",
    "envoy_package",
)

envoy_package()

envoy_cc_library(
    name = "quic_mocks",
    hdrs = ["mocks.h"],
    deps = [
        "//source/common/http/quic:codec_lib",
    ],
)
