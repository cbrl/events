cmake_minimum_required(VERSION 3.14)

project(eventsTests LANGUAGES CXX)

include(../cmake/project-is-top-level.cmake)
include(../cmake/folders.cmake)

# ---- Dependencies ----

if(PROJECT_IS_TOP_LEVEL)
  find_package(events REQUIRED)
  enable_testing()
endif()

# ---- Tests ----

add_executable(events_test source/main.cpp)
target_link_libraries(
    events_test PRIVATE
    events::events
)
target_compile_features(events_test PRIVATE cxx_std_20)

# ---- End-of-file commands ----

add_folders(Test)
