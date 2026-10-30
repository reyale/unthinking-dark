#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "entity.hpp"
#include "shape_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// ---- Shape editor UI -------------------------------------------------------

// Plain ASCII: ImGui's default font has no arrow glyphs.
static const char* kFacingLabels[4] = {"NE", "SE", "SW", "NW"};

static constexpr int kUnitTypeCount = 5;

static void draw_shape_editor(const ImGuiIO& io, UnitShapeDef* defs, int& sel, int& facing,
                               int& sel_prim, float& game_cs, char* save_path,
                               std::string& status) {
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::Begin("##se", nullptr,
    ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

  // Header bar
  ImGui::SetNextItemWidth(120.f);
  ImGui::Combo("##unit", &sel, [](void*, int i) {
    return game::unit_type_name(static_cast<game::UnitType>(i));
  }, nullptr, kUnitTypeCount);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(280.f);
  ImGui::InputText("##sp", save_path, 512);
  ImGui::SameLine();
  if (ImGui::Button("Save"))
    status = save_shapes(defs, save_path) ? "Saved." : "Save failed.";
  ImGui::SameLine();
  if (ImGui::Button("Load"))
    status = load_shapes(defs, save_path) ? "Loaded." : "Load failed.";
  ImGui::SameLine();
  if (ImGui::Button("Defaults")) {
    for (int i = 0; i < kUnitTypeCount; ++i)
      defs[i] = make_default_shape(static_cast<game::UnitType>(i));
    status = "Reset to defaults.";
  }
  if (!status.empty()) {
    ImGui::SameLine();
    ImGui::TextColored({0.5f, 1.f, 0.5f, 1.f}, "%s", status.c_str());
  }
  ImGui::Separator();

  const float ctrl_w    = 310.f;
  const float content_h = io.DisplaySize.y - ImGui::GetCursorPosY() - 8.f;
  const float preview_w = io.DisplaySize.x - ctrl_w - 8.f;

  // ---- Preview pane --------------------------------------------------------
  ImGui::BeginChild("##se_preview", {preview_w, content_h}, false,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();

  static const ImU32 kPBg   = IM_COL32(18,  18,  26,  255);
  const ImU32 kUCol = ImGui::ColorConvertFloat4ToU32(kFactionA);
  static const ImU32 kTileT = IM_COL32(38,  48,  60,  255);
  static const ImU32 kTileS = IM_COL32(25,  33,  42,  255);
  static const ImU32 kTileE = IM_COL32(0,   0,   0,   100);

  // -- Single-unit preview (drag to move primitives) --
  const float dir_bar_h  = 110.f;
  const float game_bar_h = std::max(90.f, game_cs * 2.2f);
  const float big_h = content_h - dir_bar_h - game_bar_h - 8.f;

  enum class DragMode : uint8_t {
    Move, ResizeWidth, ResizeDepth, ResizeHeight, ResizeRadius, ResizeDiameter
  };

  static int      drag_prim     = -1;
  static int      drag_prev_sel = -1;
  static DragMode drag_mode     = DragMode::Move;
  static float    drag_start_px = 0, drag_start_py = 0, drag_start_pz = 0;
  static float    drag_start_fw = 0, drag_start_fd = 0;
  static float    drag_start_h  = 0, drag_start_r  = 0;
  static ImVec2   drag_origin   = {0, 0};

  if (sel != drag_prev_sel) { drag_prim = -1; sel_prim = 0; drag_prev_sel = sel; }

  const float big_cs  = std::min(preview_w * 0.30f, big_h * 0.42f);
  const float big_cw2 = big_cs * 0.5f;
  const float big_ch2 = big_cs * 0.25f;

  ImVec2 big_p0 = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("##bigprev", {preview_w, big_h});
  bool big_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
  bool big_active  = ImGui::IsItemActive();

  ImVec2 big_ctr = {big_p0.x + preview_w * 0.5f, big_p0.y + big_h * 0.60f};

  auto& def = defs[sel];

  // Handle positions for the selected primitive, shared by hit-testing and drawing.
  struct Handle { ImVec2 pos; DragMode mode; ImU32 col; };
  static const ImU32 kHwCol = IM_COL32(0,   210, 210, 230); // width/radius
  static const ImU32 kHdCol = IM_COL32(80,  210, 80,  230); // depth/length
  static const ImU32 kHhCol = IM_COL32(240, 100, 240, 230); // height
  auto handles_for = [&](const ShapePrim& sp) {
    std::vector<Handle> hs;
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, sp.px, sp.py, sp.fw, sp.fd, rpx, rpy, rfw, rfd);
    ImVec2 anch = { big_ctr.x + (rpx - rpy) * big_cw2,
                    big_ctr.y + (rpx + rpy) * big_ch2 - sp.pz * big_ch2 };
    float h_px = big_ch2 * sp.h;
    switch (sp.kind) {
    case PrimKind::Box:
      hs.push_back({{anch.x + rfw*big_cw2, anch.y - h_px},               DragMode::ResizeWidth,  kHwCol});
      hs.push_back({{anch.x,               anch.y + rfd*big_ch2 - h_px}, DragMode::ResizeDepth,  kHdCol});
      hs.push_back({{anch.x,               anch.y - rfd*big_ch2 - h_px}, DragMode::ResizeHeight, kHhCol});
      break;
    case PrimKind::Cylinder:
      hs.push_back({{anch.x + sp.fw*big_cw2, anch.y - h_px}, DragMode::ResizeDiameter, kHwCol});
      hs.push_back({{anch.x,                 anch.y - h_px}, DragMode::ResizeHeight,   kHhCol});
      break;
    case PrimKind::Wedge:
      hs.push_back({{anch.x, anch.y - h_px}, DragMode::ResizeHeight, kHhCol});
      break;
    case PrimKind::Circle:
    case PrimKind::Sphere:
      hs.push_back({{anch.x + big_cw2 * sp.r, anch.y}, DragMode::ResizeRadius, kHwCol});
      break;
    }
    return hs;
  };

  auto begin_drag = [&](int pi, DragMode mode) {
    const auto& sp = def.prims[pi];
    drag_prim     = pi;           drag_mode     = mode;
    drag_origin   = io.MousePos;
    drag_start_px = sp.px;        drag_start_py = sp.py;   drag_start_pz = sp.pz;
    drag_start_fw = sp.fw;        drag_start_fd = sp.fd;
    drag_start_h  = sp.h;         drag_start_r  = sp.r;
  };

  if (big_clicked) {
    bool hit_handle = false;
    if (sel_prim >= 0 && sel_prim < static_cast<int>(def.prims.size())) {
      for (const auto& h : handles_for(def.prims[sel_prim])) {
        float ddx = io.MousePos.x - h.pos.x, ddy = io.MousePos.y - h.pos.y;
        if (ddx*ddx + ddy*ddy < 12.f * 12.f) {
          begin_drag(sel_prim, h.mode);
          hit_handle = true;
          break;
        }
      }
    }
    if (!hit_handle) {
      int hit = pick_prim(def, big_ctr, big_cw2, big_ch2, facing, io.MousePos);
      sel_prim = hit;
      if (hit >= 0) begin_drag(hit, DragMode::Move);
      else          drag_prim = -1;
    }
  }

  if (big_active && drag_prim >= 0 && drag_prim < static_cast<int>(def.prims.size())) {
    float dx = io.MousePos.x - drag_origin.x;
    float dy = io.MousePos.y - drag_origin.y;
    auto& dp = def.prims[drag_prim];

    if (drag_mode == DragMode::ResizeWidth) {
      // Right vertex of top face moves +dx when rfw increases.
      // rfw = fw at facing 0/2, rfw = fd at facing 1/3.
      float delta = dx / big_cw2;
      if ((facing & 1) == 0) dp.fw = std::clamp(drag_start_fw + delta, 0.05f, 1.20f);
      else                   dp.fd = std::clamp(drag_start_fd + delta, 0.05f, 1.20f);
    } else if (drag_mode == DragMode::ResizeDepth) {
      // Front vertex of top face moves +dy when rfd increases.
      // rfd = fd at facing 0/2, rfd = fw at facing 1/3.
      float delta = dy / big_ch2;
      if ((facing & 1) == 0) dp.fd = std::clamp(drag_start_fd + delta, 0.05f, 1.20f);
      else                   dp.fw = std::clamp(drag_start_fw + delta, 0.05f, 1.20f);
    } else if (drag_mode == DragMode::ResizeHeight) {
      dp.h = std::clamp(drag_start_h - dy / big_ch2, 0.05f, 3.00f);
    } else if (drag_mode == DragMode::ResizeDiameter) {
      dp.fw = std::clamp(drag_start_fw + dx / big_cw2, 0.05f, 1.20f);
    } else if (drag_mode == DragMode::ResizeRadius) {
      dp.r = std::clamp(drag_start_r + dx / big_cw2, 0.05f, 0.60f);
    } else { // Move
      if (io.KeyShift) {
        dp.pz = std::max(0.f, drag_start_pz - dy / big_ch2);
      } else {
        // Screen delta -> rotated tile delta -> unrotated model delta.
        ImVec2 rot = {( dx / big_cw2 + dy / big_ch2) * 0.5f,
                      ( dy / big_ch2 - dx / big_cw2) * 0.5f};
        ImVec2 m = rotate_facing((4 - facing) & 3, rot);
        dp.px = std::clamp(drag_start_px + m.x, -2.f, 2.f);
        dp.py = dp.sym ? dp.px : std::clamp(drag_start_py + m.y, -2.f, 2.f);
      }
    }
  }
  if (!big_active) drag_prim = -1;

  dl->AddRectFilled(big_p0, {big_p0.x + preview_w, big_p0.y + big_h}, kPBg);

  ImVec2 b_tt = {big_ctr.x,           big_ctr.y - big_ch2};
  ImVec2 b_tr = {big_ctr.x + big_cw2, big_ctr.y};
  ImVec2 b_tb = {big_ctr.x,           big_ctr.y + big_ch2};
  ImVec2 b_tl = {big_ctr.x - big_cw2, big_ctr.y};
  dl->AddTriangleFilled(b_tt, b_tr, b_tl, kTileT);
  dl->AddTriangleFilled(b_tr, b_tb, b_tl, kTileS);
  dl->AddQuad(b_tt, b_tr, b_tb, b_tl, kTileE, 0.8f);
  dl->AddLine({big_ctr.x - big_cw2, big_ctr.y}, {big_ctr.x + big_cw2, big_ctr.y},
              IM_COL32(60, 60, 80, 120), 0.5f);
  dl->AddLine({big_ctr.x, big_ctr.y - big_ch2}, {big_ctr.x, big_ctr.y + big_ch2},
              IM_COL32(60, 60, 80, 120), 0.5f);

  draw_unit_from_def(dl, big_ctr, big_cw2, big_ch2, def, kUCol, facing);

  if (sel_prim >= 0 && sel_prim < static_cast<int>(def.prims.size())) {
    static const ImU32 kHiCol = IM_COL32(255, 220, 60, 200);
    const auto& sp = def.prims[sel_prim];
    draw_prim_outline(dl, build_prim_geom(sp, big_ctr, big_cw2, big_ch2, facing), kHiCol, 1.5f);
    for (const auto& h : handles_for(sp)) {
      ImVec2 p = h.pos;
      dl->AddRectFilled({p.x-5.f, p.y-5.f}, {p.x+5.f, p.y+5.f}, h.col);
      dl->AddRect({p.x-5.f, p.y-5.f}, {p.x+5.f, p.y+5.f}, IM_COL32(0,0,0,200), 0, 0, 1.0f);
    }
  }
  {
    const char* hint = "click: select  drag: move  shift+drag: pz  "
                       "[cyan]=width  [green]=depth  [purple]=height";
    ImVec2 hsz = ImGui::CalcTextSize(hint);
    dl->AddText({big_p0.x + (preview_w - hsz.x) * 0.5f, big_p0.y + 6.f},
                IM_COL32(160, 160, 160, 180), hint);
  }

  // -- Game-scale strip: every unit, both factions, at the viewer's tile size --
  {
    const float g_cw2 = game_cs * 0.5f, g_ch2 = game_cs * 0.25f;
    const float slot_w = preview_w / kUnitTypeCount;
    ImVec2 bar0 = {big_p0.x, big_p0.y + big_h + 2.f};
    const ImU32 fcol[2] = {ImGui::ColorConvertFloat4ToU32(kFactionA),
                           ImGui::ColorConvertFloat4ToU32(kFactionB)};
    for (int u = 0; u < kUnitTypeCount; ++u) {
      ImVec2 sp0 = {bar0.x + u * slot_w, bar0.y};
      ImVec2 sp1 = {sp0.x + slot_w - 2.f, sp0.y + game_bar_h - 2.f};
      ImGui::SetCursorScreenPos(sp0);
      ImGui::PushID(u + 200);
      if (ImGui::InvisibleButton("##gs", {slot_w - 2.f, game_bar_h - 2.f})) sel = u;
      ImGui::PopID();
      bool active = (sel == u);
      dl->AddRectFilled(sp0, sp1, IM_COL32(14, 14, 20, 255));
      dl->AddRect(sp0, sp1, active ? IM_COL32(255,220,60,200) : IM_COL32(60,60,80,120),
                  0, 0, active ? 1.5f : 0.8f);
      // Two diagonal neighbours, the way units stand next to each other in a match.
      for (int fct = 0; fct < 2; ++fct) {
        ImVec2 c = {sp0.x + (slot_w - 2.f) * 0.5f + (fct == 0 ? -g_cw2 : g_cw2),
                    sp0.y + game_bar_h * 0.62f};
        ImVec2 t_top = {c.x, c.y - g_ch2}, t_rt = {c.x + g_cw2, c.y};
        ImVec2 t_bot = {c.x, c.y + g_ch2}, t_lt = {c.x - g_cw2, c.y};
        dl->AddTriangleFilled(t_top, t_rt, t_lt, kTileT);
        dl->AddTriangleFilled(t_rt, t_bot, t_lt, kTileS);
        dl->AddQuad(t_top, t_rt, t_bot, t_lt, kTileE, 0.5f);
        draw_unit_from_def(dl, c, g_cw2, g_ch2, defs[u], fcol[fct], facing);
      }
      const char* nm = game::unit_type_name(static_cast<game::UnitType>(u));
      dl->AddText({sp0.x + 4.f, sp0.y + 3.f},
                  active ? IM_COL32(255,220,60,220) : IM_COL32(140,140,160,200), nm);
    }
  }

  // -- 4-up facing selector (mini-previews) --
  {
    const float mini_w   = preview_w / 4.f;
    const float mini_cs  = std::min(mini_w * 0.45f, (dir_bar_h - 24.f) * 2.4f);
    const float mini_cw2 = mini_cs * 0.5f;
    const float mini_ch2 = mini_cs * 0.25f;
    ImVec2 bar0 = {big_p0.x, big_p0.y + big_h + game_bar_h + 6.f};
    for (int d = 0; d < 4; ++d) {
      ImVec2 mp0 = {bar0.x + d * mini_w, bar0.y};
      ImVec2 mp1 = {mp0.x + mini_w - 2.f, mp0.y + dir_bar_h - 2.f};
      ImGui::SetCursorScreenPos(mp0);
      ImGui::PushID(d + 100);
      if (ImGui::InvisibleButton("##mf", {mini_w - 2.f, dir_bar_h - 2.f})) facing = d;
      ImGui::PopID();
      bool active = (facing == d);
      dl->AddRectFilled(mp0, mp1, active ? IM_COL32(50,50,80,255) : IM_COL32(20,20,30,255));
      dl->AddRect(mp0, mp1,
                  active ? IM_COL32(255,220,60,200) : IM_COL32(60,60,80,120),
                  0, 0, active ? 1.5f : 0.8f);
      ImVec2 mctr = {mp0.x + (mini_w - 2.f) * 0.5f, mp0.y + (dir_bar_h - 2.f) * 0.65f};
      draw_unit_from_def(dl, mctr, mini_cw2, mini_ch2, def, kUCol, d);
      ImVec2 lsz = ImGui::CalcTextSize(kFacingLabels[d]);
      dl->AddText({mp0.x + ((mini_w - 2.f) - lsz.x) * 0.5f, mp0.y + 4.f},
                  active ? IM_COL32(255,220,60,220) : IM_COL32(140,140,160,200),
                  kFacingLabels[d]);
    }
  }

  ImGui::EndChild();
  ImGui::SameLine();

  // ---- Controls pane -------------------------------------------------------
  ImGui::BeginChild("##se_ctrl", {ctrl_w, content_h}, false);

  ImGui::Text("Facing: %s", kFacingLabels[facing]);
  ImGui::SetNextItemWidth(140.f);
  ImGui::SliderFloat("game zoom", &game_cs, 16.f, 64.f, "%.0f px");
  ImGui::Separator();

  int remove_idx = -1;

  for (int pi = 0; pi < static_cast<int>(def.prims.size()); ++pi) {
    auto& p = def.prims[pi];
    ImGui::PushID(pi);

    bool is_sel    = (pi == sel_prim);
    const char* kind_name = prim_kind_name(p.kind);

    if (is_sel) ImGui::PushStyleColor(ImGuiCol_Header, ImVec4{0.35f, 0.32f, 0.10f, 1.f});
    ImVec4 hdr_col = is_sel ? ImVec4{1.f, 0.88f, 0.25f, 1.f} : ImVec4{0.8f, 0.8f, 0.4f, 1.f};
    ImGui::TextColored(hdr_col, "%s %d", kind_name, pi + 1);
    if (ImGui::IsItemClicked()) { sel_prim = pi; drag_prim = -1; }
    if (is_sel) ImGui::PopStyleColor();
    ImGui::SameLine(ctrl_w - 80.f);
    if (ImGui::SmallButton("Remove") && def.prims.size() > 1)
      remove_idx = pi;

    switch (p.kind) {
    case PrimKind::Box:
      ImGui::SliderFloat("fw",  &p.fw, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("fd",  &p.fd, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("h",   &p.h,  0.05f, 3.00f, "%.3f");
      break;
    case PrimKind::Wedge:
      ImGui::SliderFloat("width",  &p.fw, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("length", &p.fd, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("h",      &p.h,  0.05f, 3.00f, "%.3f");
      break;
    case PrimKind::Cylinder:
      ImGui::SliderFloat("diam", &p.fw, 0.05f, 1.20f, "%.3f");
      ImGui::SliderFloat("h",    &p.h,  0.05f, 3.00f, "%.3f");
      break;
    case PrimKind::Circle:
    case PrimKind::Sphere:
      ImGui::SliderFloat("r",   &p.r,  0.05f, 0.60f, "%.3f");
      break;
    }
    ImGui::SliderFloat("px",  &p.px, -1.0f, 1.0f, "%.3f");
    if (p.sym && ImGui::IsItemEdited()) p.py = p.px;
    ImGui::SliderFloat("py",  &p.py, -1.0f, 1.0f, "%.3f");
    if (p.sym && ImGui::IsItemEdited()) p.px = p.py;
    ImGui::SameLine(); ImGui::Checkbox("sym", &p.sym);
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
  if (ImGui::Button("+ Wedge"))
    def.prims.push_back({PrimKind::Wedge, 0.f, 0.f, 0.f, 0.40f, 0.60f, 0.40f, 0.f, 1.0f});
  ImGui::SameLine();
  if (ImGui::Button("+ Cylinder"))
    def.prims.push_back({PrimKind::Cylinder, 0.f, 0.f, 0.f, 0.40f, 0.f, 0.50f, 0.f, 1.0f});
  if (ImGui::Button("+ Disc"))
    def.prims.push_back({PrimKind::Circle, 0.f, 0.f, 0.5f, 0.f, 0.f, 0.f, 0.16f, 1.0f});
  ImGui::SameLine();
  if (ImGui::Button("+ Sphere"))
    def.prims.push_back({PrimKind::Sphere, 0.f, 0.f, 0.3f, 0.f, 0.f, 0.f, 0.20f, 1.0f});

  ImGui::EndChild();
  ImGui::End();
}

// ---- App state ------------------------------------------------------------

struct App {
  UnitShapeDef shape_defs[kUnitTypeCount];
  int          sel_shape  = 0;
  int          sel_prim   = 0;
  int          facing     = 0;
  float        game_cs    = 48.f; // ud_viz_imgui default tile size
  char         shape_path[512] = "shapes.udshapes";
  std::string  shape_status;
} g;

// ---- Main -----------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc >= 2)
    strncpy(g.shape_path, argv[1], sizeof(g.shape_path) - 1);

  if (!load_shapes(g.shape_defs, g.shape_path))
    for (int i = 0; i < kUnitTypeCount; ++i)
      g.shape_defs[i] = make_default_shape(static_cast<game::UnitType>(i));

  if (!glfwInit()) { fprintf(stderr, "glfwInit failed\n"); return 1; }
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);

  GLFWwindow* win = glfwCreateWindow(1520, 860, "Shape Editor", nullptr, nullptr);
  if (!win) { glfwTerminate(); fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(1);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui::GetIO().IniFilename = nullptr;

  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init("#version 330");

  while (!glfwWindowShouldClose(win)) {
    glfwPollEvents();
    ImGuiIO& io = ImGui::GetIO();

    // Without the WantTextInput check, typing 'q' into the path box quits and drops edits.
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Q)) glfwSetWindowShouldClose(win, GLFW_TRUE);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_shape_editor(io, g.shape_defs, g.sel_shape, g.facing,
                      g.sel_prim, g.game_cs, g.shape_path, g.shape_status);

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
