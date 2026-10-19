# Extension config for building `yorc` in-tree with DuckDB.
#
# core_functions: the sqllogictests use format(), string_agg and friends, which
# live in that extension rather than the engine. Released DuckDB binaries link
# it statically; a bare extension-template build does not.
duckdb_extension_load(core_functions)

duckdb_extension_load(yorc
    SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}
    LOAD_TESTS
)

# DuckDB v2.0 builds the extensions loaded above but links only those listed
# here (or in STATICALLY_LINK_EXTENSIONS); v1.5.x links them already and has
# no such command.
if(COMMAND duckdb_extension_statically_link)
    duckdb_extension_statically_link(yorc)
endif()
