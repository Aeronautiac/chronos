#pragma once
#include "plain.hpp"

struct Vec2 {
  float x;
  float y;
};
static_assert(Plain<Vec2>);

constexpr Vec2 operator+(Vec2 a, Vec2 b) noexcept { return {a.x + b.x, a.y + b.y}; }
constexpr Vec2 operator-(Vec2 a, Vec2 b) noexcept { return {a.x - b.x, a.y - b.y}; }
constexpr Vec2 operator*(Vec2 v, float s) noexcept { return {v.x * s, v.y * s}; }
constexpr Vec2 operator*(float s, Vec2 v) noexcept { return v * s; }
constexpr Vec2 &operator+=(Vec2 &a, Vec2 b) noexcept { return a = a + b; }
constexpr Vec2 &operator-=(Vec2 &a, Vec2 b) noexcept { return a = a - b; }
constexpr bool operator==(Vec2 a, Vec2 b) noexcept { return a.x == b.x && a.y == b.y; }
