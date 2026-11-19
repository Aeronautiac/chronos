#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
#include "util/arena.hpp"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

//====== COMPONENTS: plain structs, nothing EnTT-specific ======//

struct Vec2 { float x, y; };
struct Transform { Vec2 pos; float rot; };
struct Physics { Vec2 vel; Vec2 raw_vel; Vec2 size; bool on_ground; };
// a tag: no data, just marks the entity
struct Player {};
struct Movement { uint8_t intents; uint8_t prev_intents; Vec2 aim; };

// anything owned by something else: an ODM by a creature, a hook by an ODM
struct Owner { entt::entity e; };

// the gear: two hook slots and a gas tank
struct Odm { entt::entity hooks[2]; float gas; };

// a projectile's behaviour. A bullet would be a different component.
enum class HookState : uint8_t { Flying, Attached, Returning };
struct Hook { HookState state; entt::entity stuck_to; };

constexpr uint8_t INTENT_HOOK = 1 << 5;

//====== WORLD ======//

struct World {
  entt::registry reg;
  // per-tick scratch for deferred changes. Not sim state, never snapshotted:
  // every system rewinds it before returning.
  Arena scratch{1 << 20};
  // plain members still fine next to the registry (the grid, t_excess, ...)
  uint64_t tick = 0;
};

//====== SPAWNING ======//

entt::entity spawn_creature(entt::registry &r, Vec2 pos) {
  const auto e = r.create();
  r.emplace<Transform>(e, pos, 0.0f);
  r.emplace<Physics>(e, Physics{.vel = {}, .raw_vel = {}, .size = {12, 24}});
  r.emplace<Movement>(e);
  r.emplace<Player>(e);

  // the ODM is its own entity, owned by the creature
  const auto odm = r.create();
  r.emplace<Owner>(odm, e);
  r.emplace<Odm>(odm, Odm{.hooks = {entt::null, entt::null}, .gas = 100});
  return e;
}

//====== SYSTEMS: views over whatever components they need ======//

// Queued while iterating, applied after the loop in loop order. Can't add
// Owners while iterating a view over Owner.
struct FireHook { entt::entity odm; int slot; Vec2 pos; Vec2 vel; };

void run_odm(entt::registry &r, Arena &scratch) {
  ArenaScope scope{scratch};
  // at most one fire per ODM per tick, so this can't fill up
  auto fire = *scratch.vec<FireHook>(
      static_cast<uint32_t>(r.storage<Odm>().size()));

  r.view<Odm, Owner>().each([&](entt::entity self, Odm &odm, const Owner &o) {
    const auto *m = r.try_get<Movement>(o.e);
    const auto *t = r.try_get<Transform>(o.e);
    if (!m || !t) return;
    const bool pressed = (m->intents & ~m->prev_intents) & INTENT_HOOK;
    if (pressed && odm.hooks[0] == entt::null) {
      (void)fire.push({self, 0, t->pos, {m->aim.x * 900, m->aim.y * 900}});
    }
  });

  for (const FireHook &f : fire) {
    const auto hook = r.create();
    r.emplace<Owner>(hook, f.odm);
    r.emplace<Hook>(hook, Hook{.state = HookState::Flying, .stuck_to = entt::null});
    r.emplace<Transform>(hook, f.pos, 0.0f);
    r.emplace<Physics>(hook, Physics{.vel = {}, .raw_vel = f.vel, .size = {4, 4}});
    r.get<Odm>(f.odm).hooks[f.slot] = hook;
  }
}

void run_hooks(entt::registry &r, Arena &scratch) {
  ArenaScope scope{scratch};
  // at most every hook dies
  auto dead = *scratch.vec<entt::entity>(
      static_cast<uint32_t>(r.storage<Hook>().size()));
  r.view<Hook, Owner, Transform>().each(
      [&](entt::entity self, Hook &h, const Owner &o, Transform &t) {
        if (!r.valid(o.e)) {
          (void)dead.push(self); // its ODM is gone, so is it
          return;
        }
        // ... Flying / Attached / Returning, same logic as hook.cpp
        (void)h;
        (void)t;
      });
  for (auto e : dead) r.destroy(e);
}

void integrate(entt::registry &r, float dt) {
  // creatures and hooks alike: anything with Transform + Physics
  r.view<Transform, Physics>().each([dt](Transform &t, Physics &p) {
    p.vel = p.raw_vel;
    t.pos.x += p.vel.x * dt;
    t.pos.y += p.vel.y * dt;
  });
}

void do_tick(World &w, float dt) {
  run_odm(w.reg, w.scratch);
  run_hooks(w.reg, w.scratch);
  // run_movement(w.reg, dt);
  integrate(w.reg, dt);
  w.tick += 1;
}

//====== SNAPSHOTS ======//

