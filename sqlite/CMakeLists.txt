# SQLite loadable module (laya.so) and static library (libsqlite_laya.a).
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(sqlaya-sqlite LANGUAGES C CXX)
endif()
if(NOT TARGET sqlaya-engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" engine)
endif()

set(SQLITE_LAYA_VERSION "v${SQLAYA_VERSION_TEXT}")
configure_file(src/sqlaya.h.in sqlaya.h)

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

# Loadable module: laya.so / laya.dylib, entry point sqlite3_laya_init.
add_library(sqlaya SHARED src/sqlaya.cpp)
set_target_properties(sqlaya PROPERTIES PREFIX "" OUTPUT_NAME "laya" CXX_VISIBILITY_PRESET hidden)
target_compile_options(sqlaya PRIVATE -Wall -Wextra -Wpedantic)
target_include_directories(sqlaya PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(sqlaya PRIVATE sqlaya-engine)
if(APPLE)
  target_link_options(sqlaya PRIVATE "-Wl,-exported_symbol,_sqlite3_laya_init")
elseif(UNIX)
  # Keep ggml and laya symbols private to the module.
  target_link_options(sqlaya PRIVATE "-Wl,--exclude-libs,ALL")
endif()

# Static library for embedding; consumers also link the sqlaya-engine target.
add_library(sqlaya-static STATIC src/sqlaya.cpp)
set_target_properties(sqlaya-static PROPERTIES OUTPUT_NAME "sqlite_laya")
target_include_directories(sqlaya-static PUBLIC "${CMAKE_CURRENT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(sqlaya-static PUBLIC sqlaya-engine)
