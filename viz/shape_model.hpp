#pragma once

#include "imgui.h"
#include "entity.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// ---- Shape model -----------------------------------------------------------

// CMake sets this to the repo's shapes.udshapes; builds without it (the browser
// build) fall back to the working directory.
#ifndef UD_DEFAULT_SHAPES_PATH
#define UD_DEFAULT_SHAPES_PATH "shapes.udshapes"
#endif

// Append only: values are not persisted, but keep existing kinds stable.
enum class PrimKind : uint8_t { Box, Circle, Sphere, Wedge, Cylinder, Block };

struct ShapePrim {
  PrimKind kind = PrimKind::Box;
  float px  = 0.0f;  // tile-space offset along NE-SW axis
  float py  = 0.0f;  // tile-space offset along NW-SE axis
  float pz  = 0.0f;  // height above tile in ch2 units
  float fw  = 0.5f;  // [Box] screen-x extent; [Wedge/Block] width; [Cylinder] diameter
  float fd  = 0.5f;  // [Box] screen-y extent; [Wedge/Block] length along the heading (NE)
  float h   = 0.5f;  // [prisms] height in ch2 units
  float r   = 0.16f; // [Circle/Sphere] radius in cw2 units
  float dim = 1.0f;  // colour multiplier
  bool  sym = false; // editor: constrain px == py for facing symmetry
  float emit = 0.0f; // 0 = painted hull; above, a light: drawn bright, with a glow this strong
};

struct ShapePoint {
  float x = 0.0f, y = 0.0f, z = 0.0f;
};

struct UnitShapeDef {
  std::vector<ShapePrim> prims;
  // Where shots leave, in the same model space as prims (authored facing NE).
  bool  has_muzzle = false;
  float mx = 0.0f, my = 0.0f, mz = 0.8f;
  // Exhausts: each glows, and leaves a trail while the ship moves.
  std::vector<ShapePoint> engines;
};

// Shapes without a muzzle fire from this height above the tile centre (ch2 units).
inline constexpr float kDefaultMuzzleZ = 0.8f;

// One shape per unit type, then one per structure type.
inline constexpr int kUnitShapeCount   = 5;
inline constexpr int kStructShapeCount = 3;
inline constexpr int kShapeCount       = kUnitShapeCount + kStructShapeCount;

// Each team flies its own hulls: the same units, with the same stats, drawn differently.
inline constexpr int kTeamCount = 2;
struct ShapeSet {
  UnitShapeDef d[kTeamCount][kShapeCount];
};

inline int shape_index(game::UnitType t) { return static_cast<int>(t); }
inline int shape_index(game::StructureType t) { return kUnitShapeCount + static_cast<int>(t); }

inline const char* shape_name(int i) {
  return i < kUnitShapeCount
           ? game::unit_type_name(static_cast<game::UnitType>(i))
           : game::structure_type_name(static_cast<game::StructureType>(i - kUnitShapeCount));
}

// ---- Colour helper ---------------------------------------------------------

// Shared with the shape editor so its game-scale preview matches the viewer.
// Cyan against orange: far apart in hue and in brightness, so the armies can't be mixed up
// (also by the common colour-blind kinds), and neither is the gold of resources.
inline constexpr ImVec4 kFactionA = {0.22f, 0.80f, 1.00f, 1.0f}; // cyan
inline constexpr ImVec4 kFactionB = {1.00f, 0.44f, 0.16f, 1.0f}; // orange
inline ImVec4 team_color(int team) { return team == 0 ? kFactionA : kFactionB; }

inline ImU32 dim_col(ImU32 col, float f) {
  ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
  return ImGui::ColorConvertFloat4ToU32({c.x*f, c.y*f, c.z*f, c.w});
}

// ---- Isometric geometry ----------------------------------------------------
//
// Tile space: px/py along the tile axes, pz up (in ch2 units). Projection:
//   screen = ctr + ((x - y) * cw2, (x + y) * ch2 - z * ch2)
// Box/Wedge/Cylinder are vertical prisms over a convex footprint; one geometry
// builder feeds drawing, depth sorting and hit-testing so they can never disagree.

// Facings turn clockwise on screen: 0=NE (tile -y), 1=SE (+x), 2=SW (+y), 3=NW (-x).
// Shapes are authored facing NE.
inline ImVec2 rotate_facing(int facing, ImVec2 v) {
  switch (facing & 3) {
  case 1:  return {-v.y, v.x};
  case 2:  return {-v.x, -v.y};
  case 3:  return {v.y, -v.x};
  default: return v;
  }
}

// The same for a turn of any size, in quarter turns: ships swing round between facings.
inline ImVec2 rotate_turn(float turn, ImVec2 v) {
  const float whole = std::round(turn);
  if (std::fabs(turn - whole) < 1e-4f) return rotate_facing(static_cast<int>(whole), v);
  const float a = turn * 1.5707963f;
  const float c = std::cos(a), sn = std::sin(a);
  return {v.x * c - v.y * sn, v.x * sn + v.y * c};
}

// Facing for a move of (dx, dy) tiles; the dominant axis wins, x on a tie.
// Returns -1 for no movement.
inline int facing_from_step(int dx, int dy) {
  if (dx == 0 && dy == 0) return -1;
  if (std::abs(dx) >= std::abs(dy)) return dx > 0 ? 1 : 3;
  return dy > 0 ? 2 : 0;
}

// Rotate (px,py) and swap (fw,fd) for the given facing.
inline void apply_facing(int facing, float px, float py, float fw, float fd,
                          float& rpx, float& rpy, float& rfw, float& rfd) {
  ImVec2 r = rotate_facing(facing, {px, py});
  rpx = r.x; rpy = r.y;
  bool swap = (facing & 1) != 0;
  rfw = swap ? fd : fw;
  rfd = swap ? fw : fd;
}

inline bool prim_is_prism(PrimKind k) {
  return k == PrimKind::Box || k == PrimKind::Wedge || k == PrimKind::Cylinder ||
         k == PrimKind::Block;
}

