#include "world.hpp"
#include "systems/combat/combat.hpp"

namespace sim {

std::expected<EntityID, SimErr> spawn_creature(World &world,
                                               Transform transform) noexcept {
  CreatureTable &t = world.creatures;
  if (t.len == CreatureTable::N) {
    return std::unexpected(SimErr::EntityLimit);
  }
  const uint32_t row = t.len;
  auto id = world.entities.insert(Entity{
      .kind = EntityKind::Creature,
      .row = row,
  });
  if (!id) {
    return std::unexpected(SimErr::EntityLimit);
  }
  t.len += 1;
  t.id[row] = *id;
  t.transform[row] = transform;
  t.physics[row] = Physics{};
  t.movement[row] = Movement{};
  t.combat[row] = Combat{};
  t.combat[row].health = CREATURE_HEALTH;
  // long ago, so it doesn't spawn looking freshly hit
  t.combat[row].since_hit = 1000;
  t.perception[row] = Perception{.value = 1};
  return *id;
}

std::expected<EntityID, SimErr> spawn_hook(World &world,
                                           Transform transform) noexcept {
  HookTable &t = world.hooks;
  if (t.len == HookTable::N) {
    return std::unexpected(SimErr::EntityLimit);
  }
  const uint32_t row = t.len;
  auto id = world.entities.insert(Entity{
      .kind = EntityKind::Hook,
      .row = row,
  });
  if (!id) {
    return std::unexpected(SimErr::EntityLimit);
  }
  t.len += 1;
  t.id[row] = *id;
  t.transform[row] = transform;
  t.physics[row] = Physics{};
  t.hook[row] = Hook{};
  return *id;
}

std::expected<void, SimErr> despawn(World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e) {
    return std::unexpected(SimErr::InvalidEntity);
  }
  const uint32_t row = e->row;
  switch (e->kind) {
  case EntityKind::Creature: {
    CreatureTable &t = world.creatures;
    const uint32_t last = t.len - 1;
    // move the last row into the gap and repoint its entity
    if (row != last) {
      t.id[row] = t.id[last];
      t.transform[row] = t.transform[last];
      t.physics[row] = t.physics[last];
      t.movement[row] = t.movement[last];
      t.combat[row] = t.combat[last];
      t.perception[row] = t.perception[last];
      world.entities.get_mut(t.id[row])->row = row;
    }
    t.len = last;
    break;
  }
  case EntityKind::Hook: {
    HookTable &t = world.hooks;
    const uint32_t last = t.len - 1;
    // move the last row into the gap and repoint its entity
    if (row != last) {
      t.id[row] = t.id[last];
      t.transform[row] = t.transform[last];
      t.physics[row] = t.physics[last];
      t.hook[row] = t.hook[last];
      world.entities.get_mut(t.id[row])->row = row;
    }
    t.len = last;
    break;
  }
  }
  // can't fail, `id` was just looked up
  (void)world.entities.erase(id);
  return {};
}

std::optional<uint32_t> creature_row(const World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e || e->kind != EntityKind::Creature) {
    return std::nullopt;
  }
  return e->row;
}

std::optional<uint32_t> hook_row(const World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e || e->kind != EntityKind::Hook) {
    return std::nullopt;
  }
  return e->row;
}

Physics *physics_of(World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e) {
    return nullptr;
  }
  switch (e->kind) {
  case EntityKind::Creature:
    return &world.creatures.physics[e->row];
  case EntityKind::Hook:
    return &world.hooks.physics[e->row];
  }
  return nullptr;
}

const Physics *physics_of(const World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e) {
    return nullptr;
  }
  switch (e->kind) {
  case EntityKind::Creature:
    return &world.creatures.physics[e->row];
  case EntityKind::Hook:
    return &world.hooks.physics[e->row];
  }
  return nullptr;
}

Transform *transform_of(World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e) {
    return nullptr;
  }
  switch (e->kind) {
  case EntityKind::Creature:
    return &world.creatures.transform[e->row];
  case EntityKind::Hook:
    return &world.hooks.transform[e->row];
  }
  return nullptr;
}

const Transform *transform_of(const World &world, EntityID id) noexcept {
  const Entity *e = world.entities.get(id);
  if (!e) {
    return nullptr;
  }
  switch (e->kind) {
  case EntityKind::Creature:
    return &world.creatures.transform[e->row];
  case EntityKind::Hook:
    return &world.hooks.transform[e->row];
  }
  return nullptr;
}

} // namespace sim
