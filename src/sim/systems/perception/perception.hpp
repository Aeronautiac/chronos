#pragma once
#include "sim/sim.hpp"

namespace sim {

// Runs over entities with Perception, Physics and Movement. Eases each
// one's perception toward what its speed calls for (see MovementParams'
// perception fields), in the entity's own perceived time: world dt *
// perception.
void run_perception(entt::registry &reg, TimeFloat dt) noexcept;

} // namespace sim
