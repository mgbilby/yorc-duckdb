cmake_minimum_required(VERSION 3.15)

set(TARGET_NAME yorc)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME} VERSION 0.2.0)

include_directories(src/include)

set(EXTENSION_SOURCES
    src/yorc_extension.cpp
    src/core/changes.cpp
    src/core/crosswalk.cpp
    src/core/id.cpp
    src/core/json.cpp
    src/core/json_map.cpp
    src/core/reservoir.cpp
    src/core/sha256.cpp
    src/core/sections.cpp
    src/core/tar.cpp
    src/core/writer.cpp
    src/core/xml.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# The core uses C++17 (std::optional); DuckDB's own targets may default lower.
set_target_properties(${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME}
                      PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
