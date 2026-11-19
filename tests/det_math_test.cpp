#include "util/det_math.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <gtest/gtest.h>
#include <limits>

namespace {

// usable at compile time, and exact where it should be
static_assert(det::sin(0.0f) == 0.0f);
static_assert(det::cos(0.0f) == 1.0f);
static_assert(det::exp(0.0f) == 1.0f);
static_assert(det::exp(-100.0f) == 0.0f);
static_assert(det::exp(100.0f) == std::numeric_limits<float>::infinity());

// Compared against libm in double, which is plenty more precise than float.
TEST(DetMath, SinCosAccurateToAFloatUlp) {
  double max_sin = 0;
  double max_cos = 0;
  for (int i = -200000; i <= 200000; i++) {
    const float x = static_cast<float>(i) * 0.001f; // [-200, 200]
    const auto xd = static_cast<double>(x);
    max_sin = std::max(max_sin, std::abs(det::sin(x) - std::sin(xd)));
    max_cos = std::max(max_cos, std::abs(det::cos(x) - std::cos(xd)));
  }
  EXPECT_LT(max_sin, 2e-7);
  EXPECT_LT(max_cos, 2e-7);
}

TEST(DetMath, ExpAccurateToAFewUlp) {
  double max_rel = 0;
  for (int i = -87300; i <= 88700; i++) {
    const float x = static_cast<float>(i) * 0.001f; // [-87.3, 88.7]
    const double ref = std::exp(static_cast<double>(x));
    max_rel = std::max(max_rel, std::abs(det::exp(x) - ref) / ref);
  }
  EXPECT_LT(max_rel, 3 * static_cast<double>(FLT_EPSILON));
}

TEST(DetMath, ExpRangeEdges) {
  // the largest finite e^x is about 3.4e38, at x = 88.72
  EXPECT_TRUE(std::isfinite(det::exp(88.72f)));
  EXPECT_EQ(det::exp(88.73f), std::numeric_limits<float>::infinity());
  // the smallest normal float is about 1.18e-38, at x = -87.34
  EXPECT_GE(det::exp(-87.33f), FLT_MIN);
  EXPECT_EQ(det::exp(-87.35f), 0.0f);
}

// The point of det_math: these exact bits on every compiler, standard
// library and CPU. If this fails on a new platform, that platform computes
// differently and determinism across environments is broken there. If
// det_math itself was changed on purpose, update the expected bits.
TEST(DetMath, SameBitsEverywhere) {
  struct Case {
    float x;
    uint32_t sin_bits;
    uint32_t cos_bits;
    uint32_t exp_bits;
  };
  const Case cases[] = {
      {0x0p+0f, 0x00000000, 0x3f800000, 0x3f800000},
      {0x1p-1f, 0x3ef57743, 0x3f60a940, 0x3fd3094d},
      {-0x1.4p+0f, 0xbf72f0a7, 0x3ea171ef, 0x3e92b0c1},
      {0x1.921fb6p+1f, 0xb3bbbd2e, 0xbf800000, 0x41b92024},
      {0x1.9p+6f, 0xbf01a12d, 0x3f5cc0ee, 0x7f800000},
      {-0x1.89374cp-9f, 0xbb449b95, 0x3f7fffb5, 0x3f7f3bb0},
      {0x1.5p+5f, 0xbf6aa128, 0xbecccadf, 0x5dc1192a},
  };
  for (const Case &c : cases) {
    EXPECT_EQ(std::bit_cast<uint32_t>(det::sin(c.x)), c.sin_bits) << c.x;
    EXPECT_EQ(std::bit_cast<uint32_t>(det::cos(c.x)), c.cos_bits) << c.x;
    EXPECT_EQ(std::bit_cast<uint32_t>(det::exp(c.x)), c.exp_bits) << c.x;
  }
}

} // namespace
