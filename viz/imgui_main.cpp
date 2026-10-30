// Usage: ud_viz_imgui [replay.ud] [--shapes file.udshapes]   (shapes hot-reload on save)
// Controls: Space=play/pause  ←/→=step  U=union  G=god  0/1=faction view  scroll=zoom  Q=quit
//           R=replay this frame's shots  T=toggle static shot lines while paused

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
  uint32_t id{0};        // unit or structure id, for hit flashes
  bool     is_struct{false};
};

// ---- Cell data builder ----------------------------------------------------

// Structures never move; turn them toward the map centre so bases face each other.
static int structure_facing(const game::Map& map, int32_t x, int32_t y) {
  int facing = facing_from_step(map.width / 2 - x, map.height / 2 - y);
  return facing < 0 ? 0 : facing;
}

// view: -1 = god (everything), -2 = union of both factions, 0/1 = that faction.
static bool tile_visible(const game::FrameState& fr, int view, int32_t x, int32_t y) {
  if (view == -1) return true;
  for (const auto& u : fr.units) {
    if (view >= 0 && static_cast<int>(u.faction.value) != view) continue;
    if (std::abs(x - u.pos.x) + std::abs(y - u.pos.y) <= game::stats_for(u.type).sight)
      return true;
  }
  for (const auto& s : fr.structures) {
    if (view >= 0 && static_cast<int>(s.faction.value) != view) continue;
    if (s.pos.x == x && s.pos.y == y) return true;
  }
  return false;
}

static CellData make_cell(const game::Map& map, const game::FrameState& fr,
                           const std::vector<uint8_t>& facings,
                           int view, int32_t x, int32_t y) {
  if (!tile_visible(fr, view, x, y)) return {kFogBg};

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
    return {bg, g, false, game::UnitType::Drone, shape_index(s.type), fg, s.hp, s.max_hp,
            structure_facing(map, x, y), s.id.value, true};
  }

  int top = -1;
  for (size_t i = 0; i < fr.units.size(); ++i)
    if (fr.units[i].pos.x == x && fr.units[i].pos.y == y) top = static_cast<int>(i);
  if (top >= 0) {
    const auto& u = fr.units[top];
    ImVec4 fg = u.faction.value == 0 ? kFactionA : kFactionB;
    int facing = top < static_cast<int>(facings.size()) ? facings[top] : 0;
    return {bg, '\0', true, u.type, shape_index(u.type), fg, u.hp, u.max_hp, facing, u.id.value};
  }

  return {bg};
}

// ---- Combat effects types --------------------------------------------------
//
// Shots and kills are short wall-clock effects spawned when the displayed frame
// changes; they are not extra frames, so the tick counter and slider are unaffected.
// Frame N's effects are the combat that produced frame N, so stepping in either
// direction shows how the current frame came about.

enum class FxKind : uint8_t { Tracer, Muzzle, Hit, Splash, Burst };

struct Effect {
  FxKind         kind;
  game::UnitType weapon;   // shooter type; picks the shot style
  game::Vec2     from, to; // tiles; only Tracer uses `from`
  ImVec4         col;
  double         start, dur;
  float          size = 1.0f;
  // Hit: the entity that flashes white. Burst: the victim (also flashes).
  game::UnitId      unit{};
  game::StructureId structure{};
  int32_t           radius = 0; // Splash: everything this close to `to` flashes
  // Tracer/Muzzle: the shooter, so shots leave from its muzzle point.
  int               shooter_shape  = -1;
  int               shooter_facing = 0;
  // Burst only: the victim, drawn until the explosion covers it. The frame being
  // shown is post-combat, so without this the target vanishes before it is hit.
  int    ghost_shape  = -1;
  int    ghost_facing = 0;
  ImVec4 ghost_col{};
};

