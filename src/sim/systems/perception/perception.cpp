#include "perception.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>

namespace sim {

void run_perception(std::span<Perception> perceptions,
                    std::span<const Physics> bodies,
                    std::span<const Movement> movements,
                    TimeFloat dt) noexcept {
  assert(perceptions.size() == bodies.size());
  assert(perceptions.size() == movements.size());
  for (size_t i = 0; i < perceptions.size(); i++) {
    const MovementParams &mp = movements[i].params;
    const Vec2 v = bodies[i].vel;
    const float speed = std::sqrt(v.x * v.x + v.y * v.y);
    const float target =
        mp.perception_base * std::max(1.0f, speed / mp.perception_speed);

    float &value = perceptions[i].value;
    // eased in the entity's own time, so it adapts at the same felt rate
    // however slow the world is going for it
    const float felt_dt = dt * value;
    value += (target - value) * (1 - std::exp(-mp.perception_ease * felt_dt));
  }
}

} // namespace sim
