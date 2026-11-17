#pragma once
#include "sim/sim.hpp"
#include <span>

namespace sim {

// units/s^2, +y is down
inline constexpr Vec2 GRAVITY{0, 800};

// A physics tick is three steps, in this order, each run over every kind
// that has the components (wired up in tick.cpp):
//
// 1. begin_bodies: clear this tick's velocity, apply gravity.
void begin_bodies(std::span<Physics> bodies, TimeFloat dt) noexcept;

// 2. apply_physics_modifiers: each modifier adds to its target's velocity,
//    or for drives, into its target's drive sum. Targets are found by key,
//    so this step takes the World. Modifiers whose target is gone (or has
//    no Physics) are erased.
void apply_physics_modifiers(World &world, TimeFloat dt) noexcept;

// 3. integrate_bodies: pull raw_vel toward the summed drives (see DriveSum),
//    add the lasting velocity and move. Bodies with
//    PhysicsFlag::Collides are swept against the grid one axis at a time (x
//    then y) and stop flush against the first solid cell, however far they'd
//    move in one tick. Hitting something zeroes vel and raw_vel on that axis
//    and sets on_ground / wall_dir. Bodies that were on the ground climb
//    ledges and follow drops up to their step_height.
//    transforms[i] and bodies[i] are the same entity.
void integrate_bodies(const Grid &grid, std::span<Transform> transforms,
                      std::span<Physics> bodies, TimeFloat dt) noexcept;

} // namespace sim
