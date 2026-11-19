#include "sim/sim.hpp"
#include "sim/systems/combat/combat.hpp"
#include "sim/timeline.hpp"
#include "sim/world.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <memory>
#include <raylib.h>
#include <string_view>

static sim::MovementIntents read_intents() {
  sim::MovementIntents intents = 0;
  auto bind = [&](KeyboardKey key, sim::MovementIntent intent) {
    if (IsKeyDown(key))
      intents |= static_cast<sim::MovementIntents>(intent);
  };
  bind(KEY_A, sim::MovementIntent::Left);
  bind(KEY_D, sim::MovementIntent::Right);
  bind(KEY_SPACE, sim::MovementIntent::Jump);
  bind(KEY_LEFT_SHIFT, sim::MovementIntent::Sprint);
  bind(KEY_LEFT_CONTROL, sim::MovementIntent::Slide);
  return intents;
}

// Unit direction from `from` (world) to the mouse, through `camera`, or zero
// if the mouse is right on top of it.
static Vec2 read_aim(const Camera2D &camera, Vec2 from) {
  const Vector2 mouse = GetScreenToWorld2D(GetMousePosition(), camera);
  const Vec2 d{mouse.x - from.x, mouse.y - from.y};
  const float len = std::sqrt(d.x * d.x + d.y * d.y);
  if (len < 0.5f) {
    return {0, 0};
  }
  return {d.x / len, d.y / len};
}

// Where to draw something this frame. The sim has only stepped whole ticks;
// `ahead_secs` of sim time has built up since the last one (World::t_excess),
// so carry the last tick's motion forward by that much. Without this, frames
// show 1 or 2 ticks of movement unevenly and motion judders.
static Vec2 render_pos(const sim::Transform &t, const sim::Physics &p,
                       float ahead_secs) {
  return t.pos + p.vel * ahead_secs;
}

// placeholder art: a box, colored and shaped by movement state
//   white   normal
//   yellow  sprinting
//   orange  sliding (drawn squashed, feet stay put)
//   cyan    wall sliding (with a bar on the wall side)
//   red     just got hit
// plus a health bar above it when hurt, and the slash's hit box while
// slashing
static void draw_creature(Vec2 pos, const sim::Physics &p,
                          const sim::Movement &m, const sim::Combat &cb) {
  using sim::MoveState;
  const sim::Collider &c = p.collider;
  float w = c.w;
  float h = c.h;
  Color color = RAYWHITE;

  if (m.state.held(MoveState::Sprinting)) {
    color = YELLOW;
  }
  if (m.state.held(MoveState::Sliding)) {
    color = ORANGE;
    w = c.w * 1.5f;
    h = c.h * 0.5f;
  }
  if (m.state.held(MoveState::WallCling)) {
    color = SKYBLUE;
  }
  if (cb.since_hit < 0.15f) {
    color = RED;
  }

  // keep the bottom edge where the collider's is
  const float feet = pos.y + c.h / 2;
  DrawRectangleV({pos.x - w / 2, feet - h}, {w, h}, color);

  if (m.state.held(MoveState::WallCling)) {
    const float bar_x = m.last_wall_dir > 0 ? pos.x + w / 2 - 2 : pos.x - w / 2;
    DrawRectangleV({bar_x, feet - h}, {2, h}, BLUE);
  }

  if (cb.health < sim::CREATURE_HEALTH) {
    const float bar_w = c.w * 2;
    const float top = pos.y - c.h / 2 - 6;
    DrawRectangleV({pos.x - bar_w / 2, top}, {bar_w, 3}, DARKGRAY);
    DrawRectangleV({pos.x - bar_w / 2, top},
                   {bar_w * cb.health / sim::CREATURE_HEALTH, 3}, RED);
  }

  if (cb.slash_left > 0) {
    const float reach = m.params.slash_reach;
    const Vec2 center = pos + cb.slash_dir * (reach * 0.5f);
    DrawRectangleLinesEx(
        {center.x - reach / 2, center.y - reach / 2, reach, reach}, 1, GOLD);
  }
}

//====== HOLOGRAM ======//

// The hologram's target is the real-time sim's now plus an offset (world
// time). The offset starts at HOLO_OFFSET_START, and - and = change it
// instantly by HOLO_OFFSET_STEP within [HOLO_OFFSET_MIN, HOLO_OFFSET_MAX].
constexpr sim::TimeInt HOLO_OFFSET_START = 1'000'000'000;
constexpr sim::TimeInt HOLO_OFFSET_STEP = 100'000'000;
constexpr sim::TimeInt HOLO_OFFSET_MIN = 100'000'000;
constexpr sim::TimeInt HOLO_OFFSET_MAX = 10'000'000'000;
// Its keyframes have to reach back to the real-time sim's now (inputs roll
// it back there): the most offset there can be, plus one keyframe interval.
constexpr sim::TimeInt HOLO_KEYFRAME_INTERVAL = 250'000'000;
constexpr uint32_t HOLO_KEYFRAME_COUNT = 48; // 12 s
static_assert(HOLO_KEYFRAME_COUNT * HOLO_KEYFRAME_INTERVAL >=
              HOLO_OFFSET_MAX + HOLO_KEYFRAME_INTERVAL);

