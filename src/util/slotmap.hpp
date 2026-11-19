#pragma once
#include "plain.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

enum class SlotMapErr {
  InvalidKey,
  Full,
};

template <typename T> class Key {
  static constexpr uint64_t IDX_MASK = 0x00000000FFFFFFFF;

private:
  uint64_t val;

public:
  constexpr Key() noexcept = default;
  constexpr Key(uint32_t idx, uint32_t gen) noexcept
      : val(static_cast<uint64_t>(idx) | (static_cast<uint64_t>(gen) << 32)) {}
  constexpr uint32_t idx() const noexcept {
    return static_cast<uint32_t>(val & IDX_MASK);
  }
  constexpr uint32_t gen() const noexcept {
    return static_cast<uint32_t>((val & ~IDX_MASK) >> 32);
  }
  constexpr bool operator==(const Key &) const = default;
};

template <Plain T, uint32_t N> class SparseSlotMap {
  static constexpr uint32_t NONE = std::numeric_limits<uint32_t>::max();
  struct Slot {
    union {
      T data;
      // the initializer gives the union a default constructor even when T's
      // isn't trivial (e.g. std::optional), which would otherwise delete it
      uint32_t next_free = 0;
    };
    // free is even, odd is taken
    uint32_t gen;
  };

public:
  // SlotT is Slot or const Slot, ValT is T or const T
  template <typename SlotT, typename ValT> class Iter {
    SlotT *slots = nullptr;
    uint32_t idx = 0;
    uint32_t end = 0;

    constexpr void skip_free() noexcept {
      while (idx < end && slots[idx].gen % 2 == 0) {
        idx += 1;
      }
    }

  public:
    struct Item {
      Key<T> key;
      ValT &val;
    };
    using value_type = Item;
    using difference_type = std::ptrdiff_t;

    constexpr Iter() noexcept = default;
    constexpr Iter(SlotT *slots, uint32_t idx, uint32_t end) noexcept
        : slots(slots), idx(idx), end(end) {
      skip_free();
    }

    constexpr Item operator*() const noexcept {
      return {
          .key = Key<T>(idx, slots[idx].gen),
          .val = slots[idx].data,
      };
    }

    constexpr Iter &operator++() noexcept {
      idx += 1;
      skip_free();
      return *this;
    }

    constexpr Iter operator++(int) noexcept {
      Iter prev = *this;
      ++*this;
      return prev;
    }

    constexpr bool operator==(const Iter &) const = default;
  };

  using iterator = Iter<Slot, T>;
  using const_iterator = Iter<const Slot, const T>;

private:
  std::array<Slot, N> data;
  uint32_t head = NONE;
  uint32_t tail = 0;

public:
  // visits taken slots in index order. erasing the current item while
  // iterating is fine, items inserted during iteration may or may not be
  // visited.
  constexpr iterator begin() noexcept { return iterator(data.data(), 0, tail); }
  constexpr iterator end() noexcept {
    return iterator(data.data(), tail, tail);
  }
  constexpr const_iterator begin() const noexcept {
    return const_iterator(data.data(), 0, tail);
  }
  constexpr const_iterator end() const noexcept {
    return const_iterator(data.data(), tail, tail);
  }

  constexpr bool valid_key(Key<T> key) const noexcept {
    uint32_t idx = key.idx();
    // out of bounds
    if (idx >= tail) {
      return false;
    }
    // not taken
    if (data[idx].gen % 2 == 0) {
      return false;
    }
    // wrong generation
    if (data[idx].gen != key.gen()) {
      return false;
    }
    return true;
  }

  constexpr std::expected<Key<T>, SlotMapErr> insert(T x) noexcept {
    uint32_t idx;
    uint32_t gen;
    if (head == NONE) {
      if (tail >= data.size()) {
        return std::unexpected(SlotMapErr::Full);
      }
      // take a new slot
      idx = tail;
      tail += 1;
      gen = 0;
    } else {
      // use a free slot
      idx = head;
      gen = data[idx].gen;
      head = data[idx].next_free;
    }
    gen += 1;
    // mark the slot as taken by making the gen odd
    data[idx] = {
        .data = x,
        .gen = gen,
    };
    return Key<T>(idx, gen);
  }

  constexpr std::expected<void, SlotMapErr> erase(Key<T> key) noexcept {
    if (!valid_key(key)) {
      return std::unexpected(SlotMapErr::InvalidKey);
    }
    uint32_t idx = key.idx();
    data[idx].next_free = head;
    data[idx].gen += 1;
    head = idx;
    return {};
  }

  constexpr const T *get(Key<T> key) const noexcept {
    return valid_key(key) ? &data[key.idx()].data : nullptr;
  }

  constexpr T *get_mut(Key<T> key) noexcept {
    return valid_key(key) ? &data[key.idx()].data : nullptr;
  }
};

