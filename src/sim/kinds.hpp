#pragma once
#include "sim/entity.hpp"
#include "util/plain.hpp"
#include <array>
#include <cstdint>

namespace sim {

// One table per entity kind. Each component the kind has gets its own array,
// and row i of every array belongs to the same entity (`id[i]`). Rows
// [0, len) are live and packed, so systems loop over them directly with no
// lookups. Despawning moves the last row into the gap (see despawn).
//
// Adding a kind: a table here, a field in World, a case in every switch on
// EntityKind (the compiler lists them), and loops in the systems it uses.

struct CreatureTable {
  static constexpr uint32_t N = 4096;
  uint32_t len;
  std::array<EntityID, N> id;
  std::array<Transform, N> transform;
  std::array<Physics, N> physics;
  std::array<Movement, N> movement;
  std::array<Combat, N> combat;
  std::array<Perception, N> perception;
};
static_assert(Plain<CreatureTable>);

struct HookTable {
  static constexpr uint32_t N = 1024;
  uint32_t len;
  std::array<EntityID, N> id;
  std::array<Transform, N> transform;
  std::array<Physics, N> physics;
  std::array<Hook, N> hook;
};
static_assert(Plain<HookTable>);

} // namespace sim
