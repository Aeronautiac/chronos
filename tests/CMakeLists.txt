# Uses an installed GoogleTest if one is found, otherwise downloads it.
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE
  SYSTEM
  FIND_PACKAGE_ARGS NAMES GTest)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

add_executable(sim_tests slotmap_test.cpp)
target_link_libraries(sim_tests PRIVATE sim small_game_warnings GTest::gtest_main)
gtest_discover_tests(sim_tests)
