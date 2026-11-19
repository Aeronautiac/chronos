#include "util/slotmap.hpp"
#include <algorithm>
#include <gtest/gtest.h>
#include <iterator>
#include <optional>
#include <ranges>

namespace {

struct Item {
  int hp;
};
using Map = SparseSlotMap<Item, 8>;

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

// non-trivial default constructor, but still Plain
constexpr bool sparse_holds_optional() {
  SparseSlotMap<std::optional<int>, 4> m{};
  auto a = *m.insert(std::nullopt);
  auto b = *m.insert(7);
  (void)m.erase(a);
  auto c = *m.insert(9);
  return m.get(a) == nullptr && **m.get(b) == 7 && **m.get(c) == 9;
}
static_assert(sparse_holds_optional());

//====== DENSE ======//

using DMap = DenseSlotMap<Item, 8>;

static_assert(std::forward_iterator<DMap::iterator>);
static_assert(std::forward_iterator<DMap::const_iterator>);
static_assert(std::ranges::forward_range<DMap>);
static_assert(std::ranges::forward_range<const DMap>);

constexpr bool dense_erase_invalidates_key() {
  DMap m{};
  auto k = *m.insert({1});
  (void)m.erase(k);
  return m.get(k) == nullptr && m.empty();
}
static_assert(dense_erase_invalidates_key());

constexpr bool dense_reused_slot_rejects_old_key() {
  DMap m{};
  auto old_key = *m.insert({1});
  (void)m.erase(old_key);
  auto new_key = *m.insert({2});
  return old_key.idx() == new_key.idx() && m.get(old_key) == nullptr &&
         m.get(new_key)->hp == 2;
}
static_assert(dense_reused_slot_rejects_old_key());

// erasing from the middle swaps the last value in; every surviving key must
// still find its own value
constexpr bool dense_swap_remove_keeps_keys() {
  DMap m{};
  Key<Item> ks[5];
  for (int i = 0; i < 5; i++) {
    ks[i] = *m.insert({i * 10});
  }
  (void)m.erase(ks[1]);
  (void)m.erase(ks[0]);
  (void)m.erase(ks[4]);
  return m.size() == 2 && m.get(ks[2])->hp == 20 && m.get(ks[3])->hp == 30 &&
         m.get(ks[0]) == nullptr && m.get(ks[1]) == nullptr &&
         m.get(ks[4]) == nullptr;
}
static_assert(dense_swap_remove_keeps_keys());

constexpr bool dense_holds_optional() {
  DenseSlotMap<std::optional<int>, 4> m{};
  auto a = *m.insert(std::nullopt);
  auto b = *m.insert(7);
  (void)m.erase(a);
  auto c = *m.insert(9);
  return m.get(a) == nullptr && **m.get(b) == 7 && **m.get(c) == 9 &&
         m.size() == 2;
}
static_assert(dense_holds_optional());

constexpr bool hp_equal(const Item &a, const Item &b) { return a.hp == b.hp; }

// Same history, same state. Constant evaluation also rejects reading a
// slot's inactive union member, so this checks `same` only reads live ones.
constexpr bool dense_same_after_same_history() {
  DMap a{};
  DMap b{};
  for (DMap *m : {&a, &b}) {
    auto k0 = *m->insert({1});
    auto k1 = *m->insert({2});
    (void)m->insert({3});
    (void)m->erase(k0);
    (void)m->erase(k1);
    (void)m->insert({4});
  }
  return a.same(b, hp_equal) && b.same(a, hp_equal);
}
static_assert(dense_same_after_same_history());

// Same live values in the same order, but different free lists: the next
// insert would hand out different keys, so the state isn't the same.
constexpr bool dense_same_sees_free_list() {
  DMap a{};
  DMap b{};
  (void)a.insert({1});
  auto tmp = *b.insert({0});
  (void)b.erase(tmp);
  (void)b.insert({1});
  return a.size() == b.size() && a.values()[0].hp == b.values()[0].hp &&
         !a.same(b, hp_equal);
}
static_assert(dense_same_sees_free_list());

constexpr bool dense_same_sees_values() {
  DMap a{};
  DMap b{};
  (void)a.insert({1});
  (void)b.insert({2});
  return !a.same(b, hp_equal);
}
static_assert(dense_same_sees_values());

TEST(DenseSlotMap, InsertUntilFull) {
  DMap m{};
  for (int i = 0; i < 8; i++) {
    ASSERT_TRUE(m.insert({i}));
  }
  auto res = m.insert({8});
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), SlotMapErr::Full);
}

