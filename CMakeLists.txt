cmake_minimum_required(VERSION 3.14)

include(cmake/prelude.cmake)

project(
    events
    VERSION 0.1.0
    DESCRIPTION "Short description"
    HOMEPAGE_URL "https://example.com/"
    LANGUAGES NONE
)

include(cmake/project-is-top-level.cmake)
include(cmake/variables.cmake)

find_package(Boost REQUIRED)
find_path(PLF_COLONY_INCLUDE_DIRS "plf_colony.h")

# ---- Declare library ----

add_library(events_events INTERFACE)
add_library(events::events ALIAS events_events)

set_property(
    TARGET events_events PROPERTY
    EXPORT_NAME events
)

target_include_directories(
    events_events ${warning_guard}
    INTERFACE
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
	"$<BUILD_INTERFACE:${PLF_COLONY_INCLUDE_DIRS}>"
)

target_compile_features(events_events INTERFACE cxx_std_20)

target_link_libraries(events_events INTERFACE Boost::boost)

# ---- Install rules ----

if(NOT CMAKE_SKIP_INSTALL_RULES)
  include(cmake/install-rules.cmake)
endif()

# ---- Examples ----

if(PROJECT_IS_TOP_LEVEL)
  option(BUILD_EXAMPLES "Build examples tree." "${events_DEVELOPER_MODE}")
  if(BUILD_EXAMPLES)
    add_subdirectory(examples)
  endif()
endif()

# ---- Developer mode ----

if(NOT events_DEVELOPER_MODE)
  return()
elseif(NOT PROJECT_IS_TOP_LEVEL)
  message(
      AUTHOR_WARNING
      "Developer mode is intended for developers of events"
  )
endif()

include(cmake/dev-mode.cmake)
