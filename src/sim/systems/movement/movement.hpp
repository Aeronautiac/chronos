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

// The SetMovementIntents input: what the entity holds from now on, and its
// aim. Rejected if it has no Movement.
SimResult set_movement_intents(entt::registry &reg, entt::entity e,
                               MovementIntents intents, Vec2 aim) noexcept;

// The SetMovementParams input: replaces the entity's movement tunables.
// Rejected if it has no Movement.
SimResult set_movement_params(entt::registry &reg, entt::entity e,
                              const MovementParams &params) noexcept;

// Runs over entities with Movement, Physics and Transform. Run after the
// ODM system (reads this tick's Odm HookPulling flag) and before physics.
// Finds ground and walls itself (box casts), retunes each entity's drive,
// and adds impulses to raw_vel. Gas bursts are the ODM's.
void run_movement(World &world, TimeFloat dt) noexcept;

} // namespace sim
