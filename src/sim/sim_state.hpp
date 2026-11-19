#pragma once
#include <algorithm>
#include <bit>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <entt/entity/registry.hpp>
#include <entt/entity/snapshot.hpp>
#include <ranges>
#include <span>
#include <type_traits>
#include <vector>

namespace sim {

//====== FIELD-WISE EQUALITY ======//

// Field-wise equality for plain data, with no per-type code: recurses into
// arrays and aggregate members (C++26 structured binding packs) down to
// scalars. Padding is never looked at. Floats compare by bit pattern, since
// this checks for identical state (0 and -0 differ, a NaN equals itself).
// Non-aggregate classes (private members: Key, FlagFsm) compare with their
// own operator==.
template <typename T> constexpr bool fields_equal(const T &a, const T &b) noexcept {
  if constexpr (std::is_floating_point_v<T>) {
    static_assert(sizeof(T) == 4 || sizeof(T) == 8);
    using Bits = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    return std::bit_cast<Bits>(a) == std::bit_cast<Bits>(b);
  } else if constexpr (std::is_scalar_v<T>) {
    return a == b;
  } else if constexpr (std::ranges::contiguous_range<const T &>) {
    // C arrays and std::array, looped rather than unpacked so huge ones
    // (the grid) don't become huge packs
    if (std::ranges::size(a) != std::ranges::size(b)) {
      return false;
    }
    auto ib = std::ranges::begin(b);
    for (const auto &x : a) {
      if (!fields_equal(x, *ib)) {
        return false;
      }
      ++ib;
    }
    return true;
  } else if constexpr (std::is_empty_v<T>) {
    return true;
  } else if constexpr (std::is_aggregate_v<T>) {
    const auto &[... fa] = a;
    const auto &[... fb] = b;
    return (fields_equal(fa, fb) && ...);
  } else {
    static_assert(std::equality_comparable<T>,
                  "non-aggregate state needs an operator==");
    return a == b;
  }
}

//====== ARCHIVES ======//

// Appends values to a byte buffer. Only for trivially copyable types, so
// the bytes are the value.
struct OutArchive {
  std::vector<std::byte> &buf;

  template <typename T> void operator()(const T &v) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto *p = reinterpret_cast<const std::byte *>(&v);
    buf.insert(buf.end(), p, p + sizeof(T));
  }
};

// Reads values back in the order OutArchive wrote them.
struct InArchive {
  std::span<const std::byte> buf;
  size_t pos = 0;

  template <typename T> void operator()(T &v) {
    static_assert(std::is_trivially_copyable_v<T>);
    assert(pos + sizeof(T) <= buf.size());
    std::memcpy(&v, buf.data() + pos, sizeof(T));
    pos += sizeof(T);
  }
};

//====== REGISTRY STATE ======//

// Everything about a registry that's sim state, for the component types Ts:
// saving, loading, reserving, and comparing. Ts must list every component
// type the sim uses (check_covered asserts it on save).
//
// Exact restore (see PORT_TO_ENTT.md) relies on:
//   - loading only into a freshly constructed registry (load is the only
//     restore path)
//   - no in-place deletion storages (asserted below)
//   - no sim state in registry.ctx(), no groups
template <typename... Ts> struct SimState {
  static_assert((!entt::component_traits<Ts>::in_place_delete && ...),
                "in-place deletion breaks exact restore");
  static_assert((std::is_trivially_copyable_v<Ts> && ...),
                "snapshots copy components as bytes");

  // Makes room for `n` entities and `n` of every component so ticks don't
  // allocate. Packed arrays are reserved directly. Sparse arrays are
  // allocated a page (ENTT_SPARSE_PAGE ids) at a time when an id in that
  // page is first added, so each component storage gets every page up to
  // `n` touched by adding and removing a throwaway element: it's pushed
  // onto the end and popped straight off, so packed order is unchanged.
  // Pages whose first id is already in the storage exist already.
  //
  // The entity storage's own sparse pages can't be touched without
  // creating entities, which is state. They're allocated as ids are first
  // handed out (the first page by the first create), and a restore
  // recreates every id ever handed out, so they exist again after load.
  //
  // Capacity isn't state: restoring gives a fresh registry, so call this
  // after every load too.
  static void reserve(entt::registry &r, size_t n) {
    r.storage<entt::entity>().reserve(n);
    (reserve_one<Ts>(r, n), ...);
  }

  static std::vector<std::byte> save(const entt::registry &r) {
    check_covered(r);
    std::vector<std::byte> buf;
    OutArchive out{buf};
    entt::snapshot snap{r};
    snap.get<entt::entity>(out);
    (snap.get<Ts>(out), ...);
    return buf;
  }

  // The only way to restore: always into a fresh registry. (clear() keeps
  // destroyed ids with bumped versions, so recycled ids would diverge.)
  static entt::registry load(std::span<const std::byte> bytes) {
    entt::registry r;
    InArchive in{bytes};
    entt::snapshot_loader loader{r};
    loader.get<entt::entity>(in);
    (loader.get<Ts>(in), ...);
    assert(in.pos == bytes.size());
    return r;
  }

  // The replay check: same entity ids and versions in the same order with
  // the same free list, and every component storage holds field-wise equal
  // values for the same entities in the same order.
  static bool same(const entt::registry &a, const entt::registry &b) {
    const auto &ea = *a.storage<entt::entity>();
    const auto &eb = *b.storage<entt::entity>();
    if (ea.size() != eb.size() || ea.free_list() != eb.free_list() ||
        !std::equal(ea.rbegin(), ea.rend(), eb.rbegin())) {
      return false;
    }
    return (same_storage<Ts>(a, b) && ...);
  }

  // A component type missing from Ts would silently vanish on restore.
  static void check_covered([[maybe_unused]] const entt::registry &r) {
#ifndef NDEBUG
    for (auto [id, pool] : r.storage()) {
      assert(pool.empty() || ((id == entt::type_hash<Ts>::value()) || ...));
    }
#endif
  }

private:
  template <typename T> static void reserve_one(entt::registry &r, size_t n) {
    auto &s = r.storage<T>();
    s.reserve(n);
    constexpr size_t PAGE = entt::entt_traits<entt::entity>::page_size;
    // version of an id whose slot is empty (or whose page doesn't exist)
    constexpr auto ABSENT = entt::to_version(entt::entity{entt::tombstone});
    for (size_t first = 0; first < n; first += PAGE) {
      const auto probe = static_cast<entt::entity>(first);
      if (s.current(probe) != ABSENT) {
        continue;
      }
      s.emplace(probe);
      s.erase(probe);
    }
  }

  template <typename T>
  static bool same_storage(const entt::registry &a, const entt::registry &b) {
    const auto *sa = a.storage<T>();
    const auto *sb = b.storage<T>();
    const size_t na = sa ? sa->size() : 0;
    const size_t nb = sb ? sb->size() : 0;
    if (na != nb) {
      return false;
    }
    if (na == 0) {
      return true;
    }
    // reach() is packed order, front to back (what snapshots write)
    auto ia = sa->reach().begin();
    auto ib = sb->reach().begin();
    for (; ia != sa->reach().end(); ++ia, ++ib) {
      if (std::get<0>(*ia) != std::get<0>(*ib)) {
        return false;
      }
      if constexpr (!std::is_empty_v<T>) {
        if (!fields_equal(std::get<1>(*ia), std::get<1>(*ib))) {
          return false;
        }
      }
    }
    return true;
  }
};

} // namespace sim
