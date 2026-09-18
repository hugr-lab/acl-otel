cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME acl_otel)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the base's contract header (acl_audit.hpp + acl_principal.hpp, header-only)
# comes from the shared repository duckdb-ext-common (spec 010; its contracts/
# directory, the same header the base compiles) - the extension compiles against
# it and links nothing of acl's (a loadable is dlopen'd RTLD_LOCAL); the level
# rules are parsed with duckdb's bundled yyjson
include_directories(src/include duckdb-ext-common/contracts
                    duckdb/third_party/yyjson/include)

# the OpenTelemetry C++ SDK (spec 002): the OTLP exporters over HTTP/protobuf
# and gRPC for the three signals, from vcpkg
# (opentelemetry-cpp[otlp-http,otlp-grpc] in vcpkg.json), linked statically into
# the loadable as the base links Arrow. The TRACE exporters are the unsuffixed
# targets; the log and metric ones carry the signal in their name (spec 008's
# note).
find_package(opentelemetry-cpp CONFIG REQUIRED)
set(ACL_OTEL_SDK_LIBS
    opentelemetry-cpp::otlp_http_log_record_exporter
    opentelemetry-cpp::otlp_grpc_log_record_exporter
    opentelemetry-cpp::otlp_http_metric_exporter
    opentelemetry-cpp::otlp_grpc_metrics_exporter
    opentelemetry-cpp::otlp_http_exporter
    opentelemetry-cpp::otlp_grpc_exporter
    opentelemetry-cpp::logs
    opentelemetry-cpp::metrics
    opentelemetry-cpp::trace
    opentelemetry-cpp::resources)

# everything but the entry point: what the loadable is, and what the SDK-linked
# tests compile in
set(ACL_OTEL_LIB_SOURCES
    src/acl_otel_rules.cpp
    src/acl_otel_sink.cpp
    src/acl_otel_state.cpp
    src/acl_otel_otlp.cpp
    src/acl_otel_metrics.cpp
    src/acl_otel_otlp_metrics.cpp
    src/acl_otel_sampling.cpp
    src/acl_otel_traces.cpp
    src/acl_otel_otlp_traces.cpp)
set(EXTENSION_SOURCES src/acl_otel_extension.cpp ${ACL_OTEL_LIB_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} ${ACL_OTEL_SDK_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${ACL_OTEL_SDK_LIBS})

# The transport's test (spec 002) links the SDK, its protobuf classes (the fake
# receivers decode what arrives) and gRPC, so it is a CMake target rather than a
# Makefile-compiled one; built on demand by `make test-cpp`, never by a plain
# `make`
find_package(gRPC CONFIG REQUIRED)
function(acl_otel_sdk_test NAME SOURCE)
  add_executable(${NAME} EXCLUDE_FROM_ALL ${SOURCE} ${ACL_OTEL_LIB_SOURCES})
  target_include_directories(
    ${NAME} PRIVATE test/cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
  target_link_libraries(
    ${NAME} duckdb ${ACL_OTEL_SDK_LIBS} opentelemetry-cpp::proto
    opentelemetry-cpp::proto_grpc gRPC::grpc++)
endfunction()
acl_otel_sdk_test(acl_otel_test_otlp test/cpp/test_acl_otel_otlp.cpp)
# spec 003's twin: the metrics transport against fake receivers of its own
acl_otel_sdk_test(acl_otel_test_metrics_otlp
                  test/cpp/test_acl_otel_metrics_otlp.cpp)
# spec 008's: the span transport against fake receivers, and - with ACL_EXT -
# the two-loadable round trip that proves the marker, the base's event and our
# span agree
acl_otel_sdk_test(acl_otel_test_traces_otlp
                  test/cpp/test_acl_otel_traces_otlp.cpp)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
