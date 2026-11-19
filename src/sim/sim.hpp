#pragma once
#include "sim/components.hpp"
#include "sim/grid.hpp"
#include "sim/sim_state.hpp"
#include "systems/physics/physics_types.hpp"
#include "util/arena.hpp"
#include "util/vec2.hpp"
#include <cstdint>
#include <entt/entity/registry.hpp>
#include <expected>
#include <variant>

namespace sim {

/*/
 * The simulation of a 2D movement and time-manipulation action game:
 * deterministic, exactly restorable and rewindable. Entities live in an
 * EnTT registry; see the README's design section for the rules that keep
 * it that way.
/*/

//========= I/O ==========//

using TimeInt = uint64_t; // ns
using TimeFloat = float;

// Why an input was rejected. A rejected input changes nothing; time still
// passes (see Sim::exec).
enum class SimErr {
  // the target is gone, or doesn't have what the input acts on
  InvalidEntity,
  InvalidMap,
  // an action that needs a direction got a zero aim
  NoAim,
  // a gas burst with no charges left
  NoGas,
  // firing the hook while it's still out (flying, stuck or coming back)
  HookOut,
  // letting go of a hook that isn't out, or is already coming back
  NoHook,
  // an action that's still on cooldown
  OnCooldown,
};

// returns nothing (monostate). Switches the map (World::grid) to `map`,
// which the caller keeps alive and unchanged while anything points at it.
// The only way the map changes, so input logs replay map switches too.
struct SetMap {
  const Grid *map;
};

// returns the creature's entity
struct SpawnCreature {
  Vec2 pos;
};

// returns nothing (monostate). What the entity is holding (walk, jump,
// sprint, slide); movement acts on it every tick.
struct SetMovementIntents {
  MovementIntents intents;
  // unit vector the entity aims along (wall jumps), or zero
  Vec2 aim;
  entt::entity target;
};

// Actions: each dispatches to its system's action function, which checks
// the action can happen right now and either does it or rejects it,
// changing nothing. `aim` is a unit vector.

// returns nothing (monostate). An impulse along `aim`, using up a gas
// charge (see systems/odm). Rejected with no charge left or a zero aim.
struct GasBurst {
  Vec2 aim;
  entt::entity target;
};

// returns the hook's entity. Fires the target's hook along `aim` (see
// systems/odm). Rejected while its hook is out or with a zero aim.
struct FireHook {
  Vec2 aim;
  entt::entity target;
};

// returns nothing (monostate). Lets go of the target's hook: it comes back.
// Rejected if no hook is out or it's already coming back.
struct ReleaseHook {
  entt::entity target;
};

// returns nothing (monostate). Starts a slash along `aim`, or the facing
// direction with a zero aim (see systems/combat). Rejected on cooldown.
struct Slash {
  Vec2 aim;
  entt::entity target;
};

// returns nothing (monostate). Not implemented yet: always rejected.
struct LookAt {
  Vec2 pos;
  entt::entity target;
};

// returns nothing (monostate). Replaces the target's movement tunables.
struct SetMovementParams {
  MovementParams params;
  entt::entity target;
};

using SimInput =
    std::variant<SetMap, SpawnCreature, SetMovementIntents, GasBurst,
                 FireHook, ReleaseHook, Slash, LookAt, SetMovementParams>;
using SimOutput = std::variant<std::monostate, entt::entity>;
using SimResult = std::expected<SimOutput, SimErr>;

//========= WORLD ==========//

// Every component type in the sim. A type missing here would vanish from
// snapshots; saving asserts none is (in debug builds).
using EntityState =
    SimState<Transform, Physics, Movement, Combat, Perception, Owner, Odm,
             Hook, VelocityModifier, ForceModifier,
             AngularVelocityModifier, AngularForceModifier, DriveModifier>;

// Storages are reserved for this many entities (and this many of every
// component) on creation and after every restore, so ticks don't allocate.
inline constexpr size_t ENTITY_RESERVE = 8192;
// per-tick scratch for deferred structural changes
inline constexpr size_t SCRATCH_BYTES = size_t{1} << 20;

// Sim state is the registry plus the plain members. Snapshots (snapshot.hpp)
// copy exactly those; anything added here has to be added there too.
struct World {
  // every entity and its components
  entt::registry reg;
  // initialized here because World has a constructor, so World{} no longer
  // zeroes them
  TimeInt t_excess = 0;
  // The map. Static: the sim only reads it, so it isn't copied into
  // snapshots, just this pointer. Not owned (see SetMap). Only ever set by
  // the SetMap input, never directly, or replaying an input log won't
  // reproduce the switch. Must be set before ticking.
  const Grid *grid = nullptr;

  // Not sim state, never snapshotted: every system that uses it rewinds it
  // before returning (ArenaScope).
  Arena scratch{SCRATCH_BYTES};

  // reserves every storage (see ENTITY_RESERVE)
  World() noexcept;
};

// base tick: 240 per world second while anything is happening (anything
// moving, or anyone holding an input)
inline constexpr TimeInt BASE_TICK = static_cast<TimeInt>(1e9 / 240);
// and 30 per world second while nothing is, so a still world costs little.
// An input's effects only happen on a tick, but this doesn't delay them: an
// input that's held makes the very next tick a BASE_TICK one (see tick_dur)
inline constexpr TimeInt IDLE_TICK = static_cast<TimeInt>(1e9 / 30);
// ticks shrink so the fastest body moves at most this far (units) per tick
inline constexpr float TICK_MAX_TRAVEL = 4;
// and never get shorter than this, whatever's going on
inline constexpr TimeInt MIN_TICK = BASE_TICK / 1000;

// defined in snapshot.hpp
struct Snapshot;

// The simulation. Its state (the World) is private: it only changes through
// the input funnel (apply, advance, exec) or restore, rewinding to a
// snapshot. Everything outside gets read-only access.
class Sim {
public:
  // Applies an input at the current time. It's checked against the state
  // right now; a rejected input changes nothing.
  SimResult apply(SimInput);

  // Lets `dt` of world time pass: runs every tick that fits, carrying the
  // leftover (World::t_excess) into the next advance. Splitting an advance
  // into pieces runs exactly the same ticks as doing it in one go, so
  // inputs can be applied at any point in time.
  void advance(TimeInt dt);

  // apply then advance. Time passes whether or not the input was accepted.
  SimResult exec(SimInput, TimeInt);

  // Replaces all state with the snapshot's (see snapshot.hpp). Not an
  // input: rollback is the Timeline's (timeline.hpp), not the sim's.
  void restore(const Snapshot &snap);
  Snapshot save() const;

  // World time of the next tick: BASE_TICK while anything moves or anyone
  // holds an input, IDLE_TICK while nothing does, divided by the highest
  // perception, and short enough that the fastest body moves at most
  // TICK_MAX_TRAVEL, whichever is shorter. Depends on every entity, and is
  // worked out again before every tick, so an input changes it at once.
  TimeInt tick_dur() const;

  const World &world() const noexcept { return world_; }

private:
  World world_;

  void do_tick(TimeFloat);
};

} // namespace sim
