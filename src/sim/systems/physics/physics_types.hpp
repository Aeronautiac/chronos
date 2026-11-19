#pragma once
#include "sim/components.hpp"
#include "util/plain.hpp"
#include "util/vec2.hpp"

namespace sim {

// Physics modifiers are entities with one of the modifier components below,
// each holding the body (`target`) it acts on. The target is in the
// component itself, so applying a kind is one storage walked in order, plus
// a lookup of each target's Physics. They last until destroyed, or until
// their target is gone or has no Physics (physics destroys them then).
// Several on one body add up.

// Added to the target's vel for every tick it exists, raw_vel untouched.
struct VelocityModifier {
  entt::entity target = entt::null;
  Vec2 vel;
};
static_assert(Plain<VelocityModifier>);

// Accumulated into the target's raw_vel, force * dt every tick (unit mass).
struct ForceModifier {
  entt::entity target = entt::null;
  Vec2 force;
};
static_assert(Plain<ForceModifier>);

// Added to the target's ang_vel for every tick it exists.
struct AngularVelocityModifier {
  entt::entity target = entt::null;
  float ang_vel;
};
static_assert(Plain<AngularVelocityModifier>);

// Accumulated into the target's raw_ang_vel, torque * dt every tick.
struct AngularForceModifier {
  entt::entity target = entt::null;
  float torque;
};
static_assert(Plain<AngularForceModifier>);

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

struct DriveModifier {
  entt::entity target = entt::null;
  DriveParams drive;
};
static_assert(Plain<DriveModifier>);

} // namespace sim
