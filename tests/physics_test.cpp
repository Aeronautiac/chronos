#include "sim/systems/physics/physics.hpp"
#include "sim/tick.hpp"
#include "sim/world.hpp"
#include <cmath>
#include <gtest/gtest.h>
#include <memory>

namespace {

using World = sim::World;

// World is a few MB, keep it off the stack
std::unique_ptr<World> make_world() { return std::make_unique<World>(); }

// a creature with only a transform; physics all zero
sim::EntityID spawn(World &w) { return *sim::spawn_creature(w, {}); }

const sim::Physics &physics(const World &w, sim::EntityID id) {
  return *sim::physics_of(w, id);
}
const sim::Transform &transform(const World &w, sim::EntityID id) {
  return *sim::transform_of(w, id);
}

sim::ModifierID add_vec(World &w, sim::PhysicsModifierKind kind,
                        sim::EntityID target, Vec2 v) {
  sim::PhysicsModifier m{};
  m.vector_val = v;
  m.target = target;
  m.kind = kind;
  return *w.physics_modifiers.insert(m);
}

sim::ModifierID add_scalar(World &w, sim::PhysicsModifierKind kind,
                           sim::EntityID target, float s) {
  sim::PhysicsModifier m{};
  m.scalar_val = s;
  m.target = target;
  m.kind = kind;
  return *w.physics_modifiers.insert(m);
}

TEST(Physics, VelocityModifierLastsOnlyWhileItExists) {
  auto wp = make_world();
  World &w = *wp;
  auto id = spawn(w);
  auto mod = add_vec(w, sim::Velocity, id, {10, 0});

  sim::tick_physics(w, 0.5f);
  const sim::Physics &ep = physics(w, id);
  const sim::Transform &et = transform(w, id);
  EXPECT_EQ(ep.vel, (Vec2{10, 0}));
  EXPECT_EQ(ep.raw_vel, (Vec2{0, 0}));
  EXPECT_EQ(et.pos, (Vec2{5, 0}));

  ASSERT_TRUE(w.physics_modifiers.erase(mod));
  sim::tick_physics(w, 0.5f);
  EXPECT_EQ(ep.vel, (Vec2{0, 0}));
  EXPECT_EQ(et.pos, (Vec2{5, 0}));
}

TEST(Physics, ForceAccumulatesAndPersists) {
  auto wp = make_world();
  World &w = *wp;
  auto id = spawn(w);
  auto mod = add_vec(w, sim::Force, id, {4, 0});

  sim::tick_physics(w, 0.5f);
  const sim::Physics &ep = physics(w, id);
  const sim::Transform &et = transform(w, id);
  EXPECT_EQ(ep.raw_vel, (Vec2{2, 0}));
  EXPECT_EQ(et.pos, (Vec2{1, 0}));

  sim::tick_physics(w, 0.5f);
  EXPECT_EQ(ep.raw_vel, (Vec2{4, 0}));
  EXPECT_EQ(et.pos, (Vec2{3, 0}));

  // no drag: velocity stays after the force is gone
  ASSERT_TRUE(w.physics_modifiers.erase(mod));
  sim::tick_physics(w, 0.5f);
  EXPECT_EQ(ep.vel, (Vec2{4, 0}));
  EXPECT_EQ(et.pos, (Vec2{5, 0}));
}

TEST(Physics, AngularModifiers) {
  auto wp = make_world();
  World &w = *wp;
  auto id = spawn(w);
  add_scalar(w, sim::AngularForce, id, 2);
  add_scalar(w, sim::AngularVelocity, id, 1);

  sim::tick_physics(w, 1.0f);
  const sim::Physics &ep = physics(w, id);
  const sim::Transform &et = transform(w, id);
  EXPECT_FLOAT_EQ(ep.raw_ang_vel, 2);
  EXPECT_FLOAT_EQ(ep.ang_vel, 3);
  EXPECT_FLOAT_EQ(et.rot, 3);
}

TEST(Physics, PrunesModifiersOfDeadEntities) {
  auto wp = make_world();
  World &w = *wp;
  auto alive = spawn(w);
  auto dead = spawn(w);
  auto keep = add_vec(w, sim::Velocity, alive, {1, 0});
  auto stale = add_vec(w, sim::Velocity, dead, {1, 0});
  ASSERT_TRUE(sim::despawn(w, dead));

  sim::tick_physics(w, 1.0f);
  EXPECT_TRUE(w.physics_modifiers.valid_key(keep));
  EXPECT_FALSE(w.physics_modifiers.valid_key(stale));
}

//====== GRAVITY & COLLISION ======//

using sim::Grid;
constexpr float CS = Grid::CELL_SIZE;
constexpr auto COLLIDES =
    static_cast<sim::PhysicsFlags>(sim::PhysicsFlag::Collides);
constexpr auto GRAVITY =
    static_cast<sim::PhysicsFlags>(sim::PhysicsFlag::Gravity);
constexpr float TICK = 1.0f / 240;

// 8x8 box
sim::EntityID spawn_box(World &w, Vec2 pos, sim::PhysicsFlags flags) {
  auto id = *sim::spawn_creature(w, sim::Transform{.pos = pos, .rot = 0});
  sim::Physics &p = *sim::physics_of(w, id);
  p.collider = {.w = 8, .h = 8};
  p.flags = flags;
  return id;
}

TEST(Collision, GravityOnlyWhenFlagged) {
  auto w = make_world();
  auto falls = spawn_box(*w, {400, 400}, GRAVITY);
  auto floats = spawn_box(*w, {600, 400}, 0);

  sim::tick_physics(*w, 0.5f);
  EXPECT_EQ(physics(*w, falls).raw_vel, sim::GRAVITY * 0.5f);
  EXPECT_EQ(physics(*w, floats).raw_vel, (Vec2{0, 0}));
}

TEST(Collision, LandsFlushOnFloorAndStops) {
  auto w = make_world();
  sim::fill_rect(w->grid, 0, 100, Grid::W - 1, 100, sim::CellKind::Solid);
  auto id = spawn_box(*w, {400, 300}, COLLIDES | GRAVITY);

  for (int i = 0; i < 240 * 5; i++) {
    sim::tick_physics(*w, TICK);
  }
  const sim::Physics &ep = physics(*w, id);
  const sim::Transform &et = transform(*w, id);
  // floor's top edge minus half the box
  EXPECT_FLOAT_EQ(et.pos.y, 100 * CS - 4);
  EXPECT_EQ(et.pos.x, 400);
  EXPECT_EQ(ep.vel.y, 0);
  // otherwise gravity keeps piling up while resting
  EXPECT_EQ(ep.raw_vel.y, 0);
}

TEST(Collision, SlidesAlongFloor) {
  auto w = make_world();
  sim::fill_rect(w->grid, 0, 100, Grid::W - 1, 100, sim::CellKind::Solid);
  auto id = spawn_box(*w, {400, 100 * CS - 4}, COLLIDES | GRAVITY);
  add_vec(*w, sim::Velocity, id, {50, 0});

  sim::tick_physics(*w, 0.1f);
  const sim::Transform &et = transform(*w, id);
  EXPECT_FLOAT_EQ(et.pos.x, 405);
  EXPECT_FLOAT_EQ(et.pos.y, 100 * CS - 4);
}

TEST(Collision, FastMoverStopsAtThinWall) {
  auto w = make_world();
  // one cell thick, left face at x = 800, right face at x = 804
  sim::fill_rect(w->grid, 200, 0, 200, Grid::H - 1, sim::CellKind::Solid);
  auto right = spawn_box(*w, {500, 1000}, COLLIDES);
  auto left = spawn_box(*w, {1100, 1000}, COLLIDES);
  // ~416 units in one tick, far more than the wall is thick
  add_vec(*w, sim::Velocity, right, {100000, 0});
  add_vec(*w, sim::Velocity, left, {-100000, 0});

  sim::tick_physics(*w, TICK);
  const sim::Physics &rp = physics(*w, right);
  const sim::Transform &rt = transform(*w, right);
  const sim::Physics &lp = physics(*w, left);
  const sim::Transform &lt = transform(*w, left);
  EXPECT_FLOAT_EQ(rt.pos.x, 200 * CS - 4);
  EXPECT_FLOAT_EQ(lt.pos.x, 201 * CS + 4);
  EXPECT_EQ(rp.vel.x, 0);
  EXPECT_EQ(lp.vel.x, 0);
}

TEST(Collision, FlushAgainstWallStillFalls) {
  auto w = make_world();
  sim::fill_rect(w->grid, 200, 0, 200, Grid::H - 1, sim::CellKind::Solid);
  auto id = spawn_box(*w, {200 * CS - 4, 1000}, COLLIDES | GRAVITY);
  // pressing into the wall while falling
  add_vec(*w, sim::Velocity, id, {50, 0});

  for (int i = 0; i < 10; i++) {
    sim::tick_physics(*w, TICK);
  }
  const sim::Transform &et = transform(*w, id);
  EXPECT_FLOAT_EQ(et.pos.x, 200 * CS - 4);
  EXPECT_GT(et.pos.y, 1000);
}

TEST(Collision, GridEdgeIsSolid) {
  auto w = make_world();
  auto id = spawn_box(*w, {20, 20}, COLLIDES);
  add_vec(*w, sim::Velocity, id, {-1e9f, -1e9f});

  sim::tick_physics(*w, TICK);
  EXPECT_EQ(transform(*w, id).pos, (Vec2{4, 4}));
}

// a walker on a floor at row 100 (top edge y = 400), settled for one tick so
// on_ground is set
sim::EntityID spawn_walker(World &w, float x, float step_height) {
  auto id = spawn_box(w, {x, 100 * CS - 4}, COLLIDES | GRAVITY);
  sim::physics_of(w, id)->step_height = step_height;
  sim::tick_physics(w, TICK);
  EXPECT_TRUE(physics(w, id).on_ground);
  return id;
}

TEST(Steps, ClimbsLedgeUpToStepHeight) {
  auto w = make_world();
  sim::fill_rect(w->grid, 0, 100, Grid::W - 1, 100, sim::CellKind::Solid);
  // one cell high, starting at x = 440
  sim::fill_rect(w->grid, 110, 99, Grid::W - 1, 99, sim::CellKind::Solid);
  auto id = spawn_walker(*w, 400, 2 * CS);
  add_vec(*w, sim::Velocity, id, {100, 0});

  for (int i = 0; i < 240; i++) {
    sim::tick_physics(*w, TICK);
  }
  const sim::Physics &ep = physics(*w, id);
  const sim::Transform &et = transform(*w, id);
  EXPECT_GT(et.pos.x, 450);
  EXPECT_FLOAT_EQ(et.pos.y, 99 * CS - 4);
  EXPECT_TRUE(ep.on_ground);
}

TEST(Steps, TallerLedgeIsAWall) {
  auto w = make_world();
  sim::fill_rect(w->grid, 0, 100, Grid::W - 1, 100, sim::CellKind::Solid);
  // three cells high
  sim::fill_rect(w->grid, 110, 97, Grid::W - 1, 99, sim::CellKind::Solid);
  auto id = spawn_walker(*w, 400, 2 * CS);
  add_vec(*w, sim::Velocity, id, {100, 0});

  for (int i = 0; i < 120; i++) {
    sim::tick_physics(*w, TICK);
  }
  const sim::Physics &ep = physics(*w, id);
  const sim::Transform &et = transform(*w, id);
  EXPECT_FLOAT_EQ(et.pos.x, 110 * CS - 4);
  EXPECT_FLOAT_EQ(et.pos.y, 100 * CS - 4);
  EXPECT_EQ(ep.wall_dir, 1);
}

TEST(Steps, FollowsSmallDropsWithoutLeavingGround) {
  auto w = make_world();
  // floor drops by one cell at x = 440
  sim::fill_rect(w->grid, 0, 100, 109, 100, sim::CellKind::Solid);
  sim::fill_rect(w->grid, 110, 101, Grid::W - 1, 101, sim::CellKind::Solid);
  auto id = spawn_walker(*w, 400, 2 * CS);
  add_vec(*w, sim::Velocity, id, {100, 0});

  for (int i = 0; i < 120; i++) {
    sim::tick_physics(*w, TICK);
    ASSERT_TRUE(physics(*w, id).on_ground) << "tick " << i;
  }
  EXPECT_FLOAT_EQ(transform(*w, id).pos.y, 101 * CS - 4);
}

TEST(Steps, FallsOffTallerDrops) {
  auto w = make_world();
  // floor drops by four cells at x = 440
  sim::fill_rect(w->grid, 0, 100, 109, 100, sim::CellKind::Solid);
  sim::fill_rect(w->grid, 110, 104, Grid::W - 1, 104, sim::CellKind::Solid);
  auto id = spawn_walker(*w, 400, 2 * CS);
  add_vec(*w, sim::Velocity, id, {100, 0});

  bool left_ground = false;
  for (int i = 0; i < 240; i++) {
    sim::tick_physics(*w, TICK);
    left_ground |= !physics(*w, id).on_ground;
  }
  EXPECT_TRUE(left_ground);
  EXPECT_FLOAT_EQ(transform(*w, id).pos.y, 104 * CS - 4);
}

TEST(Collision, WallDirWhilePushing) {
  auto w = make_world();
  sim::fill_rect(w->grid, 200, 0, 200, Grid::H - 1, sim::CellKind::Solid);
  sim::fill_rect(w->grid, 0, 0, 0, Grid::H - 1, sim::CellKind::Solid);
  auto right = spawn_box(*w, {700, 1000}, COLLIDES);
  auto left = spawn_box(*w, {10, 1000}, COLLIDES);
  auto free = spawn_box(*w, {400, 1000}, COLLIDES);
  add_vec(*w, sim::Velocity, right, {1000, 0});
  add_vec(*w, sim::Velocity, left, {-1000, 0});
  add_vec(*w, sim::Velocity, free, {10, 0});

  for (int i = 0; i < 60; i++) {
    sim::tick_physics(*w, TICK);
  }
  EXPECT_EQ(physics(*w, right).wall_dir, 1);
  EXPECT_EQ(physics(*w, left).wall_dir, -1);
  EXPECT_EQ(physics(*w, free).wall_dir, 0);
}

TEST(Collision, RidgeBelowMidpointIsNotAWall) {
  auto w = make_world();
  // one cell high ridge, left face at x = 440, top at y = 396
  sim::fill_rect(w->grid, 110, 99, 110, 99, sim::CellKind::Solid);
  // 24 tall box in the air with its feet level with the ridge: middle at
  // y = 388, above the ridge
  auto ridge = spawn_box(*w, {430, 388}, COLLIDES);
  sim::physics_of(*w, ridge)->collider.h = 24;
  // ~41 units in one tick, enough to reach the ridge
  add_vec(*w, sim::Velocity, ridge, {10000, 0});

  sim::tick_physics(*w, TICK);
  // stopped by it, but it doesn't count as a wall
  EXPECT_FLOAT_EQ(transform(*w, ridge).pos.x, 110 * CS - 4);
  EXPECT_EQ(physics(*w, ridge).wall_dir, 0);
}

//====== DRIVES ======//

sim::ModifierID add_drive(World &w, sim::EntityID target,
                          sim::DriveParams d) {
  sim::PhysicsModifier m{};
  m.drive = d;
  m.target = target;
  m.kind = sim::Drive;
  return *w.physics_modifiers.insert(m);
}

TEST(Drives, LinearDriveArrivesExactlyAtItsRate) {
  auto w = make_world();
  auto id = spawn(*w);
  add_drive(*w, id, {.target = {100, 0}, .accel = {1000, 0}, .damping = {}});

  sim::tick_physics(*w, 0.01f);
  EXPECT_FLOAT_EQ(physics(*w, id).raw_vel.x, 10);
  for (int i = 0; i < 20; i++) {
    sim::tick_physics(*w, 0.01f);
  }
  EXPECT_FLOAT_EQ(physics(*w, id).raw_vel.x, 100);
}

TEST(Drives, DisagreeingDrivesSettleAtWeightedAverage) {
  auto w = make_world();
  auto id = spawn(*w);
  // twice as strong toward 120 as toward 0: settles at 80
  add_drive(*w, id, {.target = {120, 0}, .accel = {1200, 0}, .damping = {}});
  add_drive(*w, id, {.target = {0, 0}, .accel = {600, 0}, .damping = {}});

  for (int i = 0; i < 240; i++) {
    sim::tick_physics(*w, TICK);
  }
  EXPECT_NEAR(physics(*w, id).raw_vel.x, 80, 1e-3);
}

TEST(Drives, AxisWithoutStrengthIsUntouched) {
  auto w = make_world();
  auto id = spawn(*w);
  sim::physics_of(*w, id)->raw_vel = {0, 50};
  // a target on y, but no strength on y
  add_drive(*w, id, {.target = {100, -500}, .accel = {1000, 0}, .damping = {}});

  sim::tick_physics(*w, 0.1f);
  EXPECT_EQ(physics(*w, id).raw_vel.y, 50);
}

TEST(Drives, DampingEasesOutExponentially) {
  auto w = make_world();
  auto id = spawn(*w);
  sim::physics_of(*w, id)->raw_vel = {100, 0};
  add_drive(*w, id, {.target = {0, 0}, .accel = {}, .damping = {2, 0}});

  sim::tick_physics(*w, 0.5f);
  // e^(-2 * 0.5) of the gap is left
  EXPECT_NEAR(physics(*w, id).raw_vel.x, 100 * std::exp(-1.0f), 1e-3);
}

TEST(Drives, ResultDoesntDependOnModifierOrder) {
  auto a = make_world();
  auto b = make_world();
  auto ia = spawn(*a);
  auto ib = spawn(*b);
  const sim::DriveParams d1{.target = {120, 0}, .accel = {900, 0},
                            .damping = {1, 0}};
  const sim::DriveParams d2{.target = {-40, 0}, .accel = {300, 0},
                            .damping = {3, 0}};
  add_drive(*a, ia, d1);
  add_drive(*a, ia, d2);
  add_drive(*b, ib, d2);
  add_drive(*b, ib, d1);

  for (int i = 0; i < 60; i++) {
    sim::tick_physics(*a, TICK);
    sim::tick_physics(*b, TICK);
  }
  EXPECT_NEAR(physics(*a, ia).raw_vel.x, physics(*b, ib).raw_vel.x, 1e-4);
}

TEST(Collision, NonCollidingPassesThrough) {
  auto w = make_world();
  sim::fill_rect(w->grid, 200, 0, 200, Grid::H - 1, sim::CellKind::Solid);
  auto id = spawn_box(*w, {500, 1000}, 0);
  add_vec(*w, sim::Velocity, id, {100000, 0});

  sim::tick_physics(*w, TICK);
  EXPECT_GT(transform(*w, id).pos.x, 201 * CS);
}

} // namespace