struct FxState {
  std::vector<Effect> live;
  int    seen_frame  = -1;    // last g.cur observed
  double changed_t   = 0.0;   // when g.cur last changed
  bool   pending     = false; // seen_frame's effects not yet spawned
  bool   scrubbing   = false; // tick slider is being dragged
  bool   show_static = true;  // faint lines for the current frame's shots while paused
};

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
  FxState           fx;
  std::string       shapes_path = UD_DEFAULT_SHAPES_PATH;
  std::filesystem::file_time_type shapes_mtime{};
  double            shapes_checked_t = 0.0;
} g;

static bool load_shape_defs() {
  bool ok = load_shapes(g.shape_defs, g.shapes_path.c_str());
  if (!ok)
    for (int i = 0; i < kShapeCount; ++i)
      g.shape_defs[i] = make_default_shape(i);
  std::error_code ec;
  g.shapes_mtime = std::filesystem::last_write_time(g.shapes_path, ec);
  return ok;
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

// Units face the way they last moved or fired; firing wins within a tick because
// combat runs after movement. Before either, they face the map centre, so freshly
// built units look toward the action rather than all NE.
static void compute_unit_facings() {
  g.unit_facing.assign(g.frames.size(), {});
  std::map<game::UnitId, std::pair<game::Vec2, uint8_t>> last; // pos, facing
  const int32_t cx = g.map.width / 2, cy = g.map.height / 2;
  for (size_t f = 0; f < g.frames.size(); ++f) {
    const auto& units = g.frames[f].units;
    auto& out = g.unit_facing[f];
    out.resize(units.size());
    std::map<game::UnitId, int> fired; // shooter -> facing toward its target
    for (const auto& sh : g.frames[f].shots) {
      const int fc = facing_from_step(sh.to.x - sh.from.x, sh.to.y - sh.from.y);
      if (fc >= 0) fired[sh.attacker] = fc;
    }
    for (size_t i = 0; i < units.size(); ++i) {
      const auto& u = units[i];
      auto it = last.find(u.id);
      auto shot = fired.find(u.id);
      int facing;
      if (shot != fired.end()) {
        facing = shot->second;
      } else if (it == last.end()) {
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
    g.fx     = FxState{};
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

// ---- Combat effects ---------------------------------------------------------

// Base timings in seconds, used as-is when stepping. During playback they are
// compressed so a tick's effects finish within kFxMaxTicks ticks and can't pile up.
static constexpr double kFxDelayNormal = 0.08; // first-strike shots go at t=0
static constexpr double kFxHit         = 0.18;
static constexpr double kFxSplash      = 0.38;
static constexpr double kFxMuzzle      = 0.12;
static constexpr double kFxBurst       = 0.40; // scaled up for bigger victims
static constexpr double kFxTotal       = 1.10; // worst case: artillery onto a Command Core
static constexpr double kFxMaxTicks    = 5.0;
static constexpr double kFxScrubSettle = 0.10; // slider must rest this long before spawning

// How each shooter's shot looks. Widths are in ch2 units so effects scale with zoom.
struct WeaponFx {
  double travel;   // seconds from muzzle to impact
  int    bolts;    // Interceptors fire a quick burst
  double stagger;  // between bolts
  float  width;
  float  tail;     // tracer length as a fraction of the path
  float  hit_size;
  bool   beam;     // twin parallel lines plus a muzzle flash
  bool   arc;      // lobbed shell
};

static WeaponFx weapon_fx(game::UnitType t) {
  switch (t) {
  case game::UnitType::Interceptor: return {0.14, 3, 0.05, 0.08f, 0.30f, 0.6f, false, false};
  case game::UnitType::Frigate:     return {0.20, 1, 0.00, 0.13f, 0.45f, 1.0f, false, false};
  case game::UnitType::Artillery:   return {0.45, 1, 0.00, 0.14f, 0.00f, 1.0f, false, true};
  case game::UnitType::Battleship:  return {0.24, 1, 0.00, 0.17f, 0.70f, 1.5f, true,  false};
  case game::UnitType::Drone:       break;
  }
  return {0.20, 1, 0.00, 0.10f, 0.45f, 0.8f, false, false};
}

static float burst_size(const game::KillEvent& k) {
  if (!k.unit.is_null()) {
    switch (k.unit_type) {
    case game::UnitType::Drone:       return 0.7f;
    case game::UnitType::Interceptor: return 0.8f;
    case game::UnitType::Frigate:     return 1.1f;
    case game::UnitType::Artillery:   return 1.0f;
    case game::UnitType::Battleship:  return 1.5f;
    }
  }
  switch (k.structure_type) {
  case game::StructureType::ClaimNode:   return 1.3f;
  case game::StructureType::Factory:     return 1.9f;
  case game::StructureType::CommandCore: return 2.6f;
  }
  return 1.0f;
}

static ImVec4 mix_white(ImVec4 c, float t) {
  return {c.x + (1.0f - c.x) * t, c.y + (1.0f - c.y) * t, c.z + (1.0f - c.z) * t, c.w};
}

static bool shot_visible(const game::FrameState& fr, const game::ShotEvent& sh) {
  return tile_visible(fr, g.view, sh.from.x, sh.from.y) ||
         tile_visible(fr, g.view, sh.to.x, sh.to.y);
}

static int32_t splash_radius(const game::ShotEvent& sh) {
  return game::stats_for(sh.attacker_type).splash_radius;
}

// The shooter's facing on `frame` (it turned toward its target). A shooter killed in
// the same tick isn't in the frame, so fall back to the shot direction.
static int shooter_facing(int frame, const game::ShotEvent& sh) {
  const auto& units = g.frames[static_cast<size_t>(frame)].units;
  for (size_t i = 0; i < units.size(); ++i)
    if (units[i].id == sh.attacker) return g.unit_facing[static_cast<size_t>(frame)][i];
  const int fc = facing_from_step(sh.to.x - sh.from.x, sh.to.y - sh.from.y);
  return fc < 0 ? 0 : fc;
}

static void spawn_fx(int frame, double now) {
  const auto& fr = g.frames[static_cast<size_t>(frame)];
  const double k = g.playing ? std::min(1.0, kFxMaxTicks / g.fps / kFxTotal) : 1.0;

  // Impact time per shot, so each explosion waits for the shot that caused it.
  std::vector<double> impact(fr.shots.size());
  double last_impact = now;
  for (size_t i = 0; i < fr.shots.size(); ++i) {
    const auto& sh  = fr.shots[i];
    const WeaponFx w = weapon_fx(sh.attacker_type);
    const double t0 = now + (sh.first_strike ? 0.0 : kFxDelayNormal) * k;
    impact[i]   = t0 + (w.travel + w.stagger * (w.bolts - 1)) * k;
    last_impact = std::max(last_impact, impact[i]);
    if (!shot_visible(fr, sh)) continue;

    const ImVec4 fc = sh.faction.value == 0 ? kFactionA : kFactionB;
    const ImVec4 tc = mix_white(fc, sh.attacker_type == game::UnitType::Frigate ? 0.65f : 0.45f);
    const int sshape = shape_index(sh.attacker_type), sface = shooter_facing(frame, sh);
    for (int b = 0; b < w.bolts; ++b) {
      Effect e{FxKind::Tracer, sh.attacker_type, sh.from, sh.to, tc, t0 + b * w.stagger * k,
               w.travel * k};
      e.shooter_shape = sshape; e.shooter_facing = sface;
      g.fx.live.push_back(e);
    }
    if (w.beam) {
      Effect e{FxKind::Muzzle, sh.attacker_type, sh.from, sh.to, mix_white(fc, 0.7f), t0,
               kFxMuzzle * k, w.hit_size};
      e.shooter_shape = sshape; e.shooter_facing = sface;
      g.fx.live.push_back(e);
    }
    if (sh.splash) {
      Effect e{FxKind::Splash, sh.attacker_type, sh.to, sh.to, {1.0f, 0.55f, 0.18f, 1.0f},
               impact[i], kFxSplash * k};
      e.radius = splash_radius(sh);
      g.fx.live.push_back(e);
    } else {
      Effect e{FxKind::Hit, sh.attacker_type, sh.to, sh.to, {1, 1, 1, 1}, impact[i],
               kFxHit * k, w.hit_size};
      e.unit      = sh.target_unit;
      e.structure = sh.target_struct;
      g.fx.live.push_back(e);
    }
  }

  for (const auto& kl : fr.kills) {
    if (!tile_visible(fr, g.view, kl.pos.x, kl.pos.y)) continue;
    double t = -1.0;
    for (size_t i = 0; i < fr.shots.size(); ++i) {
      const auto& sh = fr.shots[i];
      const bool caused = (!kl.unit.is_null() && sh.target_unit == kl.unit) ||
                          (!kl.structure.is_null() && sh.target_struct == kl.structure) ||
                          (sh.splash && game::manhattan(sh.to, kl.pos) <= splash_radius(sh));
      if (caused) t = std::max(t, impact[i]);
    }
    if (t < 0.0) t = last_impact;

    const float size = burst_size(kl);
    Effect e{FxKind::Burst, game::UnitType::Drone, kl.pos, kl.pos, {1.0f, 0.62f, 0.22f, 1.0f},
             t, kFxBurst * (0.7 + 0.3 * size) * k, size};
    e.unit      = kl.unit;
    e.structure = kl.structure;
    const bool is_unit = !kl.unit.is_null();
    e.ghost_shape = is_unit ? shape_index(kl.unit_type) : shape_index(kl.structure_type);
    e.ghost_col   = kl.faction.value == 0 ? kFactionA : kFactionB;
    if (is_unit && frame > 0) {
      const auto& prev = g.frames[static_cast<size_t>(frame - 1)];
      for (size_t i = 0; i < prev.units.size(); ++i)
        if (prev.units[i].id == kl.unit)
          e.ghost_facing = g.unit_facing[static_cast<size_t>(frame - 1)][i];
    } else if (!is_unit) {
      e.ghost_facing = structure_facing(g.map, kl.pos.x, kl.pos.y);
    }
    g.fx.live.push_back(e);
  }
}

// Called once per UI frame, after input and playback have settled g.cur.
static void update_fx(double now) {
  auto& fx = g.fx;
  if (g.cur != fx.seen_frame) {
    const bool continuing = g.playing && g.cur == fx.seen_frame + 1;
    if (!continuing) fx.live.clear();
    fx.seen_frame = g.cur;
    fx.changed_t  = now;
    fx.pending    = true;
  }
  if (fx.pending && !(fx.scrubbing && now - fx.changed_t < kFxScrubSettle)) {
    spawn_fx(g.cur, now);
    fx.pending = false;
  }
  std::erase_if(fx.live, [&](const Effect& e) { return now > e.start + e.dur; });
}

// 0..1: how strongly an entity should flash white right now from hits on it.
static float hit_flash(bool is_struct, uint32_t id, game::Vec2 pos, double now) {
  float best = 0.0f;
  for (const auto& e : g.fx.live) {
    if ((e.kind != FxKind::Hit && e.kind != FxKind::Splash) || now < e.start ||
        now > e.start + e.dur)
      continue;
    const bool match =
        e.kind == FxKind::Splash ? game::manhattan(pos, e.to) <= e.radius
        : is_struct              ? !e.structure.is_null() && e.structure.value == id
                                 : !e.unit.is_null() && e.unit.value == id;
    if (match)
      best = std::max(best, 1.0f - static_cast<float>((now - e.start) / e.dur));
  }
  return best;
}

static ImVec2 arc_point(ImVec2 a, ImVec2 b, float p, float h) {
  return {a.x + (b.x - a.x) * p, a.y + (b.y - a.y) * p - h * 4.0f * p * (1.0f - p)};
}

static void draw_fx(ImDrawList* dl, ImVec2 iso_org, float cw2, float ch2, double now) {
  auto ctr = [&](game::Vec2 t, float lift) -> ImVec2 {
    return {iso_org.x + (t.x - t.y) * cw2, iso_org.y + (t.x + t.y) * ch2 - lift * ch2};
  };
  auto with_a = [](ImVec4 c, float a) { return u32({c.x, c.y, c.z, c.w * a}); };
  auto arc_h  = [&](game::Vec2 a, game::Vec2 b) {
    return ch2 * (1.2f + 0.45f * static_cast<float>(game::manhattan(a, b)));
  };
  const float impact = 0.5f; // ch2 units above the target tile, roughly hull top
  auto muzzle_at = [&](game::Vec2 tile, int shape, int facing) {
    const ImVec2 c = ctr(tile, 0.0f);
    return shape >= 0 ? shape_muzzle(g.shape_defs[shape], c, cw2, ch2, facing)
                      : ImVec2{c.x, c.y - kDefaultMuzzleZ * ch2};
  };

  const bool idle = !g.playing && g.fx.live.empty() && !g.fx.pending;
  if (idle && g.fx.show_static) {
    const auto& fr = g.frames[static_cast<size_t>(g.cur)];
    for (const auto& sh : fr.shots) {
      if (!shot_visible(fr, sh)) continue;
      const ImVec4 fc = mix_white(sh.faction.value == 0 ? kFactionA : kFactionB, 0.3f);
      const ImVec2 a = muzzle_at(sh.from, shape_index(sh.attacker_type), shooter_facing(g.cur, sh));
      const ImVec2 b = ctr(sh.to, impact);
      if (weapon_fx(sh.attacker_type).arc) {
        ImVec2 pts[13];
        for (int i = 0; i <= 12; ++i) pts[i] = arc_point(a, b, i / 12.0f, arc_h(sh.from, sh.to));
        dl->AddPolyline(pts, 13, with_a(fc, 0.45f), 0, 1.5f);
      } else {
        dl->AddLine(a, b, with_a(fc, 0.45f), 1.5f);
      }
      dl->AddCircleFilled(b, std::max(2.0f, ch2 * 0.12f), with_a(fc, 0.6f));
    }
  }

  // Victims first, so tracers and explosions draw over them.
  for (const auto& e : g.fx.live) {
    if (e.ghost_shape < 0 || now > e.start + e.dur * 0.15) continue;
    const bool is_struct = e.unit.is_null();
    const float f = hit_flash(is_struct, is_struct ? e.structure.value : e.unit.value, e.to, now);
    draw_unit_from_def(dl, ctr(e.to, 0.0f), cw2, ch2, g.shape_defs[e.ghost_shape],
                       u32(mix_white(e.ghost_col, 0.75f * f)), e.ghost_facing);
  }

  for (const auto& e : g.fx.live) {
    if (now < e.start) continue;
    const float p = static_cast<float>(std::clamp((now - e.start) / e.dur, 0.0, 1.0));
    const float q = 1.0f - p;
    switch (e.kind) {
    case FxKind::Tracer: {
      const WeaponFx w = weapon_fx(e.weapon);
      const ImVec2 a = muzzle_at(e.from, e.shooter_shape, e.shooter_facing);
      const ImVec2 b = ctr(e.to, impact);
      const float  wd = std::max(1.2f, ch2 * w.width);
      if (w.arc) {
        const float h = arc_h(e.from, e.to);
        ImVec2 trail[8];
        for (int i = 0; i < 8; ++i)
          trail[i] = arc_point(a, b, std::max(0.0f, p - 0.2f * (1.0f - i / 7.0f)), h);
        dl->AddPolyline(trail, 8, with_a(e.col, 0.45f), 0, wd);
        const ImVec2 shell = trail[7];
        dl->AddCircleFilled(shell, wd * 2.4f, with_a(e.col, 0.25f));
        dl->AddCircleFilled(shell, wd * 1.1f, with_a(mix_white(e.col, 0.6f), 1.0f));
        break;
      }
      const float head = 1.0f - q * q; // ease-out
      const float tail = std::max(0.0f, head - w.tail);
      const ImVec2 h = {a.x + (b.x - a.x) * head, a.y + (b.y - a.y) * head};
      const ImVec2 t = {a.x + (b.x - a.x) * tail, a.y + (b.y - a.y) * tail};
      auto bolt = [&](ImVec2 off) {
        const ImVec2 t2{t.x + off.x, t.y + off.y}, h2{h.x + off.x, h.y + off.y};
        dl->AddLine(t2, h2, with_a(e.col, 0.25f), wd * 3.0f);
        dl->AddLine(t2, h2, with_a(e.col, 0.95f), wd);
        dl->AddCircleFilled(h2, wd * 1.1f, with_a(mix_white(e.col, 0.6f), 1.0f));
      };
      if (w.beam) {
        const float dx = b.x - a.x, dy = b.y - a.y;
        const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
        const ImVec2 n = {-dy / len * wd * 1.2f, dx / len * wd * 1.2f};
        bolt(n);
        bolt({-n.x, -n.y});
      } else {
        bolt({0.0f, 0.0f});
      }
      break;
    }
    case FxKind::Muzzle: {
      const ImVec2 c = muzzle_at(e.from, e.shooter_shape, e.shooter_facing);
      const float r = ch2 * 0.35f * e.size * (0.6f + 0.6f * p);
      dl->AddCircleFilled(c, r, with_a(e.col, 0.85f * q));
      dl->AddCircleFilled(c, r * 0.5f, with_a({1, 1, 1, 1}, q));
      break;
    }
    case FxKind::Hit: {
      const ImVec2 c = ctr(e.to, impact);
      const float r = ch2 * (0.25f + 0.55f * p) * e.size;
      dl->AddEllipseFilled(c, {r, r * 0.7f}, with_a(e.col, 0.55f * q));
      dl->AddEllipse(c, {r * 1.2f, r * 0.84f}, with_a(e.col, 0.8f * q), 0.0f, 0, 1.5f);
      break;
    }
    case FxKind::Splash: {
      // Light up every tile in the blast, friendlies included, then a ground ring.
      for (int32_t dy = -e.radius; dy <= e.radius; ++dy)
        for (int32_t dx = -e.radius; dx <= e.radius; ++dx) {
          if (std::abs(dx) + std::abs(dy) > e.radius) continue;
          const ImVec2 c = ctr({e.to.x + dx, e.to.y + dy}, 0.0f);
          const ImVec2 quad[4] = {{c.x, c.y - ch2}, {c.x + cw2, c.y}, {c.x, c.y + ch2},
                                  {c.x - cw2, c.y}};
          dl->AddConvexPolyFilled(quad, 4, with_a(e.col, 0.45f * q * q));
        }
      const ImVec2 c = ctr(e.to, 0.0f);
      const float rx = cw2 * (0.6f + (e.radius + 0.6f) * p);
      dl->AddEllipse(c, {rx, rx * 0.5f}, with_a(mix_white(e.col, 0.4f), 0.9f * q), 0.0f, 0,
                     2.5f);
      if (p < 0.35f)
        dl->AddEllipseFilled(ctr(e.to, impact), {cw2 * 0.45f, ch2 * 0.6f},
                             with_a({1, 1, 1, 1}, 1.0f - p / 0.35f));
      break;
    }
    case FxKind::Burst: {
      const ImVec2 c = ctr(e.to, impact);
      const float r = ch2 * (0.4f + 1.1f * p) * e.size;
      const ImVec4 hot = mix_white(e.col, 0.5f * q);
      dl->AddEllipseFilled(c, {r, r * 0.65f}, with_a(hot, 0.7f * q * q));
      dl->AddEllipse(c, {r * 1.25f, r * 0.8f}, with_a(e.col, 0.9f * q), 0.0f, 0, 2.0f);
      const int sparks = e.size > 1.5f ? 12 : 8;
      for (int i = 0; i < sparks; ++i) {
        const float ang = 6.2831853f * static_cast<float>(i) / sparks + 0.3f;
        const float r0 = r * 0.6f, r1 = r * 1.5f;
        dl->AddLine({c.x + std::cos(ang) * r0, c.y + std::sin(ang) * r0 * 0.6f},
                    {c.x + std::cos(ang) * r1, c.y + std::sin(ang) * r1 * 0.6f},
                    with_a(hot, 0.8f * q), 1.5f);
      }
      if (e.size > 1.8f) { // structures: a slower ground shockwave
        const ImVec2 g0 = ctr(e.to, 0.0f);
        const float sr = cw2 * (0.5f + 1.8f * p) * (e.size / 2.0f);
        dl->AddEllipse(g0, {sr, sr * 0.5f}, with_a(mix_white(e.col, 0.3f), 0.7f * q), 0.0f, 0,
                       3.0f);
      }
      break;
    }
    }
  }
}

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
      if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        g.fx.live.clear();
        spawn_fx(g.cur, glfwGetTime());
      }
      if (ImGui::IsKeyPressed(ImGuiKey_T)) g.fx.show_static = !g.fx.show_static;
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
  g.fx.scrubbing = ImGui::IsItemActive();
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

  if (cd.shape >= 0) {
    const float flash = hit_flash(cd.is_struct, cd.id, {x, y}, glfwGetTime());
    draw_unit_from_def(dl, ctr, cw2, ch2, g.shape_defs[cd.shape],
                       u32(mix_white(cd.fg, 0.75f * flash)), cd.facing);
  }

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

  draw_fx(dl, iso_org, cw2, ch2, glfwGetTime());

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

// Selected unit/structure drawn large on its own tile, in faction colour and
// facing the same way it does on the map.
static void draw_portrait(int shape, ImVec4 col, int facing) {
  const ImVec2 size = {150.0f, 120.0f};
  const ImVec2 p0   = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl  = ImGui::GetWindowDrawList();
  const float cw2 = 52.0f, ch2 = 26.0f;
  const ImVec2 c  = {p0.x + size.x * 0.5f, p0.y + size.y * 0.70f};
  dl->AddRectFilled(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(14, 14, 20, 255), 4.0f);
  ImVec2 top{c.x, c.y - ch2}, right{c.x + cw2, c.y}, bottom{c.x, c.y + ch2}, left{c.x - cw2, c.y};
  dl->AddTriangleFilled(top, right, left, u32(kOpenBg));
  dl->AddTriangleFilled(right, bottom, left, u32(dim_vec(kOpenBg, 0.70f)));
  dl->AddQuad(top, right, bottom, left, u32({0.0f, 0.0f, 0.0f, 0.40f}), 0.8f);
  dl->PushClipRect(p0, {p0.x + size.x, p0.y + size.y}, true);
  draw_unit_from_def(dl, c, cw2, ch2, g.shape_defs[shape], u32(col), facing);
  dl->PopClipRect();
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
    int facing = 0;
    for (size_t i = 0; i < fr.units.size(); ++i) {
      if (fr.units[i].id != g.sel_id) continue;
      sel = &fr.units[i];
      const auto& facings = g.unit_facing[static_cast<size_t>(g.cur)];
      if (i < facings.size()) facing = facings[i];
      break;
    }

    if (!sel) {
      g.has_sel = false;
    } else {
      ImVec4 fc = sel->faction.value == 0 ? kFactionA : kFactionB;
      ImGui::TextColored(fc, "%s", game::unit_type_name(sel->type));
      ImGui::SameLine();
      ImGui::TextColored(fc, "[%s]", sel->faction.value == 0 ? "A" : "B");
      ImGui::Separator();
      draw_portrait(shape_index(sel->type), fc, facing);
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
      draw_portrait(shape_index(sel->type), fc, structure_facing(g.map, sel->pos.x, sel->pos.y));
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
  if (load_shape_defs())
    fprintf(stderr, "Shapes: %s\n", g.shapes_path.c_str());
  else
    fprintf(stderr, "Shapes: %s not found, using built-in shapes\n", g.shapes_path.c_str());

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
    if (g.loaded) update_fx(glfwGetTime());

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
