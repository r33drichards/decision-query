# PostgreSQL extension `decision_query` (module decision_query.so, control file and SQL script).
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pgdq LANGUAGES C CXX)
endif()
if(NOT TARGET decision-query-engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" engine)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(PostgreSQL REQUIRED)

enable_testing()

# The model-backed regression test runs only when a checkpoint is configured.
set(DQ_MODEL_DIR "" CACHE PATH "Checkpoint directory for the model-backed regression test")
set(DQ_OPTIONS "{}" CACHE STRING "JSON load options for the model-backed regression test")
set(regress basic)
set(temp_config)
if(DQ_MODEL_DIR)
  get_filename_component(model_dir "${DQ_MODEL_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
  file(WRITE "${CMAKE_BINARY_DIR}/model.conf"
    "decision_query.model_dir = '${model_dir}'\ndecision_query.options = '${DQ_OPTIONS}'\n")
  list(APPEND regress model)
  set(temp_config TEMP_CONFIG "${CMAKE_BINARY_DIR}/model.conf")
endif()

# The install script is kept unversioned and named for the version at build
# time (the PGXN convention), so a release only has to set ../VERSION.
configure_file(decision_query.sql
  "${CMAKE_CURRENT_BINARY_DIR}/decision_query--${DQ_VERSION_TEXT}.sql" COPYONLY)

add_postgresql_extension(decision_query
  TARGET pgdq
  VERSION ${DQ_VERSION_TEXT}
  COMMENT "Typed decisions as SQL functions"
  SOURCES src/decision_query.c src/bridge.cpp
  SCRIPTS "${CMAKE_CURRENT_BINARY_DIR}/decision_query--${DQ_VERSION_TEXT}.sql"
  REGRESS ${regress}
  ${temp_config})

# PostgreSQL headers trip -Wextra in C.
target_compile_options(pgdq PRIVATE -Wall $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
target_compile_definitions(pgdq PRIVATE PGDQ_VERSION="v${DQ_VERSION_TEXT}")
target_link_libraries(pgdq PRIVATE decision-query-engine)
# PostgreSQL looks up the entry points in decision_query.c; keep everything else private.
set_source_files_properties(src/bridge.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
if(APPLE)
  target_link_options(pgdq PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
elseif(UNIX)
  target_link_options(pgdq PRIVATE "-Wl,--exclude-libs,ALL")
endif()
