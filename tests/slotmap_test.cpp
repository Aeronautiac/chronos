#include "sim/slotmap.hpp"
#include <algorithm>
#include <iterator>
#include <ranges>
#include <gtest/gtest.h>

namespace {

struct Item {
  int hp;
};
using Map = SlotMap<Item, 8>;

static_assert(std::forward_iterator<Map::iterator>);
static_assert(std::forward_iterator<Map::const_iterator>);
static_assert(std::ranges::forward_range<Map>);
static_assert(std::ranges::forward_range<const Map>);

// constant evaluation rejects UB, so these double as UB checks
constexpr bool erase_invalidates_key() {
  Map m{};
  auto k = *m.insert({1});
  (void)m.erase(k);
  return m.get(k) == nullptr;
}
static_assert(erase_invalidates_key());

constexpr bool reused_slot_rejects_old_key() {
  Map m{};
  auto old_key = *m.insert({1});
  (void)m.erase(old_key);
  auto new_key = *m.insert({2});
  return old_key.idx() == new_key.idx() && m.get(old_key) == nullptr &&
         m.get(new_key)->hp == 2;
}
static_assert(reused_slot_rejects_old_key());

TEST(SlotMap, InsertUntilFull) {
  Map m{};
  for (int i = 0; i < 8; i++) {
    ASSERT_TRUE(m.insert({i}));
  }
  auto res = m.insert({8});
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), SlotMapErr::Full);
}

TEST(SlotMap, EraseInvalidKey) {
  Map m{};
  auto k = *m.insert({1});
  ASSERT_TRUE(m.erase(k));
  auto res = m.erase(k);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), SlotMapErr::InvalidKey);
}

TEST(SlotMapIter, EmptyMap) {
  Map m{};
  EXPECT_EQ(m.begin(), m.end());
}

TEST(SlotMapIter, SkipsErasedAndMutates) {
  Map m{};
  auto k0 = *m.insert({10});
  auto k1 = *m.insert({20});
  auto k2 = *m.insert({30});
  auto k3 = *m.insert({40});
  ASSERT_TRUE(m.erase(k0));
  ASSERT_TRUE(m.erase(k2));

  for (auto [key, item] : m) {
    item.hp += 1;
    EXPECT_EQ(m.get(key), &item);
  }
  EXPECT_EQ(m.get(k1)->hp, 21);
  EXPECT_EQ(m.get(k3)->hp, 41);
}

TEST(SlotMapIter, EraseDuringIteration) {
  Map m{};
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(m.insert({i}));
  }
  for (auto [key, item] : m) {
    (void)item;
    EXPECT_TRUE(m.erase(key));
  }
  EXPECT_EQ(m.begin(), m.end());
}

TEST(SlotMapIter, ConstAndRanges) {
  Map m{};
  ASSERT_TRUE(m.insert({5}));
  ASSERT_TRUE(m.insert({6}));
  const Map &c = m;

  int sum = 0;
  for (auto [key, item] : c) {
    (void)key;
    sum += item.hp;
  }
  EXPECT_EQ(sum, 11);
  EXPECT_EQ(std::ranges::distance(c), 2);
  EXPECT_EQ(std::ranges::count_if(c, [](auto it) { return it.val.hp > 5; }), 1);
}

} // namespace
