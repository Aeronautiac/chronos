#include "movement.hpp"
#include "sim/sim.hpp"
#include "sim/world.hpp"
#include <algorithm>
#include <cmath>

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
  const int32_t cx = cell_coord(x, Grid::W);
  const int32_t first = cell_coord(feet_y, Grid::H) - 2;
  const int32_t last = first + 10;
  for (int32_t cy = first; cy <= last; cy++) {
    if (grid.solid(cx, cy)) {
      return static_cast<float>(cy) * Grid::CELL_SIZE;
    }
  }
  return static_cast<float>(last + 1) * Grid::CELL_SIZE;
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

} // namespace

std::expected<void, SimErr> add_movement(World &world, EntityID id) noexcept {
  auto row = creature_row(world, id);
  assert(row);

  PhysicsModifier drive{};
  drive.drive = DriveParams{};
  drive.target = id;
  drive.kind = Drive;
  auto drive_key = world.physics_modifiers.insert(drive);
  if (!drive_key) {
    return std::unexpected(SimErr::ModifierLimit);
  }

  Movement &m = world.creatures.movement[*row];
  m.drive = *drive_key;
  m.facing = 1;
  m.burst_charges = static_cast<uint8_t>(m.params.burst_charges);
  m.burst_regen_left = m.params.burst_regen_time;
  return {};
}

void run_movement(const Grid &grid, PhysicsModifierTable &modifiers,
                  std::span<Movement> movements, std::span<Physics> bodies,
                  std::span<const Transform> transforms,
                  TimeFloat dt) noexcept {
  assert(movements.size() == bodies.size());
  assert(movements.size() == transforms.size());
  for (size_t i = 0; i < movements.size(); i++) {
    Movement &m = movements[i];
    Physics &p = bodies[i];
    const Transform &t = transforms[i];
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

    set_state(state, MoveState::Sprinting,
              has_intent(held, MovementIntent::Sprint) && dir != 0);

    // sliding: a ctrl press arms it, and it starts the first moment it can
    // (on the ground, moving, not on cooldown) as long as ctrl is still
    // held, so it can be pressed early, in the air or during the cooldown.
    // Starting uses up the arm. Keeps going while held, on the ground, and
    // faster than walking.
    if (has_intent(pressed, MovementIntent::Slide)) {
      m.slide_armed = true;
    }
    if (!has_intent(held, MovementIntent::Slide)) {
      m.slide_armed = false;
    }
    if (state.held(MoveState::Sliding)) {
      set_state(state, MoveState::Sliding,
                has_intent(held, MovementIntent::Slide) && p.on_ground &&
                    std::abs(p.raw_vel.x) > mp.walk_speed);
    } else if (m.slide_armed && p.on_ground &&
               std::abs(p.raw_vel.x) >= mp.slide_start_speed &&
               state.acquire(MoveState::Sliding)) {
      const float travel_dir = p.raw_vel.x > 0 ? 1.0f : -1.0f;
      p.raw_vel.x += travel_dir * mp.slide_boost;
      m.slide_armed = false;
    }

    set_state(state, MoveState::WallCling,
              !p.on_ground && p.wall_dir != 0 && dir == p.wall_dir);

    // gas bursts come back one at a time, anywhere (used below, on jump)
    if (m.burst_charges < static_cast<uint8_t>(mp.burst_charges)) {
      m.burst_regen_left -= dt;
      if (m.burst_regen_left <= 0) {
        m.burst_charges += 1;
        m.burst_regen_left = mp.burst_regen_time;
      }
    } else {
      m.burst_regen_left = mp.burst_regen_time;
    }

    // input forgiveness timers (see MovementParams)
    const bool jump_held = has_intent(held, MovementIntent::Jump);
    const bool jump_pressed = has_intent(pressed, MovementIntent::Jump);
    m.coyote_left =
        p.on_ground ? mp.coyote_time : std::max(0.0f, m.coyote_left - dt);
    m.jump_buffer_left = jump_pressed
                             ? mp.jump_buffer_time
                             : std::max(0.0f, m.jump_buffer_left - dt);
    if (state.held(MoveState::WallCling)) {
      m.wall_grace_left = mp.wall_jump_grace;
      m.last_wall_dir = p.wall_dir;
    } else {
      m.wall_grace_left = std::max(0.0f, m.wall_grace_left - dt);
    }

    // Ground and slide jumps fire while space is held (so holding it jumps
    // again on landing) or from a buffered press, on the ground or within
    // coyote time. Wall jumps need a (buffered) press, while clinging or
    // within the grace window.
    const bool can_ground_jump = p.on_ground || m.coyote_left > 0;
    const bool can_wall_jump =
        state.held(MoveState::WallCling) || m.wall_grace_left > 0;
    if ((jump_held || m.jump_buffer_left > 0) && can_ground_jump) {
      if (p.on_ground && state.held(MoveState::Sliding)) {
        // slide jump: forward and up, flatter than a normal jump
        const float travel_dir = p.raw_vel.x > 0 ? 1.0f : -1.0f;
        p.raw_vel.x += travel_dir * mp.slide_jump_x;
        p.raw_vel.y -= mp.slide_jump_y;
        state.cancel(MoveState::Sliding);
      } else {
        // -y is up
        p.raw_vel.y -= mp.jump_speed;
      }
      p.on_ground = false;
      m.coyote_left = 0;
      m.jump_buffer_left = 0;
    } else if (m.jump_buffer_left > 0 && can_wall_jump) {
      // aimed: toward the mouse, but never into the wall; away and up if
      // there's no aim
      Vec2 launch = m.aim;
      if (launch.x == 0 && launch.y == 0) {
        launch = Vec2{-m.last_wall_dir, -1} * 0.70710678f;
      }
      if (launch.x * m.last_wall_dir > 0) {
        launch.x = -launch.x;
      }
      p.raw_vel += launch * mp.wall_jump_speed;
      state.cancel(MoveState::WallCling);
      start_timed(m, MoveState::WallJumpCooldown, mp.wall_jump_cooldown);
      m.wall_grace_left = 0;
      m.jump_buffer_left = 0;
    }

    // gas burst: a press, with a charge, is an impulse toward the aim
    if (has_intent(pressed, MovementIntent::Burst) && m.burst_charges > 0 &&
        (m.aim.x != 0 || m.aim.y != 0)) {
      p.raw_vel += m.aim * mp.burst_speed;
      m.burst_charges -= 1;
    }

    // Steering: retune movement's drive for this tick. Physics applies it
    // (and any other drives on the body) after all forces.
    PhysicsModifier *drive_mod = modifiers.get_mut(m.drive);
    assert(drive_mod && drive_mod->kind == Drive);
    DriveParams &drive = drive_mod->drive;
    drive = DriveParams{};

    // horizontal
    if (m.hook_pulling) {
      // being reeled in by the hook's own drive: don't steer against it
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
        drive.accel.x = p.on_ground ? mp.turn_accel : mp.air_turn_accel;
      } else if (p.raw_vel.x * dir < target) {
        // slower than the target in the held direction: speed up
        drive.accel.x = p.on_ground ? mp.walk_accel : mp.air_accel;
      } else {
        // already faster that way: ease back, keeping the momentum
        drive.damping.x = mp.overspeed_damping;
      }
    } else {
      drive.target.x = 0;
      if (p.on_ground) {
        drive.accel.x = mp.ground_friction;
      } else {
        drive.damping.x = mp.air_damping;
      }
    }

    // vertical: friction against a wall eases vertical speed toward 0, up or
    // down (gravity still wins slowly)
    if (state.held(MoveState::WallCling) && !m.hook_pulling) {
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
