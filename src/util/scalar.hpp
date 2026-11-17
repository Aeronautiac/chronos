#pragma once
#include <algorithm>

// Moves v toward target by at most max_delta, without overshooting.
constexpr float move_toward(float v, float target, float max_delta) noexcept {
  if (v < target) {
    return std::min(v + max_delta, target);
  }
  return std::max(v - max_delta, target);
}
