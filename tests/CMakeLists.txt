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

# Only hard guarantees and core primitives are tested, never gameplay.
add_executable(sim_tests slotmap_test.cpp arena_test.cpp fsm_test.cpp
  det_math_test.cpp spatial_test.cpp guarantees_test.cpp)
target_link_libraries(sim_tests PRIVATE sim small_game_warnings GTest::gtest_main)
# The sim is built with -fno-rtti, so EnTT objects it creates (its storages'
# shared_ptr control blocks) have no type info. UBSan's vptr check, which
# needs type info, then flags them when the tests (built with RTTI) touch
# them. Harmless when sanitizers are off.
target_compile_options(sim_tests PRIVATE -fno-sanitize=vptr)
gtest_discover_tests(sim_tests)

# Deterministic floats: no libm calls whose rounding varies by platform.
add_test(NAME SimDoesntCallLibm
  COMMAND ${CMAKE_COMMAND}
    "-DSCAN_DIRS=${PROJECT_SOURCE_DIR}/src/sim;${PROJECT_SOURCE_DIR}/src/util"
    -P ${CMAKE_CURRENT_SOURCE_DIR}/no_libm_in_sim.cmake)
