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
#include <sstream>
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
// fw = footprint scale along NE-SW (left-right on screen)
// fd = footprint scale along NW-SE (top-bottom on screen)
static void draw_iso_box(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                          float fw, float fd, float h, ImU32 col) {
  ImVec2 tt = {ctr.x,          ctr.y - fd*ch2 - h};
  ImVec2 tr = {ctr.x + fw*cw2, ctr.y - h};
  ImVec2 tb = {ctr.x,          ctr.y + fd*ch2 - h};
  ImVec2 tl = {ctr.x - fw*cw2, ctr.y - h};
  ImVec2 br = {ctr.x + fw*cw2, ctr.y};
  ImVec2 bb = {ctr.x,          ctr.y + fd*ch2};
  ImVec2 bl = {ctr.x - fw*cw2, ctr.y};

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

// ---- Shape model data -----------------------------------------------------

enum class PrimKind : uint8_t { Box, Circle, Sphere };

struct ShapePrim {
  PrimKind kind = PrimKind::Box;
  float px  = 0.0f;   // position: tile units along NE-SW axis
  float py  = 0.0f;   // position: tile units along NW-SE axis
  float pz  = 0.0f;   // position: height above tile in ch2 units
  float fw  = 0.5f;   // [Box] footprint scale along NE-SW
  float fd  = 0.5f;   // [Box] footprint scale along NW-SE
  float h   = 0.5f;   // [Box] height in ch2 units
  float r   = 0.16f;  // [Circle] radius in cw2 units
  float dim = 1.0f;   // colour multiplier
};

struct UnitShapeDef {
  std::vector<ShapePrim> prims;
};

// fields order: kind, px, py, pz, fw, fd, h, r, dim
static UnitShapeDef make_default_shape(game::UnitType t) {
  UnitShapeDef d;
  switch (t) {
  case game::UnitType::Drone:
    d.prims.push_back({PrimKind::Box,    0.f, 0.f, 0.00f, 0.42f, 0.42f, 0.30f, 0.f,   1.0f});
    d.prims.push_back({PrimKind::Circle, 0.f, 0.f, 0.30f, 0.f,   0.f,   0.f,   0.16f, 1.0f});
    break;
  case game::UnitType::Interceptor:
    d.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.0f, 0.44f, 0.44f, 1.05f, 0.f, 1.0f});
    break;
  case game::UnitType::Frigate:
    d.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.0f, 0.76f, 0.76f, 0.80f, 0.f, 1.0f});
    break;
  case game::UnitType::Artillery:
    d.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.00f, 0.46f, 0.46f, 0.75f, 0.f, 1.0f});
    d.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.75f, 0.20f, 0.20f, 1.20f, 0.f, 0.85f});
    break;
  case game::UnitType::Battleship:
    d.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.0f, 0.84f, 0.84f, 1.30f, 0.f, 1.0f});
    break;
  }
  return d;
}

// Rotate (px,py) and swap (fw,fd) for the given facing (0=NE 1=SE 2=SW 3=NW).
static void apply_facing(int facing, float px, float py, float fw, float fd,
                          float& rpx, float& rpy, float& rfw, float& rfd) {
  switch (facing & 3) {
  case 0: rpx =  px; rpy =  py; rfw = fw; rfd = fd; break;
  case 1: rpx =  py; rpy = -px; rfw = fd; rfd = fw; break;
  case 2: rpx = -px; rpy = -py; rfw = fw; rfd = fd; break;
  case 3: rpx = -py; rpy =  px; rfw = fd; rfd = fw; break;
  }
}

