cmake_minimum_required(VERSION 3.14)

project(eventsBenchmark CXX)

include(../cmake/project-is-top-level.cmake)
include(../cmake/folders.cmake)

if(PROJECT_IS_TOP_LEVEL)
  find_package(events REQUIRED)
endif()

find_package(fmt CONFIG REQUIRED)
find_package(tabulate CONFIG REQUIRED)

add_executable(events_benchmark source/main.cpp)
target_link_libraries(
    events_benchmark PRIVATE
    events::events
    fmt::fmt
    tabulate::tabulate
)
target_compile_features(events_benchmark PRIVATE cxx_std_20)
target_compile_options(events_benchmark PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

add_folders(Benchmark)
