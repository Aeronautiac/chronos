#pragma once
#include "util/fsm.hpp"
#include <cstdint>

namespace sim {

enum class MoveState : uint8_t {
  Sprinting = 1 << 0,
  Sliding = 1 << 1,
  // in the air, pushing into a wall: falls slowly, can wall jump
  WallCling = 1 << 2,
  // cooldowns: held for a while after a move, blocking it
  SlideCooldown = 1 << 3,
  WallJumpCooldown = 1 << 4,
};

// For each state:
//   blocking: can't enter it while any of these are held
//   cancel:   entering it ends all of these
constexpr FsmRules<MoveState> make_move_rules() {
  FsmRules<MoveState> r{};
  r.set(MoveState::Sprinting,
        /*blocking*/ {MoveState::Sliding},
        /*cancel*/ {});
  r.set(MoveState::Sliding,
        /*blocking*/ {MoveState::SlideCooldown},
        /*cancel*/ {MoveState::Sprinting});
  // wall jumping needs a cling, so blocking the cling blocks the wall jump
  r.set(MoveState::WallCling,
        /*blocking*/ {MoveState::Sliding, MoveState::WallJumpCooldown},
        /*cancel*/ {MoveState::Sprinting});
  return r;
}
inline constexpr FsmRules<MoveState> MOVE_RULES = make_move_rules();

using MoveFsm = FlagFsm<MoveState, MOVE_RULES>;

} // namespace sim
