// Controls: Space=play/pause  ←/→=step  U=union  G=god  0/1=faction view  scroll=zoom  Q=quit

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "entity.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "stats.hpp"
#include "wincheck.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- Native file dialog (Linux: zenity → kdialog → nothing) ---------------

static std::string pick_file() {
  const char* cmds[] = {
    "zenity --file-selection --title='Open replay' --file-filter='UD replay | *.ud' 2>/dev/null",
    "kdialog --getopenfilename . '*.ud' 2>/dev/null",
  };
  for (const char* cmd : cmds) {
    FILE* f = popen(cmd, "r");
    if (!f) continue;
    char buf[1024] = {};
    bool got = (fgets(buf, sizeof(buf), f) != nullptr);
    pclose(f);
    if (!got || buf[0] == '\0') continue;
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] == '\n') buf[n - 1] = '\0';
    return buf;
  }
  return {};
}

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
static constexpr ImVec4 kResource = {0.15f, 0.32f, 0.15f, 1.0f};
static constexpr ImVec4 kFactionA = {0.40f, 0.70f, 1.00f, 1.0f};  // blue
static constexpr ImVec4 kFactionB = {1.00f, 0.42f, 0.32f, 1.0f};  // red

static ImU32  u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImVec4 dim_vec(ImVec4 c, float f) { return {c.x*f, c.y*f, c.z*f, c.w}; }
static ImVec4 hp_color(float ratio) {
  return ratio > 0.5f
      ? ImVec4{0.2f + (1.0f - ratio) * 1.6f, 0.85f, 0.2f, 1.0f}
      : ImVec4{1.0f, ratio * 1.7f, 0.1f, 1.0f};
}

// ---- Per-cell rendering data ----------------------------------------------

struct CellData {
  ImVec4  bg;
  char    glyph{'\0'};               // non-null → structure text label
  bool    is_unit{false};
  game::UnitType unit_type{game::UnitType::Drone};
  ImVec4  fg{};
  int32_t hp{-1}, max_hp{-1};
};

// ---- Unit shape drawing ---------------------------------------------------

static ImU32 dim_col(ImU32 col, float f) {
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
  return ImGui::ColorConvertFloat4ToU32({c.x*f, c.y*f, c.z*f, c.w});
}

// Draws one isometric box.  fs = footprint scale [0..1], h = pixel height above tile.
static void draw_iso_box(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                          float fs, float h, ImU32 col) {
  const float s = fs;
  ImVec2 tt = {ctr.x,         ctr.y - s*ch2 - h};
  ImVec2 tr = {ctr.x + s*cw2, ctr.y - h};
  ImVec2 tb = {ctr.x,         ctr.y + s*ch2 - h};
  ImVec2 tl = {ctr.x - s*cw2, ctr.y - h};
  ImVec2 br = {ctr.x + s*cw2, ctr.y};
  ImVec2 bb = {ctr.x,         ctr.y + s*ch2};
  ImVec2 bl = {ctr.x - s*cw2, ctr.y};

  ImU32 top_c   = col;
  ImU32 left_c  = dim_col(col, 0.58f);
  ImU32 right_c = dim_col(col, 0.38f);
  ImU32 edge    = IM_COL32(0, 0, 0, 110);
  const float lw = 0.8f;

  dl->AddTriangleFilled(tt, tr, tl, top_c);
  dl->AddTriangleFilled(tr, tb, tl, top_c);
  ImVec2 lf[] = {tl, tb, bb, bl};
  dl->AddConvexPolyFilled(lf, 4, left_c);
  ImVec2 rf[] = {tb, tr, br, bb};
  dl->AddConvexPolyFilled(rf, 4, right_c);
  dl->AddQuad(tt, tr, tb, tl, edge, lw);
  dl->AddLine(tl, bl, edge, lw);
  dl->AddLine(tb, bb, edge, lw);
  dl->AddLine(tr, br, edge, lw);
  dl->AddLine(bl, bb, edge, lw);
  dl->AddLine(bb, br, edge, lw);
}

