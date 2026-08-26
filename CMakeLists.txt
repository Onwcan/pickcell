cmake_minimum_required(VERSION 3.24)

project(pickcell
  VERSION 0.1.0
  DESCRIPTION "A pick-and-place cell assembled from the other repositories"
  LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
  set(PICKCELL_IS_TOP_LEVEL ON)
else()
  set(PICKCELL_IS_TOP_LEVEL OFF)
endif()

option(PICKCELL_BUILD_TESTS "Build tests" ${PICKCELL_IS_TOP_LEVEL})

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

include(FetchContent)

# The dependencies are fetched from their published repositories rather than
# from sibling directories on disk.
#
# That is deliberate and it is a test in itself: it consumes motionkit and
# robot-contracts exactly the way a third party would, so a broken export, a
# missing header or a target name that only exists in-tree fails here rather
# than in someone else's build. motionkit's install/export job caught one such
# bug already; this is the same check from the other side.
#
# Tags rather than branches: an integration demo that silently changes what it
# integrates is not a demonstration of anything.
FetchContent_Declare(motionkit
  GIT_REPOSITORY https://github.com/Onwcan/motionkit.git
  GIT_TAG        main
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)

FetchContent_Declare(robot_contracts
  GIT_REPOSITORY https://github.com/Onwcan/robot-contracts.git
  GIT_TAG        main
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)

FetchContent_Declare(safeedge
  GIT_REPOSITORY https://github.com/Onwcan/safeedge.git
  GIT_TAG        main
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)

# Neither dependency should build its own tests inside our build.
set(MOTIONKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ROBOT_CONTRACTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(motionkit robot_contracts safeedge)

add_subdirectory(src)

if(PICKCELL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
