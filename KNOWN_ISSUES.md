# Known issues

Open problems, grouped by area. Fix or delete entries as they're dealt with.

## Inputs and the timeline

- **No stable references in inputs.** Inputs target entities by raw
  `entt::entity` (`SetMovementIntents`, `SetMovementParams`, `LookAt`).
  Inserting an input into the past (`Timeline::insert`) can change which ids
  later spawns get, so logged inputs after it can end up pointing at a
  different entity, or a dead one, after the re-simulation. Deterministic,
  but wrong. Inputs need a stable way to name what they control (a player
  or controller slot, a spawn-order handle, ...) instead of an id that
  depends on history.
- **Re-simulated results aren't reported.** `Timeline::exec` returns the
  input's result when it's first applied. After an insert into the past,
  the same input may get a different result (a spawn gets another id, an
  accepted input is now rejected) and nothing tells the caller. Same root
  cause as stable references.
- **Inserting far back costs a frame spike.** An insert at time `at`
  re-simulates from the keyframe before `at` all the way to now, up to the
  whole kept history (about `keyframe_count * keyframe_interval`, 16 s by
  default) in one call.
- **Allocation outside ticks, every frame.** `Timeline::exec` grows the log,
  and every keyframe allocates a fresh snapshot buffer. Ticks themselves
  don't allocate. Log inserts and front-drops are `std::vector` insert/erase
  (O(n)).
- **`LookAt` is unimplemented.** It's always rejected.

## The engine around the sim

- **The demo reads sim memory directly.** `src/main.cpp` walks the
  registry through `Sim::world()` / `Timeline::sim()` every frame. Sims are
  going to run on their own threads, so that can't last: renderable info
  has to come out through viewport primitives instead (design to come,
  along with a precision-lossy accumulator design). Until then, don't add
  new outside code that depends on reading `World`.
- **raylib is throwaway.** Rendering moves to wgpu, and input to a generic
  input API that produces `SimInput`s.

## Across the wire (different processes, machines, builds)

Everything below works within one process and breaks as soon as state or
inputs move between environments (netcode, saved replays, desync checks).

- **Map pointers aren't portable.** `SetMap` carries a `const Grid *`,
  snapshots store it, and `same_state` compares pointers. A map needs an id
  (or content hash) that means the same thing everywhere.
- **The input log can't be serialized.** `SimInput` holds that pointer, and
  there's no wire format for the variant.
- **Snapshots aren't portable.** The archive (`sim/sim_state.hpp`) writes
  components as raw bytes: struct layout, padding bytes (uninitialized) and
  endianness all depend on the compiler and platform. Fine for in-process
  keyframes, not for sending or hashing.
- **Float determinism across environments: libm is resolved; the
  assumptions below remain.** The sim's `exp`, `sin` and `cos` come from
  `util/det_math.hpp` (lookup tables built at compile time, plus exactly
  rounded operations only), and the `SimDoesntCallLibm` test fails if sim or
  util code calls a libm function whose rounding varies. `det_math_test.cpp`
  pins exact output bits, so a platform that computes differently fails
  there. What it still relies on:
  - every float operation rounding to float: `-ffp-contract=off` (set on
    the sim target), no fast-math (an `#error` in `det_math.hpp`), no x87
    excess precision (a `static_assert` on `FLT_EVAL_METHOD`)
  - nothing in the sim calling libm in a way the check doesn't spot (it
    looks for `std::` calls; a bare C `sinf` or a library doing it
    internally would get past it)
  - only the platforms `tools/cross_check.sh` has covered: x86-64 (GCC 16
    at -O2 and -O0, Clang 23), ARM64 and RISC-V (GCC 16, under qemu) agree
    bit for bit on det_math and on a 3000-input sim run. Allowing fused
    multiply-add on ARM64 (`-ffp-contract=fast`) changes every hash, so
    that flag has to stay off. Not run yet: MSVC/Windows, real ARM or
    RISC-V hardware (qemu emulates IEEE floats exactly, so it should
    agree).
- **No state hash.** Desync detection needs a field-wise hash (same
  recursion as `fields_equal`, never raw struct bytes). Not written yet.

## Sim state and guarantees

- **The map is static now.** If terrain becomes destructible or cellular,
  the grid is sim state again: snapshots, comparisons and rollback need it
  (or its changes) back.
- **The entity storage's sparse pages still allocate.** Reserving can't
  pre-create them without creating entities, so the first id in each new
  block of 4096 allocates 16 KB, possibly inside a tick (see
  `SimState::reserve`). Restores recreate them.
- **More than `ENTITY_RESERVE` (8192) of anything allocates inside a tick.**
  Storages just grow; nothing asserts.
- **Snapshot coverage is only checked in debug.** `check_covered` asserts
  every non-empty storage is listed in `EntityState`; release builds skip it.
- **EnTT is pinned to a master commit.** Exact restore depends on how its
  snapshots and storages behave (see the README's snapshot section and
  ECS rules). Updating means
  re-checking that, and re-running the guarantee tests.
- **Structural changes inside loops rely on a rule, not a check.** Systems
  create or destroy entities mid-loop only when none of the touched storages
  is one the loop iterates (the hook pull force in `systems/odm`). Nothing
  enforces it.
- **`FlagFsm` epochs wrap after 256 cancels.** A hold kept across exactly 256
  cancels of the same flag looks valid again (`util/fsm.hpp`).

## Performance

- **Entity queries are linear scans. Needs a real spatial structure.**
  `for_each_overlapping`, `first_overlapping` and the entity casts in
  `sim/spatial.hpp` walk every collider. Combat's slash loop is O(n^2). The
  plan is a proper spatial index (something like a quadtree, the 2D
  octree). The queries are the only place that walks colliders, so it can
  replace the loops without changing callers. Its traversal order has to
  depend only on history, like everything else (e.g. no ordering by
  pointer).
- **Single-threaded.** Biggest planned gain, with viewports. Separate sims
  (the real-time one, the demo's hologram) share nothing and can each get a
  thread as is. Inside one sim, the README's parallelism rules apply (fixed
  chunks, per-chunk command buffers applied in chunk order, ordered
  reductions).
- **Everything is simulated and drawn everywhere.** The other big planned
  gain: viewports. Drawing only what a viewport sees is free; simulating
  only what's near one has to be decided by sim state (deterministic
  activity regions), never by where a camera is, or sims that should agree
  stop agreeing.
- **Global adaptive tick.** The fastest body (or highest perception)
  shortens the tick for everyone. Intentional, but one very fast entity makes
  the whole world tick more often.

## Gameplay placeholders

- Creatures don't collide with each other; collision is grid-only.
- One hook slot per ODM (the original plan had two).
- Hook, gas, slash and perception tunables live in `MovementParams` until
  they get their own params.
- A slash remembers at most 8 hits; past that, targets can be hit again in
  the same slash.

## Toolchain and tests

- **Needs C++26.** `fields_equal` uses structured binding packs: GCC 16 or
  Clang 22+.
- **`std::is_implicit_lifetime_v`** (`util/plain.hpp`) needs a recent
  standard library (GCC 13's doesn't have it).
- **UBSan's `vptr` check is off for the tests.** The sim is built with
  `-fno-rtti`, the tests with RTTI, and the check false-positives across that
  boundary (`tests/CMakeLists.txt`).
