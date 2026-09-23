# Top-level build: the shared engine, the SQLite module and, when PostgreSQL
# server development files are available, the PostgreSQL extension.
cmake_minimum_required(VERSION 3.24)
project(decision-query LANGUAGES C CXX)
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
