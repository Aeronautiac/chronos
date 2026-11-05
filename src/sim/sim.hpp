#pragma once
#include "pod.hpp"
#include "slotmap.hpp"
#include <cstddef>
#include <cstdint>
#include <expected>
#include <variant>

namespace sim {

/*/
 * Making a basic space shooting game with rollback and time manipulation
 * capabilities at first to test the design.
 * Will later expand into a proper bullet hell, movement dense type of game.
 * No ECS yet. The game isn't complex enough for that.
/*/

//====== COMPONENTS ======//

struct Vec2 {
  float x;
  float y;
};

struct Physics {
  // cache (re-evaluated every tick)
  Vec2 vel;
  // offset
  Vec2 raw_vel;
  float ang_vel;
  float raw_ang_vel;
};

struct Transform {
  Vec2 pos;
  float rot;
};

enum class MovementIntent : uint8_t {
  Forward = 1 << 0,
  Right = 1 << 1,
  Left = 1 << 2,
  Backward = 1 << 3,
  Boost = 1 << 4
};
using MovementIntents = uint8_t;

enum class EntityKind {
  Ship,
};

// fat struct
struct Entity {
  Physics physics;
  Transform transform;
  MovementIntents movement_intents;
  EntityKind kind;
};

//========= I/O ==========//

using EntityID = Key<Entity>;
using TimeInt = uint64_t; // ns
using TimeFloat = float;

enum class SimErr {
  InvalidEntity,
  EntityLimit,
};

// returns EntityID
struct SpawnEntity {
  EntityKind kind;
};

// returns nothing (monostate)
struct SetMovementIntents {
  MovementIntents intents;
  EntityID target;
};

// returns nothing (monostate)
struct LookAt {
  Vec2 pos;
  EntityID target;
};

using SimInput = std::variant<SpawnEntity, SetMovementIntents, LookAt>;
using SimOutput = std::variant<std::monostate, EntityID>;
using SimResult = std::expected<SimOutput, SimErr>;

// dangling modifiers are pruned on next sweep
template <Pod T> struct PhysModifier {
  T modifier;
  EntityID target;
};

template <uint32_t N_ENTITY> struct PhysicsModifiers {};

// for now, flat data. potentially allow unbounded growth in the future, but
// isn't yet necessary.
template <uint32_t N_ENTITY> struct World {
  TimeInt t_excess;
  SlotMap<Entity, N_ENTITY> entities;
  PhysicsModifiers<N_ENTITY> physics_modifiers;
};

// should always allocate this struct on the heap.
// it contains large buffers and will exhaust the stack.
struct Sim {
  World<8092> world;
  // Perform ticks required over time period, considering excess
  SimResult exec(SimInput, TimeInt);
  // Return the current tick duration requirements based on simulation state
  // (dynamic, not fixed)
  TimeInt tick_dur();
  // Execute a game tick
  SimResult apply_input(SimInput);
  void do_tick(TimeFloat);
};

} // namespace sim
