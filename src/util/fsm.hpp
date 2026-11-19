#pragma once
#include "flags.hpp"
#include <array>
#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>

// Rules for one flag.
template <FlagEnum E> struct FsmRule {
  // the flag can't be acquired while any of these are held
  FlagBits<E> blocking;
  // acquiring the flag cancels all of these
  FlagBits<E> cancel;
};

// Static rule set: one FsmRule per bit of E. Build it in a constexpr
// function and pass it to FlagFsm as a template argument.
template <FlagEnum E> struct FsmRules {
  static constexpr uint32_t N = std::numeric_limits<FlagBits<E>>::digits;

  // indexed by flag_index. public so FsmRules can be a template argument.
  std::array<FsmRule<E>, N> by_index;

  constexpr void set(E flag, std::initializer_list<E> blocking,
                     std::initializer_list<E> cancel) noexcept {
    by_index[flag_index(flag)] = {
        .blocking = bits(blocking),
        .cancel = bits(cancel),
    };
  }
  constexpr const FsmRule<E> &get(E flag) const noexcept {
    return by_index[flag_index(flag)];
  }
};

// Handed out by acquire. A release with an epoch older than the flag's
// current one is ignored, because the hold it belonged to was cancelled.
// Wraps after 256 cancels, so a hold kept across exactly 256 cancels of the
// same flag would look valid again.
using FsmEpoch = uint8_t;

// Counted flags with static rules. Each flag works like a semaphore: it's
// held while its count is above zero.
//
//   acquire  fails if any flag in the rule's blocking set is held. Otherwise
//            cancels every flag in the rule's cancel set, then adds one hold
//            and returns the flag's current epoch.
//   release  removes one hold, unless the epoch is stale (cancelled since).
//   cancel   drops every hold at once and bumps the epoch, so all
//            outstanding epochs for that flag go stale.
//
// Plain, and zero-initialized (`FlagFsm<...> fsm{}`) means nothing held. The
// rules live in the type, not the instance, so there's no pointer in it.
template <FlagEnum E, FsmRules<E> Rules> class FlagFsm {
  using Bits = FlagBits<E>;
  static constexpr uint32_t N = FsmRules<E>::N;

  // bit i is set exactly when count[i] > 0
  Bits held_bits;
  std::array<uint8_t, N> count;
  // bumped every time flag i is cancelled
  std::array<FsmEpoch, N> epoch;

  constexpr void cancel_index(uint32_t i) noexcept {
    count[i] = 0;
    epoch[i] = static_cast<FsmEpoch>(epoch[i] + 1);
    held_bits &= static_cast<Bits>(~static_cast<Bits>(Bits{1} << i));
  }

public:
  constexpr bool held(E flag) const noexcept {
    return (held_bits & bits(flag)) != 0;
  }
  constexpr Bits held_mask() const noexcept { return held_bits; }
  constexpr uint8_t holds(E flag) const noexcept {
    return count[flag_index(flag)];
  }

  // whether acquire(flag) would fail right now
  constexpr bool blocked(E flag) const noexcept {
    return (held_bits & Rules.get(flag).blocking) != 0;
  }

  // false once the hold behind `e` was cancelled (or the flag isn't held)
  constexpr bool still_held(E flag, FsmEpoch e) const noexcept {
    uint32_t i = flag_index(flag);
    return count[i] > 0 && epoch[i] == e;
  }

  // nullopt if blocked. A flag in its own cancel set restarts: earlier holds
  // are cancelled and this becomes the only one.
  constexpr std::optional<FsmEpoch> acquire(E flag) noexcept {
    if (blocked(flag)) {
      return std::nullopt;
    }
    Bits cancel = Rules.get(flag).cancel;
    for (uint32_t i = 0; i < N; i++) {
      if ((cancel & static_cast<Bits>(Bits{1} << i)) != 0) {
        cancel_index(i);
      }
    }
    uint32_t i = flag_index(flag);
    assert(count[i] < std::numeric_limits<uint8_t>::max());
    count[i] = static_cast<uint8_t>(count[i] + 1);
    held_bits |= bits(flag);
    return epoch[i];
  }

  // Never blocked. Releasing the same epoch more times than you acquired it
  // can't be detected and takes away someone else's hold.
  constexpr void release(E flag, FsmEpoch e) noexcept {
    uint32_t i = flag_index(flag);
    if (epoch[i] != e) {
      // cancelled since this hold was acquired, nothing to give back
      return;
    }
    assert(count[i] > 0);
    count[i] = static_cast<uint8_t>(count[i] - 1);
    if (count[i] == 0) {
      held_bits &= static_cast<Bits>(~bits(flag));
    }
  }

  constexpr void cancel(E flag) noexcept { cancel_index(flag_index(flag)); }

  // members are private, so state comparisons (sim_state.hpp) use this
  friend constexpr bool operator==(const FlagFsm &, const FlagFsm &) = default;
};
