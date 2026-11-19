// The sim's hard guarantees (see PORT_TO_ENTT.md):
//   1. determinism: same start state + same inputs -> identical state
//   2. exact restore: restoring a snapshot is indistinguishable from never
//      having left it (entity ids, versions, free list, storage order)
//   4. handles never dangle
//   5. no allocation in a tick
// Gameplay only appears as a workload that churns entities.
#include "sim/sim.hpp"
#include "sim/snapshot.hpp"
#include "sim/timeline.hpp"
#include "sim/world.hpp"
#include <cstddef>
#include <cstdlib>
#include <gtest/gtest.h>
#include <memory>
#include <new>
#include <span>
#include <vector>

//====== ALLOCATION COUNTING (guarantee 5) ======//

// Every global operator new in this test binary goes through here, so a
// test can count what a stretch of code allocates. All forms are replaced
// (the sanitizer runtime and shared gtest use several), and all of them
// allocate with malloc / aligned_alloc and free with free, so they match.
namespace {
bool g_counting = false;
size_t g_allocs = 0;

void *counted_alloc(std::size_t n, std::size_t align) noexcept {
  if (g_counting) {
    g_allocs += 1;
  }
  n = n == 0 ? 1 : n;
  if (align <= alignof(std::max_align_t)) {
    return std::malloc(n);
  }
  // aligned_alloc wants the size to be a multiple of the alignment
  return std::aligned_alloc(align, (n + align - 1) / align * align);
}

void *counted_alloc_or_abort(std::size_t n, std::size_t align) noexcept {
  void *p = counted_alloc(n, align);
  if (!p) {
    std::abort();
  }
  return p;
}
} // namespace

constexpr std::size_t DEFAULT_ALIGN = alignof(std::max_align_t);
using std::align_val_t;
using std::nothrow_t;