// The hologram doesn't sit at its target: it catches up to it. Its lag (how
// far it is behind the target, world seconds) follows a critically damped
// spring to 0 (no overshoot). An input rolls it back to the present, so the
// lag jumps to the whole offset and it fast-forwards along the new future;
// an offset change moves the target, and it runs or rewinds to it.
// Stiffness in 1/s^2: 40 settles in about a second.
constexpr double HOLO_CATCH_UP_STIFFNESS = 40;

struct CatchUp {
  double lag;
  double vel;
};

// One real-time step of the lag's spring toward 0, split into short steps
// like the camera's.
static void catch_up(CatchUp &s, double frame_secs) {
  const double damping = 2 * std::sqrt(HOLO_CATCH_UP_STIFFNESS);
  constexpr double MAX_STEP = 1.0 / 120;
  while (frame_secs > 0) {
    const double dt = std::min(frame_secs, MAX_STEP);
    s.vel += (-s.lag * HOLO_CATCH_UP_STIFFNESS - s.vel * damping) * dt;
    s.lag += s.vel * dt;
    frame_secs -= dt;
  }
  // close enough: caught up exactly
  if (std::abs(s.lag) < 1e-4 && std::abs(s.vel) < 1e-3) {
    s.lag = 0;
    s.vel = 0;
  }
}

// Hologram colors, with alpha scaled by the flicker.
static Color holo(unsigned char r, unsigned char g, unsigned char b,
                  float alpha) {
  return Color{r, g, b,
               static_cast<unsigned char>(std::clamp(alpha, 0.0f, 255.0f))};
}

// The hologram's look for one box: additive cyan, scanlines scrolling up,
// a flickering outline, and now and then a small sideways glitch. Drawn
// inside BLEND_ADDITIVE. `t` is real time in seconds.
static void draw_hologram_box(Rectangle r, float t) {
  const float flicker = 0.8f + 0.2f * std::sin(t * 37.0f);
  if (GetRandomValue(0, 40) == 0) {
    r.x += static_cast<float>(GetRandomValue(-2, 2));
  }
  DrawRectangleRec(r, holo(40, 200, 255, 35 * flicker));
  constexpr float SPACING = 3;
  const float scroll = std::fmod(t * 12.0f, SPACING);
  for (float y = r.y + SPACING - scroll; y < r.y + r.height; y += SPACING) {
    DrawLineV({r.x, y}, {r.x + r.width, y}, holo(90, 230, 255, 80 * flicker));
  }
  DrawRectangleLinesEx(r, 1, holo(150, 245, 255, 220 * flicker));
}

// A creature in the hologram: shaped like draw_creature (sliding squashes
// it), plus its slash's hit box while slashing.
static void draw_hologram_creature(Vec2 pos, const sim::Physics &p,
                                   const sim::Movement &m,
                                   const sim::Combat &cb, float t) {
  const sim::Collider &c = p.collider;
  float w = c.w;
  float h = c.h;
  if (m.state.held(sim::MoveState::Sliding)) {
    w = c.w * 1.5f;
    h = c.h * 0.5f;
  }
  const float feet = pos.y + c.h / 2;
  draw_hologram_box({pos.x - w / 2, feet - h, w, h}, t);
  if (cb.slash_left > 0) {
    const float reach = m.params.slash_reach;
    const Vec2 center = pos + cb.slash_dir * (reach * 0.5f);
    DrawRectangleLinesEx(
        {center.x - reach / 2, center.y - reach / 2, reach, reach}, 1,
        holo(150, 245, 255, 160));
  }
}

//====== CAMERA ======//

constexpr float ZOOM = 2;
// The camera hangs off the player on a damped spring.
//   stiffness:     how hard it's pulled toward the player, per second^2
//   damping ratio: 1 settles without overshooting, below 1 bounces a little
constexpr float CAMERA_STIFFNESS = 60;
constexpr float CAMERA_DAMPING_RATIO = 0.7f;

struct CameraSpring {
  Vec2 pos;
  Vec2 vel;
};

// One step of the spring toward `target`. Long frames are split into short
// steps so a hitch can't make it fly off.
static void follow_camera(CameraSpring &cam, Vec2 target, float frame_secs) {
  const float damping = 2 * CAMERA_DAMPING_RATIO * std::sqrt(CAMERA_STIFFNESS);
  constexpr float MAX_STEP = 1.0f / 120;
  while (frame_secs > 0) {
    const float dt = std::min(frame_secs, MAX_STEP);
    const Vec2 accel =
        (target - cam.pos) * CAMERA_STIFFNESS - cam.vel * damping;
    cam.vel += accel * dt;
    cam.pos += cam.vel * dt;
    frame_secs -= dt;
  }
}