inline const char* prim_kind_name(PrimKind k) {
  switch (k) {
  case PrimKind::Box:      return "Box";
  case PrimKind::Circle:   return "Disc";
  case PrimKind::Sphere:   return "Sphere";
  case PrimKind::Wedge:    return "Wedge";
  case PrimKind::Cylinder: return "Cylinder";
  case PrimKind::Block:    return "Block";
  }
  return "?";
}

// Footprint at facing 0, relative to the prim's position. Box keeps its legacy
// diamond (fw spans screen-x, fd spans screen-y) so existing .udshapes files look
// the same. Wedge points NE (tile -y): fd = length, fw = width. Block is a
// rectangle on the same axes as Wedge. Cylinder: fw = screen width as a fraction
// of tile width, matching Box.
inline void prim_footprint(const ShapePrim& p, std::vector<ImVec2>& out) {
  out.clear();
  switch (p.kind) {
  case PrimKind::Box:
    out = {{p.fw * 0.5f, -p.fw * 0.5f}, {p.fd * 0.5f, p.fd * 0.5f},
           {-p.fw * 0.5f, p.fw * 0.5f}, {-p.fd * 0.5f, -p.fd * 0.5f}};
    break;
  case PrimKind::Wedge:
    out = {{0.f, -p.fd * 0.5f}, {p.fw * 0.5f, p.fd * 0.5f}, {-p.fw * 0.5f, p.fd * 0.5f}};
    break;
  case PrimKind::Block:
    out = {{-p.fw * 0.5f, -p.fd * 0.5f}, {p.fw * 0.5f, -p.fd * 0.5f},
           {p.fw * 0.5f, p.fd * 0.5f},   {-p.fw * 0.5f, p.fd * 0.5f}};
    break;
  case PrimKind::Cylinder: {
    constexpr int kSegs = 28;
    const float rad = p.fw * 0.70710678f;
    for (int i = 0; i < kSegs; ++i) {
      float a = 6.2831853f * static_cast<float>(i) / kSegs;
      out.push_back({rad * std::cos(a), rad * std::sin(a)});
    }
    break;
  }
  default:
    break;
  }
}

struct PrimSide {
  ImVec2 q[4]; // base a, base b, top b, top a
  float  shade;
};

struct PrimGeom {
  PrimKind kind = PrimKind::Box;
  std::vector<ImVec2>   top; // prisms: projected top face
  std::vector<PrimSide> sides; // prisms: camera-facing sides only
  ImVec2 centre{};             // round prims
  ImVec2 radius{};             // round prims: screen radii
  float  depth = 0.f;          // larger = nearer the camera
  float  lo[3]{}, hi[3]{};     // tile-space bounds (x, y, z) for draw ordering
  ImVec2 smin{}, smax{};       // screen bounds
};

inline ImVec2 iso_project(ImVec2 ctr, float cw2, float ch2, float x, float y, float z) {
  return {ctr.x + (x - y) * cw2, ctr.y + (x + y) * ch2 - z * ch2};
}

// Screen position shots leave from, for a shape drawn at `ctr` with `facing`.
inline ImVec2 shape_muzzle(const UnitShapeDef& d, ImVec2 ctr, float cw2, float ch2, float facing) {
  if (!d.has_muzzle) return iso_project(ctr, cw2, ch2, 0.0f, 0.0f, kDefaultMuzzleZ);
  const ImVec2 r = rotate_turn(facing, {d.mx, d.my});
  return iso_project(ctr, cw2, ch2, r.x, r.y, d.mz);
}

// Depth toward the camera for the 2:1 projection (30° elevation), up to scale.
// Height counts a third as much as ground position.
inline float iso_depth(float x, float y, float z) { return x + y + z / 3.f; }

// A hull prim sitting on the ground as a disc is the ship's shadow: it stays down when the
// ship is lifted (it hovers), and is drawn dark whatever the team colour.
inline bool prim_is_shadow(const ShapePrim& p) {
  return p.kind == PrimKind::Circle && p.pz < 0.02f;
}