void *operator new(std::size_t n) { return counted_alloc_or_abort(n, DEFAULT_ALIGN); }
void *operator new[](std::size_t n) { return counted_alloc_or_abort(n, DEFAULT_ALIGN); }
void *operator new(std::size_t n, align_val_t a) {
  return counted_alloc_or_abort(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, align_val_t a) {
  return counted_alloc_or_abort(n, static_cast<std::size_t>(a));
}
void *operator new(std::size_t n, const nothrow_t &) noexcept {
  return counted_alloc(n, DEFAULT_ALIGN);
}
void *operator new[](std::size_t n, const nothrow_t &) noexcept {
  return counted_alloc(n, DEFAULT_ALIGN);
}
void *operator new(std::size_t n, align_val_t a, const nothrow_t &) noexcept {
  return counted_alloc(n, static_cast<std::size_t>(a));
}
void *operator new[](std::size_t n, align_val_t a, const nothrow_t &) noexcept {
  return counted_alloc(n, static_cast<std::size_t>(a));
}

void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
void operator delete(void *p, align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, align_val_t) noexcept { std::free(p); }
void operator delete(void *p, std::size_t, align_val_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t, align_val_t) noexcept { std::free(p); }
void operator delete(void *p, const nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, const nothrow_t &) noexcept { std::free(p); }
void operator delete(void *p, align_val_t, const nothrow_t &) noexcept { std::free(p); }
void operator delete[](void *p, align_val_t, const nothrow_t &) noexcept {
  std::free(p);
}

namespace {

//====== WORKLOAD ======//

constexpr int CREATURES = 12;

// World time per workload step: 240 Hz, so the workloads churn quickly.
// Its own constant, not the sim's base tick, so the tests don't change
// when that does.
constexpr sim::TimeInt STEP = static_cast<sim::TimeInt>(1e9 / 240);

struct Step {
  sim::SimInput input;
  sim::TimeInt dt;
};

// Maps shared by every sim, the way they're meant to be used: static, and
// pointed at rather than copied.
const sim::Grid &test_level() {
  static const std::unique_ptr<sim::Grid> map = [] {
    auto g = std::make_unique<sim::Grid>();
    sim::fill_test_level(*g);
    return g;
  }();
  return *map;
}

// Just the test level's ground, nothing on it, so creatures on the test
// level's flat stretch land the same after a switch.
const sim::Grid &flat_level() {
  static const std::unique_ptr<sim::Grid> map = [] {
    auto g = std::make_unique<sim::Grid>();
    sim::fill_rect(*g, 0, sim::TEST_LEVEL_GROUND, sim::Grid::W - 1,
                   sim::Grid::H - 1, sim::CellKind::Solid);
    return g;
  }();
  return *map;
}

// A fresh sim on the test level, set through the input like any map switch.
std::unique_ptr<sim::Sim> make_sim() {
  auto s = std::make_unique<sim::Sim>();
  EXPECT_TRUE(s->exec(sim::SetMap{.map = &test_level()}, 0));
  return s;
}

// On the test level's flat stretch, 16 units apart: close enough that each
// one's slash reaches the next.
Vec2 creature_pos(int i) {
  return {(static_cast<float>(sim::FLAT_X0) + 40 + 4 * static_cast<float>(i)) *
              sim::Grid::CELL_SIZE,
          (sim::TEST_LEVEL_GROUND - 3) * sim::Grid::CELL_SIZE};
}

// The workload helpers run on a Sim or a Timeline (anything with exec).
template <typename Runner>
std::vector<entt::entity> spawn_creatures(Runner &s) {
  std::vector<entt::entity> ids;
  for (int i = 0; i < CREATURES; i++) {
    auto res = s.exec(sim::SpawnCreature{.pos = creature_pos(i)}, 0);
    EXPECT_TRUE(res);
    ids.push_back(std::get<entt::entity>(*res));
  }
  return ids;
}

sim::MovementIntents intents(std::initializer_list<sim::MovementIntent> list) {
  sim::MovementIntents out = 0;
  for (sim::MovementIntent i : list) {
    out |= static_cast<sim::MovementIntents>(i);
  }
  return out;
}

// the log switches to flat_level at this step
constexpr int MAP_SWITCH_STEP = 2200;

// A fixed input log that churns entities: the creatures walk, jump, burst,
// fire and release hooks (sticking to each other) and slash each other to
// death, while new creatures spawn into the freed ids. Inputs to creatures
// that have died are stale handles. At MAP_SWITCH_STEP the map switches.
// One input per STEP of world time.
std::vector<Step> make_log(std::span<const entt::entity> ids, int steps) {
  using sim::MovementIntent;
  std::vector<Step> log;
  const auto n = static_cast<int>(ids.size());
  for (int t = 0; t < steps; t++) {
    if (t == MAP_SWITCH_STEP) {
      log.push_back({sim::SetMap{.map = &flat_level()}, STEP});
      continue;
    }
    if (t % 97 == 0) {
      log.push_back({sim::SpawnCreature{.pos = creature_pos(t % n)},
                     STEP});
      continue;
    }
    const int round = t / n;
    sim::MovementIntents in = 0;
    if (round % 2 == 1) {
      in |= intents({MovementIntent::Attack});
    }
    if ((round / 10) % 2 == 0) {
      in |= intents({MovementIntent::Hook});
    }
    in |= intents({(round / 20) % 2 == 0 ? MovementIntent::Right
                                         : MovementIntent::Left});
    if (round % 15 == 0) {
      in |= intents({MovementIntent::Jump});
    }
    if (round % 7 == 3) {
      in |= intents({MovementIntent::Burst});
    }
    const Vec2 aim = (round / 10) % 3 == 0 ? Vec2{0.6f, -0.8f} : Vec2{1, 0};
    log.push_back({sim::SetMovementIntents{.intents = in,
                                           .aim = aim,
                                           .target = ids[static_cast<size_t>(t % n)]},
                   STEP});
  }
  return log;
}

// What a run went through, to check the workload covers what it should.
struct RunStats {
  int stale_inputs = 0;
  size_t max_hooks = 0;
};

const sim::Sim &sim_of(const sim::Sim &s) { return s; }
const sim::Sim &sim_of(const sim::Timeline &t) { return t.sim(); }

size_t hook_count(const sim::Sim &s) {
  const auto *hooks = s.world().reg.storage<sim::Hook>();
  return hooks ? hooks->size() : 0;
}

template <typename Runner>
RunStats run(Runner &s, std::span<const Step> log) {
  RunStats stats;
  for (const Step &step : log) {
    auto res = s.exec(step.input, step.dt);
    if (!res && res.error() == sim::SimErr::InvalidEntity) {
      stats.stale_inputs += 1;
    }
    stats.max_hooks = std::max(stats.max_hooks, hook_count(sim_of(s)));
  }
  return stats;
}

//====== DETERMINISM AND EXACT RESTORE ======//

// Keyframe -> run -> state A; rewind the same world to the keyframe ->
// replay the same inputs -> identical to A. Then one extra tick must show
// up as a difference, so the check isn't vacuous.
TEST(Guarantees, RewindAndReplayIsIdentical) {
  auto live = make_sim();
  const auto ids = spawn_creatures(*live);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> before(log.data(), 1500);
  const std::span<const Step> after(log.data() + 1500, 1500);

  const RunStats churn = run(*live, before);
  // the keyframe is taken mid-churn: deaths, recycled ids, hooks out
  const sim::Snapshot keyframe = live->save();

  const RunStats stats = run(*live, after);
  const sim::Snapshot a = live->save();

  // the map switch in `after` happened, so rewinding has to undo it
  ASSERT_EQ(live->world().grid, &flat_level());
  live->restore(keyframe);
  EXPECT_EQ(live->world().grid, &test_level());
  (void)run(*live, after);

  auto end = make_sim();
  end->restore(a);
  EXPECT_TRUE(sim::same_state(live->world(), end->world()));

  // one more tick (setting the map it's already on changes nothing itself)
  ASSERT_TRUE(live->exec(sim::SetMap{.map = &flat_level()}, STEP));
  EXPECT_FALSE(sim::same_state(live->world(), end->world()));

  // the workload really did churn
  EXPECT_GT(churn.stale_inputs + stats.stale_inputs, 0);
  EXPECT_GT(std::max(churn.max_hooks, stats.max_hooks), 0u);
}

// Two fresh sims, the same inputs from the start: identical.
TEST(Guarantees, SameLogTwiceIsIdentical) {
  auto a = make_sim();
  auto b = make_sim();
  const auto ids_a = spawn_creatures(*a);
  const auto ids_b = spawn_creatures(*b);
  ASSERT_EQ(ids_a, ids_b);
  const auto log = make_log(ids_a, 2000);
  (void)run(*a, log);
  (void)run(*b, log);
  EXPECT_TRUE(sim::same_state(a->world(), b->world()));
}

// After a restore the next ids handed out (recycled ones first, then fresh
// ones) are the ones the original world would have handed out.
TEST(Guarantees, RestoreHandsOutTheSameIds) {
  auto live = make_sim();
  const auto ids = spawn_creatures(*live);
  (void)run(*live, make_log(ids, 1500));
  const auto &entities = *live->world().reg.storage<entt::entity>();
  // destroyed ids waiting to be recycled, so both paths are exercised
  ASSERT_LT(entities.free_list(), entities.size());
  const sim::Snapshot snap = live->save();

  auto restored = make_sim();
  restored->restore(snap);
  EXPECT_TRUE(sim::same_state(live->world(), restored->world()));

  // enough spawns to use up the recycled ids and go on to fresh ones (each
  // creature is one entity)
  const size_t n = entities.size() - entities.free_list() + 4;
  for (size_t i = 0; i < n; i++) {
    const sim::SpawnCreature spawn{.pos = creature_pos(0)};
    EXPECT_EQ(std::get<entt::entity>(*live->exec(spawn, 0)),
              std::get<entt::entity>(*restored->exec(spawn, 0)));
  }
}

// The map isn't copied, but which one is part of the state: worlds on
// different maps differ, and rewinding past a switch puts the old one back.
TEST(Guarantees, RestoreBringsBackItsMap) {
  auto s = make_sim();
  (void)spawn_creatures(*s);
  const sim::Snapshot snap = s->save();

  ASSERT_TRUE(s->exec(sim::SetMap{.map = &flat_level()}, 0));
  auto on_test_level = make_sim();
  on_test_level->restore(snap);
  EXPECT_FALSE(sim::same_state(s->world(), on_test_level->world()));

  s->restore(snap);
  EXPECT_EQ(s->world().grid, &test_level());
  EXPECT_TRUE(sim::same_state(s->world(), on_test_level->world()));
}

// A rejected input is checked against the state at its time and changes
// nothing, but its time still passes: the same as no input at all.
TEST(Guarantees, RejectedInputsOnlyPassTime) {
  auto s = make_sim();
  const auto ids = spawn_creatures(*s);
  (void)run(*s, make_log(ids, 300));
  const sim::Snapshot before = s->save();
  auto just_time = make_sim();
  just_time->restore(before);

  const sim::SimInput rejected[] = {
      sim::SetMovementIntents{.intents = 0, .aim = {}, .target = entt::null},
      sim::SetMovementParams{.params = {}, .target = entt::null},
      sim::LookAt{.pos = {}, .target = ids[0]},
      sim::SetMap{.map = nullptr},
  };
  for (const sim::SimInput &in : rejected) {
    ASSERT_FALSE(s->exec(in, STEP));
    just_time->advance(STEP);
    EXPECT_TRUE(sim::same_state(s->world(), just_time->world()));
  }
}

TEST(Guarantees, NullMapIsRejected) {
  auto s = make_sim();
  auto res = s->exec(sim::SetMap{.map = nullptr}, 0);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), sim::SimErr::InvalidMap);
  EXPECT_EQ(s->world().grid, &test_level());
}

