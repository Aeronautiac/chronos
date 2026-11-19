#pragma once
#include "sim/sim.hpp"
#include <cstddef>
#include <vector>

namespace sim {

// A World's sim state: the registry (EntityState bytes) plus a copy of
// every plain member. Restoring one is indistinguishable from never having
// left it: entity ids, versions, free lists and storage order included.
// Keyframes for rewind are these.
struct Snapshot {
  std::vector<std::byte> entities;
  TimeInt t_excess;
  // which map, not a copy of it (see World::grid)
  const Grid *grid;
};

// What Sim::save and Sim::restore do. Only Sim calls these: outside code
// can't get a mutable World (state changes only through Sim).
Snapshot save_world(const World &world);

// Replaces all of `world`'s sim state with the snapshot's: a fresh registry
// (the only exact restore path), reserved like a new World's.
void restore_world(World &world, const Snapshot &snap);

// The replay check: `a` and `b` hold identical sim state, field by field
// (see SimState::same and fields_equal). Padding is never compared.
bool same_state(const World &a, const World &b);

} // namespace sim
