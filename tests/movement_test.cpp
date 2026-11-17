// Movement is tuned by feel, so these only cover how movement states start,
// end, block and cancel each other, and press vs hold behaviour. No speeds.
#include "sim/sim.hpp"
#include "sim/systems/movement/movement.hpp"
#include "sim/world.hpp"
#include <gtest/gtest.h>
#include <memory>

namespace {

using sim::MovementIntent;
using sim::MoveState;

sim::MovementIntents intents(std::initializer_list<MovementIntent> list) {
  sim::MovementIntents out = 0;
  for (MovementIntent i : list) {
    out |= static_cast<sim::MovementIntents>(i);
  }
  return out;
}

constexpr float CS = sim::Grid::CELL_SIZE;
// flat floor whose top edge is at y = 1200
constexpr int32_t FLOOR_ROW = 300;
constexpr float FLOOR_Y = FLOOR_ROW * CS;
// creature colliders are 12 x 24, so this is standing on the floor
constexpr Vec2 SPAWN{2000, FLOOR_Y - 12};

// a creature's components, by reference into the creature table. Only valid
// until something despawns.
struct CreatureRef {
  sim::Transform &transform;
  sim::Physics &physics;
  sim::Movement &movement;
};

struct MovementTest : ::testing::Test {
  std::unique_ptr<sim::Sim> game = std::make_unique<sim::Sim>();
  sim::EntityID creature{};

  void SetUp() override {
    fill(0, FLOOR_ROW, sim::Grid::W - 1, sim::Grid::H - 1);
    creature = spawn(SPAWN);
    // one tick for gravity to press it into the floor and set on_ground
    ASSERT_TRUE(tick(0).physics.on_ground);
  }

  void fill(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    sim::fill_rect(game->world.grid, x0, y0, x1, y1, sim::CellKind::Solid);
  }

  sim::EntityID spawn(Vec2 pos) {
    auto res = game->exec(
        sim::SpawnEntity{.kind = sim::EntityKind::Creature, .pos = pos}, 0);
    EXPECT_TRUE(res);
    return std::get<sim::EntityID>(*res);
  }

  // applies intents (and an aim), then runs exactly one tick
  CreatureRef tick(sim::MovementIntents in, Vec2 aim = {0, 0}) {
    auto res = game->exec(
        sim::SetMovementIntents{.intents = in, .aim = aim, .target = creature},
        game->tick_dur());
    EXPECT_TRUE(res);
    return entity();
  }

  CreatureRef ticks(int n, sim::MovementIntents in) {
    for (int i = 0; i < n - 1; i++) {
      tick(in);
    }
    return tick(in);
  }

  CreatureRef entity() {
    uint32_t row = *sim::creature_row(game->world, creature);
    sim::CreatureTable &t = game->world.creatures;
    return {t.transform[row], t.physics[row], t.movement[row]};
  }

  bool held(MoveState s) { return entity().movement.state.held(s); }

  float dt() {
    return static_cast<float>(static_cast<double>(game->tick_dur()) / 1e9);
  }

  // a quarter second of sprinting right, plenty to get moving
  void sprint_up() {
    ticks(60, intents({MovementIntent::Right, MovementIntent::Sprint}));
  }

