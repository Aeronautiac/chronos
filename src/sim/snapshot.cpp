#include "snapshot.hpp"

namespace sim {

Snapshot save_world(const World &world) {
  return Snapshot{
      .entities = EntityState::save(world.reg),
      .t_excess = world.t_excess,
      .grid = world.grid,
  };
}

void restore_world(World &world, const Snapshot &snap) {
  world.reg = EntityState::load(snap.entities);
  EntityState::reserve(world.reg, ENTITY_RESERVE);
  world.t_excess = snap.t_excess;
  world.grid = snap.grid;
}

bool same_state(const World &a, const World &b) {
  return EntityState::same(a.reg, b.reg) &&
         fields_equal(a.t_excess, b.t_excess) &&
         // same map: the same Grid, since it's static
         a.grid == b.grid;
}

} // namespace sim
