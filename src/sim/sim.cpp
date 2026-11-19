#include "sim.hpp"
#include "snapshot.hpp"
#include "systems/combat/combat.hpp"
#include "systems/movement/movement.hpp"
#include "systems/odm/odm.hpp"
#include "systems/perception/perception.hpp"
#include "systems/physics/physics.hpp"
#include "util/overloaded.hpp"
#include "world.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <expected>
#include <variant>

namespace sim {

TimeInt Sim::tick_dur() const {
  const entt::registry &r = world_.reg;
  // max is the same in any order, so view order doesn't matter here
  float max_speed = 0;
  for (auto [e, p] : r.view<const Physics>().each()) {
    max_speed =
        std::max(max_speed, std::sqrt(p.vel.x * p.vel.x + p.vel.y * p.vel.y));
  }
  float max_perception = 1;
  for (auto [e, pc] : r.view<const Perception>().each()) {
    max_perception = std::max(max_perception, pc.value);
  }
  // still: nothing moving (bodies at rest have exactly zero velocity) and
  // no one holding anything
  bool active = max_speed > 0;
  for (auto [e, m] : r.view<const Movement>().each()) {
    active = active || m.intents != 0;
  }

  const TimeInt base = active ? BASE_TICK : IDLE_TICK;
  double secs = static_cast<double>(base) / 1e9 / max_perception;
  if (max_speed > 0) {
    secs = std::min(secs, static_cast<double>(TICK_MAX_TRAVEL / max_speed));
  }
  const auto ns = static_cast<TimeInt>(secs * 1e9);
  return std::max(ns, MIN_TICK);
}

// TODO: LookAt isn't implemented yet (always rejected)
SimResult Sim::apply(SimInput input) {
  return std::visit(
      overloaded{
          [this](SetMap set_map) -> SimResult {
            if (!set_map.map) {
              return std::unexpected(SimErr::InvalidMap);
            }
            world_.grid = set_map.map;
            return std::monostate{};
          },
          [this](SpawnCreature spawn) -> SimResult {
            return spawn_creature(world_, spawn.pos);
          },
          [this](SetMovementIntents set_mvmt_intent) -> SimResult {
            auto *m = world_.reg.try_get<Movement>(set_mvmt_intent.target);
            if (!m) {
              return std::unexpected(SimErr::InvalidEntity);
            }
            m->intents = set_mvmt_intent.intents;
            m->aim = set_mvmt_intent.aim;
            return std::monostate{};
          },
          [](LookAt) -> SimResult {
            return std::unexpected(SimErr::InvalidEntity);
          },
          [this](SetMovementParams set_params) -> SimResult {
            auto *m = world_.reg.try_get<Movement>(set_params.target);
            if (!m) {
              return std::unexpected(SimErr::InvalidEntity);
            }
            m->params = set_params.params;
            return std::monostate{};
          },
      },
      input);
}

void Sim::do_tick(TimeFloat dt) {
  assert(world_.grid && "send SetMap before ticking");
  run_odm(world_, dt);
  run_combat(world_, dt);
  run_movement(world_, dt);
  run_physics(world_, dt);
  run_perception(world_.reg, dt);
}

void Sim::advance(TimeInt dt) {
  // as many ticks as fit, each as long as tick_dur says at the time
  TimeInt remaining = world_.t_excess + dt;
  TimeInt step = tick_dur();
  while (remaining >= step) {
    do_tick(static_cast<float>(static_cast<double>(step) /
                               static_cast<double>(1e9)));
    remaining -= step;
    step = tick_dur();
  }
  world_.t_excess = remaining;
}

SimResult Sim::exec(SimInput input, TimeInt dt) {
  SimResult res = apply(input);
  advance(dt);
  return res;
}

void Sim::restore(const Snapshot &snap) { restore_world(world_, snap); }

Snapshot Sim::save() const { return save_world(world_); }

} // namespace sim
