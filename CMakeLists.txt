# Top-level build: the shared engine, the SQLite module and, when PostgreSQL
# server development files are available, the PostgreSQL extension.
cmake_minimum_required(VERSION 3.24)
project(sqlaya LANGUAGES C CXX)
enable_testing()

add_subdirectory(engine)
add_subdirectory(sqlite)

find_program(PG_CONFIG pg_config)
set(SQLAYA_POSTGRES_DEFAULT OFF)
if(PG_CONFIG)
  execute_process(COMMAND ${PG_CONFIG} --includedir-server
    OUTPUT_VARIABLE pg_server_include OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(EXISTS "${pg_server_include}/postgres.h")
    set(SQLAYA_POSTGRES_DEFAULT ON)
  endif()
endif()
option(SQLAYA_POSTGRES "Build the PostgreSQL extension" ${SQLAYA_POSTGRES_DEFAULT})
if(SQLAYA_POSTGRES)
  add_subdirectory(postgres)
endif()
