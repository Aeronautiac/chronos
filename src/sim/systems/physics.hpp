#pragma once
#include "util/vec2.hpp"

enum PhysicsModifierKind {
  Velocity,
  Force,
  AngularVelocity,
  AngularForce,
};

struct PhysicsModifier {
  union {
    Vec2 vector_val;
    float scalar_val;
  };
  PhysicsModifierKind kind;
};
