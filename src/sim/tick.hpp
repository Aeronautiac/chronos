#pragma once
#include "sim/sim.hpp"

namespace sim {

// Wiring: each function runs a system over every kind that has the
// components the system needs. Systems themselves only see arrays and know
// nothing about kinds. When a kind gains a component, add its arrays here.

void tick_movement(World &world, TimeFloat dt) noexcept;
void tick_physics(World &world, TimeFloat dt) noexcept;
void tick_perception(World &world, TimeFloat dt) noexcept;

} // namespace sim
