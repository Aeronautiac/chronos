#pragma once
#include "util/plain.hpp"
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>

namespace sim {

enum class CellKind : uint8_t {
  Empty,
  Solid,
};

struct Cell {
  CellKind kind;
};
static_assert(Plain<Cell>);

// The world's geometry: a fixed grid of cells, row-major, y down.
// Cell (x, y) covers world [x, x + 1) * CELL_SIZE by [y, y + 1) * CELL_SIZE.
// Everything outside the grid counts as solid, so the grid's edge is also the
// world's boundary.
struct Grid {
  static constexpr int32_t W = 3072;
  static constexpr int32_t H = 1536;
  static constexpr float CELL_SIZE = 4.0f;

  std::array<Cell, static_cast<size_t>(W) * static_cast<size_t>(H)> cells;

  static constexpr bool in_bounds(int32_t x, int32_t y) noexcept {
    return x >= 0 && x < W && y >= 0 && y < H;
  }
  constexpr Cell &at(int32_t x, int32_t y) noexcept {
    assert(in_bounds(x, y));
    return cells[static_cast<size_t>(y * W + x)];
  }
  constexpr const Cell &at(int32_t x, int32_t y) const noexcept {
    assert(in_bounds(x, y));
    return cells[static_cast<size_t>(y * W + x)];
  }
  constexpr bool solid(int32_t x, int32_t y) const noexcept {
    return !in_bounds(x, y) || at(x, y).kind == CellKind::Solid;
  }
};

// Index of the cell containing world coordinate v on an axis with n cells.
// Clamped to [-1, n] so far-off values land just out of bounds (solid)
// instead of overflowing int32.
int32_t cell_coord(float v, int32_t n) noexcept;

// Sets every cell in [x0, x1] x [y0, y1] (inclusive) that's in bounds.
void fill_rect(Grid &grid, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
               CellKind kind) noexcept;

// Deterministic terrain for testing movement: rolling ground, platforms, a
// one-cell-thick wall, and an overhang, plus a flat stretch from column
// FLAT_X0 to FLAT_X1 with its surface at row TEST_LEVEL_GROUND, empty except
// for one tall wall (60 cells high, 6 thick) starting at ARENA_WALL_X0.
inline constexpr int32_t FLAT_X0 = 1024;
inline constexpr int32_t FLAT_X1 = 2047;
inline constexpr int32_t ARENA_WALL_X0 = 1384;
inline constexpr int32_t TEST_LEVEL_GROUND = Grid::H - 60;
void fill_test_level(Grid &grid) noexcept;

} // namespace sim
