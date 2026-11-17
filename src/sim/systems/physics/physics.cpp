#include "physics.hpp"
#include "sim/world.hpp"
#include "util/scalar.hpp"
#include <cassert>
#include <cmath>

namespace sim {

namespace {

// Overlap tests treat boxes as this much smaller on every side. A box flush
// against a wall then can't count as overlapping it because of float
// rounding. Has to stay well above one ulp at the largest world coordinate
// (~0.001 at 12288).
constexpr float SKIN = 0.01f;

enum class Axis { X, Y };

// number of cells along an axis
int32_t cell_count(Axis axis) noexcept {
  return axis == Axis::X ? Grid::W : Grid::H;
}

// Grid lookup by (index along the moving axis, index across it).
bool solid(const Grid &grid, Axis axis, int32_t along, int32_t across) noexcept {
  return axis == Axis::X ? grid.solid(along, across)
                         : grid.solid(across, along);
}

// Moves a box by `d` along one axis, stopping flush against the first solid
// cell its leading edge would enter. Returns true if it was stopped.
//
//   center, half         the box on the moving axis. `center` is updated.
//   across_lo, across_hi the box's edges on the other axis, so we know which
//                        cells to check at each step.
//
// The x sweep is the 1D problem "move center x, checking rows y_lo..y_hi";
// the y sweep is the same with the axes swapped.
//
// Cells already overlapped at the start are ignored, so an entity spawned
// inside terrain can still move out.
bool sweep(const Grid &grid, Axis axis, float &center, float half,
           float across_lo, float across_hi, float d) noexcept {
  const int32_t n = cell_count(axis);
  const int32_t n_across = cell_count(axis == Axis::X ? Axis::Y : Axis::X);

  // cells the box covers on the other axis
  const int32_t a0 = cell_coord(across_lo + SKIN, n_across);
  const int32_t a1 = cell_coord(across_hi - SKIN, n_across);
  auto blocked = [&](int32_t i) {
    for (int32_t a = a0; a <= a1; a++) {
      if (solid(grid, axis, i, a)) {
        return true;
      }
    }
    return false;
  };

  // `from` is the last cell the box already covers (skinned, so a box flush
  // against a wall counts as outside it). `to` is the cell the real leading
  // edge ends up in, unskinned, so even a tiny push into a wall registers as
  // a hit instead of sinking in. `to` is clamped to [-1, n], which is solid,
  // so these loops always terminate.
  if (d > 0) {
    const int32_t from = cell_coord(center + half - SKIN, n);
    const int32_t to = cell_coord(center + half + d, n);
    for (int32_t i = from + 1; i <= to; i++) {
      if (blocked(i)) {
        // leading edge flush with the cell's near side
        center = static_cast<float>(i) * Grid::CELL_SIZE - half;
        return true;
      }
    }
  } else if (d < 0) {
    const int32_t from = cell_coord(center - half + SKIN, n);
    const int32_t to = cell_coord(center - half + d, n);
    for (int32_t i = from - 1; i >= to; i--) {
      if (blocked(i)) {
        center = static_cast<float>(i + 1) * Grid::CELL_SIZE + half;
        return true;
      }
    }
  }
  center += d;
  return false;
}

// Called after a horizontal move was stopped. If the obstacle is a ledge no
// taller than `step`, climbs it: lifts the box by `step`, does the rest of
// the move (`dx_left`) up there, then drops back down by `step` onto
// whatever is below. Returns true if the move is still blocked; `pos` is
// only changed if the box got further.
bool climb_ledge(const Grid &grid, Vec2 &pos, float hw, float hh,
                 float dx_left, float step) noexcept {
  Vec2 trial = pos;
  // a low ceiling can cut the lift short, that's fine
  (void)sweep(grid, Axis::Y, trial.y, hh, trial.x - hw, trial.x + hw, -step);
  const float before = trial.x;
  const bool blocked =
      sweep(grid, Axis::X, trial.x, hw, trial.y - hh, trial.y + hh, dx_left);
  if (std::abs(trial.x - before) <= SKIN) {
    // too tall to climb, or no room above it
    return true;
  }
  (void)sweep(grid, Axis::Y, trial.y, hh, trial.x - hw, trial.x + hw, step);
  pos = trial;
  return blocked;
}

// Start of tick: clear this tick's velocity and drives, apply gravity.
void begin_body(Physics &p, TimeFloat dt) noexcept {
  p.vel = {};
  p.ang_vel = 0;
  p.drives = DriveSum{};
  if (has_flag(p.flags, PhysicsFlag::Gravity)) {
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
    v = target + (v - target) * std::exp(-damping * dt);
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

  const bool was_on_ground = p.on_ground;
  p.on_ground = false;
  p.wall_dir = 0;
  p.collided = false;

  if (!has_flag(p.flags, PhysicsFlag::Collides)) {
    t.pos += d;
    return;
  }

  Vec2 &pos = t.pos;
  const float hw = p.collider.w * 0.5f;
  const float hh = p.collider.h * 0.5f;
  const bool can_step = was_on_ground && p.step_height > 0;

  // horizontal: move x across the rows the box spans
  const float start_x = pos.x;
  bool hit_x = sweep(grid, Axis::X, pos.x, hw, pos.y - hh, pos.y + hh, d.x);
  if (hit_x && can_step) {
    const float dx_left = d.x - (pos.x - start_x);
    hit_x = climb_ledge(grid, pos, hw, hh, dx_left, p.step_height);
  }
  if (hit_x) {
    p.collided = true;
    // It's only a wall if it's solid level with the box's middle, not just a
    // ridge down by its feet. The column just past the leading edge (the
    // box is flush against it, SKIN steps over the boundary).
    const int32_t col = d.x > 0 ? cell_coord(pos.x + hw + SKIN, Grid::W)
                                : cell_coord(pos.x - hw - SKIN, Grid::W);
    const int32_t mid_row = cell_coord(pos.y, Grid::H);
    if (grid.solid(col, mid_row)) {
      p.wall_dir = d.x > 0 ? 1.0f : -1.0f;
    }
    p.vel.x = 0;
    p.raw_vel.x = 0;
  }

  // vertical: move y across the columns the box spans
  if (sweep(grid, Axis::Y, pos.y, hh, pos.x - hw, pos.x + hw, d.y)) {
    // +y is down, so a blocked downward move means we're standing on
    // something. Resting entities still get pushed down by gravity every
    // tick, so this stays true while standing still.
    p.on_ground = d.y > 0;
    p.collided = true;
    p.vel.y = 0;
    p.raw_vel.y = 0;
  } else if (can_step && d.y >= 0) {
    // Walked off a small drop (e.g. going down a slope). Follow the ground
    // down instead of falling off every step. Only if there's ground within
    // step_height; otherwise it's a real ledge and we fall.
    float y = pos.y;
    if (sweep(grid, Axis::Y, y, hh, pos.x - hw, pos.x + hw, p.step_height)) {
      pos.y = y;
      p.on_ground = true;
      p.vel.y = 0;
      p.raw_vel.y = 0;
    }
  }
}

} // namespace

void begin_bodies(std::span<Physics> bodies, TimeFloat dt) noexcept {
  for (Physics &p : bodies) {
    begin_body(p, dt);
  }
}

void apply_physics_modifiers(World &world, TimeFloat dt) noexcept {
  // dense order is a pure function of insert/erase history, so float sums are
  // deterministic across replays
  for (auto [key, mod] : world.physics_modifiers) {
    Physics *p = physics_of(world, mod.target);
    if (!p) {
      // target is gone, or has no physics. don't touch `mod` after this,
      // erase moves another value into it
      auto _ = world.physics_modifiers.erase(key);
      continue;
    }
    switch (mod.kind) {
    case Velocity:
      p->vel += mod.vector_val;
      break;
    case Force:
      p->raw_vel += mod.vector_val * dt;
      break;
    case AngularVelocity:
      p->ang_vel += mod.scalar_val;
      break;
    case AngularForce:
      p->raw_ang_vel += mod.scalar_val * dt;
      break;
    case Drive:
      // only summed here; applied in integrate_bodies, after every force,
      // so the result doesn't depend on modifier order
      add_drive(p->drives, mod.drive);
      break;
    }
  }
}

void integrate_bodies(const Grid &grid, std::span<Transform> transforms,
                      std::span<Physics> bodies, TimeFloat dt) noexcept {
  assert(transforms.size() == bodies.size());
  for (size_t i = 0; i < bodies.size(); i++) {
    integrate_body(grid, transforms[i], bodies[i], dt);
  }
}

} // namespace sim
