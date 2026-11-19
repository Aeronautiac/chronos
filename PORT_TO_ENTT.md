# Port: kind tables → EnTT

Handoff for porting the sim in `src/sim` from hand-written per-kind tables to
EnTT. `example.cpp` (attached alongside this file) is a working, tested sketch
of the target shape: creature + ODM + hook entities, snapshot/restore, and the
replay check. Built with Clang 22, `-std=c++2c`, against EnTT master (4.1).

## Why

The game will have hundreds of items, many with unique logic. The current
design (one table per `EntityKind`, a `World` field per kind, a case in every
`EntityKind` switch) doesn't scale to that. EnTT gives ECS-level flexibility
(any entity, any components, added/removed at runtime) while keeping the
guarantees below.

Rejected, don't revisit:
- **flecs**: no snapshot support on master (the v3 addon is gone), and exact
  restore would mean reproducing archetype/table/query-cache internals.
- **Own ECS**: the project would become the ECS instead of the game.
- **Generic item kind with function pointers + fixed state blob**: indirect
  call per item per tick and a shared state cap. Not acceptable.
- **Template registries / X-macros for per-type tables**: overengineered.

## Guarantees the sim must keep

1. Determinism: same start state + same input log → identical state.
2. Exact restore: restoring a keyframe is indistinguishable from never having
   left it (entity ids, versions, free list, storage order).
3. Iteration order depends only on history.
4. Handles never dangle (generational ids, `entt::entity` replaces `EntityID`).
5. No allocation in a tick (reserve storages up front).

`static_assert(Plain<World>)` used to enforce (2) by layout. It is replaced by
the `SimState` wrapper + the replay check (see below), which also catches
float-order bugs the static_assert never could.

## EnTT snapshot behaviour (verified by reading `entity/snapshot.hpp`)

- Snapshots write every storage in packed order (EnTT's forward iterators go
  back to front; the snapshot uses the reverse ones, i.e. front to back). The
  loader re-emplaces in that order by push_back → identical packed order.
- The entity storage writes every id ever handed out, destroyed ones included
  with their versions, plus the live/free boundary → same recycling order.
- After load, the "next fresh id" counter may be lower than originally, but
  `next()` skips ids already present and every id ever created stays present,
  so the next fresh id matches.

Conditions (all enforced by the wrapper or the replay check):
- **Always load into a freshly constructed registry.** `registry.clear()`
  keeps destroyed ids with bumped versions; the loader's emptiness assert only
  counts live entities, so it passes, and recycled ids then diverge.
- **No in-place deletion storages.** Tombstones are written as null and
  skipped on load, compacting the storage. Only types that can't be moved or
  that opt in use it; plain structs don't. `static_assert`ed in `SimState`.
- **Don't use `registry.ctx()` for sim state.** It isn't snapshotted. Put
  sim-global state on a singleton entity or in a plain `World` member.
- **No groups.** They rearrange storage order.

## World layout

```cpp
struct World {
  entt::registry reg;
  TimeInt t_excess;
  const Grid *grid;       // the map: static, not owned, set only by SetMap
  // anything else plain that isn't per-entity
};
```

Snapshots = `SimState::save(reg)` + copies of the plain members. The map is
static (the sim only reads it), so snapshots store the pointer, not the
4.7 MB grid. Switching maps is the `SetMap` input, never a direct pointer
assignment, so input-log replays reproduce it.

## SimState wrapper (in example.cpp)

```cpp
using Sim = SimState<Transform, Physics, Movement, Owner, Odm, Hook /* ... */>;
```

- `save(reg)` → bytes, `load(bytes)` → a **fresh** registry (the only restore path).
- `static_assert` against in-place deletion for every listed type.
- `check_covered(reg)` (debug): asserts every non-empty storage is in the list,
  so a forgotten component can't silently vanish from snapshots.
- `same(a, b)`: the replay check. Compares entity ids/versions/free list and
  every component storage in order, **field-wise** via a generic
  `fields_equal` using C++26 structured binding packs. No per-component code,
  no `operator==`, no explicit padding. Padding is never compared (raw-byte
  comparison failed on garbage padding in `Hook`; restore itself never reads
  padding, so only the comparison needed fixing).
- Field-wise hashing for desync detection later: same recursion, never hash
  raw struct bytes.

Replay test (run in CI): keyframe → run N ticks → save A; load keyframe →
replay same inputs → compare with `Sim::same`. Also replay a recorded input
log twice and compare. Verified to pass on identical replays and fail when a
replay diverges by one tick.

## Rewind

Keyframe snapshots + input-log replay between them (always the plan). Log
`(SimInput, sim_dt)` per `exec` call, since inputs land at exec boundaries.
Keyframe spacing trades memory against scrub latency.

