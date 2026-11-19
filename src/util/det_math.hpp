#pragma once
#include <array>
#include <bit>
#include <cassert>
#include <cfloat>
#include <cstdint>
#include <limits>

// Deterministic exp, sin and cos: the same bits on every compiler, standard
// library and CPU, unlike libm's, which aren't required to round the same
// everywhere. Built only from + - * /, conversions, comparisons and bit
// casts, which IEEE 754 rounds exactly. The lookup tables are computed at
// compile time from the same operations (compilers round those exactly
// too), so they're identical in every build. Everything is constexpr.
//
// Also needs every operation to round to float, with nothing fused or kept
// at higher precision: -ffp-contract=off (set on the sim target), no
// fast-math, and no excess precision (checked below).

#ifdef __FAST_MATH__
#error "fast-math breaks deterministic floats"
#endif
static_assert(FLT_EVAL_METHOD == 0,
              "floats must be evaluated as floats (no x87 excess precision)");

namespace det {

namespace detail {

// Exact floor for |v| < 2^63, usable at compile time on every compiler
// (std::floor isn't constexpr in every standard library).
constexpr double floor(double v) noexcept {
  assert(v > -9.2e18 && v < 9.2e18);
  // truncates toward zero, then steps down for negative non-integers
  const auto t = static_cast<double>(static_cast<int64_t>(v));
  return t > v ? t - 1 : t;
}

constexpr double PI = 3.14159265358979323846;
constexpr double TAU = 2 * PI;
constexpr double LN2 = 0.69314718055994530942;
constexpr double LOG2E = 1.44269504088896340736;

// Taylor series, for building the tables at compile time. Converge to well
// past float precision for the arguments they're given (|a| <= pi for sin
// and cos, 0 <= a < ln 2 for exp).
constexpr double exp_series(double a) {
  double term = 1;
  double sum = 1;
  for (int n = 1; n < 30; n++) {
    term *= a / n;
    sum += term;
  }
  return sum;
}
constexpr double sin_series(double a) {
  double term = a;
  double sum = a;
  for (int n = 1; n < 20; n++) {
    term *= -a * a / ((2 * n) * (2 * n + 1));
    sum += term;
  }
  return sum;
}
constexpr double cos_series(double a) {
  double term = 1;
  double sum = 1;
  for (int n = 1; n < 20; n++) {
    term *= -a * a / ((2 * n - 1) * (2 * n));
    sum += term;
  }
  return sum;
}

// sin and cos at SIN_N evenly spaced angles around a full turn
constexpr uint32_t SIN_N = 1024;
struct SinTable {
  std::array<float, SIN_N> sin;
  std::array<float, SIN_N> cos;
};
constexpr SinTable make_sin_table() {
  SinTable t{};
  for (uint32_t j = 0; j < SIN_N; j++) {
    double a = static_cast<double>(j) * (TAU / SIN_N);
    // into [-pi, pi], where the series converge quickly
    if (a > PI) {
      a -= TAU;
    }
    t.sin[j] = static_cast<float>(sin_series(a));
    t.cos[j] = static_cast<float>(cos_series(a));
  }
  return t;
}
inline constexpr SinTable SIN_TABLE = make_sin_table();
// radians per table step, and table steps per radian
inline constexpr double SIN_STEP = TAU / SIN_N;
inline constexpr double SIN_STEPS_PER_RADIAN = SIN_N / TAU;

// 2^(j / EXP_N) for j in [0, EXP_N)
constexpr uint32_t EXP_N = 256;
constexpr uint32_t EXP_SHIFT = 8; // log2(EXP_N)
constexpr std::array<float, EXP_N> make_exp_table() {
  std::array<float, EXP_N> t{};
  for (uint32_t j = 0; j < EXP_N; j++) {
    t[j] = static_cast<float>(
        exp_series(static_cast<double>(j) / EXP_N * LN2));
  }
  return t;
}
inline constexpr std::array<float, EXP_N> EXP_TABLE = make_exp_table();
inline constexpr double EXP_STEPS_PER_UNIT = EXP_N * LOG2E;
inline constexpr double EXP_UNITS_PER_STEP = LN2 / EXP_N;

// Splits x (radians) into a table index i and the rest d (radians, in
// [0, SIN_STEP)), so x = i * SIN_STEP + d plus whole turns. The split is
// done in double: in float, x * SIN_STEPS_PER_RADIAN loses the low bits of
// its fraction once x is in the hundreds. |x| has to be below 1e9 radians.
struct SinSplit {
  uint32_t i;
  float sin_d;
  float cos_d;
};
constexpr SinSplit split_angle(float x) noexcept {
  assert(x > -1e9f && x < 1e9f && "out of range or NaN");
  const double steps = static_cast<double>(x) * SIN_STEPS_PER_RADIAN;
  const double whole = floor(steps);
  const auto d = static_cast<float>((steps - whole) * SIN_STEP);
  // whole turns drop out of the mask
  const auto i = static_cast<uint32_t>(static_cast<int64_t>(whole)) &
                 (SIN_N - 1);
  // d < 0.0062, so these are good to about 1e-11
  const float d2 = d * d;
  return {
      .i = i,
      .sin_d = d - d * d2 * (1.0f / 6),
      .cos_d = 1 - d2 * 0.5f,
  };
}

} // namespace detail

// sin(x), x in radians. Accurate to about 1e-7.
constexpr float sin(float x) noexcept {
  const detail::SinSplit s = detail::split_angle(x);
  // sin(a + d) = sin a cos d + cos a sin d
  return detail::SIN_TABLE.sin[s.i] * s.cos_d +
         detail::SIN_TABLE.cos[s.i] * s.sin_d;
}

// cos(x), x in radians. Accurate to about 1e-7.
constexpr float cos(float x) noexcept {
  const detail::SinSplit s = detail::split_angle(x);
  // cos(a + d) = cos a cos d - sin a sin d
  return detail::SIN_TABLE.cos[s.i] * s.cos_d -
         detail::SIN_TABLE.sin[s.i] * s.sin_d;
}

// e^x, to within about 2 ulp. Exactly 0 once the result would be below the
// smallest normal float (x below about -87.34; no subnormals), and
// infinity once it would overflow (x above about 88.72).
constexpr float exp(float x) noexcept {
  assert(x == x && "NaN");
  // well past either end (infinities too), so the conversions below stay in
  // range; the exact cutoffs are further down
  if (x < -100.0f) {
    return 0;
  }
  if (x > 100.0f) {
    return std::numeric_limits<float>::infinity();
  }
  // e^x = 2^(x log2 e) = 2^hi * 2^(lo / EXP_N) * e^u: hi straight into the
  // exponent bits, lo from the table, and u (< ln2 / EXP_N, about 0.0027)
  // by a short polynomial, good to about 2e-12. The split is done in double
  // for the same reason as split_angle's.
  const double steps = static_cast<double>(x) * detail::EXP_STEPS_PER_UNIT;
  const double whole = detail::floor(steps);
  // 2^hi has to be a normal float: hi in [-126, 127]
  if (whole < -126.0 * detail::EXP_N) {
    return 0;
  }
  if (whole >= 128.0 * detail::EXP_N) {
    return std::numeric_limits<float>::infinity();
  }
  const auto u = static_cast<float>((steps - whole) * detail::EXP_UNITS_PER_STEP);
  const auto k = static_cast<int32_t>(whole);
  // arithmetic shift: floor division, for negative k too
  const int32_t hi = k >> detail::EXP_SHIFT;
  const auto lo = static_cast<uint32_t>(k) & (detail::EXP_N - 1);
  const float poly = 1 + u * (1 + u * (0.5f + u * (1.0f / 6)));
  // 2^hi, built from its exponent bits
  const float scale = std::bit_cast<float>(static_cast<uint32_t>(hi + 127)
                                           << 23);
  // overflows to infinity on its own right at the top
  return scale * detail::EXP_TABLE[lo] * poly;
}

} // namespace det
