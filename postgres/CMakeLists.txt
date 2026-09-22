cmake_minimum_required(VERSION 3.24)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" SQLAYA_VERSION_TEXT)
string(STRIP "${SQLAYA_VERSION_TEXT}" SQLAYA_VERSION_TEXT)
string(REGEX REPLACE "-.*$" "" SQLAYA_VERSION_NUMBER "${SQLAYA_VERSION_TEXT}")
project(pglaya VERSION ${SQLAYA_VERSION_NUMBER} LANGUAGES C CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
find_package(PostgreSQL REQUIRED)

# The module folds the laya runtime and ggml into one shared object.
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
# Metal is the GPU backend on Apple platforms, where CUDA never applies.
if(APPLE AND NOT SQLAYA_CUDA)
  set(SQLAYA_METAL_DEFAULT ON)
else()
  set(SQLAYA_METAL_DEFAULT OFF)
endif()
option(SQLAYA_METAL "Build the Apple Metal inference backend" ${SQLAYA_METAL_DEFAULT})
set(LAYA_METAL ${SQLAYA_METAL} CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor/laya.cpp" laya.cpp EXCLUDE_FROM_ALL)

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

target_compile_features(pglaya PRIVATE cxx_std_20)
# PostgreSQL headers trip -Wextra in C.
target_compile_options(pglaya PRIVATE -Wall $<$<COMPILE_LANGUAGE:CXX>:-Wextra>)
target_compile_definitions(pglaya PRIVATE
  PGLAYA_VERSION="v${SQLAYA_VERSION_TEXT}"
  SQLAYA_CUDA_DEFAULT=$<BOOL:${SQLAYA_CUDA}>
  SQLAYA_METAL_DEFAULT=$<BOOL:${SQLAYA_METAL}>)
target_include_directories(pglaya PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
target_link_libraries(pglaya PRIVATE laya)
# PostgreSQL looks up the entry points in laya.c; keep everything else private.
set_source_files_properties(src/bridge.cpp PROPERTIES COMPILE_OPTIONS "-fvisibility=hidden")
if(APPLE)
  target_link_options(pglaya PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
elseif(UNIX)
  target_link_options(pglaya PRIVATE "-Wl,--exclude-libs,ALL")
endif()