// Values are packed contiguously in [0, size()), so iteration touches no
// holes. Erase swaps the last value into the hole, so value order is not
// stable and pointers from get()/values() are invalidated by any erase.
template <Plain T, uint32_t N> class DenseSlotMap {
  static constexpr uint32_t NONE = std::numeric_limits<uint32_t>::max();
  struct Slot {
    union {
      uint32_t data_idx;
      uint32_t next_free;
    };
    // free is even, odd is taken
    uint32_t gen;
  };

public:
  // Walks the dense array back to front so erasing the current item is safe:
  // the swapped-in value comes from a higher index, which was already
  // visited. Items inserted during iteration are not visited.
  // MapT is DenseSlotMap or const DenseSlotMap, ValT is T or const T
  template <typename MapT, typename ValT> class Iter {
    MapT *map = nullptr;
    // one past the item this iterator points at
    uint32_t i = 0;

  public:
    struct Item {
      Key<T> key;
      ValT &val;
    };
    using value_type = Item;
    using difference_type = std::ptrdiff_t;

    constexpr Iter() noexcept = default;
    constexpr Iter(MapT *map, uint32_t i) noexcept : map(map), i(i) {}

    constexpr Item operator*() const noexcept {
      uint32_t di = i - 1;
      uint32_t slot = map->owners[di];
      return {
          .key = Key<T>(slot, map->slots[slot].gen),
          .val = map->vals[di],
      };
    }

    constexpr Iter &operator++() noexcept {
      i -= 1;
      return *this;
    }

    constexpr Iter operator++(int) noexcept {
      Iter prev = *this;
      ++*this;
      return prev;
    }

    constexpr bool operator==(const Iter &) const = default;
  };

  using iterator = Iter<DenseSlotMap, T>;
  using const_iterator = Iter<const DenseSlotMap, const T>;

private:
  std::array<Slot, N> slots;
  // dense, only [0, len) is live
  std::array<T, N> vals;
  // owners[i] is the slot index that points at vals[i]
  std::array<uint32_t, N> owners;
  uint32_t len = 0;
  uint32_t head = NONE;
  uint32_t tail = 0;

public:
  constexpr iterator begin() noexcept { return iterator(this, len); }
  constexpr iterator end() noexcept { return iterator(this, 0); }
  constexpr const_iterator begin() const noexcept {
    return const_iterator(this, len);
  }
  constexpr const_iterator end() const noexcept {
    return const_iterator(this, 0);
  }

  // The packed values, for loops that don't need keys. Don't erase while
  // walking this.
  constexpr std::span<T> values() noexcept { return {vals.data(), len}; }
  constexpr std::span<const T> values() const noexcept {
    return {vals.data(), len};
  }

  constexpr uint32_t size() const noexcept { return len; }
  constexpr bool empty() const noexcept { return len == 0; }

  constexpr bool valid_key(Key<T> key) const noexcept {
    uint32_t idx = key.idx();
    // out of bounds
    if (idx >= tail) {
      return false;
    }
    // not taken
    if (slots[idx].gen % 2 == 0) {
      return false;
    }
    // wrong generation
    if (slots[idx].gen != key.gen()) {
      return false;
    }
    return true;
  }

  constexpr std::expected<Key<T>, SlotMapErr> insert(T x) noexcept {
    // every taken slot owns exactly one value, so len == N means no free slot
    // and no untouched slot either
    if (len == N) {
      return std::unexpected(SlotMapErr::Full);
    }
    uint32_t idx;
    uint32_t gen;
    if (head == NONE) {
      // take a new slot
      idx = tail;
      tail += 1;
      gen = 0;
    } else {
      // use a free slot
      idx = head;
      gen = slots[idx].gen;
      head = slots[idx].next_free;
    }
    gen += 1;
    // mark the slot as taken by making the gen odd
    slots[idx] = {
        .data_idx = len,
        .gen = gen,
    };
    vals[len] = x;
    owners[len] = idx;
    len += 1;
    return Key<T>(idx, gen);
  }

  constexpr std::expected<void, SlotMapErr> erase(Key<T> key) noexcept {
    if (!valid_key(key)) {
      return std::unexpected(SlotMapErr::InvalidKey);
    }
    uint32_t idx = key.idx();
    uint32_t di = slots[idx].data_idx;
    uint32_t last = len - 1;
    // move the last value into the hole and repoint its slot
    if (di != last) {
      vals[di] = vals[last];
      owners[di] = owners[last];
      slots[owners[di]].data_idx = di;
    }
    len = last;
    slots[idx].next_free = head;
    slots[idx].gen += 1;
    head = idx;
    return {};
  }

  constexpr const T *get(Key<T> key) const noexcept {
    return valid_key(key) ? &vals[slots[key.idx()].data_idx] : nullptr;
  }

  constexpr T *get_mut(Key<T> key) noexcept {
    return valid_key(key) ? &vals[slots[key.idx()].data_idx] : nullptr;
  }

  // Same state as `other`: every slot's generation and link, the free list,
  // and the live values in the same dense order. `eq(a, b)` compares two
  // values, so the caller decides what equal means for T (e.g. T has a
  // union). Unused slots and dead values are never looked at.
  template <typename Eq>
  constexpr bool same(const DenseSlotMap &other, Eq eq) const noexcept {
    if (len != other.len || head != other.head || tail != other.tail) {
      return false;
    }
    for (uint32_t i = 0; i < tail; i++) {
      const Slot &a = slots[i];
      const Slot &b = other.slots[i];
      if (a.gen != b.gen) {
        return false;
      }
      // odd gen is taken (data_idx is the live member), even is free
      // (next_free is)
      const bool link_same = a.gen % 2 == 1 ? a.data_idx == b.data_idx
                                            : a.next_free == b.next_free;
      if (!link_same) {
        return false;
      }
    }
    for (uint32_t i = 0; i < len; i++) {
      if (owners[i] != other.owners[i] || !eq(vals[i], other.vals[i])) {
        return false;
      }
    }
    return true;
  }
};