Done as `sim::Timeline` (`src/sim/timeline.hpp`), a thin wrapper over `Sim`.
Rollback is not a sim input. `Sim` keeps its `World` private: state changes
only through `apply`/`advance`/`exec` (the input funnel) or `restore`.

- The log is the record of what happened: timestamped inputs, never
  dropped on rollback. `insert(input, at)` adds one anywhere in time; in the
  past, the present is re-simulated to match.
- `rollback_to(t)` moves the present back; advancing replays later inputs.
- Keyframes sit at multiples of the interval, each from before the inputs
  at its own time, in a ring buffer. Inserting invalidates only later ones.
- Rejected inputs change nothing, but time still passes.

Physics modifiers are entities with one modifier component each (target
inside it), not a fixed-size table.

## Component mapping from current code

Names stay as they are (`raw_vel` is the persistent momentum, `vel` the
per-tick evaluated velocity; intentional).

- `CreatureTable` rows → entity with `Transform`, `Physics`, `Movement`,
  `Combat`, `Perception`.
- `EntityKind` / `Entity{kind,row}` / `kinds.hpp` / `creature_row`,
  `hook_row`, `physics_of`, `transform_of` → gone. Use views,
  `try_get<T>`, `valid`.
- `Hook` kind → a **projectile** entity: `Owner{creature}`, `Hook` (behaviour
  state), `Transform`, `Physics`. Hooks don't deserve their own kind.
- **ODM** is a component on the creature (not its own entity):
  `Odm{hook, burst_charges, burst_regen_left, flags}`. Moved out of
  `Movement`: `hook`, `hook_pulling` (`OdmFlag::HookPulling`, which movement
  reads from the creature's `Odm`), and gas burst charges/regen. The ODM
  system (`systems/odm`) does hooks and gas bursts.
- Items: each unique item = its own component struct (exact size, no cap) +
  a system that runs a view over it. Shared fields (`Owner`, slot) are their
  own components. No dispatch tables.
- `PhysicsModifierTable` (`DenseSlotMap`) can stay as a plain `World` member
  with `target` changed to `entt::entity` (it's already deterministic: dense
  order is a function of insert/erase history), or become entities with a
  `PhysicsModifier` component. Keeping it is the smaller port.
- `MoveFsm` (bitfsm, `FlagFsm`) stays inside `Movement` unchanged.

## System rules

- Tick order unchanged: hooks/ODM → combat → movement → physics →
  perception. Run the ODM system before movement so movement sees this tick's
  hook state. Movement reacting to last tick's `on_ground`/`wall_dir` is a
  known one-tick lag.
- Physics iterates `view<Transform, Physics>` — creatures and projectiles alike.
- **Structural changes are deferred**: never create/destroy/emplace/remove
  into a storage that the current view iterates. Queue them during the loop,
  apply after it in loop order. Queues are `ArenaVec`s from a per-tick
  scratch `Arena` (`util/arena.hpp`), sized from the relevant storage's
  `size()` so they can't fill, with an `ArenaScope` per system. The scratch
  arena lives in `World` but is not sim state and is never snapshotted. No
  `std::vector` in the tick path.
- Multithreading is allowed, deterministically:
  - parallel jobs write only their own rows; shared state is read-only that phase
  - fixed-size chunks by index, never by thread count or work-stealing order
  - reductions combined in chunk order (`max`/`min` are fine in any order,
    float sums are not)
  - structural changes go to per-chunk command buffers, applied on one thread
    in chunk order
  - per-entity/per-chunk RNG seeded from id + tick, no shared RNG
- Reserve every storage up front; assert no growth during a tick.

## Known items, not bugs

- Global adaptive tick (fastest body sets the tick for everyone) is
  intentional.
- `std::is_implicit_lifetime_v` in `util/plain.hpp` needs a recent standard
  library (GCC 13's doesn't have it).
- Stale comment in `movement.cpp`: says the hook reels in with a drive; it's a
  `Force` modifier.
- Collision is grid-only; creatures don't collide with each other.

## Port order

1. ~~Add EnTT, `SimState`, `fields_equal`, the replay test (from example.cpp).~~
2. ~~Replace `CreatureTable` with components; port systems to views. Get the
   replay test green.~~
3. ~~Replace the Hook kind with projectile entities + `Owner`.~~
4. ~~Add the ODM component; move the hook, `hook_pulling`, gas out of
   `Movement`.~~
5. ~~Delete `kinds.hpp`, `EntityKind`, the row lookups, `Plain<World>`.~~
6. ~~Reserve storages.~~ Done as `EntityState::reserve` on World creation and
   after every restore: packed arrays reserved, component sparse pages
   pre-touched. No capacity assert; `Guarantees.TicksDontAllocate` counts
   heap allocations over a churning workload instead (fresh and restored).
   The entity storage's own sparse pages are allocated as ids are first
   handed out (16 KB per 4096 ids); restore recreates them.
