cmake_minimum_required(VERSION 3.14)

project(eventsTests LANGUAGES CXX)

include(../cmake/project-is-top-level.cmake)
include(../cmake/folders.cmake)

# ---- Dependencies ----

find_package(Catch2 REQUIRED)
include(Catch)

if(PROJECT_IS_TOP_LEVEL)
  find_package(events REQUIRED)
  enable_testing()
endif()

# ---- Tests ----

add_executable(events_test
    source/test_connection.cpp
    source/test_signal_handler.cpp
    source/test_synchronized_signal_handler.cpp
    source/test_event_dispatcher.cpp
    source/test_synchronized_event_dispatcher.cpp
)
target_link_libraries(
    events_test PRIVATE
    events::events
    Catch2::Catch2WithMain
)
target_compile_features(events_test PRIVATE cxx_std_20)

catch_discover_tests(events_test)

# ---- End-of-file commands ----

add_folders(Test)