struct OutArchive {
  std::vector<std::byte> &buf;
  template <typename T> void operator()(const T &v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto *p = reinterpret_cast<const std::byte *>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
  }
};

struct InArchive {
  std::span<const std::byte> buf;
  size_t pos = 0;
  template <typename T> void operator()(T &v) {
    std::memcpy(&v, buf.data() + pos, sizeof(T));
    pos += sizeof(T);
  }
};

template <typename... Ts>
bool same_state(const entt::registry &a, const entt::registry &b);

template <typename... Ts> struct SimState {
  static_assert((!entt::component_traits<Ts>::in_place_delete && ...),
                "in-place deletion breaks exact restore");

  static std::vector<std::byte> save(const entt::registry &r) {
    std::vector<std::byte> buf;
    OutArchive out{buf};
    entt::snapshot snap{r};
    snap.get<entt::entity>(out);
    (snap.get<Ts>(out), ...);
    return buf;
  }

  // the only way to restore: always into a fresh registry
  static entt::registry load(std::span<const std::byte> bytes) {
    entt::registry r;
    InArchive in{bytes};
    entt::snapshot_loader loader{r};
    loader.get<entt::entity>(in);
    (loader.get<Ts>(in), ...);
    return r;
  }

  // the replay check: same ids, order and field values in every storage
  static bool same(const entt::registry &a, const entt::registry &b) {
    return same_state<Ts...>(a, b);
  }

  // a component type missing from Ts would silently vanish on restore
  static void check_covered(const entt::registry &r) {
    for (auto [id, pool] : r.storage()) {
      assert(pool.empty() || ((id == entt::type_hash<Ts>::value()) || ...));
    }
  }
};

// Field-wise equality for any plain struct, no per-type code: recurses into
// members (C++26 structured binding packs), arrays and scalars. Padding is
// never looked at.
template <typename T> bool fields_equal(const T &a, const T &b) {
  if constexpr (std::is_scalar_v<T>) {
    return a == b;
  } else if constexpr (std::is_array_v<T>) {
    for (size_t i = 0; i < std::extent_v<T>; i++) {
      if (!fields_equal(a[i], b[i])) return false;
    }
    return true;
  } else if constexpr (std::is_empty_v<T>) {
    return true;
  } else {
    const auto &[... fa] = a;
    const auto &[... fb] = b;
    return (fields_equal(fa, fb) && ...);
  }
}

// Two registries hold the same state: same entity ids and versions in the
// same order with the same free list, and every component storage holds
// equal values for the same entities in the same order. Field-wise, so
// padding never matters.
template <typename... Ts>
bool same_state(const entt::registry &a, const entt::registry &b) {
  const auto &ea = *a.storage<entt::entity>();
  const auto &eb = *b.storage<entt::entity>();
  if (ea.size() != eb.size() || ea.free_list() != eb.free_list() ||
      !std::equal(ea.rbegin(), ea.rend(), eb.rbegin())) {
    return false;
  }
  auto same = [&]<typename T>() {
    const auto *sa = a.storage<T>();
    const auto *sb = b.storage<T>();
    const size_t na = sa ? sa->size() : 0, nb = sb ? sb->size() : 0;
    if (na != nb) return false;
    if (na == 0) return true;
    auto ia = sa->reach().begin(), ib = sb->reach().begin();
    for (; ia != sa->reach().end(); ++ia, ++ib) {
      if (std::get<0>(*ia) != std::get<0>(*ib)) return false;
      if constexpr (!std::is_empty_v<T>) {
        if (!fields_equal(std::get<1>(*ia), std::get<1>(*ib))) return false;
      }
    }
    return true;
  };
  return (same.template operator()<Ts>() && ...);
}

using Sim = SimState<Transform, Physics, Player, Movement, Owner, Odm, Hook>;

//====== USE ======//

int main() {
  World w;
  const auto player = spawn_creature(w.reg, {100, 100});

  // keyframe, then simulate with the hook pressed
  const auto keyframe = Sim::save(w.reg);
  w.reg.get<Movement>(player) = {.intents = INTENT_HOOK, .aim = {1, 0}};
  for (int i = 0; i < 240; i++) do_tick(w, 1.0f / 240);
  Sim::check_covered(w.reg);
  const auto snapshot_a = Sim::save(w.reg);
  const entt::registry after_a = Sim::load(snapshot_a);

  // rewind to the keyframe and replay the same input
  w.reg = Sim::load(keyframe);
  w.reg.get<Movement>(player) = {.intents = INTENT_HOOK, .aim = {1, 0}};
  for (int i = 0; i < 240; i++) do_tick(w, 1.0f / 240);

  // the determinism test: identical state after restore + replay
  assert(Sim::same(after_a, w.reg));
  std::printf("replay matches: %zu bytes, %d entities\n", snapshot_a.size(),
              static_cast<int>(w.reg.storage<entt::entity>().free_list()));
}
