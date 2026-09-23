# Shared configuration for every sqlaya module: the version, the vendored
# laya.cpp runtime built as static position-independent archives, the CUDA
# option, and the `decision-query-engine` interface target that modules link.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(decision-query-engine LANGUAGES C CXX)
endif()

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" DQ_VERSION_TEXT)
string(STRIP "${DQ_VERSION_TEXT}" DQ_VERSION_TEXT)
string(REGEX REPLACE "-.*$" "" SQLAYA_VERSION_NUMBER "${DQ_VERSION_TEXT}")
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(DQ_VERSION_TEXT "${DQ_VERSION_TEXT}" PARENT_SCOPE)
  set(SQLAYA_VERSION_NUMBER "${SQLAYA_VERSION_NUMBER}" PARENT_SCOPE)
endif()

# Modules fold the runtime and ggml into one shared object.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  set(DQ_CUDA_DEFAULT ON)
else()
  set(DQ_CUDA_DEFAULT OFF)
endif()
option(DQ_CUDA "Build the NVIDIA CUDA inference backend" ${DQ_CUDA_DEFAULT})
set(LAYA_CUDA ${DQ_CUDA} CACHE BOOL "" FORCE)
# Metal is the GPU backend on Apple platforms, where CUDA never applies.
if(APPLE AND NOT DQ_CUDA)
  set(DQ_METAL_DEFAULT ON)
else()
  set(DQ_METAL_DEFAULT OFF)
endif()
option(DQ_METAL "Build the Apple Metal inference backend" ${DQ_METAL_DEFAULT})
set(LAYA_METAL ${DQ_METAL} CACHE BOOL "" FORCE)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../laya.cpp" laya.cpp EXCLUDE_FROM_ALL)

add_library(decision-query-engine INTERFACE)
target_include_directories(decision-query-engine INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(decision-query-engine INTERFACE cxx_std_20)
target_compile_definitions(decision-query-engine INTERFACE
  DQ_CUDA_DEFAULT=$<BOOL:${DQ_CUDA}>
  DQ_METAL_DEFAULT=$<BOOL:${DQ_METAL}>)
target_link_libraries(decision-query-engine INTERFACE laya)
# Vendored headers are included as system headers so the warning set in
# ../cmake/CompilerWarnings.cmake applies to this project's code only.
target_include_directories(decision-query-engine SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/../laya.cpp/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/../laya.cpp/third_party/ggml/include")

# The HTTP decision backend (Jev / reflex / djev) reuses laya.cpp's vendored client.
target_include_directories(decision-query-engine SYSTEM INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/../laya.cpp/third_party/cpp-httplib")
find_package(OpenSSL QUIET)
if(OpenSSL_FOUND)
  target_compile_definitions(decision-query-engine INTERFACE CPPHTTPLIB_OPENSSL_SUPPORT)
  target_link_libraries(decision-query-engine INTERFACE OpenSSL::SSL OpenSSL::Crypto)
  message(STATUS "sqlaya: HTTPS decision endpoints enabled")
else()
  message(STATUS "sqlaya: OpenSSL not found; only http:// decision endpoints will work")
endif()
