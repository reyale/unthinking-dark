// Browser replay player — compiled with Emscripten.
// Drop a .ud replay file onto the page to load it.
// Controls: Space=play/pause  ←/→=step  U/G/0/1=view  Scroll=zoom

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include <emscripten.h>

#include "config.hpp"
#include "entity.hpp"
#include "vision.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "stats.hpp"
#include "wincheck.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

// ---- Map reconstruction ---------------------------------------------------

static game::Map map_from_log(const game::ReplayLog& log) {
  game::Map map = game::Map::make(log.map_w, log.map_h);
  for (int32_t y = 0; y < log.map_h; ++y)
    for (int32_t x = 0; x < log.map_w; ++x) {
      const auto& t = log.map_tiles[static_cast<size_t>(y * log.map_w + x)];
      if (t.terrain != game::Terrain::Open || t.resource_amount != 0)
        map.set_terrain({x, y}, t.terrain, t.resource_amount);
    }
  map.recount_passable();
  return map;
}

// ---- Color palette --------------------------------------------------------

static constexpr ImVec4 kFogBg    = {0.06f, 0.06f, 0.08f, 1.0f};
static constexpr ImVec4 kOpenBg   = {0.14f, 0.14f, 0.18f, 1.0f};
static constexpr ImVec4 kAsteroid = {0.35f, 0.30f, 0.22f, 1.0f};
static constexpr ImVec4 kNebula   = {0.22f, 0.15f, 0.35f, 1.0f};
static constexpr ImVec4 kResource = {0.24f, 0.20f, 0.14f, 1.0f}; // ground under the crystals
static constexpr ImVec4 kCargo    = {1.0f, 0.72f, 0.18f, 1.0f}; // Drone hold bar
static constexpr ImVec4 kFactionA = {0.40f, 0.70f, 1.00f, 1.0f};
static constexpr ImVec4 kFactionB = {1.00f, 0.42f, 0.32f, 1.0f};

static ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

// ---- Per-cell rendering data ----------------------------------------------

struct CellData {
  ImVec4  bg;
  char    glyph{'\0'};
  ImVec4  fg{};
  int32_t hp{-1}, max_hp{-1};
  float   cargo{0.0f}; // Drone hold fill, 0..1
  bool    node{false}; // resource node: drawn as amber crystals
};

static float cargo_fill(const game::UnitFrame& u) {
  return std::max(static_cast<float>(u.cargo_energy) / game::cfg::DRONE_CARGO_ENERGY,
                  static_cast<float>(u.cargo_alloy) / game::cfg::DRONE_CARGO_ALLOY);
}

static CellData make_cell(const game::Map& map, const game::FrameState& fr,
                           int view, int32_t x, int32_t y) {
  bool vis = (view == -1);
  if (!vis) {
    for (const auto& u : fr.units) {
      if (view >= 0 && static_cast<int>(u.faction.value) != view) continue;
      if (game::sees_tile(map, u.pos, game::stats_for(u.type).sight, {x, y}))
        { vis = true; break; }
    }
    if (!vis) {
      for (const auto& s : fr.structures) {
        if (view >= 0 && static_cast<int>(s.faction.value) != view) continue;
        if (s.pos.x == x && s.pos.y == y) { vis = true; break; }
      }
    }
  }
  if (!vis) return {kFogBg};

  ImVec4 bg;
  switch (map.tile_at({x, y}).terrain) {
    case game::Terrain::Asteroid:     bg = kAsteroid; break;
    case game::Terrain::Nebula:       bg = kNebula;   break;
    case game::Terrain::ResourceNode: bg = kResource; break;
    default:                          bg = kOpenBg;   break;
  }

  for (const auto& s : fr.structures) {
    if (s.pos.x != x || s.pos.y != y) continue;
    char g = s.type == game::StructureType::CommandCore ? 'C' :
             s.type == game::StructureType::Factory     ? 'T' :
             s.build_ticks_left > 0                      ? 'n' : 'N';
    ImVec4 fg = s.faction.value == 0 ? kFactionA : kFactionB;
    return {bg, g, fg, s.hp, s.max_hp};
  }
  const bool node = map.tile_at({x, y}).terrain == game::Terrain::ResourceNode;

  const game::UnitFrame* top = nullptr;
  for (const auto& u : fr.units)
    if (u.pos.x == x && u.pos.y == y) top = &u;
  if (top) {
    char g = top->type == game::UnitType::Drone       ? (top->carrying() ? 'D' : 'd') :
             top->type == game::UnitType::Interceptor  ? 'i' :
             top->type == game::UnitType::Frigate       ? 'f' :
             top->type == game::UnitType::Artillery     ? 'a' : 'b';
    ImVec4 fg = top->faction.value == 0 ? kFactionA : kFactionB;
    return {bg, g, fg, top->hp, top->max_hp, cargo_fill(*top), node};
  }

  CellData cd{bg};
  cd.node = node;
  return cd;
}

