#include "sim/sim.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <raylib.h>

static sim::MovementIntents read_intents() {
  sim::MovementIntents intents = 0;
  auto bind = [&](KeyboardKey key, sim::MovementIntent intent) {
    if (IsKeyDown(key))
      intents |= static_cast<sim::MovementIntents>(intent);
  };
  bind(KEY_W, sim::MovementIntent::Forward);
  bind(KEY_S, sim::MovementIntent::Backward);
  bind(KEY_A, sim::MovementIntent::Left);
  bind(KEY_D, sim::MovementIntent::Right);
  bind(KEY_LEFT_SHIFT, sim::MovementIntent::Boost);
  return intents;
}

static void draw_ship(sim::Transform t) {
  float c = std::cos(t.rot);
  float s = std::sin(t.rot);
  // local space -> world space. nose points along +x at rot 0.
  auto local = [&](float x, float y) {
    return Vector2{t.pos.x + x * c - y * s, t.pos.y + x * s + y * c};
  };
  // raylib wants counter-clockwise order (as seen on screen)
  DrawTriangle(local(20, 0), local(-12, -12), local(-12, 12), RAYWHITE);
}

int main() {
  // the sim knows nothing about raylib. main only talks to it through exec()
  // and reads world state back for drawing.
  auto game = std::make_unique<sim::Sim>();
  sim::SimResult spawned =
      game->exec(sim::SpawnEntity{.kind = sim::EntityKind::Ship}, 0);
  assert(spawned);
  sim::EntityID ship = std::get<sim::EntityID>(*spawned);

  InitWindow(1280, 720, "small-game");
  SetTargetFPS(144);

  using Clock = std::chrono::steady_clock;
  Clock::time_point prev = Clock::now();
  while (!WindowShouldClose()) {
    Clock::time_point now = Clock::now();
    sim::TimeInt dt = static_cast<sim::TimeInt>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev)
            .count());
    prev = now;

    sim::SimResult res = game->exec(
        sim::SetMovementIntents{.intents = read_intents(), .target = ship}, dt);
    assert(res);

    // world origin at screen center
    Camera2D camera{};
    camera.offset = {static_cast<float>(GetScreenWidth()) / 2,
                     static_cast<float>(GetScreenHeight()) / 2};
    camera.zoom = 1;

    BeginDrawing();
    ClearBackground(BLACK);
    BeginMode2D(camera);
    for (auto [key, entity] : game->world.entities) {
      switch (entity.kind) {
      case sim::EntityKind::Ship:
        draw_ship(entity.transform);
        break;
      }
    }
    EndMode2D();
    DrawFPS(10, 10);
    EndDrawing();
  }

  CloseWindow();
}
