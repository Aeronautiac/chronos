#include "odm.hpp"
#include "sim/spatial.hpp"
#include "sim/systems/movement/movement.hpp"
#include <cassert>
#include <cmath>

namespace sim {

namespace {

// a returning hook counts as back once it's this close to its owner
constexpr float HOOK_CATCH_DIST = 12;
// the hook's own collider
constexpr float HOOK_SIZE = 4;

float length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

MovementIntents pressed_intents(const Movement &m) noexcept {
  return static_cast<MovementIntents>(m.intents & ~m.prev_intents);
}

//====== HOOK ======//

// The pull force is its own entity (ForceModifier). Creating and destroying
// it is fine inside the hook loop: that storage isn't one the loop
// iterates.

// Takes the hook's pull force off its owner, if it has one (physics may
// already have destroyed it, if the owner lost its Physics).
void stop_pulling(entt::registry &r, Hook &hook) noexcept {
  if (r.valid(hook.pull)) {
    r.destroy(hook.pull);
  }
  hook.pull = entt::null;
}

void start_returning(entt::registry &r, Hook &hook, Physics &p) noexcept {
  stop_pulling(r, hook);
  hook.state = HookState::Returning;
  hook.stuck_to = entt::null;
  // passes through terrain on the way back
  p.flags = {};
}

// Sticks the hook where it is, `to` a creature or (null) terrain, and starts
// pulling `owner` with a force along the rope.
void attach(entt::registry &r, Hook &hook, Physics &p, entt::entity owner,
            entt::entity to) noexcept {
  hook.state = HookState::Attached;
  hook.stuck_to = to;
  p.raw_vel = {0, 0};
  p.flags = {};
  hook.pull = r.create();
  r.emplace<ForceModifier>(hook.pull,
                           ForceModifier{.target = owner, .force = {0, 0}});
}

// A hook to create, queued while iterating Odm.
struct FireHook {
  entt::entity owner;
  Vec2 pos;
  Vec2 vel;
};

// A fresh press of the hook button, with no hook out, fires one along the
// aim.
void fire_hooks(World &world) noexcept {
  entt::registry &r = world.reg;
  ArenaScope scope{world.scratch};
  // at most one per Odm
  auto fire_mem = world.scratch.vec<FireHook>(
      static_cast<uint32_t>(r.storage<Odm>().size()));
  assert(fire_mem);
  ArenaVec<FireHook> fire = *fire_mem;

  auto odms = r.view<const Odm, const Movement, const Transform>();
  for (auto [e, odm, m, t] : odms.each()) {
    if (!has_intent(pressed_intents(m), MovementIntent::Hook)) {
      continue;
    }
    // only one out at a time; it has to come back first
    if (r.valid(odm.hook)) {
      continue;
    }
    if (m.aim.x == 0 && m.aim.y == 0) {
      continue;
    }
    (void)fire.push(FireHook{
        .owner = e,
        .pos = t.pos,
        .vel = m.aim * m.params.hook_speed,
    });
  }

  // after the loop: emplacing Transform while iterating a view over it
  // isn't allowed
  for (const FireHook &f : fire) {
    const entt::entity hook = r.create();
    r.emplace<Owner>(hook, Owner{.entity = f.owner});
    r.emplace<Transform>(hook, Transform{.pos = f.pos, .rot = 0});
    Physics p{};
    p.raw_vel = f.vel;
    p.collider = {.w = HOOK_SIZE, .h = HOOK_SIZE};
    p.flags.mask = bits(PhysicsFlag::Collides);
    r.emplace<Physics>(hook, p);
    r.emplace<Hook>(hook, Hook{
                              .state = HookState::Flying,
                              .pull = entt::null,
                              .stuck_to = entt::null,
                          });
    r.get<Odm>(f.owner).hook = hook;
  }
}

// Every hook out in the world: sticks, pulls, comes back.
void update_hooks(World &world) noexcept {
  entt::registry &r = world.reg;

  ArenaScope scope{world.scratch};
  // at most every hook goes away
  auto dead_mem = world.scratch.vec<entt::entity>(
      static_cast<uint32_t>(r.storage<Hook>().size()));
  assert(dead_mem);
  ArenaVec<entt::entity> dead = *dead_mem;

  auto hooks = r.view<Hook, const Owner, Physics, Transform>();
  for (auto [self, hook, owner, p, t] : hooks.each()) {
    Vec2 &pos = t.pos;
    Odm *odm = r.try_get<Odm>(owner.entity);
    const Movement *m = r.try_get<Movement>(owner.entity);
    const Transform *owner_t = r.try_get<Transform>(owner.entity);
    if (!odm || !m || !owner_t) {
      // owner's gone, so is the hook
      stop_pulling(r, hook);
      (void)dead.push(self);
      continue;
    }
    const MovementParams &mp = m->params;
    const Vec2 owner_pos = owner_t->pos;
    const bool held = has_intent(m->intents, MovementIntent::Hook);

    switch (hook.state) {
    case HookState::Flying: {
      // creatures (things with Combat) other than the owner
      const entt::entity hit = first_overlapping<Combat>(
          r, collider_box(pos, p.collider), owner.entity);
      if (!held || length(pos - owner_pos) > mp.hook_max_length) {
        start_returning(r, hook, p);
      } else if (hit != entt::null) {
        attach(r, hook, p, owner.entity, hit);
      } else if (p.contacts.has(Contact::Collided)) {
        attach(r, hook, p, owner.entity, entt::null);
      }
      break;
    }

    case HookState::Attached: {
      if (!held) {
        start_returning(r, hook, p);
        break;
      }
      // stuck in a creature: ride along with it
      if (hook.stuck_to != entt::null) {
        const Transform *target = r.try_get<Transform>(hook.stuck_to);
        if (!target) {
          // it's gone
          start_returning(r, hook, p);
          break;
        }
        pos = target->pos;
      }
      // An elastic rope: a force toward the hook that grows with distance.
      // Weak up close, so the owner flies straight through; stronger the
      // further past they get, slowing them and pulling them back. Momentum
      // across the rope is left alone, so they swing.
      if (ForceModifier *pull = r.try_get<ForceModifier>(hook.pull)) {
        pull->force = (pos - owner_pos) * mp.hook_stiffness;
        odm->flags.set(OdmFlag::HookPulling, true);
      }
      break;
    }

    case HookState::Returning: {
      const Vec2 to_owner = owner_pos - pos;
      const float dist = length(to_owner);
      if (dist <= HOOK_CATCH_DIST) {
        // back: free to fire again
        odm->hook = entt::null;
        (void)dead.push(self);
        break;
      }
      p.raw_vel = to_owner * (mp.hook_return_speed / dist);
      break;
    }
    }
  }

  // after the loop: never destroy into a view being iterated
  for (entt::entity e : dead) {
    r.destroy(e);
  }
}

//====== GAS ======//

void run_gas(entt::registry &r, TimeFloat dt) noexcept {
  for (auto [e, odm, m, p] : r.view<Odm, const Movement, Physics>().each()) {
    const MovementParams &mp = m.params;
    // charges come back one at a time, anywhere
    if (odm.burst_charges < static_cast<uint8_t>(mp.burst_charges)) {
      odm.burst_regen_left -= dt;
      if (odm.burst_regen_left <= 0) {
        odm.burst_charges += 1;
        odm.burst_regen_left = mp.burst_regen_time;
      }
    } else {
      odm.burst_regen_left = mp.burst_regen_time;
    }

    // a press, with a charge, is an impulse toward the aim
    if (has_intent(pressed_intents(m), MovementIntent::Burst) &&
        odm.burst_charges > 0 && (m.aim.x != 0 || m.aim.y != 0)) {
      p.raw_vel += m.aim * mp.burst_speed;
      odm.burst_charges -= 1;
    }
  }
}

} // namespace

void run_odm(World &world, TimeFloat dt) noexcept {
  // set again by update_hooks for every ODM whose hook is pulling this tick
  for (auto [e, odm] : world.reg.view<Odm>().each()) {
    odm.flags.set(OdmFlag::HookPulling, false);
  }
  fire_hooks(world);
  update_hooks(world);
  run_gas(world.reg, dt);
}

} // namespace sim
