# libFuzzer targets, after cpp-best-practices/cmake_template's fuzz_test/.
# Built by `cmake -DDQ_BUILD_FUZZ_TESTS=ON` with clang, and by ClusterFuzzLite
# through .clusterfuzzlite/build.sh, which passes its own DQ_LIB_FUZZING_ENGINE.
include(FetchContent)
include(LibFuzzer)
dq_check_libfuzzer_support(DQ_LIBFUZZER_SUPPORTED)
if(NOT DQ_LIBFUZZER_SUPPORTED AND DQ_LIB_FUZZING_ENGINE STREQUAL "-fsanitize=fuzzer")
  message(FATAL_ERROR "DQ_BUILD_FUZZ_TESTS needs a compiler with libFuzzer (clang)")
endif()

find_package(SQLite3)
if(NOT SQLite3_FOUND)
  # Without system SQLite, sqlite/CMakeLists.txt fetched the amalgamation for
  # its headers; compile it in, as ClusterFuzzLite's runner has no libsqlite3.
  FetchContent_GetProperties(sqlite_amalgamation)
  if(NOT sqlite_amalgamation_POPULATED)
    message(FATAL_ERROR "Fuzz targets need SQLite: install libsqlite3-dev")
  endif()
  add_library(dq_fuzz_sqlite3 STATIC "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
  target_include_directories(dq_fuzz_sqlite3 SYSTEM PUBLIC "${sqlite_amalgamation_SOURCE_DIR}")
  target_link_libraries(dq_fuzz_sqlite3 PUBLIC ${CMAKE_DL_LIBS})
  add_library(SQLite::SQLite3 ALIAS dq_fuzz_sqlite3)
endif()

# Seconds each target runs under ctest, to catch regressions in ordinary CI.
set(FUZZ_RUNTIME 10 CACHE STRING "Number of seconds to run each fuzz target during ctest")

foreach(target fuzz_options fuzz_endpoint fuzz_sql)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE decision-query-static SQLite::SQLite3 dq_options dq_warnings)
  separate_arguments(engine_flags UNIX_COMMAND "${DQ_LIB_FUZZING_ENGINE}")
  target_link_options(${target} PRIVATE ${engine_flags})
  # libFuzzer writes new inputs to the first corpus directory, so give it a
  # scratch one and read the checked-in seeds from the second.
  file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/corpus/${target}")
  add_test(NAME ${target}_run
    COMMAND ${target} -max_total_time=${FUZZ_RUNTIME}
      "${CMAKE_CURRENT_BINARY_DIR}/corpus/${target}" "${CMAKE_CURRENT_SOURCE_DIR}/corpus/${target}")
  set_tests_properties(${target}_run PROPERTIES LABELS fuzz)
endforeach()