static void draw_unit(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                       game::UnitType type, ImU32 col) {
  switch (type) {
  case game::UnitType::Drone:
    draw_iso_box(dl, ctr, cw2, ch2, 0.42f, ch2 * 0.30f, col);
    dl->AddCircleFilled({ctr.x, ctr.y - ch2 * 0.30f}, cw2 * 0.16f, col);
    break;
  case game::UnitType::Interceptor:
    draw_iso_box(dl, ctr, cw2, ch2, 0.44f, ch2 * 1.05f, col);
    break;
  case game::UnitType::Frigate:
    draw_iso_box(dl, ctr, cw2, ch2, 0.76f, ch2 * 0.80f, col);
    break;
  case game::UnitType::Artillery:
    draw_iso_box(dl, ctr, cw2, ch2, 0.46f, ch2 * 0.75f, col);
    draw_iso_box(dl, {ctr.x, ctr.y - ch2 * 0.75f}, cw2, ch2,
                 0.20f, ch2 * 1.20f, dim_col(col, 0.85f));
    break;
  case game::UnitType::Battleship:
    draw_iso_box(dl, ctr, cw2, ch2, 0.84f, ch2 * 1.30f, col);
    break;
  }
}

// ---- Cell data builder ----------------------------------------------------

static CellData make_cell(const game::Map& map, const game::FrameState& fr,
                           int view, int32_t x, int32_t y) {
  bool vis = (view == -1);
  if (!vis) {
    for (const auto& u : fr.units) {
      if (view >= 0 && static_cast<int>(u.faction.value) != view) continue;
      if (std::abs(x - u.pos.x) + std::abs(y - u.pos.y) <= game::stats_for(u.type).sight)
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
             s.type == game::StructureType::Factory     ? 'T' : 'N';
    ImVec4 fg = s.faction.value == 0 ? kFactionA : kFactionB;
    return {bg, g, false, game::UnitType::Drone, fg, s.hp, s.max_hp};
  }

  const game::UnitFrame* top = nullptr;
  for (const auto& u : fr.units)
    if (u.pos.x == x && u.pos.y == y) top = &u;
  if (top) {
    ImVec4 fg = top->faction.value == 0 ? kFactionA : kFactionB;
    return {bg, '\0', true, top->type, fg, top->hp, top->max_hp};
  }

  return {bg};
}

// ---- App state ------------------------------------------------------------

struct App {
  bool   loaded  = false;
  std::string error;
  game::Map map;
  std::vector<game::FrameState> frames;
  int    cur     = 0;
  int    total   = 0;
  int    view    = -2;
  bool   playing = false;
  float  fps     = 15.0f;
  float  cs      = 48.0f;
  double last_t  = 0.0;
  char   path_buf[512] = {};
  bool              has_sel     = false;
  bool              sel_is_unit = true;
  game::UnitId      sel_id{};
  game::StructureId sel_struct_id{};
} g;

static void try_load(const char* path) {
  g.playing = false;
  g.error.clear();
  try {
    auto log = game::read_replay_file(path);
    if (game::replay(log) != log.expected_hash)
      fprintf(stderr, "warning: hash mismatch — replay may not reproduce original match\n");
    g.map    = map_from_log(log);
    g.frames = game::replay_frames(log);
    g.cur    = 0;
    g.total  = static_cast<int>(g.frames.size());
    g.loaded = true;
    fprintf(stderr, "Loaded: %zu ticks on %dx%d map\n",
            log.ticks.size(), log.map_w, log.map_h);
  } catch (const std::exception& e) {
    g.loaded = false;
    g.error  = e.what();
    fprintf(stderr, "error: %s\n", e.what());
  }
}

// ---- Per-frame draw functions ---------------------------------------------

static void handle_input(const ImGuiIO& io, GLFWwindow* win) {
  if (!io.WantCaptureMouse)
    g.cs = std::clamp(g.cs + io.MouseWheel * 2.0f, 8.0f, 64.0f);

  if (!io.WantCaptureKeyboard) {
    if (g.loaded) {
      if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
        g.playing = !g.playing;
        g.last_t  = glfwGetTime();
      }
      if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) && g.cur + 1 < g.total)
        { ++g.cur; g.playing = false; }
      if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) && g.cur > 0)
        { --g.cur; g.playing = false; }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_U)) g.view = -2;
    if (ImGui::IsKeyPressed(ImGuiKey_G)) g.view = -1;
    if (ImGui::IsKeyPressed(ImGuiKey_0)) g.view = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_1)) g.view = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_Q)) glfwSetWindowShouldClose(win, GLFW_TRUE);
  }
}

