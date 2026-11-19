#include "movement.hpp"
#include "sim/spatial.hpp"
#include "util/det_math.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <numbers>

namespace sim {

namespace {

// Turns a movement state on or off and returns whether it's on afterwards.
// Turning on fails if one of its blocking states is held. This system is the
// only one holding these states, so turning off can just cancel.
bool set_state(MoveFsm &fsm, MoveState s, bool on) noexcept {
  if (on && !fsm.held(s)) {
    (void)fsm.acquire(s);
  }
  if (!on && fsm.held(s)) {
    fsm.cancel(s);
  }
  return fsm.held(s);
}

// Turns on a state that turns itself off after `seconds` (see tick_timers).
// Fails, like acquire, if a blocking state is held.
bool start_timed(Movement &m, MoveState s, float seconds) noexcept {
  if (!m.state.acquire(s)) {
    return false;
  }
  m.timers[flag_index(s)] = seconds;
  return true;
}

// Counts down timed states and ends the ones whose time is up.
void tick_timers(Movement &m, float dt) noexcept {
  for (uint32_t i = 0; i < m.timers.size(); i++) {
    if (m.timers[i] <= 0) {
      continue;
    }
    const auto s = static_cast<MoveState>(1u << i);
    if (!m.state.held(s)) {
      // ended some other way (cancelled), drop its timer
      m.timers[i] = 0;
      continue;
    }
    m.timers[i] -= dt;
    if (m.timers[i] <= 0) {
      m.timers[i] = 0;
      m.state.cancel(s);
    }
  }
}

// y of the top of the ground in the column under world x, searched from 2
// cells above `feet_y` to 8 cells below. Bottom of that range if no ground.
float ground_y(const Grid &grid, float x, float feet_y) noexcept {
  const Vec2 from{x, feet_y - 2 * Grid::CELL_SIZE};
  const float max_drop = 10 * Grid::CELL_SIZE;
  return surface_below(grid, from, max_drop).value_or(from.y + max_drop);
}

// How much the ground drops per unit moved right, under a box with its feet
// at `feet_y`, sampled `sample` left and right of x. Positive means downhill
// to the right (+y is down).
float ground_slope(const Grid &grid, float x, float feet_y,
                   float sample) noexcept {
  const float left = ground_y(grid, x - sample, feet_y);
  const float right = ground_y(grid, x + sample, feet_y);
  return (right - left) / (2 * sample);
}

// On the ground: the body cast straight down finds ground within `reach`,
// and it isn't moving up (just after a jump it's still within reach of the
// ground it left).
bool probe_ground(const Grid &grid, Vec2 pos, Collider c, float raw_vel_y,
                  float reach) noexcept {
  if (raw_vel_y < 0) {
    return false;
  }
  const auto hit = boxcast_grid(grid, collider_box(pos, c), {0, 1}, reach);
  // a cast starting inside terrain hits with no normal: not ground
  return hit && hit->normal.y != 0;
}

constexpr float NO_WALL = std::numeric_limits<float>::infinity();

// How far the nearest wall is on each side, or NO_WALL if there's none
// within `reach`.
struct WallDists {
  float left;
  float right;
};

// Casts the middle half of the body sideways, so a ridge down by the feet
// (or a ledge overhead) isn't a wall. A cast starting inside terrain hits
// with no normal, which isn't a wall either.
WallDists probe_walls(const Grid &grid, Vec2 pos, Collider c,
                      float reach) noexcept {
  Box middle = collider_box(pos, c);
  middle.half.y *= 0.5f;
  auto dist = [&](Vec2 dir) {
    const auto hit = boxcast_grid(grid, middle, dir, reach);
    return hit && hit->normal.x != 0 ? hit->dist : NO_WALL;
  };
  return {.left = dist({-1, 0}), .right = dist({1, 0})};
}

// -1 or 1: the side of the nearer wall, `prefer` if they're equally near.
// 0 if there's no wall on either side.
float nearest_wall(WallDists w, float prefer) noexcept {
  if (w.left == NO_WALL && w.right == NO_WALL) {
    return 0;
  }
  if (w.left < w.right) {
    return -1;
  }
  if (w.right < w.left) {
    return 1;
  }
  return prefer;
}

// The wall jump's direction off a wall on side `wall`: the aim, clamped to
// a cone pointing straight away from the wall, `cone_deg` either side.
// Outside the cone it's the nearest edge: the upper one when aiming up (or
// straight into the wall), the lower one when aiming down. Away and up at
// 45 degrees with no aim.
Vec2 wall_jump_dir(Vec2 aim, float wall, float cone_deg) noexcept {
  const float away = -wall;
  if (aim.x == 0 && aim.y == 0) {
    return Vec2{away, -1} * 0.70710678f;
  }
  const float cone = cone_deg * (std::numbers::pi_v<float> / 180);
  const float cos_cone = det::cos(cone);
  // aim is a unit vector, so this is the cosine of its angle from straight
  // away
  if (aim.x * away >= cos_cone) {
    return aim;
  }
  // -y is up
  const float edge_y = aim.y > 0 ? det::sin(cone) : -det::sin(cone);
  return {away * cos_cone, edge_y};
}

} // namespace

void add_movement(entt::registry &reg, entt::entity e) noexcept {
  const entt::entity drive = reg.create();
  reg.emplace<DriveModifier>(drive,
                             DriveModifier{.target = e, .drive = DriveParams{}});

  Movement m{};
  m.drive = drive;
  m.facing = 1;
  reg.emplace<Movement>(e, m);
}

void run_movement(World &world, TimeFloat dt) noexcept {
  const Grid &grid = *world.grid;
  auto view = world.reg.view<Movement, Physics, const Transform>();
  for (auto [e, m, p, t] : view.each()) {
    const MovementParams &mp = m.params;
    MoveFsm &state = m.state;

    const MovementIntents held = m.intents;
    const MovementIntents pressed =
        static_cast<MovementIntents>(held & ~m.prev_intents);
    m.prev_intents = held;

    const float dir =
        static_cast<float>(has_intent(held, MovementIntent::Right)) -
        static_cast<float>(has_intent(held, MovementIntent::Left));
    if (dir != 0) {
      m.facing = dir;
    }

    // cooldowns run out
    tick_timers(m, dt);
    const bool slid_last_tick = state.held(MoveState::Sliding);

    // movement's own ground check (not physics' contacts). Cleared below
    // when jumping, so the rest of this tick steers as airborne.
    bool grounded =
        probe_ground(grid, t.pos, p.collider, p.raw_vel.y, mp.ground_reach);

    set_state(state, MoveState::Sprinting,
              has_intent(held, MovementIntent::Sprint) && dir != 0);

    // sliding: a ctrl press arms it, and it starts the first moment it can
    // (on the ground, moving, not on cooldown) as long as ctrl is still
    // held, so it can be pressed early, in the air or during the cooldown.
    // Starting uses up the arm. Keeps going while held, on the ground, and
    // faster than walking.
    if (has_intent(pressed, MovementIntent::Slide)) {
      m.flags.set(MoveFlag::SlideArmed, true);
    }
    if (!has_intent(held, MovementIntent::Slide)) {
      m.flags.set(MoveFlag::SlideArmed, false);
    }
    if (state.held(MoveState::Sliding)) {
      set_state(state, MoveState::Sliding,
                has_intent(held, MovementIntent::Slide) && grounded &&
                    std::abs(p.raw_vel.x) > mp.walk_speed);
    } else if (m.flags.has(MoveFlag::SlideArmed) && grounded &&
               std::abs(p.raw_vel.x) >= mp.slide_start_speed &&
               state.acquire(MoveState::Sliding)) {
      const float travel_dir = p.raw_vel.x > 0 ? 1.0f : -1.0f;
      p.raw_vel.x += travel_dir * mp.slide_boost;
      m.flags.set(MoveFlag::SlideArmed, false);
    }

    // walls: only in the air. Cling to one you're holding toward and (all
    // but) touching; wall jump off the nearest one in reach.
    const WallDists walls =
        !grounded ? probe_walls(grid, t.pos, p.collider,
                               std::max(mp.wall_cling_reach,
                                        mp.wall_jump_reach))
                 : WallDists{.left = NO_WALL, .right = NO_WALL};
    const float held_wall_dist =
        dir > 0 ? walls.right : (dir < 0 ? walls.left : NO_WALL);
    set_state(state, MoveState::WallCling,
              held_wall_dist <= mp.wall_cling_reach);
    const WallDists jumpable{
        .left = walls.left <= mp.wall_jump_reach ? walls.left : NO_WALL,
        .right = walls.right <= mp.wall_jump_reach ? walls.right : NO_WALL,
    };
    const float wall = nearest_wall(jumpable, dir != 0 ? dir : m.facing);

    // input forgiveness timers (see MovementParams)
    const bool jump_held = has_intent(held, MovementIntent::Jump);
    const bool jump_pressed = has_intent(pressed, MovementIntent::Jump);
    m.coyote_left =
        grounded ? mp.coyote_time : std::max(0.0f, m.coyote_left - dt);
    m.jump_buffer_left = jump_pressed
                             ? mp.jump_buffer_time
                             : std::max(0.0f, m.jump_buffer_left - dt);
    // A wall in reach keeps the grace window open, except during the
    // cooldown right after a wall jump: the wall just jumped off is still in
    // reach for a moment, and a second press then would stack another jump
    // on top.
    if (wall != 0 && !state.held(MoveState::WallJumpCooldown)) {
      m.wall_grace_left = mp.wall_jump_grace;
      m.last_wall_dir = wall;
    } else {
      m.wall_grace_left = std::max(0.0f, m.wall_grace_left - dt);
    }

    // Ground and slide jumps fire while space is held (so holding it jumps
    // again on landing) or from a buffered press, on the ground or within
    // coyote time. Wall jumps need a (buffered) press with a wall in reach
    // or within the grace window after one, and not on cooldown (which every
    // jump starts).
    const bool can_ground_jump = grounded || m.coyote_left > 0;
    const bool can_wall_jump = m.wall_grace_left > 0 &&
                               !state.held(MoveState::WallJumpCooldown);
    if ((jump_held || m.jump_buffer_left > 0) && can_ground_jump) {
      if (grounded && state.held(MoveState::Sliding)) {
        // slide jump: forward and up, flatter than a normal jump
        const float travel_dir = p.raw_vel.x > 0 ? 1.0f : -1.0f;
        p.raw_vel.x += travel_dir * mp.slide_jump_x;
        p.raw_vel.y -= mp.slide_jump_y;
        state.cancel(MoveState::Sliding);
      } else {
        // -y is up
        p.raw_vel.y -= mp.jump_speed;
      }
      grounded = false;
      m.coyote_left = 0;
      m.jump_buffer_left = 0;
      // no wall jump straight on top of this one (see wall_jump_cooldown)
      start_timed(m, MoveState::WallJumpCooldown, mp.wall_jump_cooldown);
    } else if (m.jump_buffer_left > 0 && can_wall_jump) {
      // aimed, within a cone pushing off the wall (see wall_jump_dir)
      p.raw_vel += wall_jump_dir(m.aim, m.last_wall_dir, mp.wall_jump_cone) *
                   mp.wall_jump_speed;
      state.cancel(MoveState::WallCling);
      start_timed(m, MoveState::WallJumpCooldown, mp.wall_jump_cooldown);
      m.wall_grace_left = 0;
      m.jump_buffer_left = 0;
    }

    // Steering: retune movement's drive for this tick. Physics applies it
    // (and any other drives on the body) after all forces.
    DriveParams &drive = world.reg.get<DriveModifier>(m.drive).drive;
    drive = DriveParams{};

    // horizontal
    // set this tick by the ODM system, which runs first
    const Odm *odm = world.reg.try_get<Odm>(e);
    const bool hook_pulling = odm && odm->flags.has(OdmFlag::HookPulling);
    if (hook_pulling) {
      // being pulled in by the hook's force: don't steer against it
    } else if (state.held(MoveState::Sliding)) {
      // no steering: slopes push (a force, as a per-tick impulse), friction
      // slows
      const float feet_y = t.pos.y + p.collider.h * 0.5f;
      const float slope =
          ground_slope(grid, t.pos.x, feet_y, mp.slope_sample);
      p.raw_vel.x += mp.slide_slope_accel * slope * dt;
      drive.target.x = 0;
      drive.accel.x = mp.slide_friction;
    } else if (dir != 0) {
      const float target =
          state.held(MoveState::Sprinting) ? mp.sprint_speed : mp.walk_speed;
      drive.target.x = dir * target;
      if (p.raw_vel.x * dir < 0) {
        // moving the other way: turn around, slower than speeding up
        drive.accel.x = grounded ? mp.turn_accel : mp.air_turn_accel;
      } else if (p.raw_vel.x * dir < target) {
        // slower than the target in the held direction: speed up
        drive.accel.x = grounded ? mp.walk_accel : mp.air_accel;
      } else {
        // already faster that way: ease back, keeping the momentum
        drive.damping.x = mp.overspeed_damping;
      }
    } else {
      drive.target.x = 0;
      if (grounded) {
        drive.accel.x = mp.ground_friction;
      } else {
        drive.damping.x = mp.air_damping;
      }
    }

    // vertical: friction against a wall eases vertical speed toward 0, up or
    // down (gravity still wins slowly)
    if (state.held(MoveState::WallCling) && !hook_pulling) {
      drive.target.y = 0;
      drive.damping.y = mp.wall_friction;
    }

    // a slide ended this tick, however it ended (released, too slow, jumped
    // out)
    if (slid_last_tick && !state.held(MoveState::Sliding)) {
      start_timed(m, MoveState::SlideCooldown, mp.slide_cooldown);
    }
  }
}

} // namespace sim
