#include "util/arena.hpp"
#include <cstdint>
#include <gtest/gtest.h>

namespace {

struct Wide {
  double d;
  uint8_t tag;
};

TEST(Arena, AlignsAllocations) {
  Arena a(256);
  auto bytes = a.alloc<uint8_t>(3);
  ASSERT_TRUE(bytes);
  auto wides = a.alloc<Wide>(2);
  ASSERT_TRUE(wides);
  EXPECT_EQ(reinterpret_cast<uintptr_t>(wides->data()) % alignof(Wide), 0u);
  EXPECT_EQ(a.used(), alignof(Wide) + 2 * sizeof(Wide));
}

TEST(Arena, OutOfMemory) {
  Arena a(16);
  ASSERT_TRUE(a.alloc<uint32_t>(4));
  auto res = a.alloc<uint32_t>(1);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), ArenaErr::OutOfMemory);
  // a failed alloc doesn't move the offset
  EXPECT_EQ(a.used(), 16u);
}

TEST(Arena, HugeCountDoesNotOverflow) {
  Arena a(64);
  auto res = a.alloc<uint64_t>(SIZE_MAX / 2);
  ASSERT_FALSE(res);
  EXPECT_EQ(a.used(), 0u);
}

TEST(Arena, ZeroCount) {
  Arena a(16);
  auto res = a.alloc<uint32_t>(0);
  ASSERT_TRUE(res);
  EXPECT_TRUE(res->empty());
  EXPECT_EQ(a.used(), 0u);
}

TEST(Arena, AllocZeroed) {
  Arena a(64);
  auto dirty = *a.alloc<uint32_t>(4);
  for (auto &x : dirty) {
    x = 0xDEADBEEF;
  }
  a.reset();
  auto clean = *a.alloc_zeroed<uint32_t>(4);
  for (auto x : clean) {
    EXPECT_EQ(x, 0u);
  }
}

TEST(Arena, MarkRewindAndScope) {
  Arena a(128);
  ASSERT_TRUE(a.alloc<uint32_t>(2));
  auto m = a.mark();
  ASSERT_TRUE(a.alloc<uint32_t>(4));
  a.rewind(m);
  EXPECT_EQ(a.used(), m);
  {
    ArenaScope scope(a);
    ASSERT_TRUE(a.alloc<uint32_t>(8));
    EXPECT_GT(a.used(), m);
  }
  EXPECT_EQ(a.used(), m);
  a.reset();
  EXPECT_EQ(a.used(), 0u);
}

TEST(ArenaVec, PushUntilFull) {
  Arena a(64);
  auto v = *a.vec<int>(3);
  EXPECT_TRUE(v.empty());
  for (int i = 0; i < 3; i++) {
    ASSERT_TRUE(v.push(i * 10));
  }
  EXPECT_TRUE(v.full());
  auto res = v.push(99);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), ArenaErr::Full);

  int sum = 0;
  for (int x : v) {
    sum += x;
  }
  EXPECT_EQ(sum, 30);
  EXPECT_EQ(v.pop(), 20);
  EXPECT_EQ(v.size(), 2u);
  EXPECT_EQ(v.items().back(), 10);
}

} // namespace
