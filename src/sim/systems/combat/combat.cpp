#include "combat.hpp"
#include "sim/spatial.hpp"
#include "sim/systems/movement/movement.hpp"
#include <algorithm>
#include <cassert>

namespace sim {

namespace {

bool already_hit(const Combat &cb, entt::entity e) noexcept {
  for (uint8_t k = 0; k < cb.slash_hit_count; k++) {
    if (cb.slash_hits[k] == e) {
      return true;
    }
  }
  return false;
}

} // namespace

void run_combat(World &world, TimeFloat dt) noexcept {
  entt::registry &r = world.reg;

  // timers, and starting slashes
  for (auto [e, cb, m] : r.view<Combat, const Movement>().each()) {
    const MovementParams &mp = m.params;
    cb.slash_left = std::max(0.0f, cb.slash_left - dt);
    cb.slash_cooldown_left = std::max(0.0f, cb.slash_cooldown_left - dt);
    cb.since_hit += dt;

    const MovementIntents pressed =
        static_cast<MovementIntents>(m.intents & ~m.prev_intents);
    if (has_intent(pressed, MovementIntent::Attack) &&
        cb.slash_cooldown_left <= 0) {
      cb.slash_dir = m.aim;
      if (cb.slash_dir.x == 0 && cb.slash_dir.y == 0) {
        cb.slash_dir = {m.facing, 0};
      }
      cb.slash_left = mp.slash_time;
      // counted from the start, so it runs out slash_cooldown after the
      // slash does
      cb.slash_cooldown_left = mp.slash_time + mp.slash_cooldown;
      cb.slash_hit_count = 0;
    }
  }

  // slashes in progress hit whoever they touch, once each
  auto slashers = r.view<Combat, const Transform, const Movement>();
  for (auto [e, cb, t, m] : slashers.each()) {
    if (cb.slash_left <= 0) {
      continue;
    }
    const MovementParams &mp = m.params;
    const float half = mp.slash_reach * 0.5f;
    const Box slash{.center = t.pos + cb.slash_dir * half, .half = {half, half}};
    for_each_overlapping<Combat>(r, slash, [&](entt::entity other) {
      if (other == e || already_hit(cb, other)) {
        return;
      }
      // past the hit list's size, targets can be hit again; plenty for now
      if (cb.slash_hit_count < cb.slash_hits.size()) {
        cb.slash_hits[cb.slash_hit_count] = other;
        cb.slash_hit_count += 1;
      }
      Combat &other_cb = r.get<Combat>(other);
      other_cb.health -= mp.slash_damage;
      other_cb.since_hit = 0;
    });
  }

  // the dead, destroyed after the loop (never while iterating a view over
  // their storages). At most every Combat dies, so `dead` can't fill.
  ArenaScope scope{world.scratch};
  auto dead_mem = world.scratch.vec<entt::entity>(
      static_cast<uint32_t>(r.storage<Combat>().size()));
  assert(dead_mem);
  ArenaVec<entt::entity> dead = *dead_mem;
  for (auto [e, cb] : r.view<const Combat>().each()) {
    if (cb.health <= 0) {
      (void)dead.push(e);
    }
  }
  for (entt::entity e : dead) {
    r.destroy(e);
  }
}

} // namespace sim
