#include "spatial.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace sim {

namespace {

// number of cells along an axis
int32_t cell_count(Axis axis) noexcept {
  return axis == Axis::X ? Grid::W : Grid::H;
}

// Grid lookup by (index along the moving axis, index across it).
bool solid(const Grid &grid, Axis axis, int32_t along, int32_t across) noexcept {
  return axis == Axis::X ? grid.solid(along, across)
                         : grid.solid(across, along);
}

// The cell an edge at v enters when moving toward -inf: v exactly on a
// boundary k * CELL_SIZE gives k - 1, anything else is cell_coord(v). Same
// clamping as cell_coord.
int32_t cell_below(float v, int32_t n) noexcept {
  float c = std::ceil(v / Grid::CELL_SIZE) - 1;
  c = std::clamp(c, -1.0f, static_cast<float>(n));
  return static_cast<int32_t>(c);
}

// inclusive range of cell indices on one axis
struct Span {
  int32_t lo;
  int32_t hi;
};

// Cells a box covers on one axis, given which way it's moving on that axis
// (sign of `d`). The leading side isn't skinned: a box flush there is about
// to enter the next cell, so it counts. The trailing side is: the box is
// leaving. Not moving on the axis, both sides are skinned, so being flush
// against something doesn't count.
Span moving_span(float center, float half, float d, int32_t n) noexcept {
  const float lo = center - half;
  const float hi = center + half;
  return {
      .lo = d < 0 ? cell_below(lo, n) : cell_coord(lo + SKIN, n),
      .hi = d > 0 ? cell_coord(hi, n) : cell_coord(hi - SKIN, n),
  };
}

// One axis of a box cast: the box's leading edge on this axis, stepping
// from cell to cell.
struct Lead {
  // -1, 0 or 1: direction of travel on this axis
  int32_t step;
  // the cell the leading edge is in. Skinned, so an edge flush against the
  // next cell (or into it by less than SKIN) isn't in it yet.
  int32_t cell;
  // the leading edge's coordinate at the start of the cast
  float edge;
  // dir's component on this axis
  float d;

  Lead(float center, float half, float d_, int32_t n) noexcept
      : step(d_ > 0 ? 1 : (d_ < 0 ? -1 : 0)), d(d_) {
    edge = step > 0 ? center + half : center - half;
    cell = step > 0 ? cell_coord(edge - SKIN, n) : cell_coord(edge + SKIN, n);
  }

  // Distance along the cast until the leading edge reaches the far side of
  // `cell`, from the start each time so long casts don't drift. At least 0:
  // an edge already slightly past the boundary is there now.
  float next() const noexcept {
    if (step == 0) {
      return std::numeric_limits<float>::infinity();
    }
    const int32_t boundary = step > 0 ? cell + 1 : cell;
    return std::max(
        0.0f, (static_cast<float>(boundary) * Grid::CELL_SIZE - edge) / d);
  }

  // where the box's center is on this axis when its leading edge is flush
  // against cell `c` (the one just entered)
  float flush_center(int32_t c, float half) const noexcept {
    return step > 0 ? static_cast<float>(c) * Grid::CELL_SIZE - half
                    : static_cast<float>(c + 1) * Grid::CELL_SIZE + half;
  }
};

} // namespace

bool solid_at(const Grid &grid, Vec2 p) noexcept {
  return grid.solid(cell_coord(p.x, Grid::W), cell_coord(p.y, Grid::H));
}

// The x sweep is the 1D problem "move center x, checking the rows the box
// spans"; the y sweep is the same with the axes swapped.
Sweep sweep_box(const Grid &grid, Box box, Axis axis, float d) noexcept {
  const bool along_x = axis == Axis::X;
  float center = along_x ? box.center.x : box.center.y;
  const float half = along_x ? box.half.x : box.half.y;
  const float across_center = along_x ? box.center.y : box.center.x;
  const float across_half = along_x ? box.half.y : box.half.x;

  const int32_t n = cell_count(axis);
  const int32_t n_across = cell_count(along_x ? Axis::Y : Axis::X);

  // cells the box covers on the other axis
  const int32_t a0 = cell_coord(across_center - across_half + SKIN, n_across);
  const int32_t a1 = cell_coord(across_center + across_half - SKIN, n_across);
  auto blocked = [&](int32_t i) {
    for (int32_t a = a0; a <= a1; a++) {
      if (solid(grid, axis, i, a)) {
        return true;
      }
    }
    return false;
  };

  // `from` is the last cell the box already covers (skinned, so a box flush
  // against a wall counts as outside it). `to` is the cell the real leading
  // edge ends up in, unskinned, so even a tiny push into a wall registers as
  // a hit instead of sinking in. `to` is clamped to [-1, n], which is solid,
  // so these loops always terminate.
  if (d > 0) {
    const int32_t from = cell_coord(center + half - SKIN, n);
    const int32_t to = cell_coord(center + half + d, n);
    for (int32_t i = from + 1; i <= to; i++) {
      if (blocked(i)) {
        // leading edge flush with the cell's near side
        return {.center = static_cast<float>(i) * Grid::CELL_SIZE - half,
                .hit = true};
      }
    }
  } else if (d < 0) {
    const int32_t from = cell_coord(center - half + SKIN, n);
    const int32_t to = cell_coord(center - half + d, n);
    for (int32_t i = from - 1; i >= to; i--) {
      if (blocked(i)) {
        return {.center = static_cast<float>(i + 1) * Grid::CELL_SIZE + half,
                .hit = true};
      }
    }
  }
  center += d;
  return {.center = center, .hit = false};
}

