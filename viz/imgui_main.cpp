// Usage: ud_viz_imgui [replay.ud] [--shapes file.udshapes]   (shapes hot-reload on save)
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
#include "shape_model.hpp"
#include "stats.hpp"
#include "wincheck.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
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
  char    glyph{'\0'};
  bool    is_unit{false};
  game::UnitType unit_type{game::UnitType::Drone};
  int     shape{-1}; // index into shape_defs; -1 = nothing to draw
  ImVec4  fg{};
  int32_t hp{-1}, max_hp{-1};
  int     facing{0};
};

// ---- Cell data builder ----------------------------------------------------

static CellData make_cell(const game::Map& map, const game::FrameState& fr,
                           const std::vector<uint8_t>& facings,
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
    // Structures never move; turn them toward the map centre so bases face each other.
    int facing = facing_from_step(map.width / 2 - x, map.height / 2 - y);
    return {bg, g, false, game::UnitType::Drone, shape_index(s.type), fg, s.hp, s.max_hp,
            facing < 0 ? 0 : facing};
  }

  int top = -1;
  for (size_t i = 0; i < fr.units.size(); ++i)
    if (fr.units[i].pos.x == x && fr.units[i].pos.y == y) top = static_cast<int>(i);
  if (top >= 0) {
    const auto& u = fr.units[top];
    ImVec4 fg = u.faction.value == 0 ? kFactionA : kFactionB;
    int facing = top < static_cast<int>(facings.size()) ? facings[top] : 0;
    return {bg, '\0', true, u.type, shape_index(u.type), fg, u.hp, u.max_hp, facing};
  }

  return {bg};
}

// ---- App state ------------------------------------------------------------

enum class SelType : uint8_t { Unit, Structure, Terrain };

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
  bool              has_sel  = false;
  SelType           sel_type = SelType::Unit;
  game::UnitId      sel_id{};
  game::StructureId sel_struct_id{};
  game::Vec2        sel_tile{};
  UnitShapeDef      shape_defs[kShapeCount];
  // unit_facing[f][i]: facing of frames[f].units[i], from its last move.
  std::vector<std::vector<uint8_t>> unit_facing;
  std::string       shapes_path = "shapes.udshapes";
  std::filesystem::file_time_type shapes_mtime{};
  double            shapes_checked_t = 0.0;
} g;

static void load_shape_defs() {
  if (!load_shapes(g.shape_defs, g.shapes_path.c_str()))
    for (int i = 0; i < kShapeCount; ++i)
      g.shape_defs[i] = make_default_shape(i);
  std::error_code ec;
  g.shapes_mtime = std::filesystem::last_write_time(g.shapes_path, ec);
}

// Polls rather than using inotify so it works the same everywhere; twice a second is
// plenty for "save in the editor, glance at the viewer".
static void poll_shape_reload(double now) {
#ifndef __EMSCRIPTEN__
  if (now - g.shapes_checked_t < 0.5) return;
  g.shapes_checked_t = now;
  std::error_code ec;
  auto mt = std::filesystem::last_write_time(g.shapes_path, ec);
  if (ec || mt == g.shapes_mtime) return;
  load_shape_defs();
  fprintf(stderr, "Reloaded shapes from %s\n", g.shapes_path.c_str());
#else
  (void)now;
#endif
}

// Units face the way they last moved. Before their first move they face the map
// centre, so freshly built units look toward the action rather than all NE.
static void compute_unit_facings() {
  g.unit_facing.assign(g.frames.size(), {});
  std::map<game::UnitId, std::pair<game::Vec2, uint8_t>> last; // pos, facing
  const int32_t cx = g.map.width / 2, cy = g.map.height / 2;
  for (size_t f = 0; f < g.frames.size(); ++f) {
    const auto& units = g.frames[f].units;
    auto& out = g.unit_facing[f];
    out.resize(units.size());
    for (size_t i = 0; i < units.size(); ++i) {
      const auto& u = units[i];
      auto it = last.find(u.id);
      int facing;
      if (it == last.end()) {
        facing = facing_from_step(cx - u.pos.x, cy - u.pos.y);
        if (facing < 0) facing = 0;
      } else {
        facing = facing_from_step(u.pos.x - it->second.first.x, u.pos.y - it->second.first.y);
        if (facing < 0) facing = it->second.second;
      }
      out[i] = static_cast<uint8_t>(facing);
      last[u.id] = {u.pos, static_cast<uint8_t>(facing)};
    }
  }
}