static void draw_file_bar() {
  if (ImGui::Button("Open")) {
    std::string picked = pick_file();
    if (!picked.empty()) {
      strncpy(g.path_buf, picked.c_str(), sizeof(g.path_buf) - 1);
      try_load(g.path_buf);
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(360.0f);
  if (ImGui::InputText("##path", g.path_buf, sizeof(g.path_buf),
                       ImGuiInputTextFlags_EnterReturnsTrue))
    try_load(g.path_buf);
  if (!g.error.empty()) {
    ImGui::SameLine();
    ImGui::TextColored({1.0f, 0.4f, 0.4f, 1.0f}, "%s", g.error.c_str());
  }
}

static void draw_playback_bar(const game::FrameState& fr) {
  ImGui::SameLine();
  ImGui::Text("|");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260.0f);
  char tick_fmt[32];
  snprintf(tick_fmt, sizeof(tick_fmt), "%%d / %d", g.total - 1);
  if (ImGui::SliderInt("##t", &g.cur, 0, g.total - 1, tick_fmt)) g.playing = false;
  ImGui::SameLine();
  if (ImGui::Button(g.playing ? "Pause" : " Play "))
    { g.playing = !g.playing; g.last_t = glfwGetTime(); }
  ImGui::SameLine();
  if (ImGui::ArrowButton("##l", ImGuiDir_Left) && g.cur > 0)
    { --g.cur; g.playing = false; }
  ImGui::SameLine(0, 2);
  if (ImGui::ArrowButton("##r", ImGuiDir_Right) && g.cur + 1 < g.total)
    { ++g.cur; g.playing = false; }
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
      ImVec4 wc = res.winner.value == 0 ? kFactionA : kFactionB;
      const char* why =
          res.reason == game::WinReason::BaseDestroyed      ? "base" :
          res.reason == game::WinReason::TerritoryThreshold ? "territory" : "tick cap";
      ImGui::TextColored(wc, " | %s wins (%s)",
          res.winner.value == 0 ? "A" : "B", why);
    }
  }
}

