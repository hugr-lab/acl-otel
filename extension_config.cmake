# This file is included by DuckDB's build system. It specifies which extension to load

# The extension itself. DONT_LINK: a statically linked extension loads at every database startup,
# and Load attaches a worker thread and a sink - the tests LOAD the built file by path instead.
duckdb_extension_load(acl_otel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    DONT_LINK
    LOAD_TESTS
)
