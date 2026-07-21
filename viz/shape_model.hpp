#pragma once

#include "imgui.h"
#include "entity.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

// ---- Shape model -----------------------------------------------------------

enum class PrimKind : uint8_t { Box, Circle, Sphere };

struct ShapePrim {
  PrimKind kind = PrimKind::Box;
  float px  = 0.0f;  // tile-space offset along NE-SW axis
  float py  = 0.0f;  // tile-space offset along NW-SE axis
  float pz  = 0.0f;  // height above tile in ch2 units
  float fw  = 0.5f;  // [Box] footprint scale along NE-SW
  float fd  = 0.5f;  // [Box] footprint scale along NW-SE
  float h   = 0.5f;  // [Box] height in ch2 units
  float r   = 0.16f; // [Circle/Sphere] radius in cw2 units
  float dim = 1.0f;  // colour multiplier
};

struct UnitShapeDef {
  std::vector<ShapePrim> prims;
};

// ---- Colour helper ---------------------------------------------------------

inline ImU32 dim_col(ImU32 col, float f) {
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
  return ImGui::ColorConvertFloat4ToU32({c.x*f, c.y*f, c.z*f, c.w});
}

// ---- Isometric primitives --------------------------------------------------

// fw = footprint scale along NE-SW (screen left-right)
// fd = footprint scale along NW-SE (screen top-bottom)
inline void draw_iso_box(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                          float fw, float fd, float h, ImU32 col) {
  ImVec2 tt = {ctr.x,           ctr.y - fd*ch2 - h};
  ImVec2 tr = {ctr.x + fw*cw2,  ctr.y - h};
  ImVec2 tb = {ctr.x,           ctr.y + fd*ch2 - h};
  ImVec2 tl = {ctr.x - fw*cw2,  ctr.y - h};
  ImVec2 br = {ctr.x + fw*cw2,  ctr.y};
  ImVec2 bb = {ctr.x,           ctr.y + fd*ch2};
  ImVec2 bl = {ctr.x - fw*cw2,  ctr.y};

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

// Rotate (px,py) and swap (fw,fd) for the given facing (0=NE 1=SE 2=SW 3=NW).
inline void apply_facing(int facing, float px, float py, float fw, float fd,
                          float& rpx, float& rpy, float& rfw, float& rfd) {
  switch (facing & 3) {
  case 0: rpx =  px; rpy =  py; rfw = fw; rfd = fd; break;
  case 1: rpx =  py; rpy = -px; rfw = fd; rfd = fw; break;
  case 2: rpx = -px; rpy = -py; rfw = fw; rfd = fd; break;
  case 3: rpx = -py; rpy =  px; rfw = fd; rfd = fw; break;
  }
}

inline void draw_unit_from_def(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
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
    } else { // Sphere: shadow rim + body + specular highlight
      float rad = cw2 * p.r;
      dl->AddCircleFilled(anchor, rad, dim_col(c, 0.55f));
      dl->AddCircleFilled({anchor.x - rad*0.10f, anchor.y - rad*0.14f}, rad * 0.88f, c);
      dl->AddCircleFilled({anchor.x - rad*0.28f, anchor.y - rad*0.34f}, rad * 0.30f,
                           IM_COL32(255, 255, 255, 110));
    }
  }
}

// ---- Default shapes --------------------------------------------------------

inline UnitShapeDef make_default_shape(game::UnitType t) {
  UnitShapeDef d;
  // fields: kind, px, py, pz, fw, fd, h, r, dim
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

// ---- Shape file I/O --------------------------------------------------------

inline bool save_shapes(const UnitShapeDef* defs, const char* path) {
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

inline bool load_shapes(UnitShapeDef* defs, const char* path) {
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
