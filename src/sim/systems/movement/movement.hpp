#pragma once
#include "sim/entity.hpp"
#include "sim/sim.hpp"
#include <cassert>
#include <expected>
#include <span>

namespace sim {

// Tunables live per entity in Movement::params (movement_params.hpp).

constexpr bool has_intent(MovementIntents set, MovementIntent i) noexcept {
  return (set & static_cast<MovementIntents>(i)) != 0;
}

// Sets up a freshly spawned creature's Movement, including its Drive
// modifier. On failure nothing is left behind.
std::expected<void, SimErr> add_movement(World &world, EntityID id) noexcept;

// Run before physics. Reads on_ground and wall_dir from the previous tick's
// physics, retunes each entity's drive in `modifiers`, and adds impulses to
// raw_vel. Index i of every span is the same entity.
void run_movement(const Grid &grid, PhysicsModifierTable &modifiers,
                  std::span<Movement> movements, std::span<Physics> bodies,
                  std::span<const Transform> transforms,
                  TimeFloat dt) noexcept;

} // namespace sim
