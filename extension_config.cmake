# Loaded by DuckDB's build system: which extensions to build.
duckdb_extension_load(typed_ai
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    EXTENSION_VERSION v0.1.0
    LOAD_TESTS
)
