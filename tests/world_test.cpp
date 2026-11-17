#include "sim/sim.hpp"
#include "sim/world.hpp"
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <type_traits>

namespace {

using sim::World;

static_assert(std::is_trivially_copyable_v<sim::Sim>);

std::unique_ptr<World> make_world() { return std::make_unique<World>(); }

sim::EntityID spawn_at(World &w, float x) {
  return *sim::spawn_creature(w, sim::Transform{.pos = {x, 0}, .rot = 0});
}

TEST(World, SpawnPacksRows) {
  auto w = make_world();
  auto a = spawn_at(*w, 1);
  auto b = spawn_at(*w, 2);
  EXPECT_EQ(w->creatures.len, 2u);
  EXPECT_EQ(*sim::creature_row(*w, a), 0u);
  EXPECT_EQ(*sim::creature_row(*w, b), 1u);
  EXPECT_EQ(w->creatures.id[1], b);
  EXPECT_EQ(sim::transform_of(*w, b)->pos.x, 2);
}

TEST(World, DespawnMovesLastRowIntoGap) {
  auto w = make_world();
  auto a = spawn_at(*w, 1);
  auto b = spawn_at(*w, 2);
  auto c = spawn_at(*w, 3);
  ASSERT_TRUE(sim::despawn(*w, a));

  EXPECT_EQ(w->creatures.len, 2u);
  // c moved into a's row, and its key still finds it
  EXPECT_EQ(*sim::creature_row(*w, c), 0u);
  EXPECT_EQ(w->creatures.id[0], c);
  EXPECT_EQ(sim::transform_of(*w, c)->pos.x, 3);
  EXPECT_EQ(sim::transform_of(*w, b)->pos.x, 2);
}

TEST(World, DeadKeysFindNothing) {
  auto w = make_world();
  auto a = spawn_at(*w, 1);
  ASSERT_TRUE(sim::despawn(*w, a));
  auto b = spawn_at(*w, 2);

  EXPECT_FALSE(sim::creature_row(*w, a));
  EXPECT_EQ(sim::physics_of(*w, a), nullptr);
  EXPECT_EQ(sim::transform_of(*w, a), nullptr);
  auto res = sim::despawn(*w, a);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), sim::SimErr::InvalidEntity);
  // the new entity is unaffected by the stale key
  EXPECT_EQ(sim::transform_of(*w, b)->pos.x, 2);
}

TEST(World, CreatureTableFull) {
  auto w = make_world();
  for (uint32_t i = 0; i < sim::CreatureTable::N; i++) {
    ASSERT_TRUE(sim::spawn_creature(*w, {}));
  }
  auto res = sim::spawn_creature(*w, {});
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), sim::SimErr::EntityLimit);
}

//====== TIME ======//

TEST(Time, TickIsBaseWhenNothingIsFast) {
  auto s = std::make_unique<sim::Sim>();
  (void)spawn_at(s->world, 100);
  EXPECT_EQ(s->tick_dur(), sim::BASE_TICK);
}

TEST(Time, FastestBodySetsTheTick) {
  auto s = std::make_unique<sim::Sim>();
  auto slow = spawn_at(s->world, 100);
  auto fast = spawn_at(s->world, 200);
  sim::physics_of(s->world, slow)->vel = {100, 0};
  sim::physics_of(s->world, fast)->vel = {0, 4000};
  // the fast one moves at most TICK_MAX_TRAVEL per tick
  const double secs = static_cast<double>(s->tick_dur()) / 1e9;
  EXPECT_NEAR(secs * 4000, sim::TICK_MAX_TRAVEL, 0.01);
}

TEST(Time, HighestPerceptionShortensTheTick) {
  auto s = std::make_unique<sim::Sim>();
  auto a = spawn_at(s->world, 100);
  auto b = spawn_at(s->world, 200);
  s->world.creatures.perception[*sim::creature_row(s->world, a)].value = 2;
  s->world.creatures.perception[*sim::creature_row(s->world, b)].value = 10;
  EXPECT_NEAR(static_cast<double>(s->tick_dur()),
              static_cast<double>(sim::BASE_TICK) / 10, 1);
}

TEST(Time, PerceptionRisesWithSpeed) {
  auto s = std::make_unique<sim::Sim>();
  auto res = s->exec(
      sim::SpawnEntity{.kind = sim::EntityKind::Creature, .pos = {4000, 400}},
      0);
  ASSERT_TRUE(res);
  const auto id = std::get<sim::EntityID>(*res);
  const uint32_t row = *sim::creature_row(s->world, id);
  // in the air, very fast sideways
  s->world.creatures.physics[row].raw_vel = {3000, 0};
  for (int i = 0; i < 60; i++) {
    auto ticked = s->exec(
        sim::SetMovementIntents{.intents = 0, .aim = {}, .target = id},
        s->tick_dur());
    ASSERT_TRUE(ticked);
  }
  EXPECT_GT(s->world.creatures.perception[row].value, 2);
}

// The whole point of the layout: a snapshot is a byte copy, and replaying
// the same inputs from it gives exactly the same result, storage order
// included.
TEST(World, ByteCopySnapshotReplaysIdentically) {
  auto live = std::make_unique<sim::Sim>();
  sim::fill_test_level(live->world.grid);

  std::vector<sim::EntityID> ids;
  for (int i = 0; i < 20; i++) {
    auto res = live->exec(
        sim::SpawnEntity{.kind = sim::EntityKind::Creature,
                         .pos = {200.0f + 40.0f * static_cast<float>(i), 1700}},
        0);
    ASSERT_TRUE(res);
    ids.push_back(std::get<sim::EntityID>(*res));
  }
  // churn the tables so storage order isn't just spawn order
  ASSERT_TRUE(sim::despawn(live->world, ids[3]));
  ASSERT_TRUE(sim::despawn(live->world, ids[11]));

  auto snapshot = std::make_unique<sim::Sim>();
  std::memcpy(static_cast<void *>(snapshot.get()), live.get(),
              sizeof(sim::Sim));

  auto run = [&](sim::Sim &s) {
    for (int t = 0; t < 600; t++) {
      // alternate walking and jumping for whoever is still alive
      sim::MovementIntents in =
          static_cast<sim::MovementIntents>(sim::MovementIntent::Right);
      if (t % 90 == 0) {
        in |= static_cast<sim::MovementIntents>(sim::MovementIntent::Jump);
      }
      (void)s.exec(
          sim::SetMovementIntents{.intents = in, .aim = {}, .target = ids[0]},
          s.tick_dur());
    }
  };
  run(*live);
  run(*snapshot);

  const sim::CreatureTable &a = live->world.creatures;
  const sim::CreatureTable &b = snapshot->world.creatures;
  ASSERT_EQ(a.len, b.len);
  for (uint32_t i = 0; i < a.len; i++) {
    EXPECT_EQ(a.id[i], b.id[i]);
    EXPECT_EQ(a.transform[i].pos, b.transform[i].pos);
    EXPECT_EQ(a.physics[i].vel, b.physics[i].vel);
  }
}

} // namespace
