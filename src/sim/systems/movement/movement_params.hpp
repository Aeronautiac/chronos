#pragma once

namespace sim {

// Every tunable number movement uses, per entity (Movement::params), so
// different creatures can move differently and things can change them at
// runtime. `MovementParams{}` gives the defaults below.
//
// Speeds in units/s, accelerations in units/s^2, damping in 1/s, times in
// seconds. A cell is 4 units.
//
// Movement steers with one Drive modifier it owns (see DriveParams): each
// tick it sets a target velocity and how hard to pull toward it. Anything
// else that drives the body fights it and they settle. Bursts (jumps, slide
// boosts, wall jumps, gas bursts) are impulses: flat additions to raw_vel,
// never sets.
struct MovementParams {
  // walking (A/D): pull toward the target speed while held
  float walk_speed = 90;
  float sprint_speed = 190;
  float walk_accel = 1200;
  float air_accel = 600;
  // holding the opposite direction to how you're moving: turn around at
  // this rate instead, so sudden reversals take a moment
  float turn_accel = 600;
  float air_turn_accel = 300;
  // already faster than the target in the held direction (out of a slide or
  // dodge): ease back toward the target exponentially, keeping momentum
  // with a soft tail
  float overspeed_damping = 2;

  // no direction held: on the ground stop at a fixed rate, in the air drift
  // to a stop exponentially
  float ground_friction = 1500;
  float air_damping = 1;

  // jumping, added upward once
  float jump_speed = 300;

  // input forgiveness, in seconds:
  //   coyote_time      you can still jump this long after leaving the ground
  //   jump_buffer_time a jump press counts for this long, so pressing just
  //                    before landing (or clinging) still jumps
  //   wall_jump_grace  you can still wall jump this long after letting go of
  //                    a wall
  float coyote_time = 0.1f;
  float jump_buffer_time = 0.1f;
  float wall_jump_grace = 0.12f;

  // sliding (press ctrl, hold to keep sliding; it starts as soon as you're
  // on the ground moving at least slide_start_speed, so it can be pressed
  // early, e.g. in the air before landing). A short fast burst: adds
  // slide_boost to your speed, can't steer, slowed by slide_friction, and
  // pushed downhill by slide_slope_accel * slope (slope = ground drop per
  // unit across, sampled slope_sample left and right of center). Ends once
  // it's down to walk_speed (walking takes over from there), when you leave
  // the ground, or on release. Each press starts at most one slide; holding
  // ctrl doesn't start another.
  float slide_start_speed = 40;
  float slide_boost = 200;
  float slide_friction = 400;
  float slide_slope_accel = 1200;
  float slope_sample = 12;
  // slide jump (press jump while sliding): flatter and longer than a normal
  // jump. Adds slide_jump_x in the direction of travel and slide_jump_y up.
  float slide_jump_x = 260;
  float slide_jump_y = 240;

  // wall cling (in the air, holding toward a wall you're touching): you keep
  // your vertical speed, but friction against the wall eases it toward 0
  // (damping, per second). Hit the wall going up and you slide up a way;
  // falling, the fall is braked. Gravity still wins slowly: the slowest
  // slide down is about gravity / wall_friction.
  float wall_friction = 10;
  // wall jump (press jump while clinging): adds wall_jump_speed toward the
  // aim (never into the wall; away and up if there's no aim)
  float wall_jump_speed = 550;

  // gas burst (press burst, anywhere): adds burst_speed toward the aim. Up
  // to burst_charges of
  // them; one comes back every burst_regen_time seconds, anywhere. (A count,
  // kept as a float like everything else here; whole numbers.)
  float burst_speed = 600;
  float burst_charges = 2;
  float burst_regen_time = 2.5f;

  // hook (press to fire toward the aim, hold to keep it out). Flies at
  // hook_speed and sticks to the first terrain or creature it hits (and
  // follows a creature); if it gets hook_max_length away first, it comes
  // back. Stuck, it's an elastic rope: a force toward the hook of
  // hook_stiffness * distance (units/s^2 per unit away). Weak up close, so
  // you fly straight through; stronger the further past you get. Speed
  // across the rope is untouched, so momentum carries and you swing.
  // Hanging still, you settle about gravity / hook_stiffness below it.
  // Movement doesn't steer while hooked. Let go and it flies back at
  // hook_return_speed through terrain; it can only be fired again once
  // it's back.
  float hook_speed = 900;
  float hook_max_length = 300;
  float hook_stiffness = 13;
  float hook_return_speed = 1200;

  // slash (left click, toward the aim). For slash_time a slash_reach box
  // reaching out in front hits each creature it touches once for
  // slash_damage. Then can't slash again for slash_cooldown. (Here until
  // combat has its own params.)
  float slash_time = 0.15f;
  float slash_cooldown = 0.25f;
  float slash_reach = 28;
  float slash_damage = 25;

  // cooldowns after moves
  float slide_cooldown = 0.2f;     // after a slide ends
  float wall_jump_cooldown = 0.1f; // no cling until it's up

  // time perception (seconds perceived per world second): at least
  // perception_base, and above perception_speed it rises with speed,
  // perception_base * speed / perception_speed, so going twice that fast
  // perceives twice as much. Eases toward that at perception_ease per
  // perceived second. (Here until it has its own params.)
  float perception_base = 1;
  float perception_speed = 400;
  float perception_ease = 6;
};

} // namespace sim
