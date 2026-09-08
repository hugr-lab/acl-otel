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

set(EXTENSION_SOURCES src/acl_otel_extension.cpp src/acl_otel_rules.cpp src/acl_otel_sink.cpp src/acl_otel_state.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
