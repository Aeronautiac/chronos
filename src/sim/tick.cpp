#include "tick.hpp"
#include "systems/movement/movement.hpp"
#include "systems/perception/perception.hpp"
#include "systems/physics/physics.hpp"

namespace sim {

void tick_movement(World &world, TimeFloat dt) noexcept {
  // kinds with Movement, Physics and Transform
  CreatureTable &c = world.creatures;
  run_movement(world.grid, world.physics_modifiers,
               std::span(c.movement.data(), c.len),
               std::span(c.physics.data(), c.len),
               std::span<const Transform>(c.transform.data(), c.len), dt);
}

void tick_physics(World &world, TimeFloat dt) noexcept {
  CreatureTable &c = world.creatures;
  HookTable &h = world.hooks;

  // kinds with Physics
  begin_bodies(std::span(c.physics.data(), c.len), dt);
  begin_bodies(std::span(h.physics.data(), h.len), dt);

  apply_physics_modifiers(world, dt);

  // kinds with Physics and Transform
  integrate_bodies(world.grid, std::span(c.transform.data(), c.len),
                   std::span(c.physics.data(), c.len), dt);
  integrate_bodies(world.grid, std::span(h.transform.data(), h.len),
                   std::span(h.physics.data(), h.len), dt);
}

void tick_perception(World &world, TimeFloat dt) noexcept {
  // kinds with Perception, Physics and Movement
  CreatureTable &c = world.creatures;
  run_perception(std::span(c.perception.data(), c.len),
                 std::span<const Physics>(c.physics.data(), c.len),
                 std::span<const Movement>(c.movement.data(), c.len), dt);
}

} // namespace sim
