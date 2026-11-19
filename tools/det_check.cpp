// Prints hashes of results that have to be bit-identical on every platform:
// det_math over a sweep of inputs, and the sim's state over a scripted run.
// tools/cross_check.sh builds this for several compilers and CPUs and
// compares the output; any difference means floats aren't deterministic
// across them.
#include "sim/sim.hpp"
#include "sim/world.hpp"
#include "util/det_math.hpp"
#include <bit>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// FNV-1a over 32-bit words
struct Hash {
  uint64_t h = 0xcbf29ce484222325;
  void add(uint32_t w) {
    for (int i = 0; i < 4; i++) {
      h ^= (w >> (8 * i)) & 0xff;
      h *= 0x100000001b3;
    }
  }
  void add(float f) { add(std::bit_cast<uint32_t>(f)); }
};

void check_det_math() {
  Hash s, c, e;
  for (int i = -500000; i < 500000; i++) {
    const float x = static_cast<float>(i) * 0.0004f; // [-200, 200)
    s.add(det::sin(x));
    c.add(det::cos(x));
    e.add(det::exp(x * 0.44f)); // [-88, 88)
  }
  std::printf("det::sin %016llx\n", static_cast<unsigned long long>(s.h));
  std::printf("det::cos %016llx\n", static_cast<unsigned long long>(c.h));
  std::printf("det::exp %016llx\n", static_cast<unsigned long long>(e.h));
}

// Every entity's motion and perception, in storage order (itself part of
// the state).
uint64_t hash_sim(const sim::Sim &s) {
  const entt::registry &r = s.world().reg;
  Hash h;
  for (auto [e, t, p] : r.view<const sim::Transform, const sim::Physics>().each()) {
    h.add(static_cast<uint32_t>(entt::to_integral(e)));
    h.add(t.pos.x);
    h.add(t.pos.y);
    h.add(t.rot);
    h.add(p.vel.x);
    h.add(p.vel.y);
    h.add(p.raw_vel.x);
    h.add(p.raw_vel.y);
  }
  for (auto [e, pc] : r.view<const sim::Perception>().each()) {
    h.add(static_cast<uint32_t>(entt::to_integral(e)));
    h.add(pc.value);
  }
  return h.h;
}

// world time per scripted step (its own, so the run doesn't change with the
// sim's base tick)
constexpr sim::TimeInt STEP = static_cast<sim::TimeInt>(1e9 / 240);

sim::MovementIntents bit(sim::MovementIntent i) {
  return static_cast<sim::MovementIntents>(i);
}

void check_sim() {
  using sim::MovementIntent;
  auto map = std::make_unique<sim::Grid>();
  sim::fill_test_level(*map);
  auto s = std::make_unique<sim::Sim>();
  (void)s->exec(sim::SetMap{.map = map.get()}, 0);

  // creatures 16 units apart on the flat stretch, close enough to fight
  auto pos = [](int i) {
    return Vec2{(static_cast<float>(sim::FLAT_X0) + 40 + 4 * static_cast<float>(i)) *
                    sim::Grid::CELL_SIZE,
                (sim::TEST_LEVEL_GROUND - 3) * sim::Grid::CELL_SIZE};
  };
  std::vector<entt::entity> ids;
  for (int i = 0; i < 12; i++) {
    ids.push_back(
        std::get<entt::entity>(*s->exec(sim::SpawnCreature{.pos = pos(i)}, 0)));
  }

  const auto n = static_cast<int>(ids.size());
  for (int t = 0; t < 3000; t++) {
    if (t % 97 == 0) {
      (void)s->exec(sim::SpawnCreature{.pos = pos(t % n)}, STEP);
    } else {
      const int round = t / n;
      sim::MovementIntents in = 0;
      if (round % 2 == 1) in |= bit(MovementIntent::Attack);
      if ((round / 10) % 2 == 0) in |= bit(MovementIntent::Hook);
      if (round % 15 == 0) in |= bit(MovementIntent::Jump);
      if (round % 7 == 3) in |= bit(MovementIntent::Burst);
      in |= bit((round / 20) % 2 == 0 ? MovementIntent::Right
                                      : MovementIntent::Left);
      const Vec2 aim = (round / 10) % 3 == 0 ? Vec2{0.6f, -0.8f} : Vec2{1, 0};
      (void)s->exec(sim::SetMovementIntents{.intents = in,
                                            .aim = aim,
                                            .target = ids[static_cast<size_t>(t % n)]},
                    STEP);
    }
    if ((t + 1) % 500 == 0) {
      std::printf("sim step %4d %016llx\n", t + 1,
                  static_cast<unsigned long long>(hash_sim(*s)));
    }
  }
}

} // namespace

int main() {
  check_det_math();
  check_sim();
}
