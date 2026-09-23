# Native unit tests (Catch2). They need no checkpoint, so they run in every CI
# job, under the sanitizers and under valgrind (ctest -T memcheck).
include(FetchContent)
FetchContent_Declare(Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
  SYSTEM)
FetchContent_MakeAvailable(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)

find_package(SQLite3 REQUIRED)

add_executable(dq_tests engine_test.cpp http_backend_test.cpp sqlite_test.cpp)
target_link_libraries(dq_tests PRIVATE
  decision-query-static SQLite::SQLite3 Catch2::Catch2WithMain dq_options dq_warnings)
catch_discover_tests(dq_tests PROPERTIES LABELS unit)
