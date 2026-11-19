#pragma once
#include "sim/components.hpp"
#include "sim/grid.hpp"
#include "util/vec2.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <limits>
#include <optional>
#include <utility>

namespace sim {

// Spatial queries: against the grid's solid cells, and against entity
// colliders. Systems ask these instead of scanning cells or colliders
// themselves.
//
// Entity queries visit entities in view order, which depends only on
// history, so results are deterministic. They're a linear scan for now; a
// broadphase can replace the loop without changing any caller.

// Grid queries treat boxes as this much smaller on every side, so a box
// flush against a wall doesn't count as overlapping it because of float
// rounding. Has to stay well above one ulp at the largest world coordinate
// (~0.001 at 12288).
inline constexpr float SKIN = 0.01f;

// Axis-aligned box: center and half extents.
struct Box {
  Vec2 center;
  Vec2 half;
};

// The box an entity's collider covers at `pos`.
constexpr Box collider_box(Vec2 pos, Collider c) noexcept {
  return {.center = pos, .half = {c.w * 0.5f, c.h * 0.5f}};
}

// Do the boxes overlap? Touching edges don't count.
constexpr bool boxes_overlap(Box a, Box b) noexcept {
  return std::abs(a.center.x - b.center.x) < a.half.x + b.half.x &&
         std::abs(a.center.y - b.center.y) < a.half.y + b.half.y;
}

//====== RAYS ======//

// Rays are an origin and a unit direction; distances are along the ray, in
// world units.

namespace detail {
// Narrows [t_enter, t_exit] to where the ray is strictly inside [lo, hi] on
// one axis. False if it never is.
constexpr bool clip_axis(float o, float d, float lo, float hi, float &t_enter,
                         float &t_exit) noexcept {
  if (d == 0) {
    // parallel to this axis's faces: inside the slab the whole way or never
    return lo < o && o < hi;
  }
  float t0 = (lo - o) / d;
  float t1 = (hi - o) / d;
  if (t0 > t1) {
    std::swap(t0, t1);
  }
  t_enter = std::max(t_enter, t0);
  t_exit = std::min(t_exit, t1);
  return true;
}
} // namespace detail

// Distance along the ray to where it enters `box`, 0 if it starts inside,
// nullopt if it misses. Grazing an edge or corner doesn't count, matching
// boxes_overlap.
constexpr std::optional<float> ray_box(Vec2 origin, Vec2 dir, Box box) noexcept {
  float t_enter = 0;
  float t_exit = std::numeric_limits<float>::infinity();
  if (!detail::clip_axis(origin.x, dir.x, box.center.x - box.half.x,
                         box.center.x + box.half.x, t_enter, t_exit) ||
      !detail::clip_axis(origin.y, dir.y, box.center.y - box.half.y,
                         box.center.y + box.half.y, t_enter, t_exit)) {
    return std::nullopt;
  }
  if (!(t_enter < t_exit)) {
    return std::nullopt;
  }
  return t_enter;
}

//====== GRID ======//

enum class Axis : uint8_t { X, Y };

// Is the cell containing `p` solid? Outside the grid counts as solid.
bool solid_at(const Grid &grid, Vec2 p) noexcept;

struct Sweep {
  // the box's center on the moving axis after the move
  float center;
  // stopped by a solid cell
  bool hit;
};

// Moves `box` by `d` along `axis`, stopping flush against the first solid
// cell its leading edge would enter, however far that is. Cells the box
// already overlaps at the start are ignored, so a box inside terrain can
// still move out.
Sweep sweep_box(const Grid &grid, Box box, Axis axis, float d) noexcept;

// y of the top of the first solid cell in the column under `from.x`,
// searching from the cell containing `from` down to `max_drop` below it.
// nullopt if there's none in that range.
std::optional<float> surface_below(const Grid &grid, Vec2 from,
                                   float max_drop) noexcept;

struct GridRayHit {
  // where the ray first touches a solid cell (on that cell's face)
  Vec2 point;
  // distance along the ray to `point`
  float dist;
  // outward normal of the face hit: one of (+-1, 0), (0, +-1). Zero if the
  // ray starts inside a solid cell.
  Vec2 normal;
  // the solid cell hit (may be just outside the grid, which counts as solid)
  int32_t cell_x;
  int32_t cell_y;
};

// The first solid cell along the ray within `max_dist`, visiting every cell
// the ray passes through in order, so it can't skip thin walls. Starting
// inside a solid cell (or outside the grid) hits at distance 0. Exactly
// through a corner, the x neighbour is checked before the y one. `dir`
// must be a unit vector.
std::optional<GridRayHit> raycast_grid(const Grid &grid, Vec2 origin,
                                       Vec2 dir, float max_dist) noexcept;

struct GridCastHit {
  // the box's center when it stops, flush against the face hit
  Vec2 center;
  // distance along `dir` the box moved to get there
  float dist;
  // outward normal of the face hit: one of (+-1, 0), (0, +-1). Zero if the
  // box starts overlapping a solid cell.
  Vec2 normal;
  // a solid cell the box ran into (may be just outside the grid)
  int32_t cell_x;
  int32_t cell_y;
};

// Moves `box` along `dir` (any direction, not just an axis) until it first
// touches a solid cell, within `max_dist`. Checks every cell the moving box
// enters, so it can't skip thin walls or slip between two cells that only
// meet at a corner. Like sweep_box, a box flush against a wall doesn't
// count as touching it unless it moves into it; unlike sweep_box, a box
// that starts overlapping a solid cell hits at distance 0. When the box
// reaches an x and a y boundary at the same moment, the x one is checked
// first. `dir` must be a unit vector.
std::optional<GridCastHit> boxcast_grid(const Grid &grid, Box box, Vec2 dir,
                                        float max_dist) noexcept;

//====== ENTITIES ======//

// Calls fn(entity) for every entity with Transform and Physics, plus every
// component in With, whose collider overlaps `box`. fn may change
// components but not create, destroy, emplace or remove.
template <typename... With, typename Fn>
void for_each_overlapping(const entt::registry &r, Box box, Fn &&fn) {
  auto view = r.view<const Transform, const Physics, const With...>();
  for (const entt::entity e : view) {
    const Box other = collider_box(view.template get<const Transform>(e).pos,
                                   view.template get<const Physics>(e).collider);
    if (boxes_overlap(box, other)) {
      fn(e);
    }
  }
}

// The first entity (in for_each_overlapping's order) other than `skip`
// whose collider overlaps `box`, or null.
template <typename... With>
entt::entity first_overlapping(const entt::registry &r, Box box,
                               entt::entity skip = entt::null) {
  auto view = r.view<const Transform, const Physics, const With...>();
  for (const entt::entity e : view) {
    if (e == skip) {
      continue;
    }
    const Box other = collider_box(view.template get<const Transform>(e).pos,
                                   view.template get<const Physics>(e).collider);
    if (boxes_overlap(box, other)) {
      return e;
    }
  }
  return entt::null;
}

struct EntityCastHit {
  entt::entity entity;
  // where the cast stops: the ray's entry point into the collider, or the
  // box's center when it first touches it (the start if already inside)
  Vec2 pos;
  // distance along `dir` to `pos`
  float dist;
};

namespace detail {
// Nearest entity a box of half extents `half`, moving from `start` along
// `dir`, runs into. A box hitting a box is a point (its center) hitting the
// other box grown by `half`; half = 0 is a ray.
template <typename... With>
std::optional<EntityCastHit>
nearest_cast_hit(const entt::registry &r, Vec2 start, Vec2 half, Vec2 dir,
                 float max_dist, entt::entity skip) {
  std::optional<EntityCastHit> best;
  auto view = r.view<const Transform, const Physics, const With...>();
  for (const entt::entity e : view) {
    if (e == skip) {
      continue;
    }
    Box grown = collider_box(view.template get<const Transform>(e).pos,
                             view.template get<const Physics>(e).collider);
    grown.half += half;
    const std::optional<float> t = ray_box(start, dir, grown);
    if (!t || *t > max_dist || (best && *t >= best->dist)) {
      continue;
    }
    best = EntityCastHit{.entity = e, .pos = start + dir * *t, .dist = *t};
  }
  return best;
}
} // namespace detail

// The nearest entity with Transform and Physics, plus every component in
// With, other than `skip`, whose collider the ray enters within `max_dist`.
// Equal distances go to the first in for_each_overlapping's order. `dir`
// must be a unit vector.
//
// To stop at terrain, pass the grid hit's distance as `max_dist`.
template <typename... With>
std::optional<EntityCastHit> raycast_entities(const entt::registry &r,
                                              Vec2 origin, Vec2 dir,
                                              float max_dist,
                                              entt::entity skip = entt::null) {
  return detail::nearest_cast_hit<With...>(r, origin, {0, 0}, dir, max_dist,
                                           skip);
}

// Like raycast_entities, for `box` moving along `dir`: the nearest entity
// whose collider it first touches within `max_dist`. Touching edges don't
// count until the boxes overlap; one already overlapping hits at 0.
template <typename... With>
std::optional<EntityCastHit> boxcast_entities(const entt::registry &r, Box box,
                                              Vec2 dir, float max_dist,
                                              entt::entity skip = entt::null) {
  return detail::nearest_cast_hit<With...>(r, box.center, box.half, dir,
                                           max_dist, skip);
}

} // namespace sim