static void draw_tile_cell(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                            const CellData& cd, int32_t x, int32_t y,
                            const game::FrameState& fr) {
  ImVec2 top    = {ctr.x,        ctr.y - ch2};
  ImVec2 right  = {ctr.x + cw2,  ctr.y      };
  ImVec2 bottom = {ctr.x,        ctr.y + ch2};
  ImVec2 left   = {ctr.x - cw2,  ctr.y      };

  dl->AddTriangleFilled(top, right, left, u32(cd.bg));
  dl->AddTriangleFilled(right, bottom, left, u32(dim_vec(cd.bg, 0.70f)));
  dl->AddQuad(top, right, bottom, left, u32({0.0f, 0.0f, 0.0f, 0.40f}), 0.5f);

  if (cd.glyph) {
    char   buf[2] = {cd.glyph, '\0'};
    ImVec2 tsz    = ImGui::CalcTextSize(buf);
    dl->AddText({ctr.x - tsz.x * 0.5f, ctr.y - tsz.y * 0.5f}, u32(cd.fg), buf);
    if (g.has_sel && !g.sel_is_unit)
      for (const auto& s : fr.structures)
        if (s.id == g.sel_struct_id && s.pos.x == x && s.pos.y == y)
          dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 80, 220), 2.0f);
  } else if (cd.is_unit) {
    draw_unit(dl, ctr, cw2, ch2, cd.unit_type, u32(cd.fg));
    if (g.has_sel && g.sel_is_unit)
      for (const auto& u : fr.units)
        if (u.id == g.sel_id && u.pos.x == x && u.pos.y == y)
          dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 80, 220), 2.0f);
  }

  if ((cd.glyph || cd.is_unit) && cd.hp >= 0 && cd.max_hp > 0 && g.cs >= 32.0f) {
    float  ratio  = static_cast<float>(cd.hp) / static_cast<float>(cd.max_hp);
    ImVec2 hp_end = {right.x + ratio * (bottom.x - right.x),
                     right.y + ratio * (bottom.y - right.y)};
    float  bw     = std::max(2.0f, ch2 * 0.3f);
    dl->AddLine(right, bottom, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
    dl->AddLine(right, hp_end, u32(hp_color(ratio)), bw);
  }
}

static void draw_grid(const game::FrameState& fr) {
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

  for (int32_t y = 0; y < g.map.height; ++y)
    for (int32_t x = 0; x < g.map.width; ++x)
      draw_tile_cell(dl, tile_ctr(x, y), cw2, ch2,
                     make_cell(g.map, fr, g.view, x, y), x, y, fr);

  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    ImVec2  mp  = ImGui::GetMousePos();
    float   fdx = (mp.x - iso_org.x) / cw2;
    float   fdy = (mp.y - iso_org.y) / ch2;
    int32_t tx  = static_cast<int32_t>(std::round((fdx + fdy) * 0.5f));
    int32_t ty  = static_cast<int32_t>(std::round((fdy - fdx) * 0.5f));
    if (tx >= 0 && tx < g.map.width && ty >= 0 && ty < g.map.height) {
      g.has_sel = false;
      for (const auto& u : fr.units) {
        if (u.pos.x == tx && u.pos.y == ty)
          { g.has_sel = true; g.sel_is_unit = true; g.sel_id = u.id; break; }
      }
      if (!g.has_sel) {
        for (const auto& s : fr.structures) {
          if (s.pos.x == tx && s.pos.y == ty)
            { g.has_sel = true; g.sel_is_unit = false; g.sel_struct_id = s.id; break; }
        }
      }
    }
  }

  ImGui::Dummy({(float)(g.map.width + g.map.height) * cw2 + cw2,
                (float)(g.map.width + g.map.height) * ch2 + ch2});
  ImGui::EndChild();
}

