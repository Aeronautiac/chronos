#pragma once
#include "sim/sim.hpp"

namespace sim {

// units/s^2, +y is down
inline constexpr Vec2 GRAVITY{0, 800};

// A physics tick is three steps, in this order, over every entity with
// Physics (creatures and projectiles alike):
//
// 1. Begin: clear this tick's velocity, apply gravity.
// 2. Modifiers: each modifier adds to its target's velocity, or for drives,
//    into its target's drive sum. Modifiers whose target is gone (or has no
//    Physics) are destroyed, after the loops.
// 3. Integrate (entities with Transform too): pull raw_vel toward the summed
//    drives (see DriveSum), add the lasting velocity and move. Bodies with
//    PhysicsFlag::Collides are swept against the grid one axis at a time (x
//    then y) and stop flush against the first solid cell, however far
//    they'd move in one tick. Hitting something zeroes vel and raw_vel on
//    that axis and sets contacts. Bodies that were on the ground climb
//    ledges and follow drops up to their step_height.
void run_physics(World &world, TimeFloat dt) noexcept;

} // namespace sim
