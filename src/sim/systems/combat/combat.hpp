#pragma once
#include "sim/sim.hpp"

namespace sim {

// health creatures spawn with
inline constexpr float CREATURE_HEALTH = 100;

// Slashes: the Slash input starts one toward the aim, or the facing
// direction with no aim. While it lasts, a box reaching out that way hits
// each other creature it touches once, for slash_damage (see
// MovementParams). Creatures don't collide with each other, so a slash at
// speed goes straight through. Creatures at 0 health are destroyed, after
// the loops.

// The combat action the Slash input dispatches to: starts a slash, or
// returns why it can't (changing nothing). Needs Combat and Movement, and
// not on cooldown. Not called during a tick.
SimResult slash(entt::registry &r, entt::entity e, Vec2 aim) noexcept;

// Every tick: slashes in progress hit, timers count down, the dead go. Run
// after the ODM system and before movement.
void run_combat(World &world, TimeFloat dt) noexcept;

} // namespace sim
