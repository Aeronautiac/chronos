#pragma once
#include "sim/sim.hpp"

namespace sim {

// Hooks: creatures fire them along their aim (MovementIntent::Hook) and they
// pull their owner in once they stick to terrain. See MovementParams for the
// tunables and Hook / HookState for the states.
//
// Spawns and despawns hooks and adds/removes their pull forces, so it takes
// the World. Run before movement and physics: it reads last tick's
// collisions, and its pull force is applied by this tick's physics.
void run_hooks(World &world) noexcept;

} // namespace sim
