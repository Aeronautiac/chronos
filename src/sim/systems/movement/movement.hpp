#pragma once
#include "sim/sim.hpp"

namespace sim {

// Tunables live per entity in Movement::params (movement_params.hpp).

constexpr bool has_intent(MovementIntents set, MovementIntent i) noexcept {
  return (set & static_cast<MovementIntents>(i)) != 0;
}

// Adds Movement to a freshly spawned creature, and its drive modifier (an
// entity with DriveModifier). Not while iterating a view over Movement or
// DriveModifier.
void add_movement(entt::registry &reg, entt::entity e) noexcept;

// Runs over entities with Movement, Physics and Transform. Run after the
// ODM system (reads this tick's Odm hook_pulling) and before physics.
// Finds ground and walls itself (box casts), retunes each entity's drive,
// and adds impulses to raw_vel. Gas bursts are the ODM's.
void run_movement(World &world, TimeFloat dt) noexcept;

} // namespace sim