//====== ROLLBACK ======//

// Keyframes every 50 steps, 64 kept: the workloads below are 3000 steps,
// so rollbacks cross many keyframes and the
// first one is still kept.
constexpr sim::TimelineConfig TEST_TIMELINE{
    .keyframe_interval = STEP * 50,
    .keyframe_count = 64,
};

std::unique_ptr<sim::Timeline>
make_timeline(sim::TimelineConfig config = TEST_TIMELINE) {
  auto t = std::make_unique<sim::Timeline>(config);
  EXPECT_TRUE(t->exec(sim::SetMap{.map = &test_level()}, 0));
  return t;
}

// A Sim that ran the workload's inputs up to step j, then input j and only
// `into_j` of its dt: the state partway through step j.
std::unique_ptr<sim::Sim> sim_stopped_in_step(std::span<const Step> all,
                                              size_t j, sim::TimeInt into_j) {
  auto s = make_sim();
  (void)spawn_creatures(*s);
  (void)run(*s, all.first(j));
  (void)s->exec(all[j].input, into_j);
  return s;
}

// Rolling back to a time already passed gives exactly the state then, with
// every input up to and including that time: at an input's own time, and
// partway through its dt.
TEST(Rollback, MatchesThePast) {
  auto tl = make_timeline();
  const auto ids = spawn_creatures(*tl);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> all(log);

  const size_t j = MAP_SWITCH_STEP;
  (void)run(*tl, all.first(j));
  const sim::TimeInt t_j = tl->now();
  (void)run(*tl, all.subspan(j));
  const sim::TimeInt end = tl->now();

  ASSERT_TRUE(tl->rollback_to(t_j));
  EXPECT_EQ(tl->now(), t_j);
  EXPECT_TRUE(sim::same_state(tl->sim().world(),
                              sim_stopped_in_step(all, j, 0)->world()));

  // back to the end (the log replays), then partway into step j
  tl->advance(end - t_j);
  const sim::TimeInt half = STEP / 2;
  ASSERT_TRUE(tl->rollback_to(t_j + half));
  EXPECT_TRUE(sim::same_state(tl->sim().world(),
                              sim_stopped_in_step(all, j, half)->world()));
}

