# acl_otel_sources.cmake - what acl_otel compiles and links, for its own build
# and for an extension that builds acl_otel into itself (spec 024: hugr-node's
# hugr_node, as it builds duckdb-acl and tresor in). Paths are this repository's
# (ACL_OTEL_ROOT); duckdb's are the build's own source tree (CMAKE_SOURCE_DIR -
# the duckdb a build runs from), never this repository's submodule by path.
#
# An embedder, in its own CMakeLists:
#
# * set(ACL_OTEL_EMBEDDED ON) - no acl_otel entry point, acl_otel's own version
# * include(<acl-otel>/acl_otel_sources.cmake) - ACL_OTEL_SOURCES, the include
#   directories, the SDK
# * build_static_extension / build_loadable_extension - its own targets, with
#   ACL_OTEL_SOURCES among their sources
# * acl_otel_configure_targets(<static> <loadable>) - the SDK's libraries on
#   them
#
# Include it from the directory that creates the targets: the embedding
# definitions are properties of the sources.

set(ACL_OTEL_ROOT ${CMAKE_CURRENT_LIST_DIR})

# the base's contract header (acl_audit.hpp + acl_principal.hpp, header-only)
# and tresor's audit hooks come from the shared repository duckdb-ext-common
# (spec 010; its contracts/ and hooks/ directories, the same headers the base
# compiles) - nothing of acl's is linked (a loadable is dlopen'd RTLD_LOCAL);
# the level rules are parsed with the build's own bundled yyjson
include_directories(
  ${ACL_OTEL_ROOT}/src/include ${ACL_OTEL_ROOT}/duckdb-ext-common/contracts
  ${ACL_OTEL_ROOT}/duckdb-ext-common/hooks
  ${CMAKE_SOURCE_DIR}/third_party/yyjson/include)

# the OpenTelemetry C++ SDK (spec 002): the OTLP exporters over HTTP/protobuf
# and gRPC for the three signals, from vcpkg
# (opentelemetry-cpp[otlp-http,otlp-grpc] in vcpkg.json), linked statically as
# the base links Arrow. The TRACE exporters are the unsuffixed targets; the log
# and metric ones carry the signal in their name (spec 008's note).
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
    opentelemetry-cpp::resources
    opentelemetry-cpp::http_client_curl)

# everything but the entry point: what the loadable is, and what the SDK-linked
# tests compile in
set(ACL_OTEL_LIB_SOURCES
    ${ACL_OTEL_ROOT}/src/acl_otel_rules.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_sink.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_tresor.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_state.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_otlp.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_metrics.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_otlp_metrics.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_sampling.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_traces.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_otlp_traces.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_lineage.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_lineage_http.cpp
    ${ACL_OTEL_ROOT}/src/acl_otel_lineage_key.cpp)
set(ACL_OTEL_SOURCES ${ACL_OTEL_ROOT}/src/acl_otel_extension.cpp
                     ${ACL_OTEL_LIB_SOURCES})

# Built into another extension (spec 024): acl_otel's entry point and
# AclOtelExtension are not compiled (the embedder's entry is the one), and its
# version is acl-otel's own - duckdb defines EXT_VERSION_<NAME> for the
# extension it builds, which is then the embedder's
if(ACL_OTEL_EMBEDDED)
  set_property(
    SOURCE ${ACL_OTEL_SOURCES}
    APPEND
    PROPERTY COMPILE_DEFINITIONS ACL_OTEL_EMBEDDED)
  set(ACL_OTEL_EMBEDDED_VERSION "")
  if(COMMAND duckdb_extension_generate_version)
    duckdb_extension_generate_version(ACL_OTEL_EMBEDDED_VERSION
                                      ${ACL_OTEL_ROOT})
  endif()
  if(NOT "${ACL_OTEL_EMBEDDED_VERSION}" STREQUAL "")
    # every source that names the version reads it through acl_otel_version.hpp
    set_property(
      SOURCE ${ACL_OTEL_SOURCES}
      APPEND
      PROPERTY COMPILE_DEFINITIONS
               ACL_OTEL_VERSION="${ACL_OTEL_EMBEDDED_VERSION}")
  endif()
endif()

# Built in beside duckdb-acl (ACL_ROOT) or tresor (TRESOR_ROOT): one directory,
# so whichever duckdb-ext-common the include directories name first serves the
# contracts and the hooks to every half. They must be the same files, not only
# the same contract stamps: refused otherwise.
function(acl_otel_check_shared_headers)
  # the headers only: hooks/ holds a test/ directory, and CMake hashes a
  # directory as an empty file on macOS but fails to read it on Linux
  file(
    GLOB shared
    LIST_DIRECTORIES false
    RELATIVE "${ACL_OTEL_ROOT}/duckdb-ext-common"
    "${ACL_OTEL_ROOT}/duckdb-ext-common/contracts/*.hpp"
    "${ACL_OTEL_ROOT}/duckdb-ext-common/hooks/*.hpp")
  foreach(root ${ACL_ROOT} ${TRESOR_ROOT})
    if(NOT EXISTS "${root}/duckdb-ext-common")
      continue()
    endif()
    foreach(header ${shared})
      if(EXISTS "${root}/duckdb-ext-common/${header}")
        file(SHA256 "${ACL_OTEL_ROOT}/duckdb-ext-common/${header}" ours)
        file(SHA256 "${root}/duckdb-ext-common/${header}" theirs)
        if(NOT ours STREQUAL theirs)
          message(
            FATAL_ERROR
              "acl_otel and ${root}, built into one extension, need the same "
              "duckdb-ext-common: ${header} differs (pin the submodules to one "
              "commit)")
        endif()
      endif()
    endforeach()
  endforeach()
endfunction()

# acl_otel's libraries, on the targets the including build made
function(acl_otel_configure_targets)
  acl_otel_check_shared_headers()
  foreach(target ${ARGN})
    target_link_libraries(${target} ${ACL_OTEL_SDK_LIBS})
    if(ACL_OTEL_EMBEDDED)
      # the embedder's own sources see acl_otel_extension.hpp as acl_otel's do
      target_compile_definitions(${target} PRIVATE ACL_OTEL_EMBEDDED)
    endif()
  endforeach()
endfunction()