// ---- Global state ---------------------------------------------------------

enum class AppState { Waiting, Playing };

struct App {
  AppState state    = AppState::Waiting;
  std::string error_msg;
  game::Map   map;
  std::vector<game::FrameState> frames;
  int   cur     = 0;
  int   total   = 0;
  int   view    = -2;
  bool  playing = false;
  float fps     = 10.0f;
  float cs      = 48.0f;
  double last_t = 0.0;
};

static App           g;
static GLFWwindow*   g_win = nullptr;

// ---- Called from JS after writing /replay.ud to Emscripten FS -------------

extern "C" EMSCRIPTEN_KEEPALIVE void load_replay_from_fs() {
  g.error_msg.clear();
  try {
    game::ReplayLog log = game::read_replay_file("/replay.ud");
    if (game::replay(log) != log.expected_hash)
      fprintf(stderr, "warning: hash mismatch — replay may not reproduce original\n");
    g.map    = map_from_log(log);
    g.frames = game::replay_frames(log);
    g.cur    = 0;
    g.total  = static_cast<int>(g.frames.size());
    g.state  = AppState::Playing;
    fprintf(stderr, "Loaded: %d ticks on %dx%d map\n", g.total - 1, log.map_w, log.map_h);
  } catch (const std::exception& e) {
    g.error_msg = e.what();
    fprintf(stderr, "load error: %s\n", e.what());
  }
}

// ---- Per-frame callback ---------------------------------------------------

