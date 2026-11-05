# C++ Style Guide: Data-Oriented, Restricted, Enforced

C's data model, Rust's discipline, and a small set of modern C++ features to make it pleasant, all enforced by tooling rather than willpower.

---

## Data

- Everything core logic touches is plain data.
- Define one concept and assert it next to every core type:

```cpp
  template <typename T>
  concept Pod = std::is_trivially_copyable_v<T>
             && std::is_trivially_default_constructible_v<T>
             && std::is_standard_layout_v<T>;

  static_assert(Pod<Entity>);
```

- No default member initializers in core types. Zero-initialize at creation with `T x{}` or `std::make_unique<T>()`.
- Relationships are indices or generational handles, never pointers, so state can be copied, hashed, snapshotted, and replayed.
- Hot data is struct-of-arrays. Cold data is array-of-structs.
- Variants are an `enum class` tag plus an anonymous union of POD members. Switch on the tag with no `default:` so `-Wswitch-enum -Werror` gives Rust-style exhaustiveness.

## Memory

- Lifetimes are grouped, not individual.
  - **Long-lived contiguous blocks** hold the world and other persistent state.
  - **Tick scratch arena** resets every tick, for broadphase grids, pair lists, despawn lists, and command buffers.
  - **Function scratch** takes a scratch arena, records a mark, and rolls back on return.
- Things with unpredictable individual lifetimes live in pools or slotmaps inside those blocks.
- Nothing in the core calls `new`, `delete`, or `malloc` after startup.
- RAII only for coarse OS-level resources: files, sockets, GPU objects, the arenas themselves.

## Code

- Logic is mostly free functions that transform arrays: data in, data out, explicit about what they read and write.
- Polymorphism is a `switch` on a tag, or separate arrays per type. Virtual functions only at rare, cold, plugin-style boundaries.
- Templates for a handful of generic containers and small utilities. No metaprogramming.
- Errors are values: `std::expected` or error enums.
- Invariants are checked with `assert`.

## Modern C++ to Keep

- Concepts
- `std::expected`
- Designated initializers
- `enum class`
- `constexpr`
- `std::span`
- `std::bit_cast`
- Structured bindings
- Lambdas as local helpers
- `static_assert`
- Operator overloading for math types

## What to Leave Out

- Exceptions
- RTTI and `dynamic_cast`
- Inheritance hierarchies
- `std::shared_ptr`, `std::function`
- `std::map`, `std::list`
- iostreams
- Anything with non-deterministic iteration order inside the simulation (e.g. iterating `std::unordered_map`)
- Type punning through unions or `reinterpret_cast`. Use `std::bit_cast` or `memcpy` only.

## Enforcement

Rules the compiler enforces don't depend on remembering them.

**Compiler flags**

```
-std=c++23 -fno-exceptions -fno-rtti
-Wall -Wextra -Werror -Wswitch-enum
-ffp-contract=off          # simulation code
```

**Debug builds**

- AddressSanitizer and UndefinedBehaviorSanitizer always on.
- A debug-only global `operator new` that aborts while a "no-alloc" flag is set during ticks.

**Static checks**

- `static_assert(Pod<T>)` on every core type.
- `static_assert(sizeof(World) < BUDGET)` for snapshot memory budgets.
- clang-tidy with modernize and custom checks.
- `#pragma GCC poison rand time malloc` (and similar) in simulation translation units.

**Architecture**

- The simulation is its own library target that cannot include renderer, platform, input, or clock headers.

## Skills That Make This Style Work

- Profiling with `perf` or VTune. Measure, don't guess.
- Reading assembly via Compiler Explorer.
- Reasoning about caches, branch prediction, and memory layout.
- Writing arenas, pools, and a lock-free SPSC ring buffer from scratch.
- Knowing C++'s object lifetime and aliasing rules well enough to recognize when C-style code is actually undefined behavior.

## Reading

- Mike Acton, *Data-Oriented Design and C++* (CppCon 2014)
- Ryan Fleury, *Untangling Lifetimes: The Arena Allocator*
- Carl Cook, *When a Microsecond Is an Eternity* (CppCon 2017)
- Richard Fabian, *Data-Oriented Design* (free online)
- Martin Fowler, *The LMAX Architecture*
- Orthodox C++ (Branimir Karadžić)
