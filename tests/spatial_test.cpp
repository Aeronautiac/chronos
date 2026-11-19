#include "sim/spatial.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

namespace {

using sim::Axis;
using sim::Box;
using sim::Grid;

constexpr float CS = Grid::CELL_SIZE;

// touching edges don't overlap; any overlap does
static_assert(!sim::boxes_overlap({{0, 0}, {1, 1}}, {{2, 0}, {1, 1}}));
static_assert(sim::boxes_overlap({{0, 0}, {1, 1}}, {{1.9f, 0}, {1, 1}}));
static_assert(!sim::boxes_overlap({{0, 0}, {1, 1}}, {{0, 2}, {1, 1}}));

// empty grid with one solid column at cell x = 100, rows 0..199
std::unique_ptr<Grid> grid_with_wall() {
  auto g = std::make_unique<Grid>();
  sim::fill_rect(*g, 100, 0, 100, 199, sim::CellKind::Solid);
  return g;
}

// a 4 x 4 box whose right edge is `gap` left of the wall
Box box_left_of_wall(float gap) {
  return {.center = {100 * CS - 2 - gap, 50 * CS}, .half = {2, 2}};
}

TEST(Sweep, StopsFlushAgainstAWall) {
  auto g = grid_with_wall();
  const auto s = sim::sweep_box(*g, box_left_of_wall(10), Axis::X, 20);
  EXPECT_TRUE(s.hit);
  EXPECT_EQ(s.center, 100 * CS - 2);
}

TEST(Sweep, FastBoxDoesntTunnelThroughThinWall) {
  auto g = grid_with_wall();
  const auto s = sim::sweep_box(*g, box_left_of_wall(10), Axis::X, 10000);
  EXPECT_TRUE(s.hit);
  EXPECT_EQ(s.center, 100 * CS - 2);
}

TEST(Sweep, FreeMoveGoesTheWholeWay) {
  auto g = grid_with_wall();
  const Box b = box_left_of_wall(10);
  const auto s = sim::sweep_box(*g, b, Axis::X, -30);
  EXPECT_FALSE(s.hit);
  EXPECT_EQ(s.center, b.center.x - 30);
}

TEST(Sweep, FlushBoxSlidesAlongTheWall) {
  auto g = grid_with_wall();
  const Box b = box_left_of_wall(0);
  const auto s = sim::sweep_box(*g, b, Axis::Y, 40);
  EXPECT_FALSE(s.hit);
  EXPECT_EQ(s.center, b.center.y + 40);
}

TEST(Sweep, BoxInsideTerrainCanMoveOut) {
  auto g = grid_with_wall();
  const Box inside{.center = {100 * CS + 2, 50 * CS}, .half = {2, 2}};
  const auto s = sim::sweep_box(*g, inside, Axis::X, 8);
  EXPECT_FALSE(s.hit);
}

TEST(Sweep, GridEdgeIsSolid) {
  auto g = std::make_unique<Grid>();
  const Box b{.center = {10, 50 * CS}, .half = {2, 2}};
  const auto s = sim::sweep_box(*g, b, Axis::X, -100);
  EXPECT_TRUE(s.hit);
  EXPECT_EQ(s.center, 2);
}

TEST(Grid, SolidAt) {
  auto g = grid_with_wall();
  EXPECT_TRUE(sim::solid_at(*g, {100 * CS + 1, 10}));
  EXPECT_FALSE(sim::solid_at(*g, {100 * CS - 1, 10}));
  // outside the grid
  EXPECT_TRUE(sim::solid_at(*g, {-1, 10}));
}

TEST(Grid, SurfaceBelow) {
  auto g = std::make_unique<Grid>();
  // floor whose top edge is at row 300
  sim::fill_rect(*g, 0, 300, Grid::W - 1, 310, sim::CellKind::Solid);
  EXPECT_EQ(sim::surface_below(*g, {50, 290 * CS}, 20 * CS), 300 * CS);
  // the floor is past the range
  EXPECT_FALSE(sim::surface_below(*g, {50, 200 * CS}, 20 * CS));
}

//====== RAYS ======//

// a ray straight through a box enters at its near face
static_assert(sim::ray_box({0, 0}, {1, 0}, {{10, 0}, {2, 2}}) == 8.0f);
// starting inside is distance 0
static_assert(sim::ray_box({10, 0}, {1, 0}, {{10, 0}, {2, 2}}) == 0.0f);
// behind the origin, or off to the side, misses
static_assert(!sim::ray_box({0, 0}, {-1, 0}, {{10, 0}, {2, 2}}));
static_assert(!sim::ray_box({0, 5}, {1, 0}, {{10, 0}, {2, 2}}));
// grazing an edge doesn't count, like boxes_overlap
static_assert(!sim::ray_box({0, 2}, {1, 0}, {{10, 0}, {2, 2}}));

TEST(Raycast, HitsTheWallFace) {
  auto g = grid_with_wall();
  const Vec2 origin{90 * CS, 50 * CS + 1};
  const auto hit = sim::raycast_grid(*g, origin, {1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->cell_x, 100);
  EXPECT_EQ(hit->point.x, 100 * CS);
  EXPECT_EQ(hit->dist, 10 * CS);
  EXPECT_EQ(hit->normal, (Vec2{-1, 0}));
}

TEST(Raycast, StopsAtMaxDist) {
  auto g = grid_with_wall();
  const Vec2 origin{90 * CS, 50 * CS + 1};
  EXPECT_FALSE(sim::raycast_grid(*g, origin, {1, 0}, 10 * CS - 1));
}

TEST(Raycast, DiagonalRayDoesntSkipAThinWall) {
  auto g = grid_with_wall();
  // steep enough to cross many rows per column
  const Vec2 dir{0.1f, 0.99498744f};
  const auto hit = sim::raycast_grid(*g, {99 * CS + 3, 0.5f}, dir, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->cell_x, 100);
  EXPECT_EQ(hit->normal, (Vec2{-1, 0}));
}

TEST(Raycast, StraightDownHitsTheFloor) {
  auto g = std::make_unique<Grid>();
  sim::fill_rect(*g, 0, 300, Grid::W - 1, 310, sim::CellKind::Solid);
  const auto hit = sim::raycast_grid(*g, {50, 290 * CS}, {0, 1}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->point, (Vec2{50, 300 * CS}));
  EXPECT_EQ(hit->normal, (Vec2{0, -1}));
}

TEST(Raycast, StartingInsideSolidHitsAtZero) {
  auto g = grid_with_wall();
  const auto hit = sim::raycast_grid(*g, {100 * CS + 1, 10}, {1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->dist, 0);
  EXPECT_EQ(hit->normal, (Vec2{0, 0}));
}

TEST(Raycast, GridEdgeIsSolid) {
  auto g = std::make_unique<Grid>();
  const auto hit = sim::raycast_grid(*g, {10, 50 * CS}, {-1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->cell_x, -1);
  EXPECT_EQ(hit->point.x, 0);
}

//====== BOX CASTS ======//

TEST(Boxcast, StopsFlushAgainstAWall) {
  auto g = grid_with_wall();
  const Box b = box_left_of_wall(10);
  const auto hit = sim::boxcast_grid(*g, b, {1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->center, (Vec2{100 * CS - 2, b.center.y}));
  EXPECT_EQ(hit->dist, 10);
  EXPECT_EQ(hit->normal, (Vec2{-1, 0}));
  EXPECT_EQ(hit->cell_x, 100);
}

TEST(Boxcast, StopsAtMaxDist) {
  auto g = grid_with_wall();
  EXPECT_FALSE(sim::boxcast_grid(*g, box_left_of_wall(10), {1, 0}, 9));
}

TEST(Boxcast, FlushBoxSlidesAlongTheWall) {
  auto g = grid_with_wall();
  EXPECT_FALSE(sim::boxcast_grid(*g, box_left_of_wall(0), {0, 1}, 40));
}

TEST(Boxcast, FlushBoxMovingIntoTheWallHitsAtZero) {
  auto g = grid_with_wall();
  const auto hit = sim::boxcast_grid(*g, box_left_of_wall(0), {1, 0}, 40);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->dist, 0);
  EXPECT_EQ(hit->normal, (Vec2{-1, 0}));
}

TEST(Boxcast, StartingOverlappingHitsAtZero) {
  auto g = grid_with_wall();
  const Box inside{.center = {100 * CS + 2, 50 * CS}, .half = {2, 2}};
  const auto hit = sim::boxcast_grid(*g, inside, {1, 0}, 40);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->dist, 0);
  EXPECT_EQ(hit->normal, (Vec2{0, 0}));
}

TEST(Boxcast, SteepCastDoesntSkipAThinWall) {
  auto g = grid_with_wall();
  const Vec2 dir{0.1f, 0.99498744f};
  const Box b{.center = {99 * CS - 1, 2.5f}, .half = {2, 2}};
  const auto hit = sim::boxcast_grid(*g, b, dir, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->cell_x, 100);
  EXPECT_EQ(hit->normal, (Vec2{-1, 0}));
}

// A cell-sized box moving exactly diagonally at a lone solid cell, so it
// reaches the cell's x and y boundaries at the same moment. It must hit the
// corner, not slip past it, in every direction.
TEST(Boxcast, HitsALoneCellCornerOnExactDiagonals) {
  auto g = std::make_unique<Grid>();
  sim::fill_rect(*g, 100, 100, 100, 100, sim::CellKind::Solid);
  constexpr float D = 0.70710678f;
  for (const Vec2 dir : {Vec2{D, D}, Vec2{-D, D}, Vec2{D, -D}, Vec2{-D, -D}}) {
    // two cells back from the solid one, along -dir
    const Vec2 cell_center{100 * CS + 2, 100 * CS + 2};
    const Vec2 start{cell_center.x - (dir.x > 0 ? 2 : -2) * CS,
                     cell_center.y - (dir.y > 0 ? 2 : -2) * CS};
    const auto hit = sim::boxcast_grid(*g, {start, {2, 2}}, dir, 1000);
    ASSERT_TRUE(hit) << dir.x << ", " << dir.y;
    EXPECT_EQ(hit->cell_x, 100);
    EXPECT_EQ(hit->cell_y, 100);
    // touched once it had moved one cell on each axis
    EXPECT_NEAR(hit->dist, CS * std::sqrt(2.0f), 1e-3f);
  }
}

// Just off the diagonal, so one boundary comes a hair before the other:
// still can't slip past the corner.
TEST(Boxcast, HitsALoneCellCornerJustOffDiagonal) {
  auto g = std::make_unique<Grid>();
  sim::fill_rect(*g, 100, 100, 100, 100, sim::CellKind::Solid);
  const Vec2 start{98 * CS + 2, 98 * CS + 2};
  for (const Vec2 dir : {Vec2{0.7071069f, 0.7071066f},
                         Vec2{0.7071066f, 0.7071069f}}) {
    const auto hit = sim::boxcast_grid(*g, {start, {2, 2}}, dir, 1000);
    ASSERT_TRUE(hit);
    EXPECT_EQ(hit->cell_x, 100);
    EXPECT_EQ(hit->cell_y, 100);
  }
}

//====== ENTITIES ======//

struct Tag {};

entt::entity add_body(entt::registry &r, Vec2 pos) {
  const auto e = r.create();
  r.emplace<sim::Transform>(e, sim::Transform{.pos = pos, .rot = 0});
  sim::Physics p{};
  p.collider = {.w = 4, .h = 4};
  r.emplace<sim::Physics>(e, p);
  return e;
}

TEST(Overlap, VisitsOnlyOverlappingBodiesWithTheComponents) {
  entt::registry r;
  const auto near_tagged = add_body(r, {0, 0});
  r.emplace<Tag>(near_tagged);
  (void)add_body(r, {1, 0}); // near, untagged
  const auto far_tagged = add_body(r, {100, 0});
  r.emplace<Tag>(far_tagged);

  const Box query{.center = {0, 0}, .half = {3, 3}};
  std::vector<entt::entity> seen;
  sim::for_each_overlapping<Tag>(r, query,
                                 [&](entt::entity e) { seen.push_back(e); });
  EXPECT_EQ(seen, std::vector<entt::entity>{near_tagged});

  size_t untagged_seen = 0;
  sim::for_each_overlapping(r, query, [&](entt::entity) { untagged_seen++; });
  EXPECT_EQ(untagged_seen, 2u);
}

TEST(Overlap, FirstOverlappingSkips) {
  entt::registry r;
  const auto a = add_body(r, {0, 0});
  const auto b = add_body(r, {1, 0});
  const Box query{.center = {0, 0}, .half = {3, 3}};
  const auto first = sim::first_overlapping(r, query);
  ASSERT_TRUE(first == a || first == b);
  const auto other = first == a ? b : a;
  EXPECT_EQ(sim::first_overlapping(r, query, first), other);
  EXPECT_EQ(sim::first_overlapping(r, {.center = {50, 50}, .half = {1, 1}}),
            entt::entity{entt::null});
}

TEST(EntityRaycast, NearestWithTheComponentsWins) {
  entt::registry r;
  // created far first, so view order alone wouldn't pick the near one
  const auto far = add_body(r, {40, 0});
  r.emplace<Tag>(far);
  const auto near = add_body(r, {20, 0});
  r.emplace<Tag>(near);
  (void)add_body(r, {10, 0}); // nearest, but untagged

  const auto hit = sim::raycast_entities<Tag>(r, {0, 0}, {1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->entity, near);
  EXPECT_EQ(hit->dist, 18);
  EXPECT_EQ(hit->pos, (Vec2{18, 0}));

  // skipping the nearest gives the next one
  const auto next = sim::raycast_entities<Tag>(r, {0, 0}, {1, 0}, 1000, near);
  ASSERT_TRUE(next);
  EXPECT_EQ(next->entity, far);

  // out of range
  EXPECT_FALSE(sim::raycast_entities<Tag>(r, {0, 0}, {1, 0}, 17));
}

TEST(EntityBoxcast, HitsWhatTheBoxWouldTouch) {
  entt::registry r;
  // 4 x 4 bodies, centered 3 units off the cast's line
  const auto a = add_body(r, {20, 3});
  const auto b = add_body(r, {40, -3});

  // a 4 x 4 box reaches 2 + 2 = 4 off the line, so it clips both; a is
  // first, touched when the boxes' x edges meet at 20 - 4
  const Box box{.center = {0, 0}, .half = {2, 2}};
  const auto hit = sim::boxcast_entities(r, box, {1, 0}, 1000);
  ASSERT_TRUE(hit);
  EXPECT_EQ(hit->entity, a);
  EXPECT_EQ(hit->dist, 16);
  EXPECT_EQ(hit->pos, (Vec2{16, 0}));
  EXPECT_EQ(sim::boxcast_entities(r, box, {1, 0}, 1000, a)->entity, b);

  // a ray down the same line passes between them
  EXPECT_FALSE(sim::raycast_entities(r, {0, 0}, {1, 0}, 1000));
  // a box exactly as wide as the gap only grazes them
  const Box thin{.center = {0, 0}, .half = {2, 1}};
  EXPECT_FALSE(sim::boxcast_entities(r, thin, {1, 0}, 1000));
}

} // namespace
