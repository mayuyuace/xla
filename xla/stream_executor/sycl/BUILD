# Description:
#   SYCL-platform specific StreamExecutor support code.

load(
    "@local_config_sycl//sycl:build_defs.bzl",
    "if_sycl_is_configured",
)
load("@tsl//tsl/platform:build_config_root.bzl", "if_static")
load("@tsl//tsl/platform:rules_cc.bzl", "cc_library")
load("//xla:xla.bzl", "xla_cc_test")
load(
    "//xla/stream_executor:build_defs.bzl",
    "stream_executor_friends",
)
load("//xla/tsl:tsl.bzl", "internal_visibility", "tsl_copts")

package(
    # copybara:uncomment default_applicable_licenses = ["//tensorflow:license"],
    default_visibility = internal_visibility([":friends"]),
    licenses = ["notice"],
)

package_group(
    name = "friends",
    packages = stream_executor_friends(),
)

cc_library(
    name = "sycl_collectives",
    srcs = if_sycl_is_configured(["sycl_collectives.cc"]),
    deps = if_sycl_is_configured([
        "@xla//xla/stream_executor/gpu:gpu_collectives_header",
        "@xla//xla/stream_executor/gpu:gpu_driver_header",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
    ]),
)

cc_library(
    name = "sycl_gpu_runtime",
    srcs = ["sycl_gpu_runtime.cc"],
    hdrs = ["sycl_gpu_runtime.h"],
    deps = [
        "@com_google_absl//absl/strings",
        "@xla//xla/tsl/util:env_var",
        "@local_config_sycl//sycl",
        "@local_config_sycl//sycl:sycl_headers",
    ],
)

cc_library(
    name = "hw_info",
    srcs = ["hw_info.cc"],
    hdrs = [
        "hw_info.h",
    ],
    visibility = ["//visibility:public"],
    deps = [
        ":sycl_gpu_runtime",
    ],
)

cc_library(
    name = "sycl_platform_id",
    srcs = ["sycl_platform_id.cc"],
    hdrs = ["sycl_platform_id.h"],
    deps = ["//xla/stream_executor:platform"],
)

cc_library(
    name = "sycl_platform",
    srcs = if_sycl_is_configured(["sycl_platform.cc"]),
    hdrs = if_sycl_is_configured(["sycl_platform.h"]),
    visibility = ["//visibility:public"],
    deps = if_sycl_is_configured([
        ":sycl_executor",
        ":sycl_platform_id",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/memory",
        "//xla/stream_executor",  # buildcleaner: keep
        "//xla/stream_executor:executor_cache",
        "//xla/stream_executor/platform",
        "//xla/stream_executor/gpu:gpu_types_header",
        "//xla/stream_executor/gpu:gpu_driver_header",
        "//xla/stream_executor/gpu:gpu_executor_header",
        "//xla/stream_executor/gpu:gpu_collectives_header",
    ]),
    alwayslink = True,  # Registers itself with the PlatformManager.
)

xla_cc_test(
    name = "sycl_platform_test",
    srcs = if_sycl_is_configured(["sycl_platform_test.cc"]),
    deps = if_sycl_is_configured([
        ":sycl_platform",
        "@tsl//tsl/platform:statusor",
        "@tsl//tsl/platform:test",
    ]) + [
        "@tsl//tsl/platform:test_main",
    ],
)

cc_library(
    name = "sycl_driver",
    srcs = ["sycl_driver.cc"],
    deps = [
        ":sycl_gpu_runtime",
        "@com_google_absl//absl/debugging:leak_check",
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/container:node_hash_map",
        "@com_google_absl//absl/container:inlined_vector",
        "@com_google_absl//absl/strings",
        "@xla//xla/stream_executor:stream_executor_h",
        "@xla//xla/stream_executor/gpu:gpu_driver_header",
        "@xla//xla/stream_executor/platform",
        "@xla//xla/stream_executor/platform:dso_loader",
        "@tsl//tsl/platform:env",
        "@tsl//tsl/platform:errors",
        "@tsl//tsl/platform:logging",
        "@tsl//tsl/platform:stacktrace",
        "@tsl//tsl/platform:status",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
    ] + if_sycl_is_configured([
        "@local_config_sycl//sycl:mkl",
    ]),
)

cc_library(
    name = "sycl_stream",
    hdrs = ["sycl_stream.h"],
    deps = [
        ":sycl_driver",
        "@xla//xla/stream_executor:stream_executor_h",
        "@xla//xla/stream_executor/gpu:gpu_stream",
        "@xla//xla/stream_executor/platform",
    ],
)


cc_library(
    name = "sycl_event",
    srcs = ["sycl_event.cc"],
    hdrs = ["sycl_event.h"],
    deps = [
        ":sycl_driver",
        ":sycl_stream",
        "@xla//xla/stream_executor:stream_executor_h",
        "@xla//xla/stream_executor/gpu:gpu_event",
        "@xla//xla/stream_executor/gpu:gpu_stream_header",
    ],
)

cc_library(
    name = "sycl_kernel",
    srcs = ["sycl_kernel.cc"],
    hdrs = ["sycl_kernel.h"],
    deps = [
        ":sycl_gpu_runtime",
        "@xla//xla/stream_executor:stream_executor_h",
        "@xla//xla/stream_executor/gpu:gpu_kernel_header",
        "@xla//xla/stream_executor/platform",
    ],
)

cc_library(
    name = "sycl_executor",
    srcs = ["sycl_executor.cc"],
    hdrs = ["sycl_executor.h"],
    deps = [
        ":hw_info",
        ":sycl_driver",
        ":sycl_event",
        ":sycl_kernel",
        ":sycl_platform_id",
        ":sycl_stream",
        ":sycl_collectives",
        "@com_google_absl//absl/strings",
        "@xla//xla/stream_executor:event",
        "@xla//xla/stream_executor:plugin_registry",
        "@xla//xla/stream_executor:stream_executor_h",
        "@xla//xla/stream_executor/platform",
        "@xla//xla/stream_executor/platform:dso_loader",
        "@xla//xla/stream_executor/gpu:gpu_command_buffer",
        "@xla//xla/stream_executor/gpu:gpu_timer",
        "@com_google_absl//absl/strings:str_format",
        "@tsl//tsl/platform:fingerprint",
    ],
    alwayslink = True,
)

cc_library(
    name = "all_runtime",
    copts = tsl_copts(),
    visibility = ["//visibility:public"],
    deps = if_sycl_is_configured([":sycl_platform"]),
    alwayslink = 1,
)

cc_library(
    name = "sycl_rpath",
    linkopts = ["-Wl,-rpath,../local_config_sycl/sycl/sycl/lib"],
)

cc_library(
    name = "stream_executor_sycl",
    deps = [
        ":sycl_platform_id",
        ":sycl_rpath",
        "//xla/stream_executor",
        "//xla/stream_executor:dnn",
        "//xla/stream_executor:platform_manager",
        "//xla/stream_executor:scratch_allocator",
        "//xla/stream_executor/platform:dso_loader",
    ] + if_static([":all_runtime"]),
)