// The player's last few drawn positions, oldest first once full. Purely
// visual.
struct Trail {
  static constexpr int N = 12;
  std::array<Vec2, N> pos;
  int next;
  int count;
};

static void push_trail(Trail &trail, Vec2 p) {
  trail.pos[static_cast<size_t>(trail.next)] = p;
  trail.next = (trail.next + 1) % Trail::N;
  trail.count = std::min(trail.count + 1, Trail::N);
}

// faint boxes along the recent path, fading with age
static void draw_trail(const Trail &trail, const sim::Collider &c) {
  for (int k = 0; k < trail.count; k++) {
    // oldest first
    const int i = (trail.next - trail.count + k + Trail::N) % Trail::N;
    const Vec2 p = trail.pos[static_cast<size_t>(i)];
    const float age =
        1 - static_cast<float>(k + 1) / static_cast<float>(trail.count + 1);
    const auto alpha = static_cast<unsigned char>(90 * (1 - age));
    DrawRectangleV({p.x - c.w / 2, p.y - c.h / 2}, {c.w, c.h},
                   Color{245, 245, 245, alpha});
  }
}

static uint64_t splitmix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EB;
  return x ^ (x >> 31);
}

// Infinite static starfield. Space is cut into square cells and each cell's
// stars come from a hash of its coordinates, so the same cell always has the
// same stars and nothing is stored. Only cells overlapping the view are drawn.
static void draw_stars(Vector2 view_min, Vector2 view_max) {
  constexpr float CELL = 128;
  constexpr int STARS_PER_CELL = 3;
  constexpr uint64_t SEED = 0x5EED;

  auto cell_of = [](float v) {
    return static_cast<int32_t>(std::floor(v / CELL));
  };
  // low 24 bits of h as a float in [0, 1)
  auto unit = [](uint64_t h) {
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(1 << 24);
  };

  for (int32_t cy = cell_of(view_min.y); cy <= cell_of(view_max.y); cy++) {
    for (int32_t cx = cell_of(view_min.x); cx <= cell_of(view_max.x); cx++) {
      uint64_t h = splitmix64(
          SEED ^ (static_cast<uint64_t>(static_cast<uint32_t>(cx)) |
                  static_cast<uint64_t>(static_cast<uint32_t>(cy)) << 32));
      for (int i = 0; i < STARS_PER_CELL; i++) {
        h = splitmix64(h);
        Vector2 pos{(static_cast<float>(cx) + unit(h)) * CELL,
                    (static_cast<float>(cy) + unit(h >> 24)) * CELL};
        float t = unit(h >> 40); // brightness and size
        auto shade = static_cast<unsigned char>(90 + t * 165);
        DrawCircleV(pos, 0.6f + t * t * 1.4f, Color{shade, shade, shade, 255});
      }
    }
  }
}

// Draws the solid cells in view, merging each row's consecutive solid cells
// into one rectangle to keep the draw count down.
static void draw_grid(const sim::Grid &grid, Vector2 view_min,
                      Vector2 view_max) {
  using sim::Grid;
  constexpr float CS = Grid::CELL_SIZE;
  const Color color{110, 90, 70, 255};
  int32_t x0 = std::max(sim::cell_coord(view_min.x, Grid::W), 0);
  int32_t y0 = std::max(sim::cell_coord(view_min.y, Grid::H), 0);
  int32_t x1 = std::min(sim::cell_coord(view_max.x, Grid::W), Grid::W - 1);
  int32_t y1 = std::min(sim::cell_coord(view_max.y, Grid::H), Grid::H - 1);
  for (int32_t y = y0; y <= y1; y++) {
    int32_t x = x0;
    while (x <= x1) {
      if (grid.at(x, y).kind != sim::CellKind::Solid) {
        x++;
        continue;
      }
      int32_t start = x;
      while (x <= x1 && grid.at(x, y).kind == sim::CellKind::Solid) {
        x++;
      }
      DrawRectangleV(
          {static_cast<float>(start) * CS, static_cast<float>(y) * CS},
          {static_cast<float>(x - start) * CS, CS}, color);
    }
  }
}

//====== PARAM EDITOR ======//

