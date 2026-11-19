#include "grid.hpp"
#include "util/det_math.hpp"
#include <algorithm>
#include <cmath>

namespace sim {

int32_t cell_coord(float v, int32_t n) noexcept {
  assert(!std::isnan(v));
  float c = std::floor(v / Grid::CELL_SIZE);
  c = std::clamp(c, -1.0f, static_cast<float>(n));
  return static_cast<int32_t>(c);
}

void fill_rect(Grid &grid, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
               CellKind kind) noexcept {
  x0 = std::max(x0, 0);
  y0 = std::max(y0, 0);
  x1 = std::min(x1, Grid::W - 1);
  y1 = std::min(y1, Grid::H - 1);
  for (int32_t y = y0; y <= y1; y++) {
    for (int32_t x = x0; x <= x1; x++) {
      grid.at(x, y).kind = kind;
    }
  }
}

void fill_test_level(Grid &grid) noexcept {
  for (Cell &c : grid.cells) {
    c.kind = CellKind::Empty;
  }

  // rolling ground around row `base`, except a flat stretch across the
  // middle third
  const int32_t base = TEST_LEVEL_GROUND;
  for (int32_t x = 0; x < Grid::W; x++) {
    int32_t top = base;
    if (x < FLAT_X0 || x > FLAT_X1) {
      float fx = static_cast<float>(x);
      float wave = 12.0f * det::sin(fx * 0.02f) + 5.0f * det::sin(fx * 0.07f);
      top = base + static_cast<int32_t>(wave);
    }
    fill_rect(grid, x, top, x, Grid::H - 1, CellKind::Solid);
  }

  // on the flat stretch: one tall wall to run at, left of the middle
  fill_rect(grid, ARENA_WALL_X0, base - 60, ARENA_WALL_X0 + 5, base - 1,
            CellKind::Solid);

  // the same set of features every 1024 columns, placed relative to the
  // ground, except on the flat stretch
  for (int32_t x0 = 0; x0 < Grid::W; x0 += 1024) {
    if (x0 >= FLAT_X0 && x0 <= FLAT_X1) {
      continue;
    }
    // stepped platforms
    fill_rect(grid, x0 + 100, base - 52, x0 + 180, base - 49, CellKind::Solid);
    fill_rect(grid, x0 + 200, base - 92, x0 + 280, base - 89, CellKind::Solid);
    fill_rect(grid, x0 + 300, base - 132, x0 + 380, base - 129,
              CellKind::Solid);

    // one cell thick, to check fast movers don't pass through
    fill_rect(grid, x0 + 500, base - 202, x0 + 500, Grid::H - 1,
              CellKind::Solid);

    // overhang to bonk into
    fill_rect(grid, x0 + 600, base - 122, x0 + 820, base - 112,
              CellKind::Solid);
  }
}

} // namespace sim
