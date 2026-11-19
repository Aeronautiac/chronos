#pragma once
#include "sim/sim.hpp"
#include <expected>

namespace sim {

// Adds a creature at `pos`: Transform, Physics (12 x 24, collides, falls,
// climbs two-cell steps), Movement with its drive modifier, Combat at full
// health, Perception 1, and an Odm with a full tank.
entt::entity spawn_creature(World &world, Vec2 pos) noexcept;

} // namespace sim