// Every MovementParams field, by name. Written out by hand; the
// static_assert below breaks the build if a field is added and not listed.
struct ParamField {
  const char *name;
  float sim::MovementParams::*member;
};
static constexpr ParamField PARAM_FIELDS[] = {
    {"walk_speed", &sim::MovementParams::walk_speed},
    {"sprint_speed", &sim::MovementParams::sprint_speed},
    {"walk_accel", &sim::MovementParams::walk_accel},
    {"air_accel", &sim::MovementParams::air_accel},
    {"turn_accel", &sim::MovementParams::turn_accel},
    {"air_turn_accel", &sim::MovementParams::air_turn_accel},
    {"overspeed_damping", &sim::MovementParams::overspeed_damping},
    {"ground_friction", &sim::MovementParams::ground_friction},
    {"air_damping", &sim::MovementParams::air_damping},
    {"ground_reach", &sim::MovementParams::ground_reach},
    {"jump_speed", &sim::MovementParams::jump_speed},
    {"coyote_time", &sim::MovementParams::coyote_time},
    {"jump_buffer_time", &sim::MovementParams::jump_buffer_time},
    {"wall_jump_grace", &sim::MovementParams::wall_jump_grace},
    {"slide_start_speed", &sim::MovementParams::slide_start_speed},
    {"slide_boost", &sim::MovementParams::slide_boost},
    {"slide_friction", &sim::MovementParams::slide_friction},
    {"slide_slope_accel", &sim::MovementParams::slide_slope_accel},
    {"slope_sample", &sim::MovementParams::slope_sample},
    {"slide_jump_x", &sim::MovementParams::slide_jump_x},
    {"slide_jump_y", &sim::MovementParams::slide_jump_y},
    {"wall_cling_reach", &sim::MovementParams::wall_cling_reach},
    {"wall_friction", &sim::MovementParams::wall_friction},
    {"wall_jump_reach", &sim::MovementParams::wall_jump_reach},
    {"wall_jump_speed", &sim::MovementParams::wall_jump_speed},
    {"wall_jump_cone", &sim::MovementParams::wall_jump_cone},
    {"burst_speed", &sim::MovementParams::burst_speed},
    {"burst_charges", &sim::MovementParams::burst_charges},
    {"burst_regen_time", &sim::MovementParams::burst_regen_time},
    {"hook_speed", &sim::MovementParams::hook_speed},
    {"hook_max_length", &sim::MovementParams::hook_max_length},
    {"hook_stiffness", &sim::MovementParams::hook_stiffness},
    {"hook_return_speed", &sim::MovementParams::hook_return_speed},
    {"slash_time", &sim::MovementParams::slash_time},
    {"slash_cooldown", &sim::MovementParams::slash_cooldown},
    {"slash_reach", &sim::MovementParams::slash_reach},
    {"slash_damage", &sim::MovementParams::slash_damage},
    {"perception_base", &sim::MovementParams::perception_base},
    {"perception_speed", &sim::MovementParams::perception_speed},
    {"perception_ease", &sim::MovementParams::perception_ease},
    {"slide_cooldown", &sim::MovementParams::slide_cooldown},
    {"wall_jump_cooldown", &sim::MovementParams::wall_jump_cooldown},
};
static_assert(sizeof(sim::MovementParams) ==
                  std::size(PARAM_FIELDS) * sizeof(float),
              "a MovementParams field is missing from PARAM_FIELDS");
static constexpr int N_PARAMS = static_cast<int>(std::size(PARAM_FIELDS));

struct ParamEditor {
  bool open;
  int selected;
};

constexpr int PANEL_W = 300;
constexpr int ROW_H = 18;
constexpr int PANEL_TOP = 10;
constexpr int ROWS_TOP = PANEL_TOP + 26;

// row under the mouse, or -1
static int hovered_row() {
  const Vector2 mouse = GetMousePosition();
  const float x0 = static_cast<float>(GetScreenWidth() - PANEL_W - 10);
  if (mouse.x < x0 || mouse.x > x0 + PANEL_W) {
    return -1;
  }
  const int row = static_cast<int>(std::floor((mouse.y - ROWS_TOP) / ROW_H));
  return row >= 0 && row < N_PARAMS ? row : -1;
}

// Steps are 5% of the default, so every param moves at a sensible scale.
static float param_step(int i) {
  static const sim::MovementParams DEFAULTS{};
  return std::max(std::abs(DEFAULTS.*PARAM_FIELDS[i].member) * 0.05f, 0.01f);
}

// Handles editor input. Returns true if `params` changed.
static bool edit_params(ParamEditor &ed, sim::MovementParams &params) {
  if (IsKeyPressed(KEY_F1)) {
    ed.open = !ed.open;
  }
  if (!ed.open) {
    return false;
  }

  if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) {
    ed.selected = (ed.selected + N_PARAMS - 1) % N_PARAMS;
  }
  if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) {
    ed.selected = (ed.selected + 1) % N_PARAMS;
  }
  const int hover = hovered_row();
  if (hover >= 0 && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
    ed.selected = hover;
  }

  // how many steps to move, and which param
  float steps = 0;
  int target = ed.selected;
  if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) {
    steps += 1;
  }
  if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) {
    steps -= 1;
  }
  const float wheel = GetMouseWheelMove();
  if (hover >= 0 && wheel != 0) {
    steps = wheel;
    target = hover;
  }

  float &value = params.*PARAM_FIELDS[target].member;
  if (steps != 0) {
    value = std::max(0.0f, value + steps * param_step(target));
    return true;
  }
  if (IsKeyPressed(KEY_BACKSPACE)) {
    static const sim::MovementParams DEFAULTS{};
    params.*PARAM_FIELDS[ed.selected].member =
        DEFAULTS.*PARAM_FIELDS[ed.selected].member;
    return true;
  }
  return false;
}