static void draw_sel_panel(const ImGuiIO& io) {
  if (!g.loaded || !g.has_sel) return;

  const auto& fr = g.frames[static_cast<size_t>(g.cur)];

  ImGui::SetNextWindowPos(
      {io.DisplaySize.x - 10.0f, 55.0f}, ImGuiCond_Always, {1.0f, 0.0f});
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGui::Begin("##sel", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);

  if (g.sel_is_unit) {
    const game::UnitFrame* sel = nullptr;
    for (const auto& u : fr.units)
      if (u.id == g.sel_id) { sel = &u; break; }

    if (!sel) {
      g.has_sel = false;
    } else {
      ImVec4 fc = sel->faction.value == 0 ? kFactionA : kFactionB;
      ImGui::TextColored(fc, "%s", unit_type_name(sel->type));
      ImGui::SameLine();
      ImGui::TextColored(fc, "[%s]", sel->faction.value == 0 ? "A" : "B");
      ImGui::Separator();

      float ratio = sel->max_hp > 0 ? (float)sel->hp / (float)sel->max_hp : 0.0f;
      ImGui::Text("HP   ");
      ImGui::SameLine();
      ImGui::TextColored(hp_color(ratio), "%d / %d", sel->hp, sel->max_hp);
      ImGui::Text("Pos  %d, %d", sel->pos.x, sel->pos.y);
      ImGui::Separator();

      const game::UnitStats& st = game::stats_for(sel->type);
      ImGui::Text("Damage  %d", st.dmg);
      ImGui::Text("Range   %d", st.range);
      ImGui::Text("Sight   %d", st.sight);
      ImGui::Text("Speed   %d", st.speed);
      if (st.first_strike)
        ImGui::TextColored({0.9f, 0.85f, 0.3f, 1.0f}, "* First Strike");
      if (st.splash)
        ImGui::TextColored({0.9f, 0.6f, 0.2f, 1.0f}, "* Splash  r=%d", st.splash_radius);
    }
  } else {
    const game::StructureFrame* sel = nullptr;
    for (const auto& s : fr.structures)
      if (s.id == g.sel_struct_id) { sel = &s; break; }

    if (!sel) {
      g.has_sel = false;
    } else {
      ImVec4 fc = sel->faction.value == 0 ? kFactionA : kFactionB;
      ImGui::TextColored(fc, "%s", structure_type_name(sel->type));
      ImGui::SameLine();
      ImGui::TextColored(fc, "[%s]", sel->faction.value == 0 ? "A" : "B");
      ImGui::Separator();

      float ratio = sel->max_hp > 0 ? (float)sel->hp / (float)sel->max_hp : 0.0f;
      ImGui::Text("HP   ");
      ImGui::SameLine();
      ImGui::TextColored(hp_color(ratio), "%d / %d", sel->hp, sel->max_hp);
      ImGui::Text("Pos  %d, %d", sel->pos.x, sel->pos.y);
    }
  }

  ImGui::Separator();
  if (ImGui::SmallButton("x")) g.has_sel = false;

  ImGui::End();
}

// ---- Main -----------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc >= 2) {
    strncpy(g.path_buf, argv[1], sizeof(g.path_buf) - 1);
    try_load(argv[1]);
  }

  if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

  GLFWwindow* win = glfwCreateWindow(1520, 860, "Unthinking Depths", nullptr, nullptr);
  if (!win) { glfwTerminate(); fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);

  glfwSetDropCallback(win, [](GLFWwindow*, int count, const char** paths) {
    if (count > 0) {
      strncpy(g.path_buf, paths[0], sizeof(g.path_buf) - 1);
      try_load(paths[0]);
    }
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetIO().IniFilename = nullptr;

  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  g.last_t = glfwGetTime();

  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    ImGuiIO& io = ImGui::GetIO();

    if (g.loaded && g.playing) {
      double now = glfwGetTime();
      if (now - g.last_t >= 1.0 / static_cast<double>(g.fps)) {
        if (g.cur + 1 < g.total) ++g.cur; else g.playing = false;
        g.last_t = now;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    handle_input(io, win);

    ImGui::SetNextWindowPos({0, 0});
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##ud", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    draw_file_bar();

    if (g.loaded) {
      const auto& fr = g.frames[static_cast<size_t>(g.cur)];
      draw_playback_bar(fr);
      ImGui::Separator();
      draw_grid(fr);
    } else {
      ImGui::Separator();
      ImGui::Spacing();
      ImGui::SetCursorPosX((io.DisplaySize.x - 380.0f) * 0.5f);
      ImGui::TextDisabled("Enter a path above, or drag and drop a .ud file here.");
    }

    ImGui::End();

    draw_sel_panel(io);

    ImGui::Render();
    int fb_w, fb_h;
    glfwGetFramebufferSize(win, &fb_w, &fb_h);
    glViewport(0, 0, fb_w, fb_h);
    glClearColor(0.08f, 0.08f, 0.10f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    glfwSwapBuffers(win);
  }

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
