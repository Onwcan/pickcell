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
# Pinned to exact commit revisions, not to a branch.
#
# This comment used to say "tags rather than branches" while all three
# declarations said `GIT_TAG main`, which is the same defect the rest of this
# repository argues against: the claim and the behaviour disagreed, and nothing
# failed. A pickcell commit therefore did not identify what pickcell was built
# from -- the same SHA rebuilt a week later could fetch a different motionkit and
# a different stopping law with it.
#
# That matters most for docs/external-validation, where a frozen prediction is
# handed to an outside party against a commit SHA. If the dependencies float,
# the SHA identifies the arithmetic and not the library that performed it.
#
# GIT_SHALLOW is deliberately NOT set on these three. A shallow fetch of an
# arbitrary commit works against GitHub and was verified to work here, but CMake
# documents it as depending on server support and it has moved between CMake
# versions. For the one thing in this repository that has to resolve identically
# on someone else's machine, a full clone of three small repositories is the
# cheaper trade -- protobuf, fetched below through robot-contracts, dominates
# this build regardless.
#
# These values are the single source of truth: they are passed to the code as
# compile definitions further down, so the revision the validation package
# reports cannot drift from the revision actually fetched.
set(PICKCELL_MOTIONKIT_REV  874fcc74d68143a44e4a74c2787aeae7957c7c0b)
set(PICKCELL_CONTRACTS_REV  92461cfdb018a5a184c6ba7aeb21c36ef176b3a3)
set(PICKCELL_SAFEEDGE_REV   9ccd098d80bad237f29b0a43f107cfe92ca7e06f)

FetchContent_Declare(motionkit
  GIT_REPOSITORY https://github.com/Onwcan/motionkit.git
  GIT_TAG        ${PICKCELL_MOTIONKIT_REV}
  EXCLUDE_FROM_ALL)

FetchContent_Declare(robot_contracts
  GIT_REPOSITORY https://github.com/Onwcan/robot-contracts.git
  GIT_TAG        ${PICKCELL_CONTRACTS_REV}
  EXCLUDE_FROM_ALL)

FetchContent_Declare(safeedge
  GIT_REPOSITORY https://github.com/Onwcan/safeedge.git
  GIT_TAG        ${PICKCELL_SAFEEDGE_REV}
  EXCLUDE_FROM_ALL)

# Neither dependency should build its own tests inside our build.
set(MOTIONKIT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ROBOT_CONTRACTS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(SAFEEDGE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(motionkit robot_contracts safeedge)

# --- third-party headers are not this repository's code ----------------------
#
# The libraries here are held to -Werror with -Wconversion, -Wsign-conversion
# and -Wpedantic, and this repository should not hold itself to less: an
# integration layer with warnings switched off is where a narrowing conversion
# reaches a safety verdict unnoticed.
#
# The obstacle is that FetchContent hands a dependency's headers over as
# ordinary include directories, so those flags fire inside protobuf and abseil
# -- hundreds of diagnostics about __int128 and abseil's own sign conversions,
# none of them about anything written here. CMake 3.25 added SYSTEM to
# FetchContent_Declare for exactly this; the minimum here is 3.24, and raising
# it to silence a warning is the wrong way round.
#
# So the include directories are re-marked afterwards. Recursive, because
# protobuf brings abseil as a nested subdirectory and marking only the top level
# leaves most of the headers behind.
function(pickcell_mark_includes_system directory)
  get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "INTERFACE_LIBRARY" OR type STREQUAL "UTILITY")
      # An INTERFACE library still carries include directories worth moving; a
      # UTILITY target carries none and querying it is an error.
      if(type STREQUAL "UTILITY")
        continue()
      endif()
    endif()
    get_target_property(dirs ${target} INTERFACE_INCLUDE_DIRECTORIES)
    if(dirs)
      set_target_properties(${target} PROPERTIES
        INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dirs}")
    endif()
  endforeach()

  get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
  foreach(child IN LISTS children)
    pickcell_mark_includes_system("${child}")
  endforeach()
endfunction()

if(protobuf_SOURCE_DIR)
  pickcell_mark_includes_system("${protobuf_SOURCE_DIR}")
endif()

add_subdirectory(src)

if(PICKCELL_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