static void draw_params(const ParamEditor &ed,
                        const sim::MovementParams &params) {
  if (!ed.open) {
    DrawText("F1: movement params", 10, 58, 16, GRAY);
    return;
  }
  static const sim::MovementParams DEFAULTS{};
  const int x0 = GetScreenWidth() - PANEL_W - 10;
  const int h = ROWS_TOP - PANEL_TOP + N_PARAMS * ROW_H + 40;
  DrawRectangle(x0, PANEL_TOP, PANEL_W, h, Color{0, 0, 0, 200});
  DrawText("movement params (F1)", x0 + 8, PANEL_TOP + 4, 18, RAYWHITE);

  const int hover = hovered_row();
  for (int i = 0; i < N_PARAMS; i++) {
    const int y = ROWS_TOP + i * ROW_H;
    if (i == ed.selected) {
      DrawRectangle(x0, y, PANEL_W, ROW_H, Color{70, 70, 120, 255});
    } else if (i == hover) {
      DrawRectangle(x0, y, PANEL_W, ROW_H, Color{40, 40, 60, 255});
    }
    const float value = params.*PARAM_FIELDS[i].member;
    const bool changed = value != DEFAULTS.*PARAM_FIELDS[i].member;
    DrawText(PARAM_FIELDS[i].name, x0 + 8, y + 2, 16, RAYWHITE);
    DrawText(TextFormat("%.2f", static_cast<double>(value)), x0 + PANEL_W - 80,
             y + 2, 16, changed ? YELLOW : LIGHTGRAY);
  }
  const int foot = ROWS_TOP + N_PARAMS * ROW_H + 4;
  DrawText("up/down select, left/right or wheel adjust", x0 + 8, foot, 14,
           GRAY);
  DrawText("backspace: reset selected to default", x0 + 8, foot + 16, 14, GRAY);
}

