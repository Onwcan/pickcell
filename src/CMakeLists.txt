add_library(pickcell_cell
  cell/cell.cpp
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
    safeedge::ipc)

add_executable(pickcell_reaction apps/reaction_benchmark.cpp)
target_link_libraries(pickcell_reaction PRIVATE pickcell::cell safeedge::edge)
set_target_properties(pickcell_reaction PROPERTIES OUTPUT_NAME pickcell-reaction)

add_executable(pickcelld apps/pickcelld.cpp)
target_link_libraries(pickcelld PRIVATE pickcell::cell safeedge::edge)
