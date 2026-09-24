# MySQL loadable-function module (decision_query.so) and its install scripts.
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(decision-query-mysql LANGUAGES C CXX)
endif()
if(NOT TARGET decision-query-engine)
  add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../engine" engine)
endif()

# Only the loadable-function header is needed; mysqld provides everything else.
find_program(MYSQL_CONFIG NAMES mysql_config)
set(mysql_hints)
if(MYSQL_CONFIG)
  execute_process(COMMAND ${MYSQL_CONFIG} --variable=pkgincludedir
    OUTPUT_VARIABLE mysql_hints OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
find_path(MYSQL_UDF_INCLUDE_DIR udf_registration_types.h
  HINTS ${mysql_hints} PATH_SUFFIXES mysql REQUIRED)

add_library(dq-mysql SHARED src/decision_query.cpp)
# MySQL resolves the SONAME given to CREATE FUNCTION inside its plugin_dir.
set_target_properties(dq-mysql PROPERTIES PREFIX "" OUTPUT_NAME "decision_query" SUFFIX ".so"
  CXX_VISIBILITY_PRESET hidden)
target_compile_options(dq-mysql PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(dq-mysql PRIVATE MYSQL_DQ_VERSION="v${DQ_VERSION_TEXT}")
target_include_directories(dq-mysql SYSTEM PRIVATE "${MYSQL_UDF_INCLUDE_DIR}")
target_link_libraries(dq-mysql PRIVATE decision-query-engine ${CMAKE_DL_LIBS})
if(APPLE)
  target_link_options(dq-mysql PRIVATE -undefined dynamic_lookup)
elseif(UNIX)
  # Keep ggml and laya symbols private to the module.
  target_link_options(dq-mysql PRIVATE "-Wl,--exclude-libs,ALL")
endif()
if(TARGET dq_options)
  target_link_libraries(dq-mysql PRIVATE dq_options dq_warnings)
endif()

# Installs into the server's plugin_dir when mysql_config reports one.
set(DQ_MYSQL_PLUGIN_DIR "" CACHE PATH "MySQL plugin_dir to install decision_query.so into")
if(NOT DQ_MYSQL_PLUGIN_DIR AND MYSQL_CONFIG)
  execute_process(COMMAND ${MYSQL_CONFIG} --plugindir
    OUTPUT_VARIABLE plugin_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(DQ_MYSQL_PLUGIN_DIR "${plugin_dir}" CACHE PATH "" FORCE)
endif()
if(DQ_MYSQL_PLUGIN_DIR)
  install(TARGETS dq-mysql LIBRARY DESTINATION "${DQ_MYSQL_PLUGIN_DIR}" COMPONENT mysql
    EXCLUDE_FROM_ALL)
endif()
