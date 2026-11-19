#include "physics.hpp"
#include "sim/spatial.hpp"
#include "util/det_math.hpp"
#include "util/scalar.hpp"
#include <cassert>
#include <cmath>

namespace sim {

namespace {

// Called after a horizontal move was stopped. If the obstacle is a ledge no
// taller than `step`, climbs it: lifts the box by `step`, does the rest of
// the move (`dx_left`) up there, then drops back down by `step` onto
// whatever is below. Returns true if the move is still blocked; `pos` is
// only changed if the box got further.
bool climb_ledge(const Grid &grid, Vec2 &pos, Vec2 half, float dx_left,
                 float step) noexcept {
  Box trial{.center = pos, .half = half};
  // a low ceiling can cut the lift short, that's fine
  trial.center.y = sweep_box(grid, trial, Axis::Y, -step).center;
  const float before = trial.center.x;
  const Sweep across = sweep_box(grid, trial, Axis::X, dx_left);
  trial.center.x = across.center;
  if (std::abs(trial.center.x - before) <= SKIN) {
    // too tall to climb, or no room above it
    return true;
  }
  trial.center.y = sweep_box(grid, trial, Axis::Y, step).center;
  pos = trial.center;
  return across.hit;
}

// Start of tick: clear this tick's velocity and drives, apply gravity.
void begin_body(Physics &p, TimeFloat dt) noexcept {
  p.vel = {};
  p.ang_vel = 0;
  p.drives = DriveSum{};
  if (p.flags.has(PhysicsFlag::Gravity)) {
    p.raw_vel += GRAVITY * dt;
  }
}

// Adds one drive into a body's sum, per axis.
void add_drive(DriveSum &sum, const DriveParams &d) noexcept {
  sum.accel.x += d.accel.x;
  sum.accel.y += d.accel.y;
  sum.accel_target.x += d.accel.x * d.target.x;
  sum.accel_target.y += d.accel.y * d.target.y;
  sum.damping.x += d.damping.x;
  sum.damping.y += d.damping.y;
  sum.damping_target.x += d.damping.x * d.target.x;
  sum.damping_target.y += d.damping.y * d.target.y;
}

// One axis of applying the summed drives: the linear part moves v toward
// its combined target at its combined rate, then the exponential part eases
// v toward its combined target.
float apply_drive_axis(float v, float accel, float accel_target,
                       float damping, float damping_target,
                       TimeFloat dt) noexcept {
  if (accel > 0) {
    v = move_toward(v, accel_target / accel, accel * dt);
  }
  if (damping > 0) {
    const float target = damping_target / damping;
    v = target + (v - target) * det::exp(-damping * dt);
  }
  return v;
}

// End of tick: pull raw_vel toward the drives, add the lasting velocity,
// then move (through the grid if the body collides).
void integrate_body(const Grid &grid, Transform &t, Physics &p,
                    TimeFloat dt) noexcept {
  const DriveSum &s = p.drives;
  p.raw_vel.x = apply_drive_axis(p.raw_vel.x, s.accel.x, s.accel_target.x,
                                 s.damping.x, s.damping_target.x, dt);
  p.raw_vel.y = apply_drive_axis(p.raw_vel.y, s.accel.y, s.accel_target.y,
                                 s.damping.y, s.damping_target.y, dt);

  p.vel += p.raw_vel;
  p.ang_vel += p.raw_ang_vel;
  const Vec2 d = p.vel * dt;
  t.rot += p.ang_vel * dt;

  const bool was_on_ground = p.contacts.has(Contact::Ground);
  p.contacts = {};

  if (!p.flags.has(PhysicsFlag::Collides)) {
    t.pos += d;
    return;
  }

  Vec2 &pos = t.pos;
  const Vec2 half = collider_box(pos, p.collider).half;
  const bool can_step = was_on_ground && p.step_height > 0;

  // horizontal
  const float start_x = pos.x;
  const Sweep sx = sweep_box(grid, {.center = pos, .half = half}, Axis::X, d.x);
  pos.x = sx.center;
  bool hit_x = sx.hit;
  if (hit_x && can_step) {
    const float dx_left = d.x - (pos.x - start_x);
    hit_x = climb_ledge(grid, pos, half, dx_left, p.step_height);
  }
  if (hit_x) {
    p.contacts.set(Contact::Collided, true);
    p.vel.x = 0;
    p.raw_vel.x = 0;
  }

  // vertical
  const Sweep sy = sweep_box(grid, {.center = pos, .half = half}, Axis::Y, d.y);
  pos.y = sy.center;
  if (sy.hit) {
    // +y is down, so a blocked downward move means we're standing on
    // something. Resting entities still get pushed down by gravity every
    // tick, so this stays true while standing still.
    p.contacts.set(Contact::Ground, d.y > 0);
    p.contacts.set(Contact::Collided, true);
    p.vel.y = 0;
    p.raw_vel.y = 0;
  } else if (can_step && d.y >= 0) {
    // Walked off a small drop (e.g. going down a slope). Follow the ground
    // down instead of falling off every step. Only if there's ground within
    // step_height; otherwise it's a real ledge and we fall.
    const Sweep drop = sweep_box(grid, {.center = pos, .half = half}, Axis::Y,
                                 p.step_height);
    if (drop.hit) {
      pos.y = drop.center;
      p.contacts.set(Contact::Ground, true);
      p.vel.y = 0;
      p.raw_vel.y = 0;
    }
  }
}

// Every modifier, applied to its target, one kind at a time in view order
// (a function of history only, so float sums replay exactly). Modifiers
// whose target is gone or has no Physics are destroyed, after the loops.
void apply_physics_modifiers(World &world, TimeFloat dt) noexcept {
  entt::registry &r = world.reg;
  ArenaScope scope{world.scratch};
  // at most every modifier of every kind goes
  const size_t n = r.storage<VelocityModifier>().size() +
                   r.storage<ForceModifier>().size() +
                   r.storage<AngularVelocityModifier>().size() +
                   r.storage<AngularForceModifier>().size() +
                   r.storage<DriveModifier>().size();
  auto dead_mem = world.scratch.vec<entt::entity>(static_cast<uint32_t>(n));
  assert(dead_mem);
  ArenaVec<entt::entity> dead = *dead_mem;

  // the modifier's target's Physics, or nullptr, queueing the modifier to go
  auto target_of = [&](entt::entity modifier, entt::entity target) {
    Physics *p = r.try_get<Physics>(target);
    if (!p) {
      (void)dead.push(modifier);
    }
    return p;
  };

  for (auto [e, v] : r.view<const VelocityModifier>().each()) {
    if (Physics *p = target_of(e, v.target)) {
      p->vel += v.vel;
    }
  }
  for (auto [e, f] : r.view<const ForceModifier>().each()) {
    if (Physics *p = target_of(e, f.target)) {
      p->raw_vel += f.force * dt;
    }
  }
  for (auto [e, a] : r.view<const AngularVelocityModifier>().each()) {
    if (Physics *p = target_of(e, a.target)) {
      p->ang_vel += a.ang_vel;
    }
  }
  for (auto [e, a] : r.view<const AngularForceModifier>().each()) {
    if (Physics *p = target_of(e, a.target)) {
      p->raw_ang_vel += a.torque * dt;
    }
  }
  for (auto [e, d] : r.view<const DriveModifier>().each()) {
    if (Physics *p = target_of(e, d.target)) {
      // only summed here; applied in integrate_body, after every force, so
      // the result doesn't depend on modifier order
      add_drive(p->drives, d.drive);
    }
  }

  // after the loops: never destroy into a view being iterated. An entity
  // with two modifier kinds is queued twice, so skip it the second time.
  for (const entt::entity e : dead) {
    if (r.valid(e)) {
      r.destroy(e);
    }
  }
}

} // namespace

void run_physics(World &world, TimeFloat dt) noexcept {
  for (auto [e, p] : world.reg.view<Physics>().each()) {
    begin_body(p, dt);
  }
  apply_physics_modifiers(world, dt);
  for (auto [e, t, p] : world.reg.view<Transform, Physics>().each()) {
    integrate_body(*world.grid, t, p, dt);
  }
}

} // namespace sim
