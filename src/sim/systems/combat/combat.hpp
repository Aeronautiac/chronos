#pragma once
#include "sim/sim.hpp"

namespace sim {

// health creatures spawn with
inline constexpr float CREATURE_HEALTH = 100;

// Slashes: a fresh attack press (not on cooldown) starts one toward the aim,
// or the facing direction with no aim. While it lasts, a box reaching out
// that way hits each other creature it touches once, for slash_damage (see
// MovementParams). Creatures don't collide with each other, so a slash at
// speed goes straight through. Creatures at 0 health are despawned.
//
// Despawns, so it takes the World. Run before movement (it reads presses
// against last tick's intents) and after hooks.
void run_combat(World &world, TimeFloat dt) noexcept;

} // namespace sim
