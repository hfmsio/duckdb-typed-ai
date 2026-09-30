cmake_minimum_required(VERSION 3.5)

set(TARGET_NAME typed_ai)
set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)
project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD "17" CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# DuckDB 2.0 changed several C++ APIs; src/include/typed_ai/compat.hpp switches on TYPED_AI_DUCKDB_V2.
# DuckDB's own major version decides, because single headers such as identifier.hpp get backported to 1.5
# patch releases. A build from a branch (e.g. v2.0-cyanoptera) has no version number, so it falls back to a
# header that only the 2.0 vector API has.
set(TYPED_AI_DUCKDB_V2 OFF)
if(DUCKDB_MAJOR_VERSION MATCHES "^[0-9]+$")
  if(DUCKDB_MAJOR_VERSION GREATER_EQUAL 2)
    set(TYPED_AI_DUCKDB_V2 ON)
  endif()
elseif(EXISTS "${DUCKDB_MODULE_BASE_DIR}/src/include/duckdb/common/vector/flat_vector.hpp")
  set(TYPED_AI_DUCKDB_V2 ON)
endif()
if(TYPED_AI_DUCKDB_V2)
  add_definitions(-DTYPED_AI_DUCKDB_V2=1)
endif()
message(STATUS "typed_ai: DuckDB version '${DUCKDB_MAJOR_VERSION}', 2.0 APIs: ${TYPED_AI_DUCKDB_V2}")

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
