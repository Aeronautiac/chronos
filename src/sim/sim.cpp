#include "sim.hpp"
#include "systems/movement/movement.hpp"
#include "systems/combat/combat.hpp"
#include "systems/hook/hook.hpp"
#include "tick.hpp"
#include "util/overloaded.hpp"
#include "world.hpp"
#include <algorithm>
#include <cmath>
#include <expected>
#include <variant>

namespace sim {

TimeInt Sim::tick_dur() {
  float max_speed = 0;
  float max_perception = 1;
  const CreatureTable &c = world.creatures;
  for (uint32_t i = 0; i < c.len; i++) {
    const Vec2 v = c.physics[i].vel;
    max_speed = std::max(max_speed, std::sqrt(v.x * v.x + v.y * v.y));
    max_perception = std::max(max_perception, c.perception[i].value);
  }
  const HookTable &h = world.hooks;
  for (uint32_t i = 0; i < h.len; i++) {
    const Vec2 v = h.physics[i].vel;
    max_speed = std::max(max_speed, std::sqrt(v.x * v.x + v.y * v.y));
  }

  double secs = static_cast<double>(BASE_TICK) / 1e9 / max_perception;
  if (max_speed > 0) {
    secs = std::min(secs, static_cast<double>(TICK_MAX_TRAVEL / max_speed));
  }
  const auto ns = static_cast<TimeInt>(secs * 1e9);
  return std::max(ns, MIN_TICK);
}

// TODO:
// finish this
SimResult Sim::apply_input(SimInput input) {
  return std::visit(
      overloaded{
          [this](SpawnEntity spawn_entity) -> SimResult {
            switch (spawn_entity.kind) {
            case EntityKind::Creature: {
              auto id = spawn_creature(
                  world, Transform{.pos = spawn_entity.pos, .rot = 0});
              if (!id)
                return std::unexpected(id.error());
              Physics &p = *physics_of(world, *id);
              p.collider = {.w = 12, .h = 24};
              // two cells: walks up and down the terrain's staircase slopes
              p.step_height = 2 * Grid::CELL_SIZE;
              p.flags = static_cast<PhysicsFlags>(PhysicsFlag::Collides) |
                        static_cast<PhysicsFlags>(PhysicsFlag::Gravity);
              auto mvmt = add_movement(world, *id);
              if (!mvmt) {
                (void)despawn(world, *id);
                return std::unexpected(mvmt.error());
              }
              return *id;
            }
            case EntityKind::Hook:
              // hooks are only fired by their owners
              return std::unexpected(SimErr::InvalidEntity);
            }
            // unreachable, every kind returns above
            return std::unexpected(SimErr::InvalidEntity);
          },
          [this](SetMovementIntents set_mvmt_intent) -> SimResult {
            auto row = creature_row(world, set_mvmt_intent.target);
            if (!row)
              return std::unexpected(SimErr::InvalidEntity);
            world.creatures.movement[*row].intents = set_mvmt_intent.intents;
            world.creatures.movement[*row].aim = set_mvmt_intent.aim;
            return std::monostate{};
          },
          [](LookAt) -> SimResult {
            return std::unexpected(SimErr::InvalidEntity);
          },
          [this](SetMovementParams set_params) -> SimResult {
            auto row = creature_row(world, set_params.target);
            if (!row)
              return std::unexpected(SimErr::InvalidEntity);
            world.creatures.movement[*row].params = set_params.params;
            return std::monostate{};
          },
      },
      input);
}

void Sim::do_tick(TimeFloat dt) {
  run_hooks(world);
  run_combat(world, dt);
  tick_movement(world, dt);
  tick_physics(world, dt);
  tick_perception(world, dt);
}

SimResult Sim::exec(SimInput input, TimeInt dt) {
  // gate input and apply state mutations, if necessary.
  // the simulation will behave differently based on these surface level
  // mutations. immediate ticks are not necessary.
  SimResult res = apply_input(input);
  if (!res)
    return res;

  // advance the simulation as far forward as possible.
  TimeInt remaining = world.t_excess + dt;
  TimeInt step = tick_dur();
  while (remaining >= step) {
    do_tick(static_cast<float>(static_cast<double>(step) /
                               static_cast<double>(1e9)));
    remaining -= step;
    step = tick_dur();
  }
  world.t_excess = remaining;

  return res;
}

} // namespace sim
