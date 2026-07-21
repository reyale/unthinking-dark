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
  if (ImGui::Button("Save"))
    status = save_shapes(defs, save_path) ? "Saved." : "Save failed.";
  ImGui::SameLine();
  if (ImGui::Button("Load"))
    status = load_shapes(defs, save_path) ? "Loaded." : "Load failed.";
  ImGui::SameLine();
  if (ImGui::Button("Defaults")) {
    for (int i = 0; i < 5; ++i)
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

  static int    drag_prim     = -1;
  static int    drag_prev_sel = -1;
  static float  drag_start_px = 0, drag_start_py = 0, drag_start_pz = 0;
  static ImVec2 drag_origin   = {0, 0};

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

  auto prim_anchor = [&](const ShapePrim& p) -> ImVec2 {
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, p.px, p.py, p.fw, p.fd, rpx, rpy, rfw, rfd);
    return { big_ctr.x + (rpx - rpy) * big_cw2,
             big_ctr.y + (rpx + rpy) * big_ch2 - p.pz * big_ch2 };
  };

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
      sel_prim      = hit;
      drag_prim     = hit;
      drag_origin   = io.MousePos;
      drag_start_px = def.prims[hit].px;
      drag_start_py = def.prims[hit].py;
      drag_start_pz = def.prims[hit].pz;
    } else {
      drag_prim = -1;
    }
  }

  if (big_active && drag_prim >= 0 && drag_prim < static_cast<int>(def.prims.size())) {
    float dx = io.MousePos.x - drag_origin.x;
    float dy = io.MousePos.y - drag_origin.y;
    auto& dp = def.prims[drag_prim];
    if (io.KeyShift) {
      dp.pz = std::max(0.f, drag_start_pz - dy / big_ch2);
    } else {
      float dtx_rot = ( dx / big_cw2 + dy / big_ch2) * 0.5f;
      float dty_rot = ( dy / big_ch2 - dx / big_cw2) * 0.5f;
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
    const auto& sp = def.prims[sel_prim];
    float rpx, rpy, rfw, rfd;
    apply_facing(facing, sp.px, sp.py, sp.fw, sp.fd, rpx, rpy, rfw, rfd);
    ImVec2 anch = { big_ctr.x + (rpx - rpy) * big_cw2,
                    big_ctr.y + (rpx + rpy) * big_ch2 - sp.pz * big_ch2 };
    static const ImU32 kHiCol = IM_COL32(255, 220, 60, 200);
    if (sp.kind == PrimKind::Box) {
      float h_px = big_ch2 * sp.h;
      ImVec2 s_tt = {anch.x,               anch.y - rfd*big_ch2 - h_px};
      ImVec2 s_tr = {anch.x + rfw*big_cw2,  anch.y - h_px};
      ImVec2 s_tb = {anch.x,               anch.y + rfd*big_ch2 - h_px};
      ImVec2 s_tl = {anch.x - rfw*big_cw2,  anch.y - h_px};
      ImVec2 s_bb = {anch.x,               anch.y + rfd*big_ch2};
      ImVec2 s_br = {anch.x + rfw*big_cw2,  anch.y};
      ImVec2 s_bl = {anch.x - rfw*big_cw2,  anch.y};
      dl->AddQuad(s_tt, s_tr, s_tb, s_tl, kHiCol, 1.5f);
      dl->AddLine(s_tl, s_bl, kHiCol, 1.0f);
      dl->AddLine(s_tb, s_bb, kHiCol, 1.0f);
      dl->AddLine(s_tr, s_br, kHiCol, 1.0f);
    } else {
      dl->AddCircle(anch, big_cw2 * sp.r + 3.f, kHiCol, 0, 1.5f);
    }
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

    bool is_sel    = (pi == sel_prim);
    bool is_box    = (p.kind == PrimKind::Box);
    bool is_sphere = (p.kind == PrimKind::Sphere);
    const char* kind_name = is_box ? "Box" : (is_sphere ? "Sphere" : "Circle");

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

struct App {
  UnitShapeDef shape_defs[5];
  int          sel_shape  = 0;
  int          sel_prim   = 0;
  int          facing     = 0;
  char         shape_path[512] = "shapes.udshapes";
  std::string  shape_status;
} g;

// ---- Main -----------------------------------------------------------------

int main(int argc, char** argv) {
  if (argc >= 2)
    strncpy(g.shape_path, argv[1], sizeof(g.shape_path) - 1);

  if (!load_shapes(g.shape_defs, g.shape_path))
    for (int i = 0; i < 5; ++i)
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

    if (ImGui::IsKeyPressed(ImGuiKey_Q)) glfwSetWindowShouldClose(win, GLFW_TRUE);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();

    draw_shape_editor(io, g.shape_defs, g.sel_shape, g.facing,
                      g.sel_prim, g.shape_path, g.shape_status);

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
