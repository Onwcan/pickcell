add_library(pickcell_cell
  cell/cell.cpp
  cell/safety_distance.cpp
  cell/shared_memory_link.cpp
  cell/http_poll_link.cpp)
add_library(pickcell::cell ALIAS pickcell_cell)

target_include_directories(pickcell_cell PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# The whole point of this repository: three separately-developed libraries,
# consumed here through their published targets.
target_link_libraries(pickcell_cell
  PUBLIC
    motionkit::core
    robot::contracts
    safeedge::ipc
  PRIVATE
    # The same warning bar the libraries below are held to, reusing the
    # interface target motionkit exports rather than restating the flags.
    # An integration repository holding itself to a lower standard than its
    # dependencies is how a narrowing conversion reaches a safety verdict.
    motionkit::warnings)

add_executable(pickcell_reaction apps/reaction_benchmark.cpp)
target_link_libraries(pickcell_reaction
  PRIVATE pickcell::cell safeedge::edge motionkit::warnings)
set_target_properties(pickcell_reaction PROPERTIES OUTPUT_NAME pickcell-reaction)

# No safety link and no network: it converts measured reaction times into
# distances, so it needs the arithmetic and nothing else.
add_executable(pickcell_safety_distance apps/safety_distance_report.cpp)
target_link_libraries(pickcell_safety_distance
  PRIVATE pickcell::cell motionkit::warnings)
set_target_properties(pickcell_safety_distance PROPERTIES
  OUTPUT_NAME pickcell-safety-distance)

add_executable(pickcelld apps/pickcelld.cpp)
target_link_libraries(pickcelld
  PRIVATE pickcell::cell safeedge::edge motionkit::warnings)
