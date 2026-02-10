include(CMakeFindDependencyMacro)

find_path(PLF_COLONY_INCLUDE_DIRS "plf_colony.h")

include("${CMAKE_CURRENT_LIST_DIR}/eventsTargets.cmake")

target_include_directories(events::events INTERFACE ${PLF_COLONY_INCLUDE_DIRS})
