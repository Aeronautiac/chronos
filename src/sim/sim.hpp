#pragma once
#include "sim/entity.hpp"
#include "sim/grid.hpp"
#include "sim/kinds.hpp"
#include "systems/physics/physics_types.hpp"
#include "util/slotmap.hpp"
#include "util/vec2.hpp"
#include <cstdint>
#include <expected>
#include <variant>

namespace sim {

/*/
 * Making a basic space shooting game with rollback and time manipulation
 * capabilities at first to test the design.
 * Will later expand into a proper bullet hell, movement dense type of game.
 * Entities are stored per kind, one array per component (kinds.hpp).
/*/

//========= I/O ==========//

using TimeInt = uint64_t; // ns
using TimeFloat = float;

enum class SimErr {
  InvalidEntity,
  EntityLimit,
  ModifierLimit,
};

// returns EntityID
struct SpawnEntity {
  EntityKind kind;
  Vec2 pos;
};

// returns nothing (monostate)
struct SetMovementIntents {
  MovementIntents intents;
  // unit vector the entity aims along (gas bursts), or zero
  Vec2 aim;
  EntityID target;
};

// returns nothing (monostate)
struct LookAt {
  Vec2 pos;
  EntityID target;
};

// returns nothing (monostate). Replaces the target's movement tunables.
struct SetMovementParams {
  MovementParams params;
  EntityID target;
};

using SimInput =
    std::variant<SpawnEntity, SetMovementIntents, LookAt, SetMovementParams>;
using SimOutput = std::variant<std::monostate, EntityID>;
using SimResult = std::expected<SimOutput, SimErr>;

using PhysicsModifierTable = DenseSlotMap<PhysicsModifier, 8192 * 8>;

// Fixed-size and pointer-free, so a snapshot is a plain copy of the struct,
// storage order included (which iteration order, and so float sums, depend
// on).
struct World {
  static constexpr uint32_t N_ENTITY = 8192;
  TimeInt t_excess;
  Grid grid;
  // kind and row of every entity, by EntityID (see world.hpp)
  SparseSlotMap<Entity, N_ENTITY> entities;
  // one table per kind
  CreatureTable creatures;
  HookTable hooks;
  // keyed records, not entities
  PhysicsModifierTable physics_modifiers;
};
static_assert(Plain<World>);

// should always allocate this struct on the heap.
// it contains large buffers and will exhaust the stack.
// base tick: 240 per world second when nothing is fast
inline constexpr TimeInt BASE_TICK = static_cast<TimeInt>(1e9 / 240);
// ticks shrink so the fastest body moves at most this far (units) per tick
inline constexpr float TICK_MAX_TRAVEL = 4;
// and never get shorter than this, whatever's going on
inline constexpr TimeInt MIN_TICK = BASE_TICK / 1000;

struct Sim {
  World world;
  // Perform ticks required over time period (world time), considering excess
  SimResult exec(SimInput, TimeInt);
  // World time of the next tick. Shorter when anything is fast or
  // perceptive: BASE_TICK divided by the highest perception, and short
  // enough that the fastest body moves at most TICK_MAX_TRAVEL, whichever
  // is shorter. Depends on every entity.
  TimeInt tick_dur();
  // Execute a game tick
  SimResult apply_input(SimInput);
  void do_tick(TimeFloat);
};

} // namespace sim
