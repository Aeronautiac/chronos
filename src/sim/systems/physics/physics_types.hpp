#pragma once
#include "sim/entity.hpp"
#include "util/plain.hpp"
#include "util/vec2.hpp"

namespace sim {

enum PhysicsModifierKind {
  Velocity,
  Force,
  AngularVelocity,
  AngularForce,
  Drive,
};

// Pulls raw_vel toward `target`. Per axis, so a drive can pull on x only (or
// on x and y with different strengths). An axis with no accel and no damping
// isn't pulled at all.
//   accel   linear part: changes velocity by at most this much per second,
//           arriving at the target exactly. Feels like friction or a capped
//           acceleration.
//   damping exponential part, per second: closes a fixed fraction of the
//           remaining gap over time, fast when far and slow when close.
//           Feels like drag, eases out.
// Drives on the same body fight and settle: their targets are averaged,
// weighted by strength (see DriveSum).
struct DriveParams {
  Vec2 target;
  Vec2 accel;
  Vec2 damping;
};
static_assert(Plain<DriveParams>);

// Persists until erased, or until its target dies (pruned by physics).
//   Velocity        -> vector_val added to vel this tick, raw_vel untouched
//   Force           -> vector_val * dt accumulated into raw_vel (unit mass)
//   AngularVelocity -> scalar_val added to ang_vel this tick
//   AngularForce    -> scalar_val * dt accumulated into raw_ang_vel
//   Drive           -> drive pulls raw_vel toward its target
struct PhysicsModifier {
  union {
    Vec2 vector_val;
    float scalar_val;
    DriveParams drive;
  };
  EntityID target;
  PhysicsModifierKind kind;
};
static_assert(Plain<PhysicsModifier>);

} // namespace sim
