#include "util/fsm.hpp"
#include "util/plain.hpp"
#include <gtest/gtest.h>

namespace {

enum class Act : uint8_t {
  Grounded = 1 << 0,
  Jumping = 1 << 1,
  Crouching = 1 << 2,
  Stunned = 1 << 3,
  Dashing = 1 << 4,
  Charging = 1 << 5,
};

constexpr FsmRules<Act> make_rules() {
  FsmRules<Act> r{};
  // blocked by stun, and jumping stops a crouch
  r.set(Act::Jumping, {Act::Stunned}, {Act::Crouching});
  r.set(Act::Crouching, {Act::Stunned}, {});
  // blocks itself, so there's only ever one dash at a time
  r.set(Act::Dashing, {Act::Stunned, Act::Dashing}, {});
  // cancels itself, so recharging restarts it
  r.set(Act::Charging, {}, {Act::Charging});
  return r;
}
constexpr FsmRules<Act> RULES = make_rules();
using Fsm = FlagFsm<Act, RULES>;

static_assert(Plain<Fsm>);
// held mask + 8 counters + 8 epochs, no pointer to the rules
static_assert(sizeof(Fsm) == 1 + 8 + 8);

// Each check is a constexpr function run twice: by static_assert (constant
// evaluation rejects UB) and by a TEST (runs under the sanitizers).

constexpr bool acquire_and_release() {
  Fsm f{};
  auto e = f.acquire(Act::Grounded);
  if (!e || !f.held(Act::Grounded)) {
    return false;
  }
  f.release(Act::Grounded, *e);
  return !f.held(Act::Grounded) && f.held_mask() == 0;
}
static_assert(acquire_and_release());
TEST(FlagFsm, AcquireAndRelease) { EXPECT_TRUE(acquire_and_release()); }

constexpr bool counts_holds() {
  Fsm f{};
  auto a = *f.acquire(Act::Grounded);
  auto b = *f.acquire(Act::Grounded);
  if (f.holds(Act::Grounded) != 2) {
    return false;
  }
  f.release(Act::Grounded, a);
  if (!f.held(Act::Grounded)) {
    return false;
  }
  f.release(Act::Grounded, b);
  return !f.held(Act::Grounded);
}
static_assert(counts_holds());
TEST(FlagFsm, CountsHolds) { EXPECT_TRUE(counts_holds()); }

constexpr bool blocking_set_blocks() {
  Fsm f{};
  auto stun = *f.acquire(Act::Stunned);
  if (!f.blocked(Act::Jumping) || f.acquire(Act::Jumping)) {
    return false;
  }
  f.release(Act::Stunned, stun);
  return f.acquire(Act::Jumping).has_value();
}
static_assert(blocking_set_blocks());
TEST(FlagFsm, BlockingSetBlocks) { EXPECT_TRUE(blocking_set_blocks()); }

constexpr bool cancel_set_cancels() {
  Fsm f{};
  auto crouch = *f.acquire(Act::Crouching);
  (void)f.acquire(Act::Jumping);
  if (f.held(Act::Crouching) || f.still_held(Act::Crouching, crouch)) {
    return false;
  }
  // the crouch's owner releasing late does nothing
  f.release(Act::Crouching, crouch);
  return f.holds(Act::Crouching) == 0 && f.held(Act::Jumping);
}
static_assert(cancel_set_cancels());
TEST(FlagFsm, CancelSetCancels) { EXPECT_TRUE(cancel_set_cancels()); }

// the case a single cancel bit can't handle: holds from before a cancel and
// after it, released in any order
constexpr bool stale_releases_are_ignored() {
  Fsm f{};
  auto a = *f.acquire(Act::Stunned);
  auto b = *f.acquire(Act::Stunned);
  f.cancel(Act::Stunned);
  auto c = *f.acquire(Act::Stunned);
  f.release(Act::Stunned, a);
  f.release(Act::Stunned, b);
  if (!f.still_held(Act::Stunned, c) || f.holds(Act::Stunned) != 1) {
    return false;
  }
  f.release(Act::Stunned, c);
  return !f.held(Act::Stunned);
}
static_assert(stale_releases_are_ignored());
TEST(FlagFsm, StaleReleasesAreIgnored) {
  EXPECT_TRUE(stale_releases_are_ignored());
}

constexpr bool self_blocking_is_binary() {
  Fsm f{};
  auto dash = f.acquire(Act::Dashing);
  return dash && !f.acquire(Act::Dashing) && f.holds(Act::Dashing) == 1;
}
static_assert(self_blocking_is_binary());
TEST(FlagFsm, SelfBlockingIsBinary) { EXPECT_TRUE(self_blocking_is_binary()); }

constexpr bool self_cancel_restarts() {
  Fsm f{};
  auto first = *f.acquire(Act::Charging);
  auto second = *f.acquire(Act::Charging);
  return first != second && !f.still_held(Act::Charging, first) &&
         f.still_held(Act::Charging, second) && f.holds(Act::Charging) == 1;
}
static_assert(self_cancel_restarts());
TEST(FlagFsm, SelfCancelRestarts) { EXPECT_TRUE(self_cancel_restarts()); }

constexpr bool unruled_flags_are_independent() {
  Fsm f{};
  (void)f.acquire(Act::Grounded);
  (void)f.acquire(Act::Stunned);
  return f.held_mask() == bits({Act::Grounded, Act::Stunned});
}
static_assert(unruled_flags_are_independent());
TEST(FlagFsm, UnruledFlagsAreIndependent) {
  EXPECT_TRUE(unruled_flags_are_independent());
}

// wider underlying types get more flags
enum class Wide : uint16_t {
  Low = 1 << 0,
  High = 1 << 15,
};
constexpr FsmRules<Wide> make_wide_rules() {
  FsmRules<Wide> r{};
  r.set(Wide::High, {Wide::Low}, {});
  return r;
}
constexpr FsmRules<Wide> WIDE_RULES = make_wide_rules();

constexpr bool wide_flags_work() {
  FlagFsm<Wide, WIDE_RULES> f{};
  if (!f.acquire(Wide::High)) {
    return false;
  }
  (void)f.acquire(Wide::Low);
  f.cancel(Wide::High);
  return !f.acquire(Wide::High) && f.held(Wide::Low);
}
static_assert(wide_flags_work());
TEST(FlagFsm, WideFlagsWork) { EXPECT_TRUE(wide_flags_work()); }

} // namespace
