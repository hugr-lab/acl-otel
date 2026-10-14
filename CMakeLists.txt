cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME acl_otel)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# the sources, the include directories, the SDK and its libraries (spec 024: the
# same file an embedder includes)
include(${CMAKE_CURRENT_LIST_DIR}/acl_otel_sources.cmake)
set(EXTENSION_SOURCES ${ACL_OTEL_SOURCES})

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

acl_otel_configure_targets(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME})

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
# spec 011: tresor's audit through the same transports, against a fake receiver
acl_otel_sdk_test(acl_otel_test_tresor_otlp
                  test/cpp/test_acl_otel_tresor_otlp.cpp)
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
