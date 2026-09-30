# This file is included by DuckDB's build system. It specifies which extension to load

# The extension itself, never statically linked: a linked extension loads at every database startup,
# and Load attaches a worker thread and a sink - the tests LOAD the built file by path instead. Since
# duckdb #26189 (spec 014) not linking is the default - only duckdb_extension_statically_link links,
# and the DONT_LINK this block carried is gone.
duckdb_extension_load(acl_otel
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)
