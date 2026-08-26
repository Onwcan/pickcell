include(FetchContent)

FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.17.0
  GIT_SHALLOW    TRUE
  EXCLUDE_FROM_ALL)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(pickcell_tests test_cell.cpp)
target_link_libraries(pickcell_tests PRIVATE pickcell::cell GTest::gtest_main)
gtest_discover_tests(pickcell_tests PROPERTIES LABELS integration DISCOVERY_TIMEOUT 30)
