# This file is included by DuckDB's build system. It specifies which extension to load

# Spec 017 (duckdb-acl spec 106): clang-cl with MSVC-built vcpkg ports (protobuf, gRPC, abseil, ...).
# Both pool a string literal into a COMDAT of one name; MSVC aligns its copy to 16 and reads it with
# movaps, clang-cl aligns its copy to 1 - when the linker keeps clang-cl's off a 16-byte boundary, an
# MSVC static initializer faults before main. /GF- keeps clang-cl's literals private, so every pooled
# literal the link selects is MSVC's. Read before duckdb adds src/, so it reaches duckdb's objects too.
# duckdb/extension-ci-tools#430.
if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    add_compile_options(/GF-)
endif()

# The extension itself, never statically linked: a linked extension loads at every database startup,
# and Load attaches a worker thread and a sink - the tests LOAD the built file by path instead. Since
# duckdb #26189 (spec 014) not linking is the default - only duckdb_extension_statically_link links,
# and the DONT_LINK this block carried is gone.
duckdb_extension_load(acl_otel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
