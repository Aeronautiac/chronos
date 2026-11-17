#include "combat.hpp"
#include "sim/systems/movement/movement.hpp"
#include "sim/world.hpp"
#include <algorithm>
#include <cmath>

namespace sim {

namespace {

// Do a box of size `a` at `pa` and a box of size `b` at `pb` (both centered)
// overlap?
bool boxes_overlap(Vec2 pa, Collider a, Vec2 pb, Collider b) noexcept {
  return std::abs(pa.x - pb.x) < (a.w + b.w) * 0.5f &&
         std::abs(pa.y - pb.y) < (a.h + b.h) * 0.5f;
}

bool already_hit(const Combat &cb, EntityID id) noexcept {
  for (uint8_t k = 0; k < cb.slash_hit_count; k++) {
    if (cb.slash_hits[k] == id) {
      return true;
    }
  }
  return false;
}

} // namespace

void run_combat(World &world, TimeFloat dt) noexcept {
  CreatureTable &c = world.creatures;

  // timers, and starting slashes
  for (uint32_t i = 0; i < c.len; i++) {
    Combat &cb = c.combat[i];
    const Movement &m = c.movement[i];
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
  for (uint32_t i = 0; i < c.len; i++) {
    Combat &cb = c.combat[i];
    if (cb.slash_left <= 0) {
      continue;
    }
    const MovementParams &mp = c.movement[i].params;
    const Vec2 center =
        c.transform[i].pos + cb.slash_dir * (mp.slash_reach * 0.5f);
    const Collider box{.w = mp.slash_reach, .h = mp.slash_reach};
    for (uint32_t j = 0; j < c.len; j++) {
      if (j == i || already_hit(cb, c.id[j])) {
        continue;
      }
      if (!boxes_overlap(center, box, c.transform[j].pos,
                         c.physics[j].collider)) {
        continue;
      }
      // past the hit list's size, targets can be hit again; plenty for now
      if (cb.slash_hit_count < cb.slash_hits.size()) {
        cb.slash_hits[cb.slash_hit_count] = c.id[j];
        cb.slash_hit_count += 1;
      }
      c.combat[j].health -= mp.slash_damage;
      c.combat[j].since_hit = 0;
    }
  }

  // the dead. Newest row first: despawning moves the last row into the gap,
  // and that row has already been checked.
  for (uint32_t r = c.len; r-- > 0;) {
    if (c.combat[r].health <= 0) {
      (void)despawn(world, c.id[r]);
    }
  }
}

} // namespace sim
