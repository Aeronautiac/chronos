#include "world.hpp"
#include "systems/combat/combat.hpp"
#include "systems/movement/movement.hpp"

namespace sim {

World::World() noexcept { EntityState::reserve(reg, ENTITY_RESERVE); }

entt::entity spawn_creature(World &world, Vec2 pos) noexcept {
  entt::registry &r = world.reg;
  const entt::entity e = r.create();
  r.emplace<Transform>(e, Transform{.pos = pos, .rot = 0});

  Physics p{};
  p.collider = {.w = 12, .h = 24};
  // two cells: walks up and down the terrain's staircase slopes
  p.step_height = 2 * Grid::CELL_SIZE;
  p.flags.mask = bits({PhysicsFlag::Collides, PhysicsFlag::Gravity});
  r.emplace<Physics>(e, p);

  Combat cb{};
  cb.health = CREATURE_HEALTH;
  // long ago, so it doesn't spawn looking freshly hit
  cb.since_hit = 1000;
  r.emplace<Combat>(e, cb);

  r.emplace<Perception>(e, Perception{.value = 1});
  add_movement(world.reg, e);

  // full tank
  const MovementParams &mp = r.get<Movement>(e).params;
  Odm odm{};
  odm.burst_charges = static_cast<uint8_t>(mp.burst_charges);
  odm.burst_regen_left = mp.burst_regen_time;
  r.emplace<Odm>(e, odm);
  return e;
}

} // namespace sim