static void draw_unit_from_def(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                                const UnitShapeDef& def, ImU32 col, int facing = 0) {
  for (const auto& p : def.prims) {
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, p.px, p.py, p.fw, p.fd, rpx, rpy, rfw, rfd);
    ImVec2 anchor = {
      ctr.x + (rpx - rpy) * cw2,
      ctr.y + (rpx + rpy) * ch2 - p.pz * ch2
    };
    ImU32 c = (p.dim < 0.999f) ? dim_col(col, p.dim) : col;
    if (p.kind == PrimKind::Box) {
      draw_iso_box(dl, anchor, cw2, ch2, rfw, rfd, ch2 * p.h, c);
    } else if (p.kind == PrimKind::Circle) {
      dl->AddCircleFilled(anchor, cw2 * p.r, c);
    } else { // Sphere
      float rad = cw2 * p.r;
      dl->AddCircleFilled(anchor, rad, dim_col(c, 0.55f));
      dl->AddCircleFilled({anchor.x - rad*0.10f, anchor.y - rad*0.14f}, rad * 0.88f, c);
      dl->AddCircleFilled({anchor.x - rad*0.28f, anchor.y - rad*0.34f}, rad * 0.30f,
                           IM_COL32(255, 255, 255, 110));
    }
  }
}

// ---- Shape model I/O ------------------------------------------------------

static bool save_shapes(const UnitShapeDef* defs, const char* path) {
  FILE* f = fopen(path, "w");
  if (!f) return false;
  fprintf(f, "version 1\n");
  for (int i = 0; i < 5; ++i) {
    fprintf(f, "unit %d %s\n", i, game::unit_type_name(static_cast<game::UnitType>(i)));
    for (const auto& p : defs[i].prims) {
      if (p.kind == PrimKind::Box)
        fprintf(f, "box px=%.4f py=%.4f pz=%.4f fw=%.4f fd=%.4f h=%.4f dim=%.4f\n",
                p.px, p.py, p.pz, p.fw, p.fd, p.h, p.dim);
      else if (p.kind == PrimKind::Circle)
        fprintf(f, "circle px=%.4f py=%.4f pz=%.4f r=%.4f dim=%.4f\n",
                p.px, p.py, p.pz, p.r, p.dim);
      else
        fprintf(f, "sphere px=%.4f py=%.4f pz=%.4f r=%.4f dim=%.4f\n",
                p.px, p.py, p.pz, p.r, p.dim);
    }
  }
  fclose(f);
  return true;
}

