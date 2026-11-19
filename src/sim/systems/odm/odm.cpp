#include "odm.hpp"
#include "sim/spatial.hpp"
#include <cassert>
#include <cmath>

namespace sim {

namespace {

// a returning hook counts as back once it's this close to its owner
constexpr float HOOK_CATCH_DIST = 12;
// the hook's own collider
constexpr float HOOK_SIZE = 4;

float length(Vec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }

bool is_zero(Vec2 v) noexcept { return v.x == 0 && v.y == 0; }

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

    switch (hook.state) {
    case HookState::Flying: {
      // creatures (things with Combat) other than the owner
      const entt::entity hit = first_overlapping<Combat>(
          r, collider_box(pos, p.collider), owner.entity);
      if (length(pos - owner_pos) > mp.hook_max_length) {
        start_returning(r, hook, p);
      } else if (hit != entt::null) {
        attach(r, hook, p, owner.entity, hit);
      } else if (p.contacts.has(Contact::Collided)) {
        attach(r, hook, p, owner.entity, entt::null);
      }
      break;
    }

    case HookState::Attached: {
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

// Charges come back one at a time, anywhere.
void regen_gas(entt::registry &r, TimeFloat dt) noexcept {
  for (auto [e, odm, m] : r.view<Odm, const Movement>().each()) {
    const MovementParams &mp = m.params;
    if (odm.burst_charges < static_cast<uint8_t>(mp.burst_charges)) {
      odm.burst_regen_left -= dt;
      if (odm.burst_regen_left <= 0) {
        odm.burst_charges += 1;
        odm.burst_regen_left = mp.burst_regen_time;
      }
    } else {
      odm.burst_regen_left = mp.burst_regen_time;
    }
  }
}

} // namespace

//====== ACTIONS ======//

SimResult gas_burst(entt::registry &r, entt::entity e, Vec2 aim) noexcept {
  if (!r.valid(e) || !r.all_of<Odm, Movement, Physics>(e)) {
    return std::unexpected(SimErr::InvalidEntity);
  }
  if (is_zero(aim)) {
    return std::unexpected(SimErr::NoAim);
  }
  Odm &odm = r.get<Odm>(e);
  if (odm.burst_charges == 0) {
    return std::unexpected(SimErr::NoGas);
  }
  r.get<Physics>(e).raw_vel += aim * r.get<Movement>(e).params.burst_speed;
  odm.burst_charges -= 1;
  return std::monostate{};
}

SimResult fire_hook(entt::registry &r, entt::entity e, Vec2 aim) noexcept {
  if (!r.valid(e) || !r.all_of<Odm, Movement, Transform>(e)) {
    return std::unexpected(SimErr::InvalidEntity);
  }
  if (is_zero(aim)) {
    return std::unexpected(SimErr::NoAim);
  }
  // only one out at a time; it has to come back first
  if (r.valid(r.get<Odm>(e).hook)) {
    return std::unexpected(SimErr::HookOut);
  }

  // copied out first: emplacing below can move the storages they live in
  const Vec2 pos = r.get<Transform>(e).pos;
  const Vec2 vel = aim * r.get<Movement>(e).params.hook_speed;
  const entt::entity hook = r.create();
  r.emplace<Owner>(hook, Owner{.entity = e});
  r.emplace<Transform>(hook, Transform{.pos = pos, .rot = 0});
  Physics p{};
  p.raw_vel = vel;
  p.collider = {.w = HOOK_SIZE, .h = HOOK_SIZE};
  p.flags.mask = bits(PhysicsFlag::Collides);
  r.emplace<Physics>(hook, p);
  r.emplace<Hook>(hook, Hook{
                            .state = HookState::Flying,
                            .pull = entt::null,
                            .stuck_to = entt::null,
                        });
  r.get<Odm>(e).hook = hook;
  return hook;
}

SimResult release_hook(entt::registry &r, entt::entity e) noexcept {
  if (!r.valid(e) || !r.all_of<Odm>(e)) {
    return std::unexpected(SimErr::InvalidEntity);
  }
  const entt::entity hook = r.get<Odm>(e).hook;
  if (!r.valid(hook) || r.get<Hook>(hook).state == HookState::Returning) {
    return std::unexpected(SimErr::NoHook);
  }
  start_returning(r, r.get<Hook>(hook), r.get<Physics>(hook));
  return std::monostate{};
}

//====== TICK ======//

void run_odm(World &world, TimeFloat dt) noexcept {
  // set again by update_hooks for every ODM whose hook is pulling this tick
  for (auto [e, odm] : world.reg.view<Odm>().each()) {
    odm.flags.set(OdmFlag::HookPulling, false);
  }
  update_hooks(world);
  regen_gas(world.reg, dt);
}

} // namespace sim
