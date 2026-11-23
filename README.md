# chronos

[chronos_demo_trimmed.webm](https://github.com/user-attachments/assets/62001607-5399-42d4-bd0d-645936f54719)

A 2D movement and time-manipulation action game, built on a deterministic,
rewindable simulation. This repo is the sim plus a throwaway raylib demo
used for feel prototyping.

The sim is the point: given the same starting state and the same inputs it
produces the same state, bit for bit, on every supported platform, and any
moment of it can be rewound to or rewritten.

**Most gameplay code is a prototype.** Movement, physics behaviour, combat,
the ODM (hook and gas), perception and the test level exist to try out feel
and to give the sim something to run, and will be rewritten. The parts this
README describes as guarantees (the input funnel, snapshots, the timeline,
deterministic floats, spatial queries, the ECS rules) are the foundation;
the gameplay built on them isn't.

**The engine around the sim doesn't exist yet.** `src/main.cpp` is a
throwaway raylib demo. The plan is the rest of the engine as its own layer:
rendering on wgpu, a generic input API that turns devices into `SimInput`s,
and so on. The sim stays its own library target that knows nothing about
any of it, as now.

Simulations will run on their own threads, so nothing outside a sim can
keep reading its memory directly (the demo does today, through
`const World &`). Renderable info will come out through viewport
primitives instead, alongside a precision-lossy accumulator design (to
come).

## Building

CMake presets (Ninja). C++26 is required: GCC 16 or Clang 22+.

```sh
cmake --workflow --preset ci      # debug + ASan/UBSan, warnings as errors, tests
cmake --workflow --preset debug   # same without -Werror
cmake --preset release && cmake --build --preset release
./build/release/game              # the demo
```

EnTT, raylib and GoogleTest are fetched by CMake (installed copies of raylib
and GoogleTest are used if found). EnTT is pinned to an exact commit, since
exact restore depends on how its storages and snapshots behave.

## Layout

```
src/sim/            the simulation (its own library target, no renderer/platform code)
  sim.hpp           Sim (the input funnel), inputs, World, the component list
  components.hpp    plain-struct components
  sim_state.hpp     registry snapshots, field-wise comparison, reserving
  snapshot.hpp      World snapshots and the replay check
  timeline.hpp      input log, keyframes, insert and rollback (wraps Sim)
  spatial.hpp       grid and collider queries, rays, box casts
  grid.hpp          the map
  systems/          odm, combat, movement, physics, perception
src/util/           primitives: arena, flag FSM, flags, slot maps, det_math
src/main.cpp        throwaway raylib demo: reads the sim, sends it inputs
tests/              guarantee and primitive tests
tools/              cross-platform determinism check
```

Related docs: `KNOWN_ISSUES.md` (open problems), `style.md` (code style).

## Design

### The sim and the input funnel

`Sim` owns a `World` and keeps it private. Outside code gets
`const World &world()` and nothing else, so the only ways state changes are:

- `apply(input)`: applies a `SimInput` at the current time. It's checked
  against the state right now; a rejected input changes nothing.
- `advance(dt)`: lets world time pass, running every tick that fits.
- `exec(input, dt)`: `apply` then `advance`. Time passes whether or not the
  input was accepted.
- `restore(snapshot)`: replaces the whole state (used by the timeline).

Anything that affects the sim is a `SimInput` (`SetMap`, `SpawnCreature`,
`SetMovementIntents`, `GasBurst`, `FireHook`, `ReleaseHook`, `Slash`,
`SetMovementParams`, `LookAt`), so a log of inputs reproduces everything.
Even switching maps is an input.

`SetMovementIntents` is what a creature holds (walk, jump, sprint, slide),
which movement acts on every tick. Actions (gas bursts, the hook, slashes)
are inputs of their own. `apply` holds no game logic for them: each one
dispatches to an action function in its system (`gas_burst`, `fire_hook`
and `release_hook` in `systems/odm`, `slash` in `systems/combat`), which
checks the action against the state right then (gas left, hook out,
cooldown) and either does it or rejects it, so an action that can't happen
changes nothing.

### Time

Ticks are adaptive. `tick_dur()` is worked out again before every tick, from
the state:

- Tick size is dynamic based on simulation state. If an entity has high perception, or something is moving very fast, the tick duration will shorten. If not much is happening, it will widen.
- Shorter still so the fastest body moves at most `TICK_MAX_TRAVEL` per
  tick, and divided by the highest perception (high perception slows the
  world).

Time that doesn't fill a tick carries over in `World::t_excess`. Because of
that carry, splitting an `advance` into pieces runs exactly the same ticks
as doing it in one go, so inputs can land at any point in time, not just on
tick boundaries.

Tick order: ODM (hooks, gas) → combat → movement → physics → perception.

### World and entities

`World` is an EnTT registry plus a few plain members: `t_excess` and a
pointer to the map. Everything per-entity lives in the registry as plain,
trivially copyable component structs, and systems run views over the
components they need. There are no entity kinds: a creature is just an
entity with Transform, Physics, Movement, Combat, Perception and Odm; a hook
is a projectile with Owner, Hook, Transform and Physics; a physics modifier
is an entity with one modifier component.

Rules that keep this deterministic and exactly restorable:

- **Every component type is listed in `EntityState`** (`sim.hpp`). A
  missing one would silently vanish from snapshots; saving asserts none is
  (debug builds).
- **No in-place-delete storages.** EnTT snapshots write their holes as null
  and skip them on load, so a restored storage would come back compacted, in
  a different order. Plain structs never use in-place delete;
  `SimState` `static_assert`s it for every listed type.
- **No sim state in `registry.ctx()`.** Snapshots don't include it. State
  that isn't per-entity is a plain `World` member.
- **No groups.** They rearrange storages' order, which is state.
- **Structural changes are deferred.** Creating, destroying, emplacing or
  removing never touches a storage the current loop iterates. Changes are
  queued in `ArenaVec`s from the per-tick scratch arena, sized from the
  relevant storage's size so they can't fill, and applied after the loop in
  loop order. (Touching a storage the loop doesn't iterate is allowed, e.g.
  creating a hook's pull force inside the hook loop.)
- **Storages are reserved up front**, on creation and after every restore,
  so ticks don't allocate.

### Parallelism (planned)

Not done yet, but the sim is built for it, under these rules so results
don't depend on threads:

- Parallel jobs write only their own rows; anything shared is read-only in
  that phase.
- Work is split into fixed-size chunks by index, never by thread count or
  work-stealing order.
- Reductions are combined in chunk order (min and max are fine in any
  order; float sums aren't).
- Structural changes go to per-chunk command buffers, applied on one thread
  in chunk order.
- Randomness is seeded per entity or per chunk from ids and the tick, never
  from a shared generator.

Separate sims (the demo's real-time one and its hologram) share nothing and
can each have a thread as they are.

### The map

The map (`Grid`) is static: the sim only reads it, and `World` points at it
instead of owning it. Snapshots store the pointer, not the 4.7 MB grid. It's
switched with the `SetMap` input.

### Physics, movement and spatial queries

(Physics and movement are prototype gameplay, see the top: this is how
they work today, not a design to keep. The spatial query module is part of
the foundation.)

Physics integrates bodies against the grid (swept per axis, flush stops,
stepping over small ledges) and applies modifiers: velocity, force, angular
ones, and drives (pull toward a target velocity; drives on one body average
by strength). Each modifier is an entity holding its target, and each kind
is one storage walked in order.

Movement finds ground and walls itself with box casts rather than reading
physics' contacts; the only physics behaviour it relies on is stepping over
small ridges. Spatial queries all live in `spatial.hpp`: point and box tests
against the grid, sweeps, rays, box casts, and collider overlap and cast
queries. Nothing else scans cells or colliders, so a real spatial index can
replace the linear loops later without changing callers.

### Snapshots and the replay check

`SimState<Ts...>` (`sim_state.hpp`) saves the registry with EnTT's snapshot
(every storage in packed order, the entity storage with every id ever handed
out and its free list) and loads it only into a freshly constructed
registry, which reproduces ids, versions, recycling order and storage order
exactly. A cleared registry wouldn't do: `clear()` keeps destroyed ids with
bumped versions, so recycled ids would come out different.

`same_state(a, b)` is the replay check: same entity ids, versions and free
list, and every storage holding the same entities in the same order with
equal values. Values are compared field by field (C++26 structured binding
packs, no per-type code), so padding never matters, and floats by bit
pattern.

### The timeline

`Timeline` (`timeline.hpp`) wraps a `Sim` with everything rewinding needs.
None of it is sim state, and rollback is not an input.

- The log of timestamped inputs is the record of what happened. It's never
  dropped by a rollback.
- `insert(input, at)` adds an input anywhere in time. In the past, the sim
  re-simulates from the last keyframe before it to now, so the present is
  what it would have been with that input all along.
- `rollback_to(t)` moves the present back; advancing again replays the
  logged inputs after it.
- Keyframes are taken at fixed multiples of an interval, each from before
  the inputs at its own time, in a ring buffer. A new input, whether
  inserted into the past or applied at a present that was rolled back to,
  invalidates only the keyframes after it.
- A timeline can start from any snapshot, on its own clock from 0, and
  carries on exactly like the sim the snapshot came from.

### The demo

`src/main.cpp` (throwaway, raylib) runs the test level with a player and a
dummy, and shows off rollback with a hologram: a second timeline started
from a snapshot of the real one, heading for an offset ahead of it. Every
input rolls the hologram back to the present, where it takes the input,
and it then catches up to its target on a spring. It shows where everything
goes if you keep doing what you're doing.

Movement input is only sent when the held keys change, with the aim
alongside (the sim only reads aim on a press), so frames with nothing new
just let time pass.

Controls: A/D move, space jump, shift sprint, ctrl slide, Q gas burst,
right mouse hook, left mouse slash. H turns the hologram on and off (off
means not simulated at all; `--no-hologram` starts with it off), `-`/`=`
change its offset, `[`/`]` scale time, R respawns the dummy, F1 opens the
movement parameter editor.

### Deterministic floats

Same binary on the same machine is easy; across compilers and CPUs it isn't.

- The sim target builds with `-ffp-contract=off` (no fused multiply-add),
  never fast-math, and with no excess precision.
- libm's transcendental functions aren't required to round the same
  everywhere, so the sim uses `util/det_math.hpp`: `det::exp`, `det::sin`,
  `det::cos` from compile-time lookup tables plus only exactly rounded
  operations. About one float ulp of error, about as fast as glibc.
- `+ - * /`, `sqrt`, `floor`, `abs`, min/max and comparisons are exactly
  rounded by IEEE 754, so they're used freely.

## Core requirements and how each is verified

Tests cover hard guarantees and core primitives only, never gameplay (feel
gets tuned in the game). `ctest --preset ci` runs them with ASan and UBSan.
Several were also checked against a deliberately broken implementation and
fail there: the allocation counter (with sparse pages not pre-touched), the
box-cast corner tests (with the corner handling removed), the rollback
tests (with partial replay and keyframe invalidation removed), the libm
check (with a planted `std::sin`), and the cross-platform check (with fused
multiply-add allowed).

| Requirement | Enforced by | Verified by |
|---|---|---|
| Determinism: same start + same inputs → identical state | Input funnel; history-only iteration order; no libm | `Guarantees.SameLogTwiceIsIdentical`, `Guarantees.RewindAndReplayIsIdentical` |
| The replay check isn't vacuous | | `RewindAndReplayIsIdentical` also asserts one extra tick makes the states differ |
| Exact restore (ids, versions, free list, storage order) | Load only into a fresh registry; `static_assert` against in-place-delete storages | `Guarantees.RewindAndReplayIsIdentical`, `Guarantees.RestoreHandsOutTheSameIds` (spawns after a restore get the same ids as the original would) |
| Every component is snapshotted | `EntityState` list; `check_covered` on every save (debug) | `Guarantees.SavingAnUnlistedComponentAsserts` |
| The map is part of the state (which one, not its cells) | `SetMap` is the only way to set it; snapshots store it | `Guarantees.RestoreBringsBackItsMap`, `Guarantees.NullMapIsRejected`, map switch inside the rewind workload |
| Iteration order depends only on history | No groups, no unordered containers; deferred structural changes | Indirectly: every replay test compares storages in order, so a different order fails them |
| Handles never dangle | Generational `entt::entity` ids | `Guarantees.StaleHandlesFindNothing` (a dead creature's id finds nothing even after its slot is reused) |
| No allocation in a tick | Reserved storages, pre-touched sparse pages, scratch arena for queues | `Guarantees.TicksDontAllocate` counts every `operator new` over a churning workload, on a fresh world and after a restore |
| State only changes through the input funnel | `Sim` keeps `World` private (a compile error otherwise) | `Guarantees.RejectedInputsOnlyPassTime` (a rejected input equals just letting time pass) |
| Rollback restores the past exactly | Keyframes + replay; split advances run the same ticks | `Rollback.MatchesThePast` (at an input's time and partway through one), `Rollback.KeepsTheLog`, `Rollback.StartsFromASnapshot` |
| Inputs inserted into the past adjust the present | Keyframe invalidation + re-simulation | `Rollback.InsertingIntoThePastAdjustsThePresent`, `Rollback.InputAfterRollbackRedoesLaterKeyframes`, `Rollback.InsertingIntoTheFutureWaitsForIt`, `Rollback.OnlyWithinTheKeptRange` |
| Same float bits on every platform | `-ffp-contract=off`; `#error` on fast-math; `static_assert(FLT_EVAL_METHOD == 0)`; `det_math` | `DetMath.*` (accuracy, edge values, pinned output bits); `SimDoesntCallLibm` (fails on libm calls in sim/util); `tools/cross_check.sh` |

The tests run on the machine you build on. `tools/cross_check.sh` covers other
platforms: it builds `tools/det_check.cpp` (det_math over a million inputs
each, plus a 3000-input sim run hashed every 500 steps) with every compiler
and target it finds, runs other CPUs under qemu user mode, and diffs each
against native GCC. So far x86-64 (GCC at -O2 and -O0, Clang), ARM64 and
RISC-V agree bit for bit, and building ARM64 with fused multiply-add allowed
changes every hash, so the check does catch drift.

Core primitives have their own tests: `Arena*`, `FlagFsm*`, `SlotMap*` /
`DenseSlotMap*` (many as `static_assert`s, so constant evaluation also
rules out UB), and the spatial queries (`Sweep`, `Raycast`, `Boxcast`
including diagonal corner cases, `Grid`, `Overlap`, `EntityRaycast`,
`EntityBoxcast`).

## Known gaps

See `KNOWN_ISSUES.md`. The big ones: inputs reference entities by raw id,
which isn't stable when inputs are inserted into the past; map pointers,
snapshots and the input log aren't portable between processes yet; and
entity queries are linear scans until a spatial index lands.
