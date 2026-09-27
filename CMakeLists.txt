cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME typed_ai)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# JSON comes from DuckDB's own copy of yyjson, compiled into this extension.
include_directories(src/include ${DUCKDB_MODULE_BASE_DIR}/third_party/yyjson/include)

set(EXTENSION_SOURCES
    src/typed_ai_extension.cpp
    src/functions.cpp
    src/guard.cpp
    src/runner.cpp
    src/profile.cpp
    src/json.cpp
    src/providers/jev.cpp
    src/providers/openai.cpp
    ${DUCKDB_MODULE_BASE_DIR}/third_party/yyjson/yyjson.cpp)

build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")
