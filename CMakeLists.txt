cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SQLAYA_VERSION_TEXT)
string(STRIP "${SQLAYA_VERSION_TEXT}" SQLAYA_VERSION_TEXT)
string(REGEX REPLACE "-.*$" "" SQLAYA_VERSION_NUMBER "${SQLAYA_VERSION_TEXT}")
project(sqlaya VERSION ${SQLAYA_VERSION_NUMBER} LANGUAGES C CXX)
set(SQLITE_LAYA_VERSION "v${SQLAYA_VERSION_TEXT}")
configure_file(src/sqlaya.h.in sqlaya.h)

# The loadable module folds the laya runtime and ggml into one shared object.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  set(SQLAYA_CUDA_DEFAULT ON)
else()
  set(SQLAYA_CUDA_DEFAULT OFF)
endif()
option(SQLAYA_CUDA "Build the NVIDIA CUDA inference backend" ${SQLAYA_CUDA_DEFAULT})
set(LAYA_CUDA ${SQLAYA_CUDA} CACHE BOOL "" FORCE)
add_subdirectory(vendor/laya.cpp EXCLUDE_FROM_ALL)

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

set(SQLAYA_SOURCES src/sqlaya.cpp)
set(SQLAYA_DEFINITIONS SQLAYA_CUDA_DEFAULT=$<BOOL:${SQLAYA_CUDA}>)

# Loadable module: laya.so / laya.dylib, entry point sqlite3_laya_init.
add_library(sqlaya SHARED ${SQLAYA_SOURCES})
set_target_properties(sqlaya PROPERTIES PREFIX "" OUTPUT_NAME "laya" CXX_VISIBILITY_PRESET hidden)
target_compile_features(sqlaya PRIVATE cxx_std_20)
target_compile_options(sqlaya PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(sqlaya PRIVATE ${SQLAYA_DEFINITIONS})
target_include_directories(sqlaya PRIVATE "${PROJECT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(sqlaya PRIVATE laya)
if(APPLE)
  target_link_options(sqlaya PRIVATE "-Wl,-exported_symbol,_sqlite3_laya_init")
elseif(UNIX)
  # Keep ggml and laya symbols private to the module.
  target_link_options(sqlaya PRIVATE "-Wl,--exclude-libs,ALL")
endif()

# Static library for embedding; consumers also link the laya target.
add_library(sqlaya-static STATIC ${SQLAYA_SOURCES})
set_target_properties(sqlaya-static PROPERTIES OUTPUT_NAME "sqlite_laya")
target_compile_features(sqlaya-static PRIVATE cxx_std_20)
target_compile_definitions(sqlaya-static PRIVATE ${SQLAYA_DEFINITIONS})
target_include_directories(sqlaya-static PUBLIC "${PROJECT_BINARY_DIR}" "${SQLITE3EXT_INCLUDE_DIR}")
target_link_libraries(sqlaya-static PUBLIC laya)