inline PrimGeom build_prim_geom(const ShapePrim& p, ImVec2 ctr, float cw2, float ch2,
                                float facing, float lift = 0.0f) {
  PrimGeom g;
  g.kind = p.kind;
  ImVec2 pos = rotate_turn(facing, {p.px, p.py});
  const float pz = prim_is_shadow(p) ? p.pz : p.pz + lift;

  if (!prim_is_prism(p.kind)) {
    g.centre = iso_project(ctr, cw2, ch2, pos.x, pos.y, pz);
    g.radius = p.kind == PrimKind::Circle ? ImVec2{cw2 * p.r, ch2 * p.r}
                                          : ImVec2{cw2 * p.r, cw2 * p.r};
    g.depth  = iso_depth(pos.x, pos.y, pz);
    // Screen radius r*cw2 is r/sqrt(2) tiles across the ground and ~1.73r z-units tall.
    const float e  = p.r * 0.70710678f;
    const float ez = p.kind == PrimKind::Sphere ? p.r * 1.7320508f : 0.f;
    g.lo[0] = pos.x - e; g.hi[0] = pos.x + e;
    g.lo[1] = pos.y - e; g.hi[1] = pos.y + e;
    g.lo[2] = pz - ez; g.hi[2] = pz + ez;
    g.smin = {g.centre.x - g.radius.x, g.centre.y - g.radius.y};
    g.smax = {g.centre.x + g.radius.x, g.centre.y + g.radius.y};
    return g;
  }

  std::vector<ImVec2> fp;
  prim_footprint(p, fp);
  for (auto& v : fp) {
    ImVec2 r = rotate_turn(facing, v);
    v = {r.x + pos.x, r.y + pos.y};
  }
  const float z0 = pz, z1 = pz + p.h;
  g.depth = iso_depth(pos.x, pos.y, (z0 + z1) * 0.5f);

  const size_t n = fp.size();
  g.lo[0] = g.lo[1] = 1e9f; g.hi[0] = g.hi[1] = -1e9f;
  g.lo[2] = z0; g.hi[2] = z1;
  for (const auto& v : fp) {
    g.lo[0] = std::min(g.lo[0], v.x); g.hi[0] = std::max(g.hi[0], v.x);
    g.lo[1] = std::min(g.lo[1], v.y); g.hi[1] = std::max(g.hi[1], v.y);
    g.top.push_back(iso_project(ctr, cw2, ch2, v.x, v.y, z1));
  }
  // Screen bounds: the top face plus the lowest point of the base.
  g.smin = g.smax = g.top[0];
  for (const auto& v : fp) {
    for (float z : {z0, z1}) {
      ImVec2 sp = iso_project(ctr, cw2, ch2, v.x, v.y, z);
      g.smin = {std::min(g.smin.x, sp.x), std::min(g.smin.y, sp.y)};
      g.smax = {std::max(g.smax.x, sp.x), std::max(g.smax.y, sp.y)};
    }
  }

  // Outward normal via the footprint winding; visible when it points toward +x+y.
  float area2 = 0.f;
  for (size_t i = 0; i < n; ++i) {
    const ImVec2 &a = fp[i], &b = fp[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
  }
  const float wind = area2 >= 0.f ? 1.f : -1.f;
  for (size_t i = 0; i < n; ++i) {
    const ImVec2 &a = fp[i], &b = fp[(i + 1) % n];
    float nx = (b.y - a.y) * wind, ny = -(b.x - a.x) * wind;
    float len = std::sqrt(nx * nx + ny * ny);
    if (len < 1e-6f || nx + ny <= 1e-6f) continue;
    nx /= len; ny /= len;
    g.sides.push_back({{iso_project(ctr, cw2, ch2, a.x, a.y, z0),
                        iso_project(ctr, cw2, ch2, b.x, b.y, z0),
                        iso_project(ctr, cw2, ch2, b.x, b.y, z1),
                        iso_project(ctr, cw2, ch2, a.x, a.y, z1)},
                       0.48f + 0.10f * (ny - nx)});
  }
  return g;
}

// ---- Drawing ---------------------------------------------------------------

// ImGui's anti-aliased fill expects clockwise winding in screen space.
inline void fill_convex(ImDrawList* dl, const ImVec2* pts, int n, ImU32 col) {
  float area2 = 0.f;
  for (int i = 0; i < n; ++i) {
    const ImVec2 &a = pts[i], &b = pts[(i + 1) % n];
    area2 += a.x * b.y - b.x * a.y;
  }
  if (area2 >= 0.f) { dl->AddConvexPolyFilled(pts, n, col); return; }
  ImVec2 rev[32];
  int m = n < 32 ? n : 32;
  for (int i = 0; i < m; ++i) rev[i] = pts[m - 1 - i];
  dl->AddConvexPolyFilled(rev, m, col);
}

// `edge` outlines the faces: near black for a plain model, or a bright rim in the team's colour.
inline void draw_prim_geom(ImDrawList* dl, const PrimGeom& g, ImU32 col,
                           ImU32 edge = IM_COL32(0, 0, 0, 110)) {
  const float lw   = 0.8f;
  const ImU32 dark = IM_COL32(0, 0, 0, (edge >> IM_COL32_A_SHIFT & 0xFF) * 110 / 255);
  if (g.kind == PrimKind::Sphere) {
    const float rad = g.radius.x;
    const ImVec2 a  = g.centre;
    dl->AddCircleFilled(a, rad, dim_col(col, 0.55f));
    dl->AddCircleFilled({a.x - rad * 0.10f, a.y - rad * 0.14f}, rad * 0.88f, col);
    dl->AddCircleFilled({a.x - rad * 0.28f, a.y - rad * 0.34f}, rad * 0.30f,
                        IM_COL32(255, 255, 255, (col >> IM_COL32_A_SHIFT & 0xFF) * 110 / 255));
    return;
  }
  if (g.kind == PrimKind::Circle) {
    dl->AddEllipseFilled(g.centre, g.radius, col);
    dl->AddEllipse(g.centre, g.radius, dark, 0.f, 0, lw);
    return;
  }
  const bool smooth = g.kind == PrimKind::Cylinder;
  // Anti-aliased fringes on adjacent facets leave visible seams, which make a
  // cylinder look ribbed; the silhouette lines below cover the jagged outer edge.
  const ImDrawListFlags saved_flags = dl->Flags;
  if (smooth) dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  for (const auto& s : g.sides) {
    fill_convex(dl, s.q, 4, dim_col(col, s.shade));
    dl->AddLine(s.q[0], s.q[1], dark, lw);
    if (!smooth) {
      dl->AddLine(s.q[0], s.q[3], dark, lw);
      dl->AddLine(s.q[1], s.q[2], dark, lw);
    }
  }
  dl->Flags = saved_flags;
  if (smooth && !g.sides.empty()) {
    // Silhouette verticals only, so the cylinder doesn't look faceted.
    dl->AddLine(g.sides.front().q[0], g.sides.front().q[3], dark, lw);
    dl->AddLine(g.sides.back().q[1], g.sides.back().q[2], dark, lw);
  }
  fill_convex(dl, g.top.data(), static_cast<int>(g.top.size()), col);
  dl->AddPolyline(g.top.data(), static_cast<int>(g.top.size()), edge, ImDrawFlags_Closed, lw);
}

inline void draw_prim_outline(ImDrawList* dl, const PrimGeom& g, ImU32 col, float thick) {
  if (!prim_is_prism(g.kind)) {
    dl->AddEllipse(g.centre, {g.radius.x + 3.f, g.radius.y + 3.f}, col, 0.f, 0, thick);
    return;
  }
  for (const auto& s : g.sides) {
    dl->AddLine(s.q[0], s.q[1], col, thick);
    dl->AddLine(s.q[0], s.q[3], col, thick);
    dl->AddLine(s.q[1], s.q[2], col, thick);
  }
  dl->AddPolyline(g.top.data(), static_cast<int>(g.top.size()), col, ImDrawFlags_Closed,
                  thick);
}

// True if a must be painted before b. The camera looks from +x, +y and +z, so if the
// bounds are separated on any axis, the one on the low side can never occlude the
// other. Intersecting pieces (a turret sunk into a hull) fall back to "the one whose
// centre sits above the other's top goes last", then to centre depth.
inline bool paint_before(const PrimGeom& a, const PrimGeom& b) {
  constexpr float kEps = 1e-4f;
  for (int ax = 0; ax < 3; ++ax) {
    if (a.hi[ax] <= b.lo[ax] + kEps) return true;
    if (b.hi[ax] <= a.lo[ax] + kEps) return false;
  }
  const float az = (a.lo[2] + a.hi[2]) * 0.5f, bz = (b.lo[2] + b.hi[2]) * 0.5f;
  if (az >= b.hi[2] - kEps) return false;
  if (bz >= a.hi[2] - kEps) return true;
  return a.depth < b.depth;
}

inline bool screen_overlap(const PrimGeom& a, const PrimGeom& b) {
  return a.smin.x < b.smax.x && b.smin.x < a.smax.x &&
         a.smin.y < b.smax.y && b.smin.y < a.smax.y;
}

// Back-to-front draw order for the given facing: a topological sort over the pairs
// that overlap on screen, breaking ties (and any cycle) by centre depth, then index.
inline void build_draw_order(const UnitShapeDef& def, ImVec2 ctr, float cw2, float ch2,
                             float facing, std::vector<PrimGeom>& geoms,
                             std::vector<int>& order, float lift = 0.0f) {
  geoms.clear();
  order.clear();
  const int n = static_cast<int>(def.prims.size());
  for (int i = 0; i < n; ++i)
    geoms.push_back(build_prim_geom(def.prims[i], ctr, cw2, ch2, facing, lift));

  thread_local std::vector<uint8_t> before; // before[i*n+j]: i must precede j
  thread_local std::vector<int>     indeg;
  thread_local std::vector<uint8_t> done;
  before.assign(static_cast<size_t>(n) * n, 0);
  indeg.assign(n, 0);
  done.assign(n, 0);
  for (int i = 0; i < n; ++i)
    for (int j = i + 1; j < n; ++j) {
      if (!screen_overlap(geoms[i], geoms[j])) continue;
      if (paint_before(geoms[i], geoms[j])) { before[i * n + j] = 1; ++indeg[j]; }
      else                                  { before[j * n + i] = 1; ++indeg[i]; }
    }

  auto nearer = [&](int a, int b) {
    return geoms[a].depth < geoms[b].depth || (geoms[a].depth == geoms[b].depth && a < b);
  };
  for (int step = 0; step < n; ++step) {
    int pick = -1, fallback = -1;
    for (int i = 0; i < n; ++i) {
      if (done[i]) continue;
      if (fallback < 0 || nearer(i, fallback)) fallback = i;
      if (indeg[i] == 0 && (pick < 0 || nearer(i, pick))) pick = i;
    }
    if (pick < 0) pick = fallback; // cycle: give up on exactness for this piece
    done[pick] = 1;
    order.push_back(pick);
    for (int j = 0; j < n; ++j)
      if (before[pick * n + j]) --indeg[j];
  }
}

// A light to add over the scene once it is drawn (the viewer does so additively).
struct GlowReq {
  ImVec2 c;
  float  r;
  ImU32  col;
};

// How to paint a shape beyond its base colour. The defaults give a plain model, as the editor
// shows it.
struct ShapePaint {
  float lift  = 0.0f; // raises everything but the shadow, in ch2 units (ships hover)
  ImU32 edge  = IM_COL32(0, 0, 0, 110); // outline of each face
  ImU32 light = IM_COL32(255, 255, 255, 255); // what a light (emit > 0) shines
  float alpha = 1.0f; // the whole shape, shadow included
  std::vector<GlowReq>* glows = nullptr; // filled with one glow per light, if given
  ImU32 glow_col = 0; // their colour; 0 = `light`
};

inline ImU32 scale_alpha(ImU32 col, float a) {
  const ImU32 al = static_cast<ImU32>(static_cast<float>(col >> IM_COL32_A_SHIFT & 0xFF) * a);
  return (col & ~IM_COL32_A_MASK) | (al << IM_COL32_A_SHIFT);
}

inline ImU32 lerp_col(ImU32 a, ImU32 b, float t) {
  const ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
  return ImGui::ColorConvertFloat4ToU32({x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t,
                                         x.z + (y.z - x.z) * t, x.w + (y.w - x.w) * t});
}

inline void draw_unit_from_def(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2,
                                const UnitShapeDef& def, ImU32 col, float facing = 0.0f,
                                const ShapePaint& paint = {}) {
  thread_local std::vector<PrimGeom> geoms;
  thread_local std::vector<int>      order;
  build_draw_order(def, ctr, cw2, ch2, facing, geoms, order, paint.lift);
  const ImU32 base = scale_alpha(col, paint.alpha);
  for (int i : order) {
    const ShapePrim& p = def.prims[i];
    if (prim_is_shadow(p)) {
      dl->AddEllipseFilled(geoms[i].centre, geoms[i].radius,
                           IM_COL32(0, 0, 0, static_cast<int>(95.0f * paint.alpha)));
      continue;
    }
    ImU32 c = p.dim < 0.999f ? dim_col(base, p.dim) : base;
    if (p.emit > 0.0f) {
      c =
        lerp_col(c, scale_alpha(paint.light, paint.alpha), std::min(1.0f, 0.55f + p.emit * 0.45f));
      if (paint.glows) {
        const PrimGeom& g = geoms[i];
        const ImVec2 at = prim_is_prism(p.kind)
                            ? ImVec2{(g.smin.x + g.smax.x) * 0.5f, (g.smin.y + g.smax.y) * 0.5f}
                            : g.centre;
        const float size = std::max(g.smax.x - g.smin.x, g.smax.y - g.smin.y);
        paint.glows->push_back({at, size * (0.8f + 0.9f * p.emit),
                                scale_alpha(paint.glow_col ? paint.glow_col : paint.light,
                                            paint.alpha * std::min(1.0f, p.emit) * 0.75f)});
      }
    }
    draw_prim_geom(dl, geoms[i], c, scale_alpha(paint.edge, paint.alpha));
  }
}

// ---- Hit-testing -----------------------------------------------------------

inline bool point_in_convex(const ImVec2* pts, int n, ImVec2 m) {
  int sign = 0;
  for (int i = 0; i < n; ++i) {
    const ImVec2 &a = pts[i], &b = pts[(i + 1) % n];
    float c = (b.x - a.x) * (m.y - a.y) - (b.y - a.y) * (m.x - a.x);
    if (std::fabs(c) < 1e-6f) continue;
    int s = c > 0.f ? 1 : -1;
    if (sign == 0) sign = s;
    else if (s != sign) return false;
  }
  return true;
}

inline bool prim_geom_hit(const PrimGeom& g, ImVec2 m) {
  if (!prim_is_prism(g.kind)) {
    float dx = (m.x - g.centre.x) / std::max(g.radius.x, 1.f);
    float dy = (m.y - g.centre.y) / std::max(g.radius.y, 1.f);
    return dx * dx + dy * dy <= 1.f;
  }
  if (point_in_convex(g.top.data(), static_cast<int>(g.top.size()), m)) return true;
  for (const auto& s : g.sides)
    if (point_in_convex(s.q, 4, m)) return true;
  return false;
}

// Front-most primitive under the mouse, or -1.
inline int pick_prim(const UnitShapeDef& def, ImVec2 ctr, float cw2, float ch2, float facing,
                     ImVec2 mouse) {
  std::vector<PrimGeom> geoms;
  std::vector<int>      order;
  build_draw_order(def, ctr, cw2, ch2, facing, geoms, order);
  for (auto it = order.rbegin(); it != order.rend(); ++it)
    if (prim_geom_hit(geoms[*it], mouse)) return *it;
  return -1;
}

// ---- Default shapes --------------------------------------------------------

// Ships are authored facing NE: forward is -py, starboard is +px. They hover, so each has a
// ground disc as a shadow. Structures sit on the tile.
//
// The two teams' hulls differ in silhouette as well as colour: team 0 is angular (wedges,
// swept wings, spires), team 1 is blunt and rounded (drums, domes, slabs). A unit's role must
// still read the same on both: the Artillery is the one with the long gun, the Battleship the
// biggest, and so on.
inline UnitShapeDef make_default_shape(int team, int shape) {
  UnitShapeDef d;
  auto add = [&](PrimKind k, float px, float py, float pz, float fw, float fd, float h,
                 float r, float dim, float emit = 0.0f) {
    d.prims.push_back({k, px, py, pz, fw, fd, h, r, dim, false, emit});
  };
  auto block  = [&](float px, float py, float pz, float w, float l, float h, float dim) {
    add(PrimKind::Block, px, py, pz, w, l, h, 0.f, dim);
  };
  auto wedge  = [&](float px, float py, float pz, float w, float l, float h, float dim) {
    add(PrimKind::Wedge, px, py, pz, w, l, h, 0.f, dim);
  };
  auto cyl    = [&](float px, float py, float pz, float diam, float h, float dim) {
    add(PrimKind::Cylinder, px, py, pz, diam, 0.f, h, 0.f, dim);
  };
  auto sphere = [&](float px, float py, float pz, float r, float dim) {
    add(PrimKind::Sphere, px, py, pz, 0.f, 0.f, 0.f, r, dim);
  };
  auto light  = [&](float px, float py, float pz, float r, float emit) {
    add(PrimKind::Sphere, px, py, pz, 0.f, 0.f, 0.f, r, 1.0f, emit);
  };
  auto shadow = [&](float r) { add(PrimKind::Circle, 0.f, 0.f, 0.f, 0.f, 0.f, 0.f, r, 0.22f); };
  auto muzzle = [&](float px, float py, float pz) {
    d.has_muzzle = true;
    d.mx = px; d.my = py; d.mz = pz;
  };
  auto engine = [&](float px, float py, float pz) { d.engines.push_back({px, py, pz}); };

  if (team == 0) {
    switch (shape) {
    case 0: // Drone: stubby mining tug with side ore pods
      shadow(0.26f);
      block(0.f, 0.07f, 0.22f, 0.32f, 0.36f, 0.38f, 1.0f);
      wedge(0.f, -0.20f, 0.22f, 0.32f, 0.20f, 0.38f, 0.9f);
      cyl(-0.23f, 0.07f, 0.20f, 0.17f, 0.36f, 0.65f);
      cyl( 0.23f, 0.07f, 0.20f, 0.17f, 0.36f, 0.65f);
      light(0.f, 0.03f, 0.74f, 0.08f, 0.8f);
      engine(0.f, 0.27f, 0.40f);
      break;
    case 1: // Interceptor: narrow dart with swept wings
      shadow(0.24f);
      wedge(0.f, -0.02f, 0.40f, 0.24f, 0.78f, 0.24f, 1.0f);
      wedge(-0.17f, 0.16f, 0.40f, 0.20f, 0.40f, 0.08f, 0.75f);
      wedge( 0.17f, 0.16f, 0.40f, 0.20f, 0.40f, 0.08f, 0.75f);
      cyl(-0.07f, 0.30f, 0.38f, 0.10f, 0.24f, 0.55f);
      cyl( 0.07f, 0.30f, 0.38f, 0.10f, 0.24f, 0.55f);
      light(0.f, 0.02f, 0.70f, 0.07f, 0.7f);
      muzzle(0.f, -0.40f, 0.50f); // nose
      engine(-0.07f, 0.38f, 0.50f);
      engine( 0.07f, 0.38f, 0.50f);
      break;
    case 2: // Frigate: bowed hull, bridge aft, forward turret
      shadow(0.36f);
      block(0.f, 0.10f, 0.25f, 0.42f, 0.50f, 0.42f, 1.0f);
      wedge(0.f, -0.29f, 0.25f, 0.42f, 0.28f, 0.42f, 0.95f);
      block(0.f, 0.20f, 0.67f, 0.24f, 0.22f, 0.40f, 0.8f);
      cyl(0.f, -0.10f, 0.67f, 0.20f, 0.18f, 0.9f);
      block(0.f, -0.30f, 0.76f, 0.05f, 0.26f, 0.06f, 0.7f);
      cyl(-0.13f, 0.38f, 0.28f, 0.13f, 0.32f, 0.55f);
      cyl( 0.13f, 0.38f, 0.28f, 0.13f, 0.32f, 0.55f);
      light(0.f, 0.20f, 1.13f, 0.05f, 0.8f);
      muzzle(0.f, -0.43f, 0.79f); // turret barrel tip
      engine(-0.13f, 0.48f, 0.44f);
      engine( 0.13f, 0.48f, 0.44f);
      break;
    case 3: // Artillery: squat platform carrying one very long gun
      shadow(0.32f);
      block(0.f, 0.12f, 0.20f, 0.44f, 0.42f, 0.34f, 0.9f);
      cyl(0.f, 0.10f, 0.54f, 0.30f, 0.24f, 1.0f);
      block(0.f, -0.24f, 0.70f, 0.07f, 0.62f, 0.08f, 0.75f);
      block(0.f, 0.30f, 0.54f, 0.22f, 0.12f, 0.22f, 0.6f);
      light(0.f, 0.10f, 0.84f, 0.05f, 0.8f);
      muzzle(0.f, -0.55f, 0.74f); // end of the long gun
      engine(0.f, 0.36f, 0.36f);
      break;
    case 4: // Battleship: long armoured hull, twin forward guns, tall bridge
      shadow(0.48f);
      block(0.f, 0.10f, 0.22f, 0.62f, 0.66f, 0.50f, 1.0f);
      wedge(0.f, -0.38f, 0.22f, 0.62f, 0.32f, 0.50f, 0.95f);
      block(0.f, 0.16f, 0.72f, 0.42f, 0.42f, 0.34f, 0.85f);
      block(0.f, 0.26f, 1.06f, 0.20f, 0.16f, 0.50f, 0.9f);
      cyl(0.f, -0.20f, 0.72f, 0.24f, 0.16f, 0.9f);
      block(-0.04f, -0.40f, 0.80f, 0.04f, 0.26f, 0.06f, 0.7f);
      block( 0.04f, -0.40f, 0.80f, 0.04f, 0.26f, 0.06f, 0.7f);
      cyl(-0.15f, 0.44f, 0.30f, 0.12f, 0.30f, 0.55f);
      cyl( 0.15f, 0.44f, 0.30f, 0.12f, 0.30f, 0.55f);
      light(0.f, 0.26f, 1.63f, 0.06f, 0.8f);
      muzzle(0.f, -0.53f, 0.83f); // between the twin barrels
      engine(-0.15f, 0.53f, 0.45f);
      engine( 0.15f, 0.53f, 0.45f);
      break;
    case 5: // Command Core: plinth, corner pylons, tiered tower, domed top, spire
      block(0.f, 0.f, 0.f, 0.86f, 0.86f, 0.30f, 0.65f);
      cyl(-0.31f, -0.31f, 0.30f, 0.11f, 0.55f, 0.8f);
      cyl( 0.31f, -0.31f, 0.30f, 0.11f, 0.55f, 0.8f);
      cyl(-0.31f,  0.31f, 0.30f, 0.11f, 0.55f, 0.8f);
      cyl( 0.31f,  0.31f, 0.30f, 0.11f, 0.55f, 0.8f);
      cyl(0.f, 0.f, 0.30f, 0.62f, 0.70f, 1.0f);
      cyl(0.f, 0.f, 1.00f, 0.44f, 0.30f, 0.85f);
      light(0.f, 0.f, 1.42f, 0.19f, 1.0f);
      cyl(0.f, 0.f, 1.55f, 0.05f, 0.75f, 0.9f);
      break;
    case 6: // Factory: hall with an annexe and two smokestacks
      block(0.f, 0.f, 0.f, 0.86f, 0.86f, 0.18f, 0.6f);
      block(-0.08f, 0.06f, 0.18f, 0.56f, 0.60f, 0.55f, 1.0f);
      block(0.27f, 0.14f, 0.18f, 0.22f, 0.40f, 0.32f, 0.8f);
      cyl(0.20f, -0.26f, 0.18f, 0.13f, 1.10f, 0.75f);
      cyl(0.02f, -0.30f, 0.18f, 0.11f, 0.85f, 0.75f);
      light(-0.08f, 0.06f, 0.80f, 0.07f, 0.8f);
      break;
    case 7: // Claim Node: tripod base, slim pylon, glowing beacon
      cyl(0.f, 0.f, 0.f, 0.46f, 0.14f, 0.65f);
      cyl(-0.16f, 0.10f, 0.14f, 0.07f, 0.35f, 0.75f);
      cyl( 0.16f, 0.10f, 0.14f, 0.07f, 0.35f, 0.75f);
      cyl( 0.f,  -0.19f, 0.14f, 0.07f, 0.35f, 0.75f);
      cyl(0.f, 0.f, 0.14f, 0.10f, 1.25f, 0.9f);
      light(0.f, 0.f, 1.52f, 0.13f, 1.0f);
      break;
    default:
      break;
    }
    return d;
  }

  switch (shape) {
  case 0: // Drone: round pod under a dome, two grabber arms forward
    shadow(0.26f);
    cyl(0.f, 0.02f, 0.20f, 0.27f, 0.32f, 1.0f);
    block(-0.13f, -0.25f, 0.24f, 0.07f, 0.18f, 0.10f, 0.7f);
    block( 0.13f, -0.25f, 0.24f, 0.07f, 0.18f, 0.10f, 0.7f);
    cyl(0.f, 0.25f, 0.22f, 0.10f, 0.20f, 0.6f);
    light(0.f, 0.02f, 0.64f, 0.13f, 0.75f);
    engine(0.f, 0.32f, 0.34f);
    break;
  case 1: // Interceptor: twin booms joined by a crossbar, cockpit ball between
    shadow(0.24f);
    block(-0.15f, 0.f, 0.38f, 0.11f, 0.60f, 0.16f, 1.0f);
    block( 0.15f, 0.f, 0.38f, 0.11f, 0.60f, 0.16f, 1.0f);
    block(0.f, 0.06f, 0.41f, 0.30f, 0.16f, 0.10f, 0.8f);
    light(0.f, 0.04f, 0.63f, 0.10f, 0.7f);
    muzzle(0.f, -0.30f, 0.50f); // between the booms
    engine(-0.15f, 0.32f, 0.46f);
    engine( 0.15f, 0.32f, 0.46f);
    break;
  case 2: // Frigate: hammerhead bow, narrow neck, engine drum aft, round turret
    shadow(0.36f);
    block(0.f, -0.27f, 0.25f, 0.56f, 0.20f, 0.36f, 1.0f);
    block(0.f, 0.04f, 0.25f, 0.26f, 0.46f, 0.36f, 0.9f);
    cyl(0.f, 0.30f, 0.22f, 0.28f, 0.44f, 0.85f);
    cyl(0.f, -0.04f, 0.61f, 0.16f, 0.18f, 0.95f);
    block(0.f, -0.25f, 0.68f, 0.06f, 0.28f, 0.06f, 0.7f);
    light(0.f, 0.30f, 0.80f, 0.07f, 0.8f);
    muzzle(0.f, -0.40f, 0.71f); // turret barrel tip
    engine(0.f, 0.50f, 0.44f);
    break;
  case 3: // Artillery: round carriage, drum turret, one fat long barrel, counterweights
    shadow(0.34f);
    cyl(0.f, 0.08f, 0.18f, 0.38f, 0.30f, 0.9f);
    cyl(0.f, 0.08f, 0.48f, 0.24f, 0.26f, 1.0f);
    block(0.f, -0.20f, 0.60f, 0.14f, 0.52f, 0.14f, 0.8f);
    block(-0.22f, 0.30f, 0.48f, 0.10f, 0.14f, 0.20f, 0.6f);
    block( 0.22f, 0.30f, 0.48f, 0.10f, 0.14f, 0.20f, 0.6f);
    light(0.f, 0.08f, 0.82f, 0.06f, 0.8f);
    muzzle(0.f, -0.48f, 0.67f); // end of the barrel
    engine(0.f, 0.36f, 0.30f);
    break;
  case 4: // Battleship: slab hull with a round bow, drum citadel under a dome, twin guns
    shadow(0.50f);
    block(0.f, 0.06f, 0.22f, 0.64f, 0.72f, 0.46f, 1.0f);
    cyl(0.f, -0.30f, 0.22f, 0.40f, 0.46f, 0.95f);
    cyl(0.f, 0.16f, 0.68f, 0.26f, 0.30f, 0.85f);
    cyl(0.f, -0.24f, 0.68f, 0.17f, 0.16f, 0.9f);
    block(-0.05f, -0.44f, 0.76f, 0.05f, 0.28f, 0.06f, 0.7f);
    block( 0.05f, -0.44f, 0.76f, 0.05f, 0.28f, 0.06f, 0.7f);
    cyl(-0.20f, 0.44f, 0.30f, 0.11f, 0.30f, 0.55f);
    cyl( 0.20f, 0.44f, 0.30f, 0.11f, 0.30f, 0.55f);
    light(0.f, 0.16f, 1.14f, 0.13f, 0.8f);
    muzzle(0.f, -0.58f, 0.79f); // between the twin barrels
    engine(-0.20f, 0.53f, 0.45f);
    engine( 0.20f, 0.53f, 0.45f);
    break;
  case 5: // Command Core: slab, broad drum, four corner globes, great dome
    block(0.f, 0.f, 0.f, 0.90f, 0.90f, 0.22f, 0.6f);
    cyl(0.f, 0.f, 0.22f, 0.56f, 0.50f, 1.0f);
    sphere(-0.33f, -0.33f, 0.42f, 0.13f, 0.85f);
    sphere( 0.33f, -0.33f, 0.42f, 0.13f, 0.85f);
    sphere(-0.33f,  0.33f, 0.42f, 0.13f, 0.85f);
    sphere( 0.33f,  0.33f, 0.42f, 0.13f, 0.85f);
    cyl(0.f, 0.f, 0.72f, 0.36f, 0.24f, 0.85f);
    light(0.f, 0.f, 1.25f, 0.30f, 0.9f);
    break;
  case 6: // Factory: two drums and a shed
    block(0.f, 0.f, 0.f, 0.86f, 0.86f, 0.18f, 0.6f);
    cyl(-0.16f, 0.12f, 0.18f, 0.32f, 0.60f, 1.0f);
    cyl(0.20f, -0.16f, 0.18f, 0.24f, 0.42f, 0.85f);
    block(0.20f, 0.24f, 0.18f, 0.30f, 0.26f, 0.30f, 0.8f);
    light(-0.16f, 0.12f, 0.93f, 0.09f, 0.8f);
    break;
  case 7: // Claim Node: fat pylon, collar, big globe
    cyl(0.f, 0.f, 0.f, 0.40f, 0.18f, 0.65f);
    cyl(0.f, 0.f, 0.18f, 0.20f, 0.55f, 0.9f);
    cyl(0.f, 0.f, 0.73f, 0.30f, 0.10f, 0.75f);
    light(0.f, 0.f, 1.10f, 0.19f, 1.0f);
    break;
  default:
    break;
  }
  return d;
}

inline void make_default_shapes(ShapeSet& set) {
  for (int t = 0; t < kTeamCount; ++t)
    for (int i = 0; i < kShapeCount; ++i)
      set.d[t][i] = make_default_shape(t, i);
}

// ---- Shape file I/O --------------------------------------------------------
//
// Version 2: every section names its team (`unit 0 Drone team=1`), a prim may carry `emit=`,
// and a shape may list `engine` points. A section with no team (a version 1 file) is both
// teams' shape.

// Writes to a temp file and renames, so a viewer hot-reloading the file never
// sees a half-written version.
inline bool save_shapes(const ShapeSet& set, const char* path) {
  const std::string tmp = std::string(path) + ".tmp";
  FILE* f = fopen(tmp.c_str(), "w");
  if (!f) return false;
  fprintf(f, "version 2\n");
  for (int t = 0; t < kTeamCount; ++t)
    for (int i = 0; i < kShapeCount; ++i) {
      const UnitShapeDef& def = set.d[t][i];
      if (i < kUnitShapeCount)
        fprintf(f, "unit %d %s team=%d\n", i, shape_name(i), t);
      else
        fprintf(f, "structure %d %s team=%d\n", i - kUnitShapeCount, shape_name(i), t);
      for (const auto& p : def.prims) {
        switch (p.kind) {
        case PrimKind::Box:
        case PrimKind::Wedge:
        case PrimKind::Cylinder:
        case PrimKind::Block:
          fprintf(f, "%s px=%.4f py=%.4f pz=%.4f fw=%.4f fd=%.4f h=%.4f dim=%.4f sym=%d",
                  p.kind == PrimKind::Box   ? "box"   : p.kind == PrimKind::Wedge ? "wedge" :
                  p.kind == PrimKind::Block ? "block" : "cylinder",
                  p.px, p.py, p.pz, p.fw, p.fd, p.h, p.dim, p.sym ? 1 : 0);
          break;
        case PrimKind::Circle:
        case PrimKind::Sphere:
          fprintf(f, "%s px=%.4f py=%.4f pz=%.4f r=%.4f dim=%.4f sym=%d",
                  p.kind == PrimKind::Circle ? "circle" : "sphere",
                  p.px, p.py, p.pz, p.r, p.dim, p.sym ? 1 : 0);
          break;
        }
        if (p.emit > 0.0f) fprintf(f, " emit=%.4f", p.emit);
        fprintf(f, "\n");
      }
      if (def.has_muzzle)
        fprintf(f, "muzzle px=%.4f py=%.4f pz=%.4f\n", def.mx, def.my, def.mz);
      for (const auto& e : def.engines)
        fprintf(f, "engine px=%.4f py=%.4f pz=%.4f\n", e.x, e.y, e.z);
    }
  bool ok = fclose(f) == 0;
  if (!ok || std::rename(tmp.c_str(), path) != 0) {
    std::remove(tmp.c_str());
    return false;
  }
  return true;
}

// Shapes the file doesn't mention keep their defaults, so older files (written
// before structures had shapes) still give every structure something to draw.
inline bool load_shapes(ShapeSet& set, const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return false;
  make_default_shapes(set);
  char line[256];
  UnitShapeDef* cur[kTeamCount] = {};
  int n_cur = 0;
  while (fgets(line, sizeof(line), f)) {
    int uid; char name[64];
    int shape = -1;
    if (sscanf(line, "unit %d %63s", &uid, name) == 2 && uid >= 0 && uid < kUnitShapeCount)
      shape = uid;
    else if (sscanf(line, "structure %d %63s", &uid, name) == 2 && uid >= 0 &&
             uid < kStructShapeCount)
      shape = kUnitShapeCount + uid;
    if (shape >= 0) {
      int team = -1;
      if (const char* tp = strstr(line, "team=")) sscanf(tp, "team=%d", &team);
      n_cur = 0;
      for (int t = 0; t < kTeamCount; ++t)
        if (team < 0 || team == t) {
          set.d[t][shape] = UnitShapeDef{};
          cur[n_cur++] = &set.d[t][shape];
        }
      continue;
    }
    if (n_cur == 0) continue;
    float mx, my, mz;
    if (sscanf(line, "muzzle px=%f py=%f pz=%f", &mx, &my, &mz) == 3) {
      for (int k = 0; k < n_cur; ++k) {
        cur[k]->has_muzzle = true;
        cur[k]->mx = mx; cur[k]->my = my; cur[k]->mz = mz;
      }
      continue;
    }
    if (sscanf(line, "engine px=%f py=%f pz=%f", &mx, &my, &mz) == 3) {
      for (int k = 0; k < n_cur; ++k) cur[k]->engines.push_back({mx, my, mz});
      continue;
    }
    ShapePrim p{};
    struct { const char* fmt; PrimKind kind; } kPrismFmts[] = {
      {"box px=%f py=%f pz=%f fw=%f fd=%f h=%f dim=%f",      PrimKind::Box},
      {"wedge px=%f py=%f pz=%f fw=%f fd=%f h=%f dim=%f",    PrimKind::Wedge},
      {"cylinder px=%f py=%f pz=%f fw=%f fd=%f h=%f dim=%f", PrimKind::Cylinder},
      {"block px=%f py=%f pz=%f fw=%f fd=%f h=%f dim=%f",    PrimKind::Block},
    };
    struct { const char* fmt; PrimKind kind; } kRoundFmts[] = {
      {"circle px=%f py=%f pz=%f r=%f dim=%f", PrimKind::Circle},
      {"sphere px=%f py=%f pz=%f r=%f dim=%f", PrimKind::Sphere},
    };
    bool parsed = false;
    for (const auto& pf : kPrismFmts) {
      if (sscanf(line, pf.fmt, &p.px, &p.py, &p.pz, &p.fw, &p.fd, &p.h, &p.dim) == 7) {
        p.kind = pf.kind; parsed = true; break;
      }
    }
    for (const auto& rf : kRoundFmts) {
      if (parsed) break;
      if (sscanf(line, rf.fmt, &p.px, &p.py, &p.pz, &p.r, &p.dim) == 5) {
        p.kind = rf.kind; parsed = true;
      }
    }
    if (!parsed) continue;
    int sv = 0;
    if (const char* sp = strstr(line, "sym=")) sscanf(sp, "sym=%d", &sv);
    p.sym = sv != 0;
    if (const char* ep = strstr(line, "emit=")) sscanf(ep, "emit=%f", &p.emit);
    for (int k = 0; k < n_cur; ++k) cur[k]->prims.push_back(p);
  }
  fclose(f);
  return true;
}
