# Shared configuration for every sqlaya module: the version, the vendored
# laya.cpp runtime built as static position-independent archives, the CUDA
# option, and the `sqlaya-engine` interface target that modules link.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(sqlaya-engine LANGUAGES C CXX)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" SQLAYA_VERSION_TEXT)
string(STRIP "${SQLAYA_VERSION_TEXT}" SQLAYA_VERSION_TEXT)
string(REGEX REPLACE "-.*$" "" SQLAYA_VERSION_NUMBER "${SQLAYA_VERSION_TEXT}")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SQLAYA_VERSION_TEXT "${SQLAYA_VERSION_TEXT}" PARENT_SCOPE)
  set(SQLAYA_VERSION_NUMBER "${SQLAYA_VERSION_NUMBER}" PARENT_SCOPE)
endif()

# Modules fold the runtime and ggml into one shared object.
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
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../laya.cpp" laya.cpp EXCLUDE_FROM_ALL)

add_library(sqlaya-engine INTERFACE)
target_include_directories(sqlaya-engine INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(sqlaya-engine INTERFACE cxx_std_20)
target_compile_definitions(sqlaya-engine INTERFACE SQLAYA_CUDA_DEFAULT=$<BOOL:${SQLAYA_CUDA}>)
target_link_libraries(sqlaya-engine INTERFACE laya)
