#include "sim.hpp"
#include "overloaded.hpp"
#include <expected>
#include <variant>

namespace sim {

// fixed at 240hz for now
TimeInt Sim::tick_dur() { return static_cast<TimeInt>(1e9 / 240); }

// TODO:
// finish this
SimResult Sim::apply_input(SimInput input) {
  return std::visit(
      overloaded{
          [this](SpawnEntity spawn_entity) -> SimResult {
            Entity entity{};
            entity.kind = spawn_entity.kind;
            auto key = world.entities.insert(entity);
            if (!key)
              return std::unexpected(SimErr::EntityLimit);
            return *key;
          },
          [this](SetMovementIntents set_mvmt_intent) -> SimResult {
            Entity *entity = world.entities.get_mut(set_mvmt_intent.target);
            if (!entity)
              return std::unexpected(SimErr::InvalidEntity);
            entity->movement_intents = set_mvmt_intent.intents;
            return std::monostate{};
          },
          [](LookAt look_at) -> SimResult {
            return std::unexpected(SimErr::InvalidEntity);
          },
      },
      input);
}

// TODO:
// physics updates and stuff
void Sim::do_tick(TimeFloat dt) {}

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
