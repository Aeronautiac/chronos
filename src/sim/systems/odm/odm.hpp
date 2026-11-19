#pragma once
#include "sim/sim.hpp"

namespace sim {

// The ODM gear (Odm on a creature), tuned by the creature's MovementParams:
//
// Hook: fired along the aim (fire_hook), it pulls its owner in once it
// sticks to terrain or a creature, until let go (release_hook) and it comes
// back. A hook is a projectile entity: Owner (the creature), Hook,
// Transform, Physics. See Hook / HookState for the states.
//
// Gas: a burst (gas_burst) uses a charge for an impulse toward the aim.
// Charges come back one at a time, anywhere.

// The ODM's actions, which the GasBurst, FireHook and ReleaseHook inputs
// dispatch to. Each checks it can happen right now and either does it or
// returns why it can't, changing nothing. Not called during a tick.

// An impulse along `aim`, using a charge. Needs Odm, Movement and Physics.
SimResult gas_burst(entt::registry &r, entt::entity e, Vec2 aim) noexcept;
// Fires a hook along `aim` from the creature; returns the hook. Needs Odm,
// Movement and Transform, and no hook out.
SimResult fire_hook(entt::registry &r, entt::entity e, Vec2 aim) noexcept;
// Sends the creature's hook back. Needs a hook out that isn't already
// coming back.
SimResult release_hook(entt::registry &r, entt::entity e) noexcept;

// Every tick: moves hooks along (sticking, pulling, coming back) and
// regenerates gas. Destroys hooks (after its loops) and adds/removes their
// pull forces. Run first: before movement, which reads this tick's
// Odm::flags HookPulling, and before physics, which applies the pull. Reads
// last tick's collisions.
void run_odm(World &world, TimeFloat dt) noexcept;

} // namespace sim
