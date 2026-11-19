#pragma once
#include "sim/sim.hpp"

namespace sim {

// The ODM gear (Odm on a creature), driven by the creature's Movement
// intents and aim, tuned by its MovementParams:
//
// Hook: a fresh press (MovementIntent::Hook) fires one along the aim, and
// it pulls its owner in once it sticks to terrain or a creature. A hook is
// a projectile entity: Owner (the creature), Hook, Transform, Physics. See
// Hook / HookState for the states.
//
// Gas: a fresh burst press, with a charge, is an impulse toward the aim.
// Charges come back one at a time, anywhere.
//
// Creates and destroys hooks (after its loops) and adds/removes their pull
// forces. Run first: before movement, which reads this tick's
// Odm::flags HookPulling, and before physics, which applies the pull. Reads
// last tick's collisions.
void run_odm(World &world, TimeFloat dt) noexcept;

} // namespace sim
