cmake_minimum_required(VERSION 3.25)
project(small_game LANGUAGES CXX)

# 26 for structured binding packs (fields_equal in sim/sim_state.hpp)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SMALL_GAME_BUILD_TESTS "Build the test suite" ${PROJECT_IS_TOP_LEVEL})

include(FetchContent)

# Warning set shared by every target we own. Warnings-as-errors is not
# hardcoded here; it's the built-in CMAKE_COMPILE_WARNING_AS_ERROR, which the
# "ci" preset turns on.
add_library(small_game_warnings INTERFACE)
target_compile_options(small_game_warnings INTERFACE
  -Wall -Wextra -Wconversion -Wsign-conversion -Wswitch-enum)

# Language restrictions for core code (see style.md). Tests don't use these
# so they can use GoogleTest normally.
add_library(small_game_core INTERFACE)
target_compile_options(small_game_core INTERFACE -fno-exceptions -fno-rtti)

#====== SIM ======#

# Header-only ECS. Pinned to an exact commit: exact restore depends on how
# its snapshots and storages behave (see PORT_TO_ENTT.md), so updating it
# means re-checking that.
FetchContent_Declare(EnTT
  GIT_REPOSITORY https://github.com/skypjack/entt.git
  GIT_TAG 1dcd7aab6548786f7672ff578639188bd2f0d837
  SYSTEM)
FetchContent_MakeAvailable(EnTT)

# Every .cpp under src except main.cpp, which belongs to the game executable.
# CONFIGURE_DEPENDS re-checks the glob on each build, so new files are picked
# up without re-running cmake by hand.
file(GLOB_RECURSE SIM_SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM SIM_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

add_library(sim STATIC ${SIM_SOURCES})
target_include_directories(sim PUBLIC src)
target_link_libraries(sim PUBLIC EnTT::EnTT)
target_link_libraries(sim PRIVATE small_game_warnings small_game_core)
# PUBLIC so sim headers instantiated in other targets get the same
# floating point behavior.
target_compile_options(sim PUBLIC -ffp-contract=off)

#====== GAME ======#

# Uses an installed raylib if one is found, otherwise downloads it.
FetchContent_Declare(raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG 6.2
  GIT_SHALLOW TRUE
  SYSTEM
  FIND_PACKAGE_ARGS)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

add_executable(game src/main.cpp)
target_link_libraries(game PRIVATE sim raylib small_game_warnings small_game_core)

#====== TESTS ======#

if(SMALL_GAME_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
