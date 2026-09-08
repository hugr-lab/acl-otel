cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME acl_otel)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the base's contract header (acl_audit.hpp, header-only) comes from the duckdb-acl submodule - the
# extension compiles against it and links nothing of acl's (a loadable is dlopen'd RTLD_LOCAL); the
# level rules are parsed with duckdb's bundled yyjson
include_directories(src/include duckdb-acl/src/include duckdb/third_party/yyjson/include)

# the OpenTelemetry C++ SDK (spec 002): the OTLP log exporters over HTTP/protobuf and gRPC, from
# vcpkg (opentelemetry-cpp[otlp-http,otlp-grpc] in vcpkg.json), linked statically into the loadable
# as the base links Arrow
find_package(opentelemetry-cpp CONFIG REQUIRED)
set(ACL_OTEL_SDK_LIBS opentelemetry-cpp::otlp_http_log_record_exporter opentelemetry-cpp::otlp_grpc_log_record_exporter
                      opentelemetry-cpp::otlp_http_metric_exporter opentelemetry-cpp::otlp_grpc_metrics_exporter
                      opentelemetry-cpp::logs opentelemetry-cpp::metrics opentelemetry-cpp::resources)

set(EXTENSION_SOURCES src/acl_otel_extension.cpp src/acl_otel_rules.cpp src/acl_otel_sink.cpp src/acl_otel_state.cpp
                      src/acl_otel_otlp.cpp src/acl_otel_metrics.cpp src/acl_otel_otlp_metrics.cpp
                      src/acl_otel_sampling.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

target_link_libraries(${EXTENSION_NAME} ${ACL_OTEL_SDK_LIBS})
target_link_libraries(${LOADABLE_EXTENSION_NAME} ${ACL_OTEL_SDK_LIBS})

# The transport's test (spec 002) links the SDK, its protobuf classes (the fake receivers decode
# what arrives) and gRPC, so it is a CMake target rather than a Makefile-compiled one; built on
# demand by `make test-cpp`, never by a plain `make`
find_package(gRPC CONFIG REQUIRED)
add_executable(acl_otel_test_otlp EXCLUDE_FROM_ALL test/cpp/test_acl_otel_otlp.cpp src/acl_otel_otlp.cpp
               src/acl_otel_sink.cpp src/acl_otel_state.cpp src/acl_otel_rules.cpp src/acl_otel_metrics.cpp
               src/acl_otel_otlp_metrics.cpp src/acl_otel_sampling.cpp)
target_include_directories(acl_otel_test_otlp PRIVATE test/cpp ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
target_link_libraries(acl_otel_test_otlp duckdb ${ACL_OTEL_SDK_LIBS} opentelemetry-cpp::proto
                      opentelemetry-cpp::proto_grpc gRPC::grpc++)

# spec 003's twin: the metrics transport against fake receivers of its own
add_executable(acl_otel_test_metrics_otlp EXCLUDE_FROM_ALL test/cpp/test_acl_otel_metrics_otlp.cpp
               src/acl_otel_otlp.cpp src/acl_otel_otlp_metrics.cpp src/acl_otel_metrics.cpp src/acl_otel_sink.cpp
               src/acl_otel_state.cpp src/acl_otel_rules.cpp src/acl_otel_sampling.cpp)
target_include_directories(acl_otel_test_metrics_otlp PRIVATE test/cpp
                           ${CMAKE_CURRENT_SOURCE_DIR}/duckdb/third_party/httplib)
target_link_libraries(acl_otel_test_metrics_otlp duckdb ${ACL_OTEL_SDK_LIBS} opentelemetry-cpp::proto
                      opentelemetry-cpp::proto_grpc gRPC::grpc++)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
