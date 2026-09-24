# Top-level build: the shared engine, the SQLite module and, when PostgreSQL
# server development files are available, the PostgreSQL extension.
cmake_minimum_required(VERSION 3.24)
project(decision-query LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Hygiene options, following cpp-best-practices/cmake_template's ProjectOptions.cmake.
# All default off so the shipped build is unchanged; CI turns them on per job.
option(DQ_WARNINGS_AS_ERRORS "Treat warnings in this project's own code as errors" OFF)
option(DQ_ENABLE_SANITIZER_ADDRESS "Enable AddressSanitizer" OFF)
option(DQ_ENABLE_SANITIZER_LEAK "Enable LeakSanitizer" OFF)
option(DQ_ENABLE_SANITIZER_UNDEFINED "Enable UndefinedBehaviorSanitizer" OFF)
option(DQ_ENABLE_SANITIZER_THREAD "Enable ThreadSanitizer" OFF)
option(DQ_ENABLE_SANITIZER_MEMORY "Enable MemorySanitizer" OFF)
option(DQ_BUILD_TESTS "Build the native unit tests (fetches Catch2)" OFF)
option(DQ_BUILD_FUZZ_TESTS "Build the libFuzzer targets in fuzz/" OFF)
# ClusterFuzzLite and OSS-Fuzz pass their engine in $LIB_FUZZING_ENGINE.
set(DQ_LIB_FUZZING_ENGINE "-fsanitize=fuzzer" CACHE STRING "Link flags that provide the fuzzing engine")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(cmake/CompilerWarnings.cmake)
include(cmake/Sanitizers.cmake)

# Interface targets every first-party target links: dq_warnings for the warning
# set, dq_options for the sanitizers. Vendored laya.cpp and ggml link neither.
add_library(dq_warnings INTERFACE)
dq_set_project_warnings(dq_warnings ${DQ_WARNINGS_AS_ERRORS} "" "" "" "")
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # GCC 13 reports -Wnull-dereference inside libstdc++'s <streambuf> once
  # optimizing inlines it into our code; the warning is not ours to fix.
  target_compile_options(dq_warnings INTERFACE -Wno-null-dereference)
endif()
add_library(dq_options INTERFACE)
dq_enable_sanitizers(dq_options
  ${DQ_ENABLE_SANITIZER_ADDRESS}
  ${DQ_ENABLE_SANITIZER_LEAK}
  ${DQ_ENABLE_SANITIZER_UNDEFINED}
  ${DQ_ENABLE_SANITIZER_THREAD}
  ${DQ_ENABLE_SANITIZER_MEMORY})
if(DQ_ENABLE_SANITIZER_UNDEFINED)
  # Make UBSan findings fail the test rather than only print.
  target_compile_options(dq_options INTERFACE -fno-sanitize-recover=all)
  target_link_options(dq_options INTERFACE -fno-sanitize-recover=all)
endif()

if(DQ_BUILD_FUZZ_TESTS)
  # Coverage instrumentation for the code under test, not just the harnesses.
  target_compile_options(dq_options INTERFACE -fsanitize=fuzzer-no-link)
endif()

# Provides `ctest -T memcheck`, which runs the tests under valgrind.
find_program(MEMORYCHECK_COMMAND valgrind)
set(MEMORYCHECK_COMMAND_OPTIONS
  "--leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --track-origins=yes --error-exitcode=1"
  CACHE STRING "valgrind options for ctest -T memcheck")
set(MEMORYCHECK_SUPPRESSIONS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/.valgrind.supp"
  CACHE FILEPATH "valgrind suppressions for ctest -T memcheck")
include(CTest)
enable_testing()

add_subdirectory(engine)
add_subdirectory(sqlite)

find_program(PG_CONFIG pg_config)
set(DQ_POSTGRES_DEFAULT OFF)
if(PG_CONFIG)
  execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE pg_server_include OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(EXISTS "${pg_server_include}/postgres.h")
    set(DQ_POSTGRES_DEFAULT ON)
  endif()
endif()
option(DQ_POSTGRES "Build the PostgreSQL extension" ${DQ_POSTGRES_DEFAULT})
if(DQ_POSTGRES)
  add_subdirectory(postgres)
endif()

find_path(DQ_MYSQL_UDF_HEADER udf_registration_types.h PATH_SUFFIXES mysql)
if(DQ_MYSQL_UDF_HEADER)
  set(DQ_MYSQL_DEFAULT ON)
else()
  set(DQ_MYSQL_DEFAULT OFF)
endif()
option(DQ_MYSQL "Build the MySQL loadable-function module" ${DQ_MYSQL_DEFAULT})
if(DQ_MYSQL)
  add_subdirectory(mysql)
endif()

if(DQ_BUILD_TESTS)
  add_subdirectory(tests)
endif()
if(DQ_BUILD_FUZZ_TESTS)
  add_subdirectory(fuzz)
endif()