static void main_loop() {
  glfwPollEvents();
  ImGuiIO& io = ImGui::GetIO();

  if (g.state == AppState::Playing && g.playing) {
    double now = glfwGetTime();
    if (now - g.last_t >= 1.0 / static_cast<double>(g.fps)) {
      if (g.cur + 1 < g.total) ++g.cur; else g.playing = false;
      g.last_t = now;
    }
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  if (g.state == AppState::Playing && !io.WantCaptureMouse)
    g.cs = std::clamp(g.cs + io.MouseWheel * 2.0f, 8.0f, 64.0f);

  if (g.state == AppState::Playing && !io.WantCaptureKeyboard) {
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
      g.playing = !g.playing;
      g.last_t  = glfwGetTime();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && g.cur + 1 < g.total) { ++g.cur; g.playing = false; }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)  && g.cur > 0)           { --g.cur; g.playing = false; }
    if (ImGui::IsKeyPressed(ImGuiKey_U)) g.view = -2;
    if (ImGui::IsKeyPressed(ImGuiKey_G)) g.view = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_0)) g.view = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_1)) g.view = 1;
  }

  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##ud", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  if (g.state == AppState::Waiting) {
    const ImVec2 sz  = io.DisplaySize;
    const ImVec2 ctr = {sz.x * 0.5f, sz.y * 0.5f};
    const char* msg  = "Drop a .ud replay file onto the page to load it.";
    ImVec2 tsz = ImGui::CalcTextSize(msg);
    ImGui::SetCursorPos({ctr.x - tsz.x * 0.5f, ctr.y - tsz.y * 0.5f - 12.0f});
    ImGui::TextDisabled("%s", msg);
    if (!g.error_msg.empty()) {
      ImVec2 esz = ImGui::CalcTextSize(g.error_msg.c_str());
      ImGui::SetCursorPos({ctr.x - esz.x * 0.5f, ctr.y + tsz.y * 0.5f + 4.0f});
      ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", g.error_msg.c_str());
    }
  } else {
    const auto& fr = g.frames[static_cast<size_t>(g.cur)];

    // ---- Control bar -------------------------------------------------------
    ImGui::SetNextItemWidth(300.0f);
    char tick_fmt[32];
    snprintf(tick_fmt, sizeof(tick_fmt), "%%d / %d", g.total - 1);
    if (ImGui::SliderInt("##t", &g.cur, 0, g.total - 1, tick_fmt)) g.playing = false;
    ImGui::SameLine();
    if (ImGui::Button(g.playing ? "Pause" : " Play ")) { g.playing = !g.playing; g.last_t = glfwGetTime(); }
    ImGui::SameLine();
    if (ImGui::ArrowButton("##l", ImGuiDir_Left)  && g.cur > 0)          { --g.cur; g.playing = false; }
    ImGui::SameLine(0, 2);
    if (ImGui::ArrowButton("##r", ImGuiDir_Right) && g.cur + 1 < g.total){ ++g.cur; g.playing = false; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::SliderFloat("fps##s", &g.fps, 1.0f, 30.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    if (ImGui::Button("Union")) g.view = -2;
    ImGui::SameLine();
    if (ImGui::Button("God")) g.view = -1;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4{kFactionA.x * 0.6f, kFactionA.y * 0.6f, kFactionA.z * 0.6f, 1.0f});
    if (ImGui::Button(" A ")) g.view = 0;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button,
        ImVec4{kFactionB.x * 0.6f, kFactionB.y * 0.6f, kFactionB.z * 0.6f, 1.0f});
    if (ImGui::Button(" B ")) g.view = 1;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(65.0f);
    ImGui::SliderFloat("px##z", &g.cs, 16.0f, 128.0f, "%.0f");
    ImGui::SameLine();
    ImGui::Text("|");
    ImGui::SameLine();
    ImGui::TextColored(kFactionA, "A  e:%-4d a:%d",
        fr.resources[0].energy, fr.resources[0].alloy);
    ImGui::SameLine();
    ImGui::TextColored(kFactionB, "B  e:%-4d a:%d",
        fr.resources[1].energy, fr.resources[1].alloy);

    if (fr.result.has_value()) {
      const auto& res = *fr.result;
      ImGui::SameLine();
      if (res.reason == game::WinReason::Draw) {
        ImGui::TextColored({0.9f, 0.85f, 0.2f, 1.0f}, " | DRAW");
      } else {
        ImVec4      wc  = res.winner.value == 0 ? kFactionA : kFactionB;
        const char* why =
            res.reason == game::WinReason::BaseDestroyed      ? "base" :
            res.reason == game::WinReason::TerritoryThreshold ? "territory" : "tick cap";
        ImGui::TextColored(wc, " | %s wins (%s)", res.winner.value == 0 ? "A" : "B", why);
      }
    }

    ImGui::Separator();

    // ---- Isometric grid ---------------------------------------------------
    ImGui::BeginChild("##grid", {0, 0}, false,
        ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl     = ImGui::GetWindowDrawList();
    ImVec2      origin = ImGui::GetCursorScreenPos();

    const float  cw2     = g.cs * 0.5f;
    const float  ch2     = g.cs * 0.25f;
    const ImVec2 iso_org = {origin.x + (float)g.map.height * cw2, origin.y + ch2};

    auto tile_ctr = [&](int32_t x, int32_t y) -> ImVec2 {
      return {iso_org.x + (x - y) * cw2, iso_org.y + (x + y) * ch2};
    };
    auto dim = [](ImVec4 c, float f) -> ImVec4 {
      return {c.x * f, c.y * f, c.z * f, c.w};
    };

    for (int32_t y = 0; y < g.map.height; ++y) {
      for (int32_t x = 0; x < g.map.width; ++x) {
        CellData cd  = make_cell(g.map, fr, g.view, x, y);
        ImVec2   ctr = tile_ctr(x, y);

        ImVec2 top    = {ctr.x,        ctr.y - ch2};
        ImVec2 right  = {ctr.x + cw2,  ctr.y      };
        ImVec2 bottom = {ctr.x,        ctr.y + ch2};
        ImVec2 left   = {ctr.x - cw2,  ctr.y      };

        dl->AddTriangleFilled(top, right, left, u32(cd.bg));
        dl->AddTriangleFilled(right, bottom, left, u32(dim(cd.bg, 0.70f)));
        dl->AddQuad(top, right, bottom, left, u32({0.0f, 0.0f, 0.0f, 0.40f}), 0.5f);

        if (cd.node) {
          // Three amber shards, tallest in the middle; units draw over them.
          const float s = cw2 * 0.22f;
          for (int i = -1; i <= 1; ++i) {
            const float h = (i == 0 ? 1.0f : 0.6f) * ch2 * 1.6f;
            const ImVec2 b = {ctr.x + i * s, ctr.y + ch2 * 0.35f};
            dl->AddTriangleFilled({b.x - s * 0.5f, b.y}, {b.x + s * 0.5f, b.y}, {b.x, b.y - h},
                                  u32(kCargo));
            dl->AddTriangleFilled({b.x, b.y}, {b.x + s * 0.5f, b.y}, {b.x, b.y - h},
                                  u32({1.0f, 0.88f, 0.5f, 1.0f}));
          }
        }

        if (cd.glyph) {
          char   buf[2] = {cd.glyph, '\0'};
          ImVec2 tsz    = ImGui::CalcTextSize(buf);
          dl->AddText({ctr.x - tsz.x * 0.5f, ctr.y - tsz.y * 0.5f}, u32(cd.fg), buf);

          if (cd.hp >= 0 && cd.max_hp > 0 && g.cs >= 32.0f) {
            float  ratio  = static_cast<float>(cd.hp) / static_cast<float>(cd.max_hp);
            ImVec2 hp_end = {right.x + ratio * (bottom.x - right.x),
                             right.y + ratio * (bottom.y - right.y)};
            float  bw     = std::max(2.0f, ch2 * 0.3f);
            ImVec4 hc     = ratio > 0.5f
                ? ImVec4{0.2f + (1.0f - ratio) * 1.6f, 0.85f, 0.2f, 1.0f}
                : ImVec4{1.0f, ratio * 1.7f, 0.1f, 1.0f};
            dl->AddLine(right, bottom, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
            dl->AddLine(right, hp_end, u32(hc), bw);
          }
          if (cd.cargo > 0.0f && g.cs >= 32.0f) {
            ImVec2 c_end = {left.x + cd.cargo * (bottom.x - left.x),
                            left.y + cd.cargo * (bottom.y - left.y)};
            float  bw    = std::max(2.0f, ch2 * 0.3f);
            dl->AddLine(left, bottom, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
            dl->AddLine(left, c_end, u32(kCargo), bw);
          }
        }
      }
    }

    ImGui::Dummy({(float)(g.map.width + g.map.height) * cw2 + cw2,
                  (float)(g.map.width + g.map.height) * ch2 + ch2});
    ImGui::EndChild();
  }

  ImGui::End();

  ImGui::Render();
  int fb_w, fb_h;
  glfwGetFramebufferSize(g_win, &fb_w, &fb_h);
  glViewport(0, 0, fb_w, fb_h);
  glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(g_win);
}

// ---- Entry point ----------------------------------------------------------

int main() {
  if (!glfwInit()) return 1;
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

  g_win = glfwCreateWindow(1280, 800, "Unthinking Depths", nullptr, nullptr);
  if (!g_win) { glfwTerminate(); return 1; }
  glfwMakeContextCurrent(g_win);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetIO().IniFilename = nullptr;

  ImGui_ImplGlfw_InitForOpenGL(g_win, true);
  ImGui_ImplOpenGL3_Init("#version 300 es");

  emscripten_set_main_loop(main_loop, 0, 1);
  return 0;
}