  // sprint, then start a slide
  void start_slide() {
    sprint_up();
    tick(intents({MovementIntent::Slide}));
    ASSERT_TRUE(held(MoveState::Sliding));
  }
};

//====== SPRINT ======//

TEST_F(MovementTest, SprintOnlyWhileMoving) {
  sprint_up();
  EXPECT_TRUE(held(MoveState::Sprinting));
  tick(intents({MovementIntent::Sprint}));
  EXPECT_FALSE(held(MoveState::Sprinting));
}

TEST_F(MovementTest, SlideCancelsSprint) {
  start_slide();
  EXPECT_FALSE(held(MoveState::Sprinting));
}

//====== JUMP ======//

TEST_F(MovementTest, CantJumpInAir) {
  tick(intents({MovementIntent::Jump}));
  float vy = tick(0).physics.vel.y;
  // a fresh press while airborne doesn't add another jump
  EXPECT_GE(tick(intents({MovementIntent::Jump})).physics.vel.y, vy);
}

TEST_F(MovementTest, HoldingJumpRejumpsOnLanding) {
  int takeoffs = 0;
  bool was_on_ground = true;
  for (int i = 0; i < 240 * 3; i++) {
    bool on_ground =
        tick(intents({MovementIntent::Jump})).physics.on_ground;
    if (was_on_ground && !on_ground) {
      takeoffs++;
    }
    was_on_ground = on_ground;
  }
  EXPECT_GE(takeoffs, 2);
}

TEST_F(MovementTest, CanJumpAgainAfterLanding) {
  tick(intents({MovementIntent::Jump}));
  ASSERT_TRUE(ticks(240, 0).physics.on_ground);
  EXPECT_FALSE(tick(intents({MovementIntent::Jump})).physics.on_ground);
}

TEST_F(MovementTest, CoyoteJumpJustAfterLeavingGround) {
  // open a pit under the creature
  sim::fill_rect(game->world.grid, 0, FLOOR_ROW, sim::Grid::W - 1,
                 FLOOR_ROW + 40, sim::CellKind::Empty);
  ticks(5, 0);
  ASSERT_FALSE(entity().physics.on_ground);
  // still within coyote time
  EXPECT_LT(tick(intents({MovementIntent::Jump})).physics.vel.y, 0);
}

TEST_F(MovementTest, NoCoyoteJumpLongAfterLeavingGround) {
  sim::fill_rect(game->world.grid, 0, FLOOR_ROW, sim::Grid::W - 1,
                 FLOOR_ROW + 40, sim::CellKind::Empty);
  ticks(120, 0);
  ASSERT_FALSE(entity().physics.on_ground);
  EXPECT_GT(tick(intents({MovementIntent::Jump})).physics.vel.y, 0);
}

TEST_F(MovementTest, JumpPressedJustBeforeLandingStillJumps) {
  // drop from just above the floor, tap jump in the air, let go
  ASSERT_TRUE(sim::despawn(game->world, creature));
  creature = spawn({SPAWN.x, SPAWN.y - 2});
  tick(intents({MovementIntent::Jump}));
  ASSERT_FALSE(entity().physics.on_ground);
  bool jumped = false;
  for (int i = 0; i < 60; i++) {
    jumped |= tick(0).physics.vel.y < -100;
  }
  EXPECT_TRUE(jumped);
}

//====== SLIDE ======//

TEST_F(MovementTest, CantSlideStandingStill) {
  tick(intents({MovementIntent::Slide}));
  EXPECT_FALSE(held(MoveState::Sliding));
}

TEST_F(MovementTest, SlideEndsOnItsOwn) {
  start_slide();
  ticks(240 * 3, intents({MovementIntent::Slide}));
  EXPECT_FALSE(held(MoveState::Sliding));
}

TEST_F(MovementTest, ReleasingCtrlEndsSlide) {
  start_slide();
  tick(0);
  EXPECT_FALSE(held(MoveState::Sliding));
}

// holding ctrl and a direction used to restart the slide every time it ran
// out, because walking pushed the speed back up
TEST_F(MovementTest, HoldingSlideDoesntRestart) {
  ticks(60, intents({MovementIntent::Right}));
  bool ended = false;
  bool restarted = false;
  bool was_sliding = false;
  for (int i = 0; i < 240 * 4; i++) {
    bool sliding =
        tick(intents({MovementIntent::Right, MovementIntent::Slide}))
            .movement.state.held(MoveState::Sliding);
    if (was_sliding && !sliding) {
      ended = true;
    }
    if (ended && sliding) {
      restarted = true;
    }
    was_sliding = sliding;
  }
  EXPECT_TRUE(ended);
  EXPECT_FALSE(restarted);
}

TEST_F(MovementTest, SlideCooldownBlocksImmediateReslide) {
  start_slide();
  // let go of ctrl while still sprinting, then press it again right away
  tick(intents({MovementIntent::Right, MovementIntent::Sprint}));
  ASSERT_FALSE(held(MoveState::Sliding));
  EXPECT_TRUE(held(MoveState::SlideCooldown));
  tick(intents({MovementIntent::Right, MovementIntent::Sprint,
                MovementIntent::Slide}));
  EXPECT_FALSE(held(MoveState::Sliding));

  // once the cooldown is over, a new press slides again
  ticks(120, intents({MovementIntent::Right, MovementIntent::Sprint}));
  ASSERT_FALSE(held(MoveState::SlideCooldown));
  tick(intents({MovementIntent::Right, MovementIntent::Sprint,
                MovementIntent::Slide}));
  EXPECT_TRUE(held(MoveState::Sliding));
}

TEST_F(MovementTest, SlidePressedInAirStartsOnLanding) {
  sprint_up();
  tick(intents({MovementIntent::Right, MovementIntent::Sprint,
                MovementIntent::Jump}));
  ASSERT_FALSE(entity().physics.on_ground);
  // press ctrl in the air and keep holding it
  bool slid = false;
  for (int i = 0; i < 240; i++) {
    auto e = tick(intents({MovementIntent::Right, MovementIntent::Slide}));
    slid |= e.movement.state.held(MoveState::Sliding);
  }
  EXPECT_TRUE(slid);
}

TEST_F(MovementTest, SlidePressedDuringCooldownStartsAfter) {
  start_slide();
  // end it, then press and hold ctrl again while still on cooldown
  tick(intents({MovementIntent::Right, MovementIntent::Sprint}));
  ASSERT_TRUE(held(MoveState::SlideCooldown));
  tick(intents({MovementIntent::Right, MovementIntent::Sprint,
                MovementIntent::Slide}));
  ASSERT_FALSE(held(MoveState::Sliding));
  bool slid = false;
  for (int i = 0; i < 120; i++) {
    slid |= tick(intents({MovementIntent::Right, MovementIntent::Sprint,
                          MovementIntent::Slide}))
                .movement.state.held(MoveState::Sliding);
  }
  EXPECT_TRUE(slid);
}

TEST_F(MovementTest, SlideJumpEndsSlideAndLeavesGround) {
  start_slide();
  auto e = tick(intents({MovementIntent::Slide, MovementIntent::Jump}));
  EXPECT_FALSE(e.movement.state.held(MoveState::Sliding));
  EXPECT_FALSE(e.physics.on_ground);
}

//====== WALLS ======//

struct WallTest : MovementTest {
  // wall's left face at x = 2400, creature hanging in the air next to it
  static constexpr int32_t WALL_COL = 600;
  static constexpr float WALL_X = WALL_COL * CS;

