#pragma once
#include "plain.hpp"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <span>

enum class ArenaErr {
  OutOfMemory,
  Full,
};

// Fixed-capacity vector over memory it doesn't own (usually from an Arena).
// Never grows. Becomes dangling once its arena is reset or rewound past it.
template <Plain T> class ArenaVec {
  T *ptr = nullptr;
  uint32_t len = 0;
  uint32_t cap = 0;

public:
  constexpr ArenaVec() noexcept = default;
  constexpr explicit ArenaVec(std::span<T> mem) noexcept
      : ptr(mem.data()), cap(static_cast<uint32_t>(mem.size())) {
    assert(mem.size() <= std::numeric_limits<uint32_t>::max());
  }

  constexpr std::expected<void, ArenaErr> push(T x) noexcept {
    if (len == cap) {
      return std::unexpected(ArenaErr::Full);
    }
    ptr[len] = x;
    len += 1;
    return {};
  }
  constexpr T pop() noexcept {
    assert(len > 0);
    len -= 1;
    return ptr[len];
  }
  constexpr void clear() noexcept { len = 0; }

  constexpr uint32_t size() const noexcept { return len; }
  constexpr uint32_t capacity() const noexcept { return cap; }
  constexpr bool empty() const noexcept { return len == 0; }
  constexpr bool full() const noexcept { return len == cap; }

  constexpr T &operator[](uint32_t i) noexcept {
    assert(i < len);
    return ptr[i];
  }
  constexpr const T &operator[](uint32_t i) const noexcept {
    assert(i < len);
    return ptr[i];
  }
  constexpr T &back() noexcept {
    assert(len > 0);
    return ptr[len - 1];
  }

  constexpr T *begin() noexcept { return ptr; }
  constexpr T *end() noexcept { return ptr + len; }
  constexpr const T *begin() const noexcept { return ptr; }
  constexpr const T *end() const noexcept { return ptr + len; }

  constexpr std::span<T> items() noexcept { return {ptr, len}; }
  constexpr std::span<const T> items() const noexcept { return {ptr, len}; }
};

// Linear (bump) allocator. Owns one heap block allocated at construction.
// Allocations are freed all at once with reset() or back to a mark with
// rewind(). Memory handed out is uninitialized unless you use alloc_zeroed.
class Arena {
  std::unique_ptr<std::byte[]> buf;
  size_t cap;
  size_t off = 0;

public:
  using Mark = size_t;

  // The block comes from new std::byte[], so it's aligned for any type with
  // fundamental alignment.
  explicit Arena(size_t bytes)
      : buf(std::make_unique_for_overwrite<std::byte[]>(bytes)), cap(bytes) {}

  Arena(const Arena &) = delete;
  Arena &operator=(const Arena &) = delete;
  Arena(Arena &&) = delete;
  Arena &operator=(Arena &&) = delete;

  template <Plain T>
  [[nodiscard]] std::expected<std::span<T>, ArenaErr>
  alloc(size_t count) noexcept {
    static_assert(alignof(T) <= alignof(std::max_align_t),
                  "over-aligned types are not supported");
    if (count == 0) {
      return std::span<T>{};
    }
    size_t start = (off + alignof(T) - 1) & ~(alignof(T) - 1);
    if (start > cap || count > (cap - start) / sizeof(T)) {
      return std::unexpected(ArenaErr::OutOfMemory);
    }
    off = start + count * sizeof(T);
    return std::span<T>(
        std::start_lifetime_as_array<T>(buf.get() + start, count), count);
  }

  template <Plain T>
  [[nodiscard]] std::expected<std::span<T>, ArenaErr>
  alloc_zeroed(size_t count) noexcept {
    return alloc<T>(count).transform([](std::span<T> s) {
      std::ranges::fill(s, T{});
      return s;
    });
  }

  template <Plain T>
  [[nodiscard]] std::expected<ArenaVec<T>, ArenaErr> vec(uint32_t n) noexcept {
    return alloc<T>(n).transform([](std::span<T> s) { return ArenaVec<T>(s); });
  }

  Mark mark() const noexcept { return off; }
  void rewind(Mark m) noexcept {
    assert(m <= off);
    off = m;
  }
  void reset() noexcept { off = 0; }

  size_t used() const noexcept { return off; }
  size_t capacity() const noexcept { return cap; }
};

// Rewinds the arena to where it was when the scope was created.
class ArenaScope {
  Arena &arena;
  Arena::Mark m;

public:
  explicit ArenaScope(Arena &a) noexcept : arena(a), m(a.mark()) {}
  ~ArenaScope() { arena.rewind(m); }

  ArenaScope(const ArenaScope &) = delete;
  ArenaScope &operator=(const ArenaScope &) = delete;
};