static bool load_shapes(UnitShapeDef* defs, const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  char line[256];
  int cur = -1;
  while (fgets(line, sizeof(line), f)) {
    int uid; char name[64];
    if (sscanf(line, "unit %d %63s", &uid, name) == 2 && uid >= 0 && uid < 5) {
      cur = uid;
      defs[cur].prims.clear();
      continue;
    }
    if (cur < 0) continue;
    ShapePrim p{};
    if (sscanf(line, "box px=%f py=%f pz=%f fw=%f fd=%f h=%f dim=%f",
               &p.px, &p.py, &p.pz, &p.fw, &p.fd, &p.h, &p.dim) == 7) {
      p.kind = PrimKind::Box;
      defs[cur].prims.push_back(p);
    } else if (sscanf(line, "circle px=%f py=%f pz=%f r=%f dim=%f",
                      &p.px, &p.py, &p.pz, &p.r, &p.dim) == 5) {
      p.kind = PrimKind::Circle;
      defs[cur].prims.push_back(p);
    } else if (sscanf(line, "sphere px=%f py=%f pz=%f r=%f dim=%f",
                      &p.px, &p.py, &p.pz, &p.r, &p.dim) == 5) {
      p.kind = PrimKind::Sphere;
      defs[cur].prims.push_back(p);
    }
  }
  fclose(f);
  return true;
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

// ---- Shape editor UI ------------------------------------------------------

static const char* kFacingLabels[4] = {"↗ NE", "↘ SE", "↙ SW", "↖ NW"};

static void draw_shape_editor(const ImGuiIO& io, UnitShapeDef* defs, int& sel, int& facing,
                               int& sel_prim, char* save_path, std::string& status) {
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##se", nullptr,
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Header bar
  ImGui::Text("Shape Editor");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(280.f);
  ImGui::InputText("##sp", save_path, 512);
  ImGui::SameLine();
  if (ImGui::Button("Save")) {
    status = save_shapes(defs, save_path) ? "Saved." : "Save failed.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Load")) {
    status = load_shapes(defs, save_path) ? "Loaded." : "Load failed.";
  }
  ImGui::SameLine();
  if (ImGui::Button("Defaults")) {
    for (int i = 0; i < 5; ++i)
      defs[i] = make_default_shape(static_cast<game::UnitType>(i));
    status = "Reset to defaults.";
  }
  if (!status.empty()) { ImGui::SameLine(); ImGui::TextColored({0.5f,1.f,0.5f,1.f}, "%s", status.c_str()); }
  ImGui::Separator();

  const float ctrl_w    = 310.f;
  const float content_h = io.DisplaySize.y - ImGui::GetCursorPosY() - 8.f;
  const float preview_w = io.DisplaySize.x - ctrl_w - 8.f;

  // ---- Preview pane --------------------------------------------------------
  ImGui::BeginChild("##se_preview", {preview_w, content_h}, false,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  static const ImU32 kPBg   = IM_COL32(18,  18,  26,  255);
  static const ImU32 kSelBr = IM_COL32(255, 255, 80,  220);
  static const ImU32 kNrmBr = IM_COL32(55,  55,  75,  200);
  static const ImU32 kUCol  = IM_COL32(110, 170, 255, 255);
  static const ImU32 kTileT = IM_COL32(38,  48,  60,  255);
  static const ImU32 kTileS = IM_COL32(25,  33,  42,  255);
  static const ImU32 kTileE = IM_COL32(0,   0,   0,   100);

  // -- Thumbnail strip (all 5 units) --
  const float thumb_h = std::min(content_h * 0.26f, 140.f);
  const float thumb_w = preview_w / 5.f;
  const float th_cs   = std::min(thumb_w * 0.55f, thumb_h * 0.40f);
  const float th_cw2  = th_cs * 0.5f;
  const float th_ch2  = th_cs * 0.25f;

  for (int i = 0; i < 5; ++i) {
    if (i > 0) ImGui::SameLine(0, 0);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushID(100 + i);
    if (ImGui::InvisibleButton("##th", {thumb_w, thumb_h})) sel = i;
    ImGui::PopID();
    dl->AddRectFilled(p0, {p0.x + thumb_w, p0.y + thumb_h}, kPBg);
    dl->AddRect(p0, {p0.x + thumb_w, p0.y + thumb_h},
                i == sel ? kSelBr : kNrmBr, 0, 0, i == sel ? 2.f : 1.f);
    ImVec2 tc = {p0.x + thumb_w * 0.5f, p0.y + thumb_h * 0.60f};
    ImVec2 t_top = {tc.x, tc.y - th_ch2}, t_rt = {tc.x + th_cw2, tc.y};
    ImVec2 t_bot = {tc.x, tc.y + th_ch2}, t_lt = {tc.x - th_cw2, tc.y};
    dl->AddTriangleFilled(t_top, t_rt, t_lt, kTileT);
    dl->AddTriangleFilled(t_rt, t_bot, t_lt, kTileS);
    dl->AddQuad(t_top, t_rt, t_bot, t_lt, kTileE, 0.5f);
    draw_unit_from_def(dl, tc, th_cw2, th_ch2, defs[i], kUCol, facing);
    const char* nm = game::unit_type_name(static_cast<game::UnitType>(i));
    ImVec2 tsz = ImGui::CalcTextSize(nm);
    dl->AddText({p0.x + (thumb_w - tsz.x) * 0.5f, p0.y + thumb_h - tsz.y - 4.f},
                IM_COL32(200, 200, 200, 255), nm);
  }

  // -- Large single-unit preview (drag to move primitives) --
  const float dir_bar_h = 38.f;
  const float big_h = content_h - thumb_h - dir_bar_h - 4.f;

  // Drag state — persists across frames.
  static int    drag_prim      = -1;
  static int    drag_prev_sel  = -1;
  static float  drag_start_px  = 0, drag_start_py = 0, drag_start_pz = 0;
  static ImVec2 drag_origin    = {0, 0};

  // Reset drag/selection when unit changes.
  if (sel != drag_prev_sel) { drag_prim = -1; sel_prim = 0; drag_prev_sel = sel; }

  const float big_cs  = std::min(preview_w * 0.30f, big_h * 0.42f);
  const float big_cw2 = big_cs * 0.5f;
  const float big_ch2 = big_cs * 0.25f;

  ImVec2 big_p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##bigprev", {preview_w, big_h});
  bool big_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  bool big_active  = ImGui::IsItemActive();

  ImVec2 big_ctr = {big_p0.x + preview_w * 0.5f, big_p0.y + big_h * 0.60f};

  // Helper: screen anchor of primitive pi at current facing.
  auto prim_anchor = [&](const ShapePrim& p) -> ImVec2 {
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, p.px, p.py, p.fw, p.fd, rpx, rpy, rfw, rfd);
    return { big_ctr.x + (rpx - rpy) * big_cw2,
             big_ctr.y + (rpx + rpy) * big_ch2 - p.pz * big_ch2 };
  };

  auto& def = defs[sel];

  // On click: hit-test to nearest primitive anchor.
  if (big_clicked) {
    float best = 50.f;
    int   hit  = -1;
    for (int pi = 0; pi < static_cast<int>(def.prims.size()); ++pi) {
      ImVec2 a = prim_anchor(def.prims[pi]);
      float dx = io.MousePos.x - a.x, dy = io.MousePos.y - a.y;
      float d  = std::sqrt(dx*dx + dy*dy);
      if (d < best) { best = d; hit = pi; }
    }
    if (hit >= 0) {
      sel_prim        = hit;
      drag_prim       = hit;
      drag_origin     = io.MousePos;
      drag_start_px   = def.prims[hit].px;
      drag_start_py   = def.prims[hit].py;
      drag_start_pz   = def.prims[hit].pz;
    } else {
      drag_prim = -1;
    }
  }

  // While dragging: update the primitive's position.
  if (big_active && drag_prim >= 0 && drag_prim < static_cast<int>(def.prims.size())) {
    float dx = io.MousePos.x - drag_origin.x;
    float dy = io.MousePos.y - drag_origin.y;
    auto& dp = def.prims[drag_prim];
    if (io.KeyShift) {
      // Vertical drag → pz only.
      dp.pz = std::max(0.f, drag_start_pz - dy / big_ch2);
    } else {
      // Horizontal drag → px/py in tile space.
      // Convert screen delta → rotated-frame tile delta.
      float dtx_rot = ( dx / big_cw2 + dy / big_ch2) * 0.5f;
      float dty_rot = ( dy / big_ch2 - dx / big_cw2) * 0.5f;
      // Apply inverse facing rotation to get model-space delta.
      float dpx_m, dpy_m;
      switch (facing & 3) {
      case 1:  dpx_m = -dty_rot; dpy_m =  dtx_rot; break;
      case 2:  dpx_m = -dtx_rot; dpy_m = -dty_rot; break;
      case 3:  dpx_m =  dty_rot; dpy_m = -dtx_rot; break;
      default: dpx_m =  dtx_rot; dpy_m =  dty_rot; break;
      }
      dp.px = std::clamp(drag_start_px + dpx_m, -2.f, 2.f);
      dp.py = std::clamp(drag_start_py + dpy_m, -2.f, 2.f);
    }
  }
  if (!big_active) drag_prim = -1;

  // Draw background and tile.
  dl->AddRectFilled(big_p0, {big_p0.x + preview_w, big_p0.y + big_h}, kPBg);
  ImVec2 b_tt = {big_ctr.x,            big_ctr.y - big_ch2};
  ImVec2 b_tr = {big_ctr.x + big_cw2,  big_ctr.y};
  ImVec2 b_tb = {big_ctr.x,            big_ctr.y + big_ch2};
  ImVec2 b_tl = {big_ctr.x - big_cw2,  big_ctr.y};
  dl->AddTriangleFilled(b_tt, b_tr, b_tl, kTileT);
  dl->AddTriangleFilled(b_tr, b_tb, b_tl, kTileS);
  dl->AddQuad(b_tt, b_tr, b_tb, b_tl, kTileE, 0.8f);

  // Draw axis cross-hairs at origin so users can orient px/py.
  dl->AddLine({big_ctr.x - big_cw2, big_ctr.y}, {big_ctr.x + big_cw2, big_ctr.y},
              IM_COL32(60, 60, 80, 120), 0.5f);
  dl->AddLine({big_ctr.x, big_ctr.y - big_ch2}, {big_ctr.x, big_ctr.y + big_ch2},
              IM_COL32(60, 60, 80, 120), 0.5f);

  draw_unit_from_def(dl, big_ctr, big_cw2, big_ch2, def, kUCol, facing);

  // Draw selection highlight over the selected primitive.
  if (sel_prim >= 0 && sel_prim < static_cast<int>(def.prims.size())) {
    const auto& sp = def.prims[sel_prim];
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, sp.px, sp.py, sp.fw, sp.fd, rpx, rpy, rfw, rfd);
    ImVec2 anch = { big_ctr.x + (rpx - rpy) * big_cw2,
                    big_ctr.y + (rpx + rpy) * big_ch2 - sp.pz * big_ch2 };
    static const ImU32 kHiCol = IM_COL32(255, 220, 60, 200);
    if (sp.kind == PrimKind::Box) {
      float h_px = big_ch2 * sp.h;
      ImVec2 s_tt = {anch.x,              anch.y - rfd*big_ch2 - h_px};
      ImVec2 s_tr = {anch.x + rfw*big_cw2, anch.y - h_px};
      ImVec2 s_tb = {anch.x,              anch.y + rfd*big_ch2 - h_px};
      ImVec2 s_tl = {anch.x - rfw*big_cw2, anch.y - h_px};
      ImVec2 s_bb = {anch.x,              anch.y + rfd*big_ch2};
      ImVec2 s_br = {anch.x + rfw*big_cw2, anch.y};
      ImVec2 s_bl = {anch.x - rfw*big_cw2, anch.y};
      dl->AddQuad(s_tt, s_tr, s_tb, s_tl, kHiCol, 1.5f); // top face
      dl->AddLine(s_tl, s_bl, kHiCol, 1.0f);
      dl->AddLine(s_tb, s_bb, kHiCol, 1.0f);
      dl->AddLine(s_tr, s_br, kHiCol, 1.0f);
    } else { // Circle or Sphere
      dl->AddCircle(anch, big_cw2 * sp.r + 3.f, kHiCol, 0, 1.5f);
    }
    // Drag hint label.
    const char* hint = io.KeyShift ? "shift+drag: pz" : "drag: px/py  shift+drag: pz";
    ImVec2 hsz = ImGui::CalcTextSize(hint);
    dl->AddText({big_p0.x + (preview_w - hsz.x) * 0.5f, big_p0.y + 6.f},
                IM_COL32(160, 160, 160, 180), hint);
  }

  // -- Direction buttons --
  ImVec2 dir_bar_pos = {big_p0.x, big_p0.y + big_h + 2.f};
  ImGui::SetCursorScreenPos({dir_bar_pos.x + (preview_w - 4 * 78.f) * 0.5f, dir_bar_pos.y});
  for (int d = 0; d < 4; ++d) {
    if (d > 0) ImGui::SameLine(0, 4);
    bool active = (facing == d);
    if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (ImGui::Button(kFacingLabels[d], {76.f, 30.f})) facing = d;
    if (active) ImGui::PopStyleColor();
  }

  ImGui::EndChild();
  ImGui::SameLine();

  // ---- Controls pane -------------------------------------------------------
  ImGui::BeginChild("##se_ctrl", {ctrl_w, content_h}, false);

  ImGui::Text("Editing: %s  |  Facing: %s",
              game::unit_type_name(static_cast<game::UnitType>(sel)), kFacingLabels[facing]);
  ImGui::Separator();

  int remove_idx = -1;

  for (int pi = 0; pi < static_cast<int>(def.prims.size()); ++pi) {
    auto& p = def.prims[pi];
    ImGui::PushID(pi);

    bool is_sel = (pi == sel_prim);
    bool is_box    = (p.kind == PrimKind::Box);
    bool is_sphere = (p.kind == PrimKind::Sphere);
    const char* kind_name = is_box ? "Box" : (is_sphere ? "Sphere" : "Circle");

    // Highlight the selected primitive's section.
    if (is_sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{0.35f, 0.32f, 0.10f, 1.f});
    ImVec4 hdr_col = is_sel ? ImVec4{1.f, 0.88f, 0.25f, 1.f} : ImVec4{0.8f, 0.8f, 0.4f, 1.f};
    ImGui::TextColored(hdr_col, "%s %d", kind_name, pi + 1);
    if (ImGui::IsItemClicked()) { sel_prim = pi; drag_prim = -1; }
    if (is_sel) ImGui::PopStyleColor();
    ImGui::SameLine(ctrl_w - 80.f);
    if (ImGui::SmallButton("Remove") && def.prims.size() > 1)
      remove_idx = pi;

    if (is_box) {
      ImGui::SliderFloat("fw",  &p.fw, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("fd",  &p.fd, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("h",   &p.h,  0.05f, 3.00f, "%.3f");
    } else {
      ImGui::SliderFloat("r",   &p.r,  0.05f, 0.60f, "%.3f");
    }
    ImGui::SliderFloat("px",  &p.px, -1.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("py",  &p.py, -1.0f, 1.0f, "%.3f");
    ImGui::SliderFloat("pz",  &p.pz,  0.0f, 3.0f, "%.3f");
    ImGui::SliderFloat("dim", &p.dim, 0.3f,  1.0f, "%.2f");
    ImGui::Separator();

    ImGui::PopID();
  }

  if (remove_idx >= 0)
    def.prims.erase(def.prims.begin() + remove_idx);

  if (ImGui::Button("+ Box"))
    def.prims.push_back({PrimKind::Box, 0.f, 0.f, 0.f, 0.40f, 0.40f, 0.50f, 0.f, 1.0f});
  ImGui::SameLine();
  if (ImGui::Button("+ Circle"))
    def.prims.push_back({PrimKind::Circle, 0.f, 0.f, 0.5f, 0.f, 0.f, 0.f, 0.16f, 1.0f});
  ImGui::SameLine();
  if (ImGui::Button("+ Sphere"))
    def.prims.push_back({PrimKind::Sphere, 0.f, 0.f, 0.3f, 0.f, 0.f, 0.f, 0.20f, 1.0f});

  ImGui::EndChild();
  ImGui::End();
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
  // Shape model data (used by both game view and editor)
  UnitShapeDef shape_defs[5];
  // Shape editor
  bool         shape_editor_mode = false;
  int          sel_shape = 0;
  int          sel_prim  = 0;
  int          facing    = 0;
  char         shape_path[512]   = "shapes.udshapes";
  std::string  shape_status;
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
    if (g.has_sel && g.sel_type == SelType::Structure)
      for (const auto& s : fr.structures)
        if (s.id == g.sel_struct_id && s.pos.x == x && s.pos.y == y)
          dl->AddQuad(top, right, bottom, left, IM_COL32(255, 255, 80, 220), 2.0f);
  } else if (cd.is_unit) {
    draw_unit_from_def(dl, ctr, cw2, ch2, g.shape_defs[static_cast<int>(cd.unit_type)], u32(cd.fg));
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
  // Always load shape defs — game renderer and editor both use them.
  if (!load_shapes(g.shape_defs, g.shape_path))
    for (int i = 0; i < 5; ++i)
      g.shape_defs[i] = make_default_shape(static_cast<game::UnitType>(i));

  if (argc >= 2 && std::string_view(argv[1]) == "--shape-editor") {
    g.shape_editor_mode = true;
  } else if (argc >= 2) {
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

    if (g.shape_editor_mode) {
      draw_shape_editor(io, g.shape_defs, g.sel_shape, g.facing, g.sel_prim, g.shape_path, g.shape_status);
    } else {
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
    }

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