static void try_load(const char* path) {
  g.playing = false;
  g.error.clear();
  try {
    auto log = game::read_replay_file(path);
    if (game::replay(log) != log.expected_hash)
      fprintf(stderr, "warning: hash mismatch — replay may not reproduce original match\n");
    g.map    = map_from_log(log);
    g.frames = game::replay_frames(log);
    compute_unit_facings();
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
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Q))
      glfwSetWindowShouldClose(win, GLFW_TRUE);
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

  if (cd.shape >= 0)
    draw_unit_from_def(dl, ctr, cw2, ch2, g.shape_defs[cd.shape], u32(cd.fg), cd.facing);

  if (cd.glyph) {
    if (g.has_sel && g.sel_type == SelType::Structure)
      for (const auto& s : fr.structures)
        if (s.id == g.sel_struct_id && s.pos.x == x && s.pos.y == y)
          dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 80, 220), 2.0f);
  } else if (cd.is_unit) {
    if (g.has_sel && g.sel_type == SelType::Unit)
      for (const auto& u : fr.units)
        if (u.id == g.sel_id && u.pos.x == x && u.pos.y == y)
          dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 80, 220), 2.0f);
  }

  if (g.has_sel && g.sel_type == SelType::Terrain &&
      g.sel_tile.x == x && g.sel_tile.y == y)
    dl->AddQuad(top, right, bottom, left, IM_COL32(180, 220, 255, 200), 2.0f);

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
                     make_cell(g.map, fr, g.unit_facing[g.cur], g.view, x, y), x, y, fr);

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
          { g.has_sel = true; g.sel_type = SelType::Unit; g.sel_id = u.id; break; }
      }
      if (!g.has_sel) {
        for (const auto& s : fr.structures) {
          if (s.pos.x == tx && s.pos.y == ty)
            { g.has_sel = true; g.sel_type = SelType::Structure; g.sel_struct_id = s.id; break; }
        }
      }
      if (!g.has_sel) {
        g.has_sel = true; g.sel_type = SelType::Terrain; g.sel_tile = {tx, ty};
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

  if (g.sel_type == SelType::Unit) {
    const game::UnitFrame* sel = nullptr;
    for (const auto& u : fr.units)
      if (u.id == g.sel_id) { sel = &u; break; }

    if (!sel) {
      g.has_sel = false;
    } else {
      ImVec4 fc = sel->faction.value == 0 ? kFactionA : kFactionB;
      ImGui::TextColored(fc, "%s", game::unit_type_name(sel->type));
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
  } else if (g.sel_type == SelType::Structure) {
    const game::StructureFrame* sel = nullptr;
    for (const auto& s : fr.structures)
      if (s.id == g.sel_struct_id) { sel = &s; break; }

    if (!sel) {
      g.has_sel = false;
    } else {
      ImVec4 fc = sel->faction.value == 0 ? kFactionA : kFactionB;
      ImGui::TextColored(fc, "%s", game::structure_type_name(sel->type));
      ImGui::SameLine();
      ImGui::TextColored(fc, "[%s]", sel->faction.value == 0 ? "A" : "B");
      ImGui::Separator();

      float ratio = sel->max_hp > 0 ? (float)sel->hp / (float)sel->max_hp : 0.0f;
      ImGui::Text("HP   ");
      ImGui::SameLine();
      ImGui::TextColored(hp_color(ratio), "%d / %d", sel->hp, sel->max_hp);
      ImGui::Text("Pos  %d, %d", sel->pos.x, sel->pos.y);
    }
  } else {
    const game::Tile& tile = g.map.tile_at(g.sel_tile);
    ImGui::Text("%s", game::terrain_type_name(tile.terrain));
    ImGui::Separator();
    ImGui::Text("Pos       %d, %d", g.sel_tile.x, g.sel_tile.y);
    ImGui::Text("Passable  %s", game::is_passable(tile.terrain) ? "yes" : "no");
    if (tile.terrain == game::Terrain::ResourceNode)
      ImGui::Text("Resources  %d", tile.resource_amount);
  }

  ImGui::Separator();
  if (ImGui::SmallButton("x")) g.has_sel = false;

  ImGui::End();
}

// ---- Main -----------------------------------------------------------------

int main(int argc, char** argv) {
  const char* replay_arg = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--shapes") == 0 && i + 1 < argc)
      g.shapes_path = argv[++i];
    else if (!replay_arg)
      replay_arg = argv[i];
  }
  load_shape_defs();

  if (replay_arg) {
    strncpy(g.path_buf, replay_arg, sizeof(g.path_buf) - 1);
    try_load(replay_arg);
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
    poll_shape_reload(glfwGetTime());

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
