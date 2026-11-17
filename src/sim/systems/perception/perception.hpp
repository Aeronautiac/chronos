#pragma once
#include "sim/sim.hpp"
#include <span>

namespace sim {

// Eases each entity's perception toward what its speed calls for (see
// MovementParams' perception fields), in the entity's own perceived time:
// world dt * perception. Index i of every span is the same entity.
void run_perception(std::span<Perception> perceptions,
                    std::span<const Physics> bodies,
                    std::span<const Movement> movements,
                    TimeFloat dt) noexcept;

} // namespace sim