// Going back forgets nothing: letting time run again from there replays the
// logged inputs and ends up exactly where it was.
TEST(Rollback, KeepsTheLog) {
  auto tl = make_timeline();
  const auto ids = spawn_creatures(*tl);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> all(log);
  (void)run(*tl, all.first(1000));
  const sim::TimeInt t = tl->now();
  (void)run(*tl, all.subspan(1000));
  const sim::TimeInt end = tl->now();
  const sim::Snapshot at_end = tl->sim().save();

  ASSERT_TRUE(tl->rollback_to(t));
  tl->advance(end - t);
  EXPECT_EQ(tl->now(), end);
  auto expected = make_sim();
  expected->restore(at_end);
  EXPECT_TRUE(sim::same_state(tl->sim().world(), expected->world()));
}

// An input inserted into the past changes the present to what it would have
// been had it been there all along, and keyframes after it are redone.
TEST(Rollback, InsertingIntoThePastAdjustsThePresent) {
  const size_t j = MAP_SWITCH_STEP;
  const sim::TimeInt half = STEP / 2;
  // shifts every later spawn's id, and lands among the creatures
  const sim::SimInput late = sim::SpawnCreature{.pos = creature_pos(5)};

  auto inserted = make_timeline();
  const auto ids = spawn_creatures(*inserted);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> all(log);
  (void)run(*inserted, all.first(j));
  const sim::TimeInt t_j = inserted->now();
  (void)run(*inserted, all.subspan(j));
  ASSERT_TRUE(inserted->insert(late, t_j + half));

  // the same inputs, with `late` in its place from the start
  auto all_along = make_timeline();
  (void)spawn_creatures(*all_along);
  (void)run(*all_along, all.first(j));
  (void)all_along->exec(all[j].input, half);
  (void)all_along->exec(late, all[j].dt - half);
  (void)run(*all_along, all.subspan(j + 1));

  ASSERT_EQ(inserted->now(), all_along->now());
  EXPECT_TRUE(sim::same_state(inserted->sim().world(), all_along->sim().world()));

  // keyframes redone after the insert: going back past it still matches
  const sim::TimeInt later = t_j + 300 * STEP;
  ASSERT_TRUE(inserted->rollback_to(later));
  ASSERT_TRUE(all_along->rollback_to(later));
  EXPECT_TRUE(sim::same_state(inserted->sim().world(), all_along->sim().world()));
}

