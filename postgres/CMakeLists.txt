# PostgreSQL extension `laya` (module laya.so, control file and SQL script).
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(pglaya LANGUAGES C CXX)
endif()
if(NOT TARGET sqlaya-engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" engine)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(PostgreSQL REQUIRED)

enable_testing()

# The model-backed regression test runs only when a checkpoint is configured.
set(LAYA_MODEL_DIR "" CACHE PATH "Checkpoint directory for the model-backed regression test")
set(LAYA_OPTIONS "{}" CACHE STRING "JSON load options for the model-backed regression test")
set(regress basic)
set(temp_config)
if(LAYA_MODEL_DIR)
  get_filename_component(model_dir "${LAYA_MODEL_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
  file(WRITE "${CMAKE_BINARY_DIR}/model.conf"
    "laya.model_dir = '${model_dir}'\nlaya.options = '${LAYA_OPTIONS}'\n")
  list(APPEND regress model)
  set(temp_config TEMP_CONFIG "${CMAKE_BINARY_DIR}/model.conf")
endif()

add_postgresql_extension(laya
  TARGET pglaya
  VERSION ${SQLAYA_VERSION_TEXT}
  COMMENT "Laya typed decisions as SQL functions"
  SOURCES src/laya.c src/bridge.cpp
  SCRIPTS laya--${SQLAYA_VERSION_TEXT}.sql
  REGRESS ${regress}
  ${temp_config})

# PostgreSQL headers trip -Wextra in C.
target_compile_options(pglaya PRIVATE -Wall $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
target_compile_definitions(pglaya PRIVATE PGLAYA_VERSION="v${SQLAYA_VERSION_TEXT}")
target_link_libraries(pglaya PRIVATE sqlaya-engine)
# PostgreSQL looks up the entry points in laya.c; keep everything else private.
set_source_files_properties(src/bridge.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
if(APPLE)
  target_link_options(pglaya PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
elseif(UNIX)
  target_link_options(pglaya PRIVATE "-Wl,--exclude-libs,ALL")
endif()
