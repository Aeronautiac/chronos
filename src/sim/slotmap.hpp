#pragma once
#include "pod.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>

enum class SlotMapErr {
  InvalidKey,
  Full,
};

template <Pod T> class Key {
  static constexpr uint64_t IDX_MASK = 0x00000000FFFFFFFF;

private:
  uint64_t val;

public:
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

template <Pod T, uint32_t N> class SlotMap {
  static constexpr uint32_t NONE = std::numeric_limits<uint32_t>::max();
  struct Slot {
    union {
      T data;
      uint32_t next_free;
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
  constexpr iterator begin() noexcept {
    return iterator(data.data(), 0, tail);
  }
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