TEST(DenseSlotMap, FullAfterChurn) {
  DMap m{};
  Key<Item> ks[8];
  for (int i = 0; i < 8; i++) {
    ks[i] = *m.insert({i});
  }
  ASSERT_TRUE(m.erase(ks[3]));
  ASSERT_TRUE(m.erase(ks[6]));
  ASSERT_TRUE(m.insert({100}));
  ASSERT_TRUE(m.insert({101}));
  auto res = m.insert({102});
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), SlotMapErr::Full);
  EXPECT_EQ(m.size(), 8u);
}

TEST(DenseSlotMap, EraseInvalidKey) {
  DMap m{};
  auto k = *m.insert({1});
  ASSERT_TRUE(m.erase(k));
  auto res = m.erase(k);
  ASSERT_FALSE(res);
  EXPECT_EQ(res.error(), SlotMapErr::InvalidKey);
}

TEST(DenseSlotMap, ValuesArePacked) {
  DMap m{};
  auto k0 = *m.insert({10});
  ASSERT_TRUE(m.insert({20}));
  ASSERT_TRUE(m.insert({30}));
  ASSERT_TRUE(m.erase(k0));

  auto vals = m.values();
  ASSERT_EQ(vals.size(), 2u);
  int sum = 0;
  for (Item &it : vals) {
    sum += it.hp;
  }
  EXPECT_EQ(sum, 50);
}

TEST(DenseSlotMapIter, EmptyMap) {
  DMap m{};
  EXPECT_EQ(m.begin(), m.end());
}

TEST(DenseSlotMapIter, VisitsAllAndMutates) {
  DMap m{};
  auto k0 = *m.insert({10});
  auto k1 = *m.insert({20});
  auto k2 = *m.insert({30});
  auto k3 = *m.insert({40});
  ASSERT_TRUE(m.erase(k0));
  ASSERT_TRUE(m.erase(k2));

  int visited = 0;
  for (auto [key, item] : m) {
    item.hp += 1;
    EXPECT_EQ(m.get(key), &item);
    visited += 1;
  }
  EXPECT_EQ(visited, 2);
  EXPECT_EQ(m.get(k1)->hp, 21);
  EXPECT_EQ(m.get(k3)->hp, 41);
}

TEST(DenseSlotMapIter, EraseDuringIteration) {
  DMap m{};
  for (int i = 0; i < 5; i++) {
    ASSERT_TRUE(m.insert({i}));
  }
  int visited = 0;
  for (auto [key, item] : m) {
    (void)item;
    EXPECT_TRUE(m.erase(key));
    visited += 1;
  }
  EXPECT_EQ(visited, 5);
  EXPECT_TRUE(m.empty());
  EXPECT_EQ(m.begin(), m.end());
}

TEST(DenseSlotMapIter, EraseSomeDuringIteration) {
  DMap m{};
  Key<Item> ks[6];
  for (int i = 0; i < 6; i++) {
    ks[i] = *m.insert({i});
  }
  int visited = 0;
  for (auto [key, item] : m) {
    visited += 1;
    if (item.hp % 2 == 0) {
      EXPECT_TRUE(m.erase(key));
    }
  }
  EXPECT_EQ(visited, 6);
  EXPECT_EQ(m.size(), 3u);
  for (int i = 0; i < 6; i++) {
    if (i % 2 == 0) {
      EXPECT_EQ(m.get(ks[i]), nullptr);
    } else {
      EXPECT_EQ(m.get(ks[i])->hp, i);
    }
  }
}

TEST(DenseSlotMapIter, ConstAndRanges) {
  DMap m{};
  ASSERT_TRUE(m.insert({5}));
  ASSERT_TRUE(m.insert({6}));
  const DMap &c = m;

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