// Rolling back and then taking a new input at the present changes the
// future too: keyframes from before the rollback must not be reused. (The
// demo's hologram does this on every input.)
TEST(Rollback, InputAfterRollbackRedoesLaterKeyframes) {
  const size_t j = MAP_SWITCH_STEP;
  const sim::TimeInt half = STEP / 2;
  const sim::SimInput late = sim::SpawnCreature{.pos = creature_pos(5)};

  auto rewound = make_timeline();
  const auto ids = spawn_creatures(*rewound);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> all(log);
  (void)run(*rewound, all.first(j));
  const sim::TimeInt t_j = rewound->now();
  (void)run(*rewound, all.subspan(j));
  const sim::TimeInt end = rewound->now();
  // back partway into step j, a new input there, then on to the end (the
  // later logged inputs play again)
  ASSERT_TRUE(rewound->rollback_to(t_j + half));
  (void)rewound->exec(late, 0);
  rewound->advance(end - rewound->now());

  auto all_along = make_timeline();
  (void)spawn_creatures(*all_along);
  (void)run(*all_along, all.first(j));
  (void)all_along->exec(all[j].input, half);
  (void)all_along->exec(late, all[j].dt - half);
  (void)run(*all_along, all.subspan(j + 1));

  ASSERT_EQ(rewound->now(), all_along->now());
  EXPECT_TRUE(sim::same_state(rewound->sim().world(), all_along->sim().world()));
  // going back past the new input restores its keyframes, not stale ones
  const sim::TimeInt later = t_j + 300 * STEP;
  ASSERT_TRUE(rewound->rollback_to(later));
  ASSERT_TRUE(all_along->rollback_to(later));
  EXPECT_TRUE(sim::same_state(rewound->sim().world(), all_along->sim().world()));
}

// An input inserted ahead of now applies when time gets there.
TEST(Rollback, InsertingIntoTheFutureWaitsForIt) {
  const sim::TimeInt half = STEP / 2;
  const sim::SimInput later = sim::SpawnCreature{.pos = creature_pos(5)};

  auto ahead = make_timeline();
  (void)spawn_creatures(*ahead);
  ASSERT_TRUE(ahead->insert(later, ahead->now() + half));
  ahead->advance(STEP);

  auto on_time = make_timeline();
  (void)spawn_creatures(*on_time);
  on_time->advance(half);
  (void)on_time->exec(later, STEP - half);

  EXPECT_TRUE(sim::same_state(ahead->sim().world(), on_time->sim().world()));
}

