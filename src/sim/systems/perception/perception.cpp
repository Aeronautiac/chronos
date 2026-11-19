#include "perception.hpp"
#include "util/det_math.hpp"
#include <algorithm>
#include <cmath>

namespace sim {

void run_perception(entt::registry &reg, TimeFloat dt) noexcept {
  auto view = reg.view<Perception, const Physics, const Movement>();
  for (auto [e, pc, body, m] : view.each()) {
    const MovementParams &mp = m.params;
    const Vec2 v = body.vel;
    const float speed = std::sqrt(v.x * v.x + v.y * v.y);
    const float target =
        mp.perception_base * std::max(1.0f, speed / mp.perception_speed);

    // eased in the entity's own time, so it adapts at the same felt rate
    // however slow the world is going for it
    const float felt_dt = dt * pc.value;
    pc.value += (target - pc.value) * (1 - det::exp(-mp.perception_ease * felt_dt));
  }
}

} // namespace sim
