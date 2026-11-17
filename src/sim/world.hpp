#pragma once
#include "sim/sim.hpp"
#include <cstdint>
#include <expected>
#include <optional>

namespace sim {

// Adds a creature with the given transform, full health, and every other
// component zeroed.
std::expected<EntityID, SimErr> spawn_creature(World &world,
                                               Transform transform) noexcept;

// Adds a hook with the given transform and every other component zeroed.
std::expected<EntityID, SimErr> spawn_hook(World &world,
                                           Transform transform) noexcept;

// Removes an entity of any kind. The last row of its kind's table moves into
// its place, so rows and pointers into that table are invalidated.
std::expected<void, SimErr> despawn(World &world, EntityID id) noexcept;

// Row in world.creatures, or nullopt if `id` is dead or not a creature.
std::optional<uint32_t> creature_row(const World &world, EntityID id) noexcept;
// Row in world.hooks, or nullopt if `id` is dead or not a hook.
std::optional<uint32_t> hook_row(const World &world, EntityID id) noexcept;

// Component lookup by key, for any kind that has the component. nullptr if
// `id` is dead or its kind doesn't have one. For one-off access (a modifier
// reaching its target, main finding the player); systems should loop over
// the tables instead.
Physics *physics_of(World &world, EntityID id) noexcept;
const Physics *physics_of(const World &world, EntityID id) noexcept;
Transform *transform_of(World &world, EntityID id) noexcept;
const Transform *transform_of(const World &world, EntityID id) noexcept;

} // namespace sim
