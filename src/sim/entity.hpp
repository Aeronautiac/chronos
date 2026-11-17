#pragma once
#include "sim/systems/movement/movement_params.hpp"
#include "sim/systems/movement/movement_state.hpp"
#include "util/plain.hpp"
#include "util/slotmap.hpp"
#include "util/vec2.hpp"
#include <array>
#include <cstdint>

namespace sim {

// defined in systems/physics/physics_types.hpp. Key only uses it as a tag.
struct PhysicsModifier;
using ModifierID = Key<PhysicsModifier>;

// defined at the bottom. Key only uses it as a tag.
struct Entity;
using EntityID = Key<Entity>;

//====== COMPONENTS ======//

enum class PhysicsFlag : uint8_t {
  // collides with solid grid cells
  Collides = 1 << 0,
  // accelerated by GRAVITY every tick
  Gravity = 1 << 1,
};
using PhysicsFlags = uint8_t;

constexpr bool has_flag(PhysicsFlags set, PhysicsFlag f) noexcept {
  return (set & static_cast<PhysicsFlags>(f)) != 0;
}

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
  PhysicsFlags flags;
  // While on the ground, ledges up to this tall are climbed automatically
  // and drops up to this tall are followed instead of fallen off. 0 = off.
  float step_height;
  // set by run_physics: standing on something this tick
  bool on_ground;
  // set by run_physics: -1 or 1 if a horizontal move was stopped this tick
  // by a wall (left or right), else 0. Only counts as a wall if it's solid
  // level with the collider's vertical middle, not a ridge at its feet.
  float wall_dir;
  // set by run_physics: any move (floor, wall, ceiling, ridge) was stopped by
  // the grid this tick
  bool collided;
};

struct Transform {
  Vec2 pos;
  float rot;
};

enum class MovementIntent : uint8_t {
  Left = 1 << 0,
  Right = 1 << 1,
  Jump = 1 << 2,
  Sprint = 1 << 3,
  Slide = 1 << 4,
  // fire the hook toward Movement::aim, hold to keep it out
  Hook = 1 << 5,
  // slash toward Movement::aim
  Attack = 1 << 6,
  // gas burst toward Movement::aim
  Burst = 1 << 7,
};
using MovementIntents = uint8_t;

// Input-driven movement. run_movement turns `intents` into changes to
// Physics::raw_vel every tick.
struct Movement {
  // how this entity moves (speeds, rates, cooldowns)
  MovementParams params;
  MovementIntents intents;
  // last tick's intents, so presses can be told apart from holds
  MovementIntents prev_intents;
  MoveFsm state;
  // unit vector the entity is aiming (gas bursts and the hook go this way),
  // or zero
  Vec2 aim;
  // this entity's hook while it's out, zeroed key once it's back
  EntityID hook;
  // set by the hook system: the hook is stuck and pulling, so movement
  // doesn't steer against it
  bool hook_pulling;
  // -1 or 1: last direction walked
  float facing;
  // gas bursts available, and seconds until the next one regenerates
  uint8_t burst_charges;
  float burst_regen_left;
  // input forgiveness, seconds left on each (see MovementParams)
  float coyote_left;
  float jump_buffer_left;
  float wall_grace_left;
  // which side the wall was on when last clinging, for wall jumps in the
  // grace window
  float last_wall_dir;
  // ctrl was pressed and is still held, and hasn't started a slide yet: the
  // slide starts the first moment it can (landing, enough speed, cooldown
  // over)
  bool slide_armed;
  // seconds left on timed states (dodging, cooldowns), indexed by
  // flag_index. 0 means the state isn't timed.
  std::array<float, FsmRules<MoveState>::N> timers;
  // movement's own Drive modifier. Retuned every tick to whatever movement
  // is trying to do (walk, stop, slide, slow a wall slide).
  ModifierID drive;
};

enum class HookState : uint8_t {
  // on its way out, sticks to the first thing it hits
  Flying,
  // stuck to terrain, pulling its owner in
  Attached,
  // on its way back to its owner, through terrain
  Returning,
};

// A grappling hook out in the world (see systems/hook).
struct Hook {
  EntityID owner;
  HookState state;
  // Force modifier on the owner while attached, zeroed key otherwise
  ModifierID pull;
  // the creature it's stuck in, or zeroed key if it's stuck in terrain (or
  // not stuck)
  EntityID stuck_to;
};

// How many seconds this entity perceives per world second (see
// systems/perception). Rises with speed. The observer's perception is the
// time scale: perception 10 means the world plays at a tenth of real speed.
// The highest perception anywhere also shortens ticks.
struct Perception {
  float value;
};

// Fighting: health, and the slash in progress (see systems/combat).
struct Combat {
  float health;
  // seconds left on the current slash, and until the next can start
  float slash_left;
  float slash_cooldown_left;
  // unit vector the current slash points along
  Vec2 slash_dir;
  // who the current slash has already hit, so each is hit once per slash
  std::array<EntityID, 8> slash_hits;
  uint8_t slash_hit_count;
  // seconds since this was last hit (counts up), for hit reactions
  float since_hit;
};

//====== ENTITIES ======//

// Each kind has a fixed set of components and its own table (kinds.hpp).
enum class EntityKind : uint8_t {
  // anything living: the player, enemies, NPCs
  Creature,
  Hook,
};

// Where an entity's data lives: row `row` of its kind's table. Every entity
// of every kind has one of these, and its EntityID is the key to it.
struct Entity {
  EntityKind kind;
  uint32_t row;
};
static_assert(Plain<Entity>);

} // namespace sim
