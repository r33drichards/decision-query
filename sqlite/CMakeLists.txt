# SQLite loadable module (laya.so) and static library (libdecision_query.a).
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(decision-query-sqlite LANGUAGES C CXX)
endif()
if(NOT TARGET decision-query-engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" engine)
endif()

set(SQLITE_DQ_VERSION "v${DQ_VERSION_TEXT}")
configure_file(src/decision_query.h.in decision_query.h)

# Only the extension headers are required; the host application provides SQLite itself.
find_path(SQLITE3EXT_INCLUDE_DIR sqlite3ext.h)
if(NOT SQLITE3EXT_INCLUDE_DIR)
  message(STATUS "sqlite3ext.h not found on the system; fetching the SQLite amalgamation headers")
  include(FetchContent)
  FetchContent_Declare(sqlite_amalgamation
    URL https://sqlite.org/2024/sqlite-amalgamation-3450100.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(sqlite_amalgamation)
  set(SQLITE3EXT_INCLUDE_DIR "${sqlite_amalgamation_SOURCE_DIR}")
endif()

# SQLite strips non-alphanumeric characters from the base name when it derives
# the entry point, so decision_query.dylib resolves sqlite3_decisionquery_init.
# Loadable module: decision_query.so / decision_query.dylib, entry point sqlite3_decisionquery_init.
add_library(decision-query SHARED src/decision_query.cpp)
set_target_properties(decision-query PROPERTIES PREFIX "" OUTPUT_NAME "decision_query" CXX_VISIBILITY_PRESET hidden)
target_compile_options(decision-query PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(decision-query PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(decision-query PRIVATE decision-query-engine)
if(APPLE)
  target_link_options(decision-query PRIVATE "-Wl,-exported_symbol,_sqlite3_decisionquery_init")
elseif(UNIX)
  # Keep ggml and laya symbols private to the module.
  target_link_options(decision-query PRIVATE "-Wl,--exclude-libs,ALL")
endif()

# Static library for embedding; consumers also link the decision-query-engine target.
add_library(decision-query-static STATIC src/decision_query.cpp)
set_target_properties(decision-query-static PROPERTIES OUTPUT_NAME "decision_query")
target_include_directories(decision-query-static PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(decision-query-static PUBLIC decision-query-engine)

# Warning set and sanitizers from the top-level build (see ../CMakeLists.txt).
if(TARGET dq_options)
  target_link_libraries(decision-query PRIVATE dq_options dq_warnings)
  target_link_libraries(decision-query-static PRIVATE dq_options dq_warnings)
endif()
