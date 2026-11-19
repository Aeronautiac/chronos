#pragma once
#include <bit>
#include <cassert>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <type_traits>

// A flag type: a scoped enum with an unsigned underlying type, where every
// enumerator is a single bit (1 << n).
template <typename E>
concept FlagEnum = std::is_scoped_enum_v<E> &&
                   std::unsigned_integral<std::underlying_type_t<E>>;

template <FlagEnum E> using FlagBits = std::underlying_type_t<E>;

template <FlagEnum E> constexpr FlagBits<E> bits(E flag) noexcept {
  return static_cast<FlagBits<E>>(flag);
}

template <FlagEnum E>
constexpr FlagBits<E> bits(std::initializer_list<E> flags) noexcept {
  FlagBits<E> out = 0;
  for (E f : flags) {
    out |= bits(f);
  }
  return out;
}

// bit position of a single-bit enumerator, e.g. 1 << 3 -> 3
template <FlagEnum E> constexpr uint32_t flag_index(E flag) noexcept {
  assert(std::has_single_bit(bits(flag)));
  return static_cast<uint32_t>(std::countr_zero(bits(flag)));
}

// A set of flags from E, one bit each, in E's underlying type. Plain;
// zero-initialized (`Flags<E>{}`) is the empty set.
template <FlagEnum E> struct Flags {
  FlagBits<E> mask;

  constexpr bool has(E flag) const noexcept { return (mask & bits(flag)) != 0; }

  constexpr void set(E flag, bool on) noexcept {
    if (on) {
      mask |= bits(flag);
    } else {
      mask &= static_cast<FlagBits<E>>(~bits(flag));
    }
  }
};