  void SetUp() override {
    MovementTest::SetUp();
    fill(WALL_COL, 0, WALL_COL + 4, FLOOR_ROW - 1);
    ASSERT_TRUE(sim::despawn(game->world, creature));
    creature = spawn({WALL_X - 6, 400});
  }
};

TEST_F(WallTest, PushingIntoWallClings) {
  ticks(10, intents({MovementIntent::Right}));
  EXPECT_TRUE(held(MoveState::WallCling));
}

TEST_F(WallTest, NotPushingDoesntCling) {
  ticks(10, 0);
  EXPECT_FALSE(held(MoveState::WallCling));
}

TEST_F(WallTest, WallJumpEndsCling) {
  ticks(10, intents({MovementIntent::Right}));
  auto e = tick(intents({MovementIntent::Right, MovementIntent::Jump}));
  EXPECT_FALSE(e.movement.state.held(MoveState::WallCling));
  EXPECT_LT(e.physics.vel.y, 0);
}

TEST_F(WallTest, WallJumpCooldownBlocksCling) {
  ticks(10, intents({MovementIntent::Right}));
  tick(intents({MovementIntent::Right, MovementIntent::Jump}));
  EXPECT_TRUE(held(MoveState::WallJumpCooldown));
  EXPECT_FALSE(held(MoveState::WallCling));
}

TEST_F(WallTest, WallJumpJustAfterLettingGo) {
  ticks(10, intents({MovementIntent::Right}));
  ASSERT_TRUE(held(MoveState::WallCling));
  ticks(3, 0);
  ASSERT_FALSE(held(MoveState::WallCling));
  // still within the grace window
  auto e = tick(intents({MovementIntent::Jump}));
  EXPECT_LT(e.physics.vel.y, 0);
  EXPECT_LT(e.physics.vel.x, 0);
}

TEST_F(WallTest, NoWallJumpWithoutHoldingIntoWall) {
  ticks(10, 0);
  auto e = tick(intents({MovementIntent::Jump}));
  EXPECT_GT(e.physics.vel.y, 0);
}

//====== GAS BURST ======//

constexpr Vec2 UP{0, -1};
const auto BURST = intents({MovementIntent::Burst});

int charges(CreatureRef e) { return e.movement.burst_charges; }

// jump off the ground and let go of space
void jump_and_release(MovementTest &t) {
  t.tick(intents({MovementIntent::Jump}));
  t.tick(0);
}

TEST_F(MovementTest, BurstWorksOnTheGround) {
  const int full = charges(entity());
  ASSERT_TRUE(entity().physics.on_ground);
  auto e = tick(BURST, UP);
  EXPECT_EQ(charges(e), full - 1);
  EXPECT_FALSE(e.physics.on_ground);
}

TEST_F(MovementTest, BurstsUseChargesUntilEmpty) {
  jump_and_release(*this);
  const int full = charges(entity());
  ASSERT_GT(full, 0);
  for (int i = 0; i < full; i++) {
    EXPECT_EQ(charges(tick(BURST, UP)), full - 1 - i);
    tick(0);
  }
  // out of gas: another press does nothing
  EXPECT_EQ(charges(tick(BURST, UP)), 0);
}

TEST_F(MovementTest, HoldingBurstFiresOnce) {
  jump_and_release(*this);
  const int full = charges(entity());
  for (int i = 0; i < 10; i++) {
    tick(BURST, UP);
  }
  EXPECT_EQ(charges(entity()), full - 1);
}

TEST_F(MovementTest, BurstsComeBackOverTime) {
  jump_and_release(*this);
  const int full = charges(entity());
  tick(BURST, UP);
  ASSERT_EQ(charges(entity()), full - 1);
  // not straight away
  EXPECT_EQ(charges(ticks(10, 0)), full - 1);
  // but after a while, wherever you are
  EXPECT_EQ(charges(ticks(240 * 4, 0)), full);
}

//====== HOOK ======//

const auto HOOK = intents({MovementIntent::Hook});

TEST_F(MovementTest, OnlyOneHookOutAtATime) {
  tick(HOOK, UP);
  ticks(10, HOOK);
  ASSERT_EQ(game->world.hooks.len, 1u);
  const sim::EntityID first = entity().movement.hook;
  // let go (it starts coming back) and press again before it's back
  tick(0);
  tick(HOOK, UP);
  EXPECT_EQ(game->world.hooks.len, 1u);
  EXPECT_EQ(entity().movement.hook, first);
}

TEST_F(MovementTest, ReleasedHookComesBackAndCanFireAgain) {
  tick(HOOK, UP);
  ticks(5, HOOK);
  tick(0);
  ASSERT_EQ(game->world.hooks.len, 1u);
  EXPECT_EQ(game->world.hooks.hook[0].state, sim::HookState::Returning);
  ticks(240, 0);
  EXPECT_EQ(game->world.hooks.len, 0u);
  tick(HOOK, UP);
  EXPECT_EQ(game->world.hooks.len, 1u);
}

TEST_F(MovementTest, HookComesBackPastMaxLengthEvenWhileHeld) {
  // straight up into open air, held the whole time
  tick(HOOK, UP);
  ticks(240, HOOK);
  EXPECT_EQ(game->world.hooks.len, 0u);
}

TEST_F(MovementTest, HookGoesAwayWithItsOwner) {
  tick(HOOK, UP);
  ASSERT_EQ(game->world.hooks.len, 1u);
  ASSERT_TRUE(sim::despawn(game->world, creature));
  game->do_tick(dt());
  EXPECT_EQ(game->world.hooks.len, 0u);
}

TEST_F(WallTest, HookSticksToWallAndPullsUntilReleased) {
  constexpr Vec2 RIGHT{1, 0};
  tick(HOOK, RIGHT);
  ticks(10, HOOK);
  ASSERT_EQ(game->world.hooks.len, 1u);
  const sim::Hook &h = game->world.hooks.hook[0];
  EXPECT_EQ(h.state, sim::HookState::Attached);
  const sim::PhysicsModifier *pull = game->world.physics_modifiers.get(h.pull);
  ASSERT_TRUE(pull);
  EXPECT_EQ(pull->kind, sim::Force);
  EXPECT_EQ(pull->target, creature);

  const sim::ModifierID pull_key = h.pull;
  tick(0);
  EXPECT_EQ(game->world.hooks.hook[0].state, sim::HookState::Returning);
  EXPECT_FALSE(game->world.physics_modifiers.valid_key(pull_key));
}

TEST_F(MovementTest, HookSticksToACreatureAndFollowsIt) {
  const sim::EntityID target = spawn({SPAWN.x + 80, SPAWN.y});
  constexpr Vec2 RIGHT{1, 0};
  tick(HOOK, RIGHT);
  // ~20 ticks to get there, then it starts pulling the tick after
  ticks(40, HOOK);
  ASSERT_EQ(game->world.hooks.len, 1u);
  EXPECT_EQ(game->world.hooks.hook[0].state, sim::HookState::Attached);
  EXPECT_EQ(game->world.hooks.hook[0].stuck_to, target);
  EXPECT_TRUE(entity().movement.hook_pulling);

  // move the target: the hook goes with it
  sim::transform_of(game->world, target)->pos.x += 40;
  tick(HOOK);
  EXPECT_EQ(game->world.hooks.transform[0].pos,
            sim::transform_of(game->world, target)->pos);
}

TEST_F(MovementTest, HookComesBackWhenItsCreatureDies) {
  const sim::EntityID target = spawn({SPAWN.x + 80, SPAWN.y});
  tick(HOOK, {1, 0});
  ticks(40, HOOK);
  ASSERT_EQ(game->world.hooks.hook[0].stuck_to, target);
  ASSERT_TRUE(sim::despawn(game->world, target));
  tick(HOOK);
  EXPECT_EQ(game->world.hooks.hook[0].state, sim::HookState::Returning);
  EXPECT_FALSE(entity().movement.hook_pulling);
}

//====== SLASH ======//

const auto ATTACK = intents({MovementIntent::Attack});

sim::Combat &combat_of(sim::World &w, sim::EntityID id) {
  return w.creatures.combat[*sim::creature_row(w, id)];
}

TEST_F(MovementTest, SlashHitsEachCreatureOncePerSlash) {
  const sim::EntityID target = spawn({SPAWN.x + 16, SPAWN.y});
  const float full = combat_of(game->world, target).health;
  tick(ATTACK, {1, 0});
  const float after_one = combat_of(game->world, target).health;
  EXPECT_LT(after_one, full);
  // the rest of the slash (still overlapping) doesn't hit again
  ticks(10, 0);
  EXPECT_EQ(combat_of(game->world, target).health, after_one);
}

TEST_F(MovementTest, NoNewSlashUntilCooldownIsOver) {
  const sim::EntityID target = spawn({SPAWN.x + 16, SPAWN.y});
  tick(ATTACK, {1, 0});
  const float after_one = combat_of(game->world, target).health;
  tick(0);
  // pressed again straight away: nothing
  tick(ATTACK, {1, 0});
  EXPECT_EQ(combat_of(game->world, target).health, after_one);
  // after the cooldown: hits again
  ticks(240, 0);
  tick(ATTACK, {1, 0});
  EXPECT_LT(combat_of(game->world, target).health, after_one);
}

TEST_F(MovementTest, CreaturesAtZeroHealthAreRemoved) {
  const sim::EntityID target = spawn({SPAWN.x + 16, SPAWN.y});
  combat_of(game->world, target).health = 1;
  tick(ATTACK, {1, 0});
  EXPECT_FALSE(sim::creature_row(game->world, target));
}

TEST_F(MovementTest, SlashDoesntHitItsOwner) {
  const float full = combat_of(game->world, creature).health;
  tick(ATTACK, {1, 0});
  EXPECT_EQ(combat_of(game->world, creature).health, full);
}

} // namespace