std::optional<float> surface_below(const Grid &grid, Vec2 from,
                                   float max_drop) noexcept {
  const int32_t cx = cell_coord(from.x, Grid::W);
  const int32_t first = cell_coord(from.y, Grid::H);
  const int32_t last = cell_coord(from.y + max_drop, Grid::H);
  for (int32_t cy = first; cy <= last; cy++) {
    if (grid.solid(cx, cy)) {
      return static_cast<float>(cy) * Grid::CELL_SIZE;
    }
  }
  return std::nullopt;
}

std::optional<GridRayHit> raycast_grid(const Grid &grid, Vec2 origin,
                                       Vec2 dir, float max_dist) noexcept {
  assert(std::abs(dir.x * dir.x + dir.y * dir.y - 1) < 1e-3f);
  int32_t cx = cell_coord(origin.x, Grid::W);
  int32_t cy = cell_coord(origin.y, Grid::H);
  if (grid.solid(cx, cy)) {
    return GridRayHit{
        .point = origin, .dist = 0, .normal = {0, 0}, .cell_x = cx, .cell_y = cy};
  }

  const int32_t step_x = dir.x > 0 ? 1 : (dir.x < 0 ? -1 : 0);
  const int32_t step_y = dir.y > 0 ? 1 : (dir.y < 0 ? -1 : 0);
  constexpr float NEVER = std::numeric_limits<float>::infinity();
  // Distance along the ray to the next cell boundary on one axis: the far
  // side of cell `c` in the direction of travel. Computed from the origin
  // each time rather than accumulated, so long rays don't drift.
  auto next_boundary = [](float o, float d, int32_t c, int32_t step) {
    if (step == 0) {
      return NEVER;
    }
    const int32_t boundary = step > 0 ? c + 1 : c;
    return (static_cast<float>(boundary) * Grid::CELL_SIZE - o) / d;
  };

  // Walk cell to cell. Each step crosses whichever boundary comes first.
  // The cell indices stop at -1 and W (or H), which are solid, so this ends
  // even past max_dist's reach.
  for (;;) {
    const float tx = next_boundary(origin.x, dir.x, cx, step_x);
    const float ty = next_boundary(origin.y, dir.y, cy, step_y);
    float t;
    Vec2 normal;
    if (tx <= ty) {
      t = tx;
      cx += step_x;
      normal = {static_cast<float>(-step_x), 0};
    } else {
      t = ty;
      cy += step_y;
      normal = {0, static_cast<float>(-step_y)};
    }
    if (t > max_dist) {
      return std::nullopt;
    }
    if (grid.solid(cx, cy)) {
      return GridRayHit{.point = origin + dir * t,
                        .dist = t,
                        .normal = normal,
                        .cell_x = cx,
                        .cell_y = cy};
    }
  }
}

// Steps the box's leading edges cell boundary to cell boundary, in the order
// the moving box reaches them. Each time the leading x edge enters a new
// column, that column is checked over the rows the box covers at that
// moment (and the same for y). The covered rows/columns include the cell
// the box is just about to enter on the other axis (see moving_span), so a
// diagonal move can't pass between two cells that only meet at a corner.
std::optional<GridCastHit> boxcast_grid(const Grid &grid, Box box, Vec2 dir,
                                        float max_dist) noexcept {
  assert(std::abs(dir.x * dir.x + dir.y * dir.y - 1) < 1e-3f);
  // smaller than this, the skinned spans are empty: use raycast_grid
  assert(box.half.x > SKIN && box.half.y > SKIN);
  const Vec2 c = box.center;
  const Vec2 h = box.half;

  // already overlapping (skinned: flush doesn't count)
  const Span cols0 = moving_span(c.x, h.x, 0, Grid::W);
  const Span rows0 = moving_span(c.y, h.y, 0, Grid::H);
  for (int32_t y = rows0.lo; y <= rows0.hi; y++) {
    for (int32_t x = cols0.lo; x <= cols0.hi; x++) {
      if (grid.solid(x, y)) {
        return GridCastHit{
            .center = c, .dist = 0, .normal = {0, 0}, .cell_x = x, .cell_y = y};
      }
    }
  }

  Lead lx(c.x, h.x, dir.x, Grid::W);
  Lead ly(c.y, h.y, dir.y, Grid::H);
  // The leads stop at -1 and W (or H), which are solid, so this ends.
  for (;;) {
    const float tx = lx.next();
    const float ty = ly.next();
    const float t = std::min(tx, ty);
    if (t > max_dist) {
      return std::nullopt;
    }
    const Vec2 at = c + dir * t;
    if (tx <= ty) {
      // entering column lx.cell: check it over the rows covered now
      lx.cell += lx.step;
      const Span rows = moving_span(at.y, h.y, dir.y, Grid::H);
      for (int32_t y = rows.lo; y <= rows.hi; y++) {
        if (grid.solid(lx.cell, y)) {
          return GridCastHit{
              .center = {lx.flush_center(lx.cell, h.x), at.y},
              .dist = t,
              .normal = {static_cast<float>(-lx.step), 0},
              .cell_x = lx.cell,
              .cell_y = y,
          };
        }
      }
    } else {
      // entering row ly.cell: check it over the columns covered now
      ly.cell += ly.step;
      const Span cols = moving_span(at.x, h.x, dir.x, Grid::W);
      for (int32_t x = cols.lo; x <= cols.hi; x++) {
        if (grid.solid(x, ly.cell)) {
          return GridCastHit{
              .center = {at.x, ly.flush_center(ly.cell, h.y)},
              .dist = t,
              .normal = {0, static_cast<float>(-ly.step)},
              .cell_x = x,
              .cell_y = ly.cell,
          };
        }
      }
    }
  }
}

} // namespace sim
