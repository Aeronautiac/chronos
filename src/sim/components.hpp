#pragma once
#include "sim/systems/movement/movement_params.hpp"
#include "sim/systems/movement/movement_state.hpp"
#include "util/flags.hpp"
#include "util/plain.hpp"
#include "util/vec2.hpp"
#include <array>
#include <cstdint>
#include <entt/entity/entity.hpp>

namespace sim {

// Components are plain structs with nothing EnTT-specific. An entity is
// whatever components it has; systems run views over the ones they need.
//
// entt::entity{} is a real id (index 0), so entity fields that can be
// empty default to entt::null instead.

//====== PHYSICS ======//

enum class PhysicsFlag : uint8_t {
  // collides with solid grid cells
  Collides = 1 << 0,
  // accelerated by GRAVITY every tick
  Gravity = 1 << 1,
};

// What a body touched during the last physics tick. Set by physics.
enum class Contact : uint8_t {
  // standing on something
  Ground = 1 << 0,
  // any move (floor, wall, ceiling, ridge) was stopped by the grid
  Collided = 1 << 1,
};

// Axis-aligned box centered on Transform::pos. Never rotates.
struct Collider {
  float w;
  float h;
};

// All the drives pulling on one body this tick, summed per axis. Physics
// rebuilds it every tick, then pulls raw_vel toward
//   accel_target / accel      at rate accel        (linear)
//   damping_target / damping  at rate damping      (exponential)
// i.e. the strength-weighted average of the drives' targets, so drives that
// disagree fight and settle in between, closer to the stronger one.
struct DriveSum {
  Vec2 accel;          // sum of accel
  Vec2 accel_target;   // sum of accel * target
  Vec2 damping;        // sum of damping
  Vec2 damping_target; // sum of damping * target
};

struct Physics {
  Vec2 vel;     // cache (re-evaluated every tick)
  Vec2 raw_vel; // offset
  DriveSum drives; // cache (rebuilt every tick)
  float ang_vel;
  float raw_ang_vel;
  Collider collider;
  // While on the ground, ledges up to this tall are climbed automatically
  // and drops up to this tall are followed instead of fallen off. 0 = off.
  float step_height;
  Flags<PhysicsFlag> flags;
  // set by physics every tick
  Flags<Contact> contacts;
};
static_assert(Plain<Physics>);

struct Transform {
  Vec2 pos;
  float rot;
};
static_assert(Plain<Transform>);

//====== MOVEMENT ======//

enum class MovementIntent : uint8_t {
  Left = 1 << 0,
  Right = 1 << 1,
  Jump = 1 << 2,
  Sprint = 1 << 3,
  Slide = 1 << 4,
};
using MovementIntents = uint8_t;

enum class MoveFlag : uint8_t {
  // ctrl was pressed and is still held, and hasn't started a slide yet: the
  // slide starts the first moment it can (landing, enough speed, cooldown
  // over)
  SlideArmed = 1 << 0,
};

// Input-driven movement. run_movement turns `intents` into changes to
// Physics::raw_vel every tick.
struct Movement {
  // how this entity moves (speeds, rates, cooldowns)
  MovementParams params;
  MovementIntents intents;
  // last tick's intents, so presses can be told apart from holds
  MovementIntents prev_intents;
  Flags<MoveFlag> flags;
  MoveFsm state;
  // unit vector the entity is aiming (wall jumps go this way), or zero
  Vec2 aim;
  // -1 or 1: last direction walked
  float facing;
  // input forgiveness, seconds left on each (see MovementParams)
  float coyote_left;
  float jump_buffer_left;
  float wall_grace_left;
  // -1 or 1: which side the wall was on when one was last in wall jump
  // reach, for wall jumps in the grace window
  float last_wall_dir;
  // seconds left on timed states (dodging, cooldowns), indexed by
  // flag_index. 0 means the state isn't timed.
  std::array<float, FsmRules<MoveState>::N> timers;
  // movement's own drive modifier (an entity with DriveModifier). Retuned
  // every tick to whatever movement is trying to do (walk, stop, slide,
  // slow a wall slide).
  entt::entity drive = entt::null;
};
static_assert(Plain<Movement>);

//====== OWNERSHIP ======//

// Belongs to another entity (a hook to the creature that fired it).
struct Owner {
  entt::entity entity = entt::null;
};
static_assert(Plain<Owner>);

//====== ODM ======//

enum class OdmFlag : uint8_t {
  // set by the ODM system: the hook is stuck and pulling, so movement
  // doesn't steer against it
  HookPulling = 1 << 0,
};

// The gear: a hook and a gas tank (see systems/odm). Driven by the
// GasBurst, FireHook and ReleaseHook inputs, tuned by the creature's
// MovementParams.
struct Odm {
  // the hook while it's out, null once it's back
  entt::entity hook = entt::null;
  // gas bursts available (see burst_regen_left)
  uint8_t burst_charges;
  Flags<OdmFlag> flags;
  // seconds until the next gas burst regenerates
  float burst_regen_left;
};
static_assert(Plain<Odm>);

//====== HOOKS ======//

enum class HookState : uint8_t {
  // on its way out, sticks to the first thing it hits
  Flying,
  // stuck to terrain, pulling its owner in
  Attached,
  // on its way back to its owner, through terrain
  Returning,
};

// A grappling hook out in the world (see systems/odm). A projectile: it
// also has Owner (the creature whose Odm fired it), Transform and Physics.
struct Hook {
  HookState state;
  // force modifier (an entity with ForceModifier) on the owner while
  // attached, null otherwise
  entt::entity pull = entt::null;
  // the creature it's stuck in, or null if it's stuck in terrain (or not
  // stuck)
  entt::entity stuck_to = entt::null;
};
static_assert(Plain<Hook>);

//====== PERCEPTION ======//

// How many seconds this entity perceives per world second (see
// systems/perception). Rises with speed. The observer's perception is the
// time scale: perception 10 means the world plays at a tenth of real speed.
// The highest perception anywhere also shortens ticks.
struct Perception {
  float value;
};
static_assert(Plain<Perception>);

//====== COMBAT ======//

// Fighting: health, and the slash in progress (see systems/combat).
struct Combat {
  float health;
  // seconds left on the current slash, and until the next can start
  float slash_left;
  float slash_cooldown_left;
  // unit vector the current slash points along
  Vec2 slash_dir;
  // who the current slash has already hit, so each is hit once per slash
  std::array<entt::entity, 8> slash_hits;
  uint8_t slash_hit_count;
  // seconds since this was last hit (counts up), for hit reactions
  float since_hit;
};
static_assert(Plain<Combat>);

} // namespace sim
