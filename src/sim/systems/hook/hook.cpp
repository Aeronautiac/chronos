#include "hook.hpp"
#include "sim/systems/movement/movement.hpp"
#include "sim/world.hpp"
#include <cmath>

namespace sim {

namespace {

// a returning hook counts as back once it's this close to its owner
constexpr float HOOK_CATCH_DIST = 12;
// the hook's own collider
constexpr float HOOK_SIZE = 4;

float length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

// Do a box of size `a` at `pa` and a box of size `b` at `pb` (both centered)
// overlap?
bool boxes_overlap(Vec2 pa, Collider a, Vec2 pb, Collider b) noexcept {
  return std::abs(pa.x - pb.x) < (a.w + b.w) * 0.5f &&
         std::abs(pa.y - pb.y) < (a.h + b.h) * 0.5f;
}

// Takes the hook's pull drive off its owner, if it has one.
void stop_pulling(PhysicsModifierTable &modifiers, Hook &hook) noexcept {
  if (modifiers.valid_key(hook.pull)) {
    (void)modifiers.erase(hook.pull);
  }
  hook.pull = ModifierID{};
}

void start_returning(PhysicsModifierTable &modifiers, Hook &hook,
                     Physics &p) noexcept {
  stop_pulling(modifiers, hook);
  hook.state = HookState::Returning;
  hook.stuck_to = EntityID{};
  // passes through terrain on the way back
  p.flags = 0;
}

// Sticks the hook where it is, `to` a creature or (zeroed key) terrain, and
// starts pulling its owner with a force along the rope.
void attach(PhysicsModifierTable &modifiers, Hook &hook, Physics &p,
            EntityID to) noexcept {
  hook.state = HookState::Attached;
  hook.stuck_to = to;
  p.raw_vel = {0, 0};
  p.flags = 0;
  PhysicsModifier pull{};
  pull.vector_val = {0, 0};
  pull.target = hook.owner;
  pull.kind = Force;
  // no room for the force: stuck, but doesn't pull
  if (auto key = modifiers.insert(pull)) {
    hook.pull = *key;
  }
}

// The first creature other than `owner` that a hook at `pos` touches, or a
// zeroed key.
EntityID creature_hit(const World &world, Vec2 pos, EntityID owner) noexcept {
  const CreatureTable &c = world.creatures;
  const Collider hook_box{.w = HOOK_SIZE, .h = HOOK_SIZE};
  for (uint32_t i = 0; i < c.len; i++) {
    if (c.id[i] == owner) {
      continue;
    }
    if (boxes_overlap(pos, hook_box, c.transform[i].pos,
                      c.physics[i].collider)) {
      return c.id[i];
    }
  }
  return EntityID{};
}

// A fresh press of the hook button, with no hook out, fires one along the
// aim.
void fire_hooks(World &world) noexcept {
  CreatureTable &c = world.creatures;
  for (uint32_t i = 0; i < c.len; i++) {
    Movement &m = c.movement[i];
    const MovementIntents pressed =
        static_cast<MovementIntents>(m.intents & ~m.prev_intents);
    if (!has_intent(pressed, MovementIntent::Hook)) {
      continue;
    }
    // only one out at a time; it has to come back first
    if (world.entities.get(m.hook)) {
      continue;
    }
    if (m.aim.x == 0 && m.aim.y == 0) {
      continue;
    }
    auto id =
        spawn_hook(world, Transform{.pos = c.transform[i].pos, .rot = 0});
    if (!id) {
      // no room: no hook
      continue;
    }
    // spawning a hook doesn't touch the creature table, so `m` and `i` are
    // still good
    HookTable &h = world.hooks;
    const uint32_t row = *hook_row(world, *id);
    h.physics[row].collider = {.w = HOOK_SIZE, .h = HOOK_SIZE};
    h.physics[row].flags = static_cast<PhysicsFlags>(PhysicsFlag::Collides);
    h.physics[row].raw_vel = m.aim * m.params.hook_speed;
    h.hook[row] = Hook{
        .owner = c.id[i],
        .state = HookState::Flying,
        .pull = ModifierID{},
        .stuck_to = EntityID{},
    };
    m.hook = *id;
  }
}

} // namespace

void run_hooks(World &world) noexcept {
  CreatureTable &c = world.creatures;
  // set again below for every owner whose hook is pulling this tick
  for (uint32_t i = 0; i < c.len; i++) {
    c.movement[i].hook_pulling = false;
  }

  fire_hooks(world);

  HookTable &h = world.hooks;
  // Newest row first: despawning moves the last row into the gap, and the
  // last row has already been handled, so nothing gets skipped.
  for (uint32_t r = h.len; r-- > 0;) {
    Hook &hook = h.hook[r];
    Physics &p = h.physics[r];
    Vec2 &pos = h.transform[r].pos;

    const auto owner_row = creature_row(world, hook.owner);
    if (!owner_row) {
      // owner's gone, so is the hook
      stop_pulling(world.physics_modifiers, hook);
      (void)despawn(world, h.id[r]);
      continue;
    }
    Movement &m = c.movement[*owner_row];
    const MovementParams &mp = m.params;
    const Vec2 owner_pos = c.transform[*owner_row].pos;
    const bool held = has_intent(m.intents, MovementIntent::Hook);

    switch (hook.state) {
    case HookState::Flying: {
      const EntityID hit = creature_hit(world, pos, hook.owner);
      if (!held || length(pos - owner_pos) > mp.hook_max_length) {
        start_returning(world.physics_modifiers, hook, p);
      } else if (hit != EntityID{}) {
        attach(world.physics_modifiers, hook, p, hit);
      } else if (p.collided) {
        attach(world.physics_modifiers, hook, p, EntityID{});
      }
      break;
    }

    case HookState::Attached: {
      if (!held) {
        start_returning(world.physics_modifiers, hook, p);
        break;
      }
      // stuck in a creature: ride along with it
      if (hook.stuck_to != EntityID{}) {
        const auto target_row = creature_row(world, hook.stuck_to);
        if (!target_row) {
          // it's gone
          start_returning(world.physics_modifiers, hook, p);
          break;
        }
        pos = c.transform[*target_row].pos;
      }
      // An elastic rope: a force toward the hook that grows with distance.
      // Weak up close, so the owner flies straight through; stronger the
      // further past they get, slowing them and pulling them back. Momentum
      // across the rope is left alone, so they swing.
      if (PhysicsModifier *pull =
              world.physics_modifiers.get_mut(hook.pull)) {
        pull->vector_val = (pos - owner_pos) * mp.hook_stiffness;
        m.hook_pulling = true;
      }
      break;
    }

    case HookState::Returning: {
      const Vec2 to_owner = owner_pos - pos;
      const float dist = length(to_owner);
      if (dist <= HOOK_CATCH_DIST) {
        // back: free to fire again
        m.hook = EntityID{};
        (void)despawn(world, h.id[r]);
        break;
      }
      p.raw_vel = to_owner * (mp.hook_return_speed / dist);
      break;
    }
    }
  }
}

} // namespace sim