int main(int argc, char **argv) {
  // the sim knows nothing about raylib. main only talks to it through exec()
  // and reads world state back for drawing.
  //
  // Two copies of the same sim, each with an input log and keyframes:
  //   game   real time
  //   ahead  the hologram, heading for holo_offset of world time ahead
  // `ahead` starts from a snapshot of `game` and gets every input after
  // that too: each one rolls it back to the input's time (the present),
  // where it takes the input, and then it catches up to its target again
  // (see CatchUp). It shows where everything goes if you keep doing what
  // you're doing.
  //
  // Only real inputs are sent: a frame with nothing new just lets time pass.
  //
  // H turns the hologram off completely (no simulating at all) and back on
  // (a fresh one from the present). --no-hologram starts with it off.
  auto game = std::make_unique<sim::Timeline>();
  std::unique_ptr<sim::Timeline> ahead;
  // `ahead` keeps its own clock, from 0 when it started: game time `holo_base`
  sim::TimeInt holo_base = 0;
  sim::TimeInt holo_offset = HOLO_OFFSET_START;
  CatchUp catch_up_state{};
  // how long moving the hologram took this frame, for the HUD
  double holo_ms = 0;
  auto timed = [&](auto &&f) {
    const auto start = std::chrono::steady_clock::now();
    f();
    holo_ms += std::chrono::duration<double, std::milli>(
                   std::chrono::steady_clock::now() - start)
                   .count();
  };
  // the hologram's position in game time, and where it's heading
  auto holo_time = [&] { return holo_base + ahead->now(); };
  auto holo_goal = [&] { return game->now() + holo_offset; };
  // moves the hologram to game time `t`: running on, or rolling back (its
  // log is kept, so nothing is lost)
  auto move_hologram_to = [&](sim::TimeInt t) {
    const sim::TimeInt now = holo_time();
    if (t > now) {
      ahead->advance(t - now);
    } else if (t < now) {
      const auto back = ahead->rollback_to(t - holo_base);
      assert(back);
      (void)back;
    }
  };
  // a fresh hologram from the present, catching up to its target
  auto start_hologram = [&] {
    ahead = std::make_unique<sim::Timeline>(
        sim::TimelineConfig{
            .keyframe_interval = HOLO_KEYFRAME_INTERVAL,
            .keyframe_count = HOLO_KEYFRAME_COUNT,
        },
        game->sim().save());
    holo_base = game->now();
    catch_up_state = {
        .lag = static_cast<double>(holo_offset) / 1e9,
        .vel = 0,
    };
  };

  auto exec = [&](sim::SimInput input, sim::TimeInt dt) {
    const sim::TimeInt at = game->now();
    const sim::SimResult res = game->exec(input, dt);
    // A rejected input changes nothing but time, and the hologram has had
    // the same inputs up to `at`, so it would reject it too: leave it be.
    if (ahead && res) {
      timed([&] {
        // back to the input's time, take it there, and catch up again
        move_hologram_to(at);
        const auto inserted = ahead->insert(input, at - holo_base);
        assert(inserted);
        (void)inserted;
      });
      catch_up_state = {
          .lag = static_cast<double>(holo_goal() - holo_time()) / 1e9,
          .vel = 0,
      };
    }
    return res;
  };
  auto advance = [&](sim::TimeInt dt) { game->advance(dt); };

  // the map: static, owned here, the sim only points at it
  auto map = std::make_unique<sim::Grid>();
  sim::fill_test_level(*map);
  sim::SimResult map_set = exec(sim::SetMap{.map = map.get()}, 0);
  assert(map_set);
  sim::SimResult spawned = exec(
      sim::SpawnCreature{
          // middle of the test level's flat stretch, a little above it
          .pos = {(sim::FLAT_X0 + sim::FLAT_X1) / 2 * sim::Grid::CELL_SIZE,
                  (sim::TEST_LEVEL_GROUND - 10) * sim::Grid::CELL_SIZE}},
      0);
  assert(spawned);
  // the hologram starts from a snapshot of `game`, so ids match in both
  const entt::entity player = std::get<entt::entity>(*spawned);
  // read-only from here on: all changes go through exec()
  const entt::registry &reg = game->sim().world().reg;

  // a dummy to hook and slash: a creature nobody controls, out on the flat
  // stretch to the right of the arena wall. R brings it back.
  constexpr Vec2 DUMMY_POS{1600 * sim::Grid::CELL_SIZE,
                           (sim::TEST_LEVEL_GROUND - 3) * sim::Grid::CELL_SIZE};
  auto spawn_dummy = [&] {
    sim::SimResult res = exec(sim::SpawnCreature{.pos = DUMMY_POS}, 0);
    assert(res);
    return std::get<entt::entity>(*res);
  };
  entt::entity dummy = spawn_dummy();

  if (!(argc > 1 && std::string_view(argv[1]) == "--no-hologram")) {
    start_hologram();
  }
  // the longest frame in the last second, for spotting hitches (an fps
  // counter averages them away)
  double worst_frame_ms = 0;
  double worst_frame_shown_ms = 0;
  double worst_frame_window = 0;
  // the last intents sent (see the input below)
  sim::MovementIntents sent_intents = 0;

  SetConfigFlags(FLAG_WINDOW_RESIZABLE);
  InitWindow(1280, 720, "small-game");
  SetTargetFPS(144);

  // subtract or add 10% every second held
  double change_factor = 0.5;
  double time_factor = 1.0;

  ParamEditor editor{};

  // render-side state, none of it in the sim
  Vec2 player_drawn = reg.get<sim::Transform>(player).pos;
  CameraSpring cam{.pos = player_drawn, .vel = {0, 0}};
  Camera2D camera{};
  camera.zoom = ZOOM;
  Trail trail{};

  using Clock = std::chrono::steady_clock;
  Clock::time_point prev = Clock::now();
  while (!WindowShouldClose()) {
    Clock::time_point now = Clock::now();
    // protentially bad precision, but whatever
    sim::TimeInt dt = static_cast<sim::TimeInt>(

        std::chrono::duration_cast<std::chrono::nanoseconds>(now - prev)
            .count());
    prev = now;

    double dt_secs = (static_cast<double>(dt) / static_cast<double>(1e9));
    worst_frame_ms = std::max(worst_frame_ms, dt_secs * 1e3);
    worst_frame_window += dt_secs;
    if (worst_frame_window >= 1) {
      worst_frame_shown_ms = worst_frame_ms;
      worst_frame_ms = 0;
      worst_frame_window = 0;
    }
    // time scale: [ slower, ] faster
    if (IsKeyDown(KEY_LEFT_BRACKET)) {
      time_factor -= std::max(0.0, time_factor * dt_secs * change_factor);
    }
    if (IsKeyDown(KEY_RIGHT_BRACKET)) {
      time_factor += time_factor * dt_secs * change_factor;
    }

    // Real time to world time: the player perceives `perception` seconds per
    // world second, so this frame is 1 / perception as much world time.
    // [ and ] scale it further, for debugging.
    const double perception = reg.get<sim::Perception>(player).value;
    sim::TimeInt sim_dt = static_cast<sim::TimeInt>(static_cast<double>(dt) *
                                                    time_factor / perception);

    if (IsKeyPressed(KEY_R) && !reg.valid(dummy)) {
      dummy = spawn_dummy();
    }
    holo_ms = 0;
    if (IsKeyPressed(KEY_H)) {
      if (ahead) {
        ahead.reset();
      } else {
        start_hologram();
      }
    }
    // The offset changes at once; the hologram doesn't jump with it: its
    // lag takes the change, and it catches up to the new target.
    if (IsKeyPressed(KEY_EQUAL) && holo_offset < HOLO_OFFSET_MAX) {
      holo_offset += HOLO_OFFSET_STEP;
      catch_up_state.lag += static_cast<double>(HOLO_OFFSET_STEP) / 1e9;
    }
    if (IsKeyPressed(KEY_MINUS) && holo_offset > HOLO_OFFSET_MIN) {
      holo_offset -= HOLO_OFFSET_STEP;
      catch_up_state.lag -= static_cast<double>(HOLO_OFFSET_STEP) / 1e9;
    }

    // param edits go in as an input like everything else (no time passes)
    sim::MovementParams params = reg.get<sim::Movement>(player).params;
    if (edit_params(editor, params)) {
      sim::SimResult set =
          exec(sim::SetMovementParams{.params = params, .target = player}, 0);
      assert(set);
    }

    // aimed through last frame's camera, from where the player was drawn
    const Vec2 aim = read_aim(camera, player_drawn);

    // Actions, one input per press (or release), before this frame's time
    // passes. The sim checks each one; a rejected one (no gas, hook already
    // out, on cooldown) changes nothing, so the hologram is left alone.
    if (IsKeyPressed(KEY_Q)) {
      (void)exec(sim::GasBurst{.aim = aim, .target = player}, 0);
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
      (void)exec(sim::FireHook{.aim = aim, .target = player}, 0);
    }
    if (IsMouseButtonReleased(MOUSE_BUTTON_RIGHT)) {
      (void)exec(sim::ReleaseHook{.target = player}, 0);
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
      (void)exec(sim::Slash{.aim = aim, .target = player}, 0);
    }

    // Movement input only when the intents change: every input rolls the
    // hologram back to the present. Aim goes along with it: the sim only
    // reads it on a (buffered) jump press, which is an intent change, so
    // it arrives exactly when it's used.
    const sim::MovementIntents intents = read_intents();
    if (intents != sent_intents) {
      sim::SimResult res =
          exec(sim::SetMovementIntents{.intents = intents,
                                       .aim = aim,
                                       .target = player},
               sim_dt);
      assert(res);
      sent_intents = intents;
    } else {
      advance(sim_dt);
    }

    // The hologram keeps pace with its target and closes its lag on the
    // spring: it goes to the target minus whatever lag is left.
    if (ahead) {
      catch_up(catch_up_state, dt_secs);
      const auto lag = static_cast<sim::TimeInt>(
          std::llround(catch_up_state.lag * 1e9));
      // never behind the present, never past the furthest offset
      const sim::TimeInt t = std::clamp(holo_goal() - lag, game->now(),
                                        game->now() + HOLO_OFFSET_MAX);
      timed([&] { move_hologram_to(t); });
    }

    // sim time since the last tick, to draw ahead by
    const float ahead_secs =
        static_cast<float>(static_cast<double>(game->sim().world().t_excess) /
                           1e9);

    const sim::Physics &player_physics = reg.get<sim::Physics>(player);
    player_drawn = render_pos(reg.get<sim::Transform>(player), player_physics,
                              ahead_secs);
    push_trail(trail, player_drawn);

    follow_camera(cam, player_drawn, static_cast<float>(dt_secs));
    const float screen_half_w = static_cast<float>(GetScreenWidth()) / 2;
    const float screen_half_h = static_cast<float>(GetScreenHeight()) / 2;
    camera.offset = {screen_half_w, screen_half_h};
    camera.target = {cam.pos.x, cam.pos.y};

    // visible world rect
    const float view_half_w = screen_half_w / ZOOM;
    const float view_half_h = screen_half_h / ZOOM;
    Vector2 view_min{cam.pos.x - view_half_w, cam.pos.y - view_half_h};
    Vector2 view_max{cam.pos.x + view_half_w, cam.pos.y + view_half_h};

    BeginDrawing();
    ClearBackground(BLACK);
    BeginMode2D(camera);
    draw_stars(view_min, view_max);
    draw_grid(*game->sim().world().grid, view_min, view_max);
    draw_trail(trail, player_physics.collider);
    // hooks: a rope from the owner, and the hook itself
    auto hooks = reg.view<const sim::Hook, const sim::Owner,
                          const sim::Transform, const sim::Physics>();
    for (auto [e, hook, owner, t, p] : hooks.each()) {
      const Vec2 hook_pos = render_pos(t, p, ahead_secs);
      const auto *owner_t = reg.try_get<sim::Transform>(owner.entity);
      const auto *owner_p = reg.try_get<sim::Physics>(owner.entity);
      if (owner_t && owner_p) {
        const Vec2 owner_pos = render_pos(*owner_t, *owner_p, ahead_secs);
        DrawLineEx({owner_pos.x, owner_pos.y}, {hook_pos.x, hook_pos.y}, 1,
                   GRAY);
      }
      DrawRectangleV({hook_pos.x - 2, hook_pos.y - 2}, {4, 4}, LIGHTGRAY);
    }
    auto creatures = reg.view<const sim::Transform, const sim::Physics,
                              const sim::Movement, const sim::Combat>();
    for (auto [e, t, p, m, cb] : creatures.each()) {
      draw_creature(render_pos(t, p, ahead_secs), p, m, cb);
    }

    // the hologram on top: everything as it'll be where the hologram is
    const auto real_t = static_cast<float>(GetTime());
    Vec2 holo_player{};
    bool holo_player_alive = false;
    if (ahead) {
      const entt::registry &holo_reg = ahead->sim().world().reg;
      const float holo_ahead_secs = static_cast<float>(
          static_cast<double>(ahead->sim().world().t_excess) / 1e9);
      BeginBlendMode(BLEND_ADDITIVE);
      auto holo_hooks = holo_reg.view<const sim::Hook, const sim::Owner,
                                      const sim::Transform, const sim::Physics>();
      for (auto [e, hook, owner, t, p] : holo_hooks.each()) {
        const Vec2 hook_pos = render_pos(t, p, holo_ahead_secs);
        const auto *owner_t = holo_reg.try_get<sim::Transform>(owner.entity);
        const auto *owner_p = holo_reg.try_get<sim::Physics>(owner.entity);
        if (owner_t && owner_p) {
          const Vec2 owner_pos = render_pos(*owner_t, *owner_p, holo_ahead_secs);
          DrawLineEx({owner_pos.x, owner_pos.y}, {hook_pos.x, hook_pos.y}, 1,
                     holo(90, 230, 255, 140));
        }
        draw_hologram_box({hook_pos.x - 2, hook_pos.y - 2, 4, 4}, real_t);
      }
      auto holo_creatures =
          holo_reg.view<const sim::Transform, const sim::Physics,
                        const sim::Movement, const sim::Combat>();
      for (auto [e, t, p, m, cb] : holo_creatures.each()) {
        const Vec2 pos = render_pos(t, p, holo_ahead_secs);
        draw_hologram_creature(pos, p, m, cb, real_t);
        if (e == player) {
          holo_player = {pos.x, pos.y - p.collider.h / 2};
          holo_player_alive = true;
        }
      }
      EndBlendMode();
    }
    EndMode2D();

    // how far ahead it actually is right now (it catches up to the offset)
    const double holo_ahead =
        ahead ? static_cast<double>(holo_time() - game->now()) / 1e9 : 0;
    if (holo_player_alive) {
      const Vector2 tag =
          GetWorldToScreen2D({holo_player.x, holo_player.y - 6}, camera);
      const char *label = TextFormat("+%.2fs", holo_ahead);
      DrawText(label, static_cast<int>(tag.x) - MeasureText(label, 16) / 2,
               static_cast<int>(tag.y) - 16, 16, holo(150, 245, 255, 230));
    }
    DrawFPS(10, 10);
    DrawText(TextFormat("worst frame %.1f ms", worst_frame_shown_ms), 110, 10,
             20, worst_frame_shown_ms > 20 ? RED : LIME);
    DrawText(TextFormat("%.2fx time  perception %.2f  tick %.0f us",
                        time_factor, perception,
                        static_cast<double>(game->sim().tick_dur()) / 1e3),
             10, 34, 20, LIME);
    const sim::Movement &player_movement = reg.get<sim::Movement>(player);
    DrawText(TextFormat("gas: %d", reg.get<sim::Odm>(player).burst_charges),
             10, 80, 20, SKYBLUE);
    if (ahead) {
      DrawText(TextFormat("hologram (H): +%.2fs of +%.2fs (-/=), %.2f ms "
                          "this frame",
                          holo_ahead, static_cast<double>(holo_offset) / 1e9,
                          holo_ms),
               10, 104, 20, holo(150, 245, 255, 230));
    } else {
      DrawText("hologram off (H): not simulated", 10, 104, 20, GRAY);
    }
    if (!reg.valid(dummy)) {
      DrawText("R: respawn dummy", 10, 128, 20, GRAY);
    }
    draw_params(editor, player_movement.params);
    EndDrawing();
  }

  CloseWindow();
}