// A timeline started from a snapshot carries on exactly like the sim the
// snapshot came from, and can roll back and take inserts on its own clock.
TEST(Rollback, StartsFromASnapshot) {
  auto original = make_timeline();
  const auto ids = spawn_creatures(*original);
  const auto log = make_log(ids, 3000);
  const std::span<const Step> all(log);
  (void)run(*original, all.first(1500));

  sim::Timeline copy(TEST_TIMELINE, original->sim().save());
  EXPECT_EQ(copy.now(), 0u);
  EXPECT_TRUE(sim::same_state(original->sim().world(), copy.sim().world()));

  (void)run(*original, all.subspan(1500));
  (void)run(copy, all.subspan(1500));
  EXPECT_TRUE(sim::same_state(original->sim().world(), copy.sim().world()));

  // and it rewinds within its own history
  const sim::TimeInt back = 500 * STEP;
  ASSERT_TRUE(original->rollback_to(original->now() - back));
  ASSERT_TRUE(copy.rollback_to(copy.now() - back));
  EXPECT_TRUE(sim::same_state(original->sim().world(), copy.sim().world()));
}

TEST(Rollback, OnlyWithinTheKeptRange) {
  // 4 keyframes 10 ticks apart: about 40 ticks of history
  auto tl = make_timeline({.keyframe_interval = STEP * 10,
                           .keyframe_count = 4});
  const auto ids = spawn_creatures(*tl);
  (void)run(*tl, make_log(ids, 500));

  EXPECT_GT(tl->earliest(), 0u);
  EXPECT_LE(tl->now() - tl->earliest(), 4 * 10 * STEP);
  EXPECT_EQ(tl->rollback_to(0).error(), sim::TimelineErr::TooFarBack);
  EXPECT_EQ(tl->insert(sim::SpawnCreature{.pos = creature_pos(0)}, 0).error(),
            sim::TimelineErr::TooFarBack);
  EXPECT_EQ(tl->rollback_to(tl->now() + 1).error(), sim::TimelineErr::InFuture);
  EXPECT_TRUE(tl->rollback_to(tl->earliest()));
  EXPECT_EQ(tl->now(), tl->earliest());
}

//====== HANDLES ======//

// A dead entity's handle finds nothing, even once its slot is reused. The
// churn workload kills creatures and spawns new ones into their slots.
TEST(Guarantees, StaleHandlesFindNothing) {
  auto s = make_sim();
  const auto ids = spawn_creatures(*s);
  (void)run(*s, make_log(ids, 1500));

  // a creature from the start that died, and whoever got its slot
  entt::entity old_id = entt::null;
  entt::entity new_id = entt::null;
  for (const entt::entity id : ids) {
    for (const entt::entity e : s->world().reg.view<const sim::Movement>()) {
      if (e != id && entt::to_entity(e) == entt::to_entity(id)) {
        old_id = id;
        new_id = e;
      }
    }
  }
  ASSERT_NE(old_id, entt::entity{entt::null});
  ASSERT_FALSE(s->world().reg.valid(old_id));

  auto stale = s->exec(
      sim::SetMovementIntents{.intents = 0, .aim = {}, .target = old_id}, 0);
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error(), sim::SimErr::InvalidEntity);
  EXPECT_TRUE(s->exec(
      sim::SetMovementIntents{.intents = 0, .aim = {}, .target = new_id}, 0));
}

//====== NO ALLOCATION ======//

// Ticks, spawns into reserved storages and destroys never allocate: not on
// a fresh world, and not after a restore.
TEST(Guarantees, TicksDontAllocate) {
  auto s = make_sim();
  const auto ids = spawn_creatures(*s);
  const auto log = make_log(ids, 1500);

  g_allocs = 0;
  g_counting = true;
  const RunStats stats = run(*s, log);
  g_counting = false;
  EXPECT_EQ(g_allocs, 0u);
  EXPECT_GT(stats.max_hooks, 0u);

  const sim::Snapshot snap = s->save();
  s->restore(snap);
  g_allocs = 0;
  g_counting = true;
  (void)run(*s, log);
  g_counting = false;
  EXPECT_EQ(g_allocs, 0u);
}

//====== SNAPSHOT COVERAGE ======//

// a component the sim doesn't list in EntityState
struct Stray {
  int x;
};

// The sim itself only ever emplaces listed types, so this goes to the
// registry layer directly.
TEST(Guarantees, SavingAnUnlistedComponentAsserts) {
  entt::registry r;
  const auto e = r.create();
  r.emplace<Stray>(e, Stray{.x = 1});
  EXPECT_DEBUG_DEATH((void)sim::EntityState::save(r), "");
}

} // namespace
