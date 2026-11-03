// Usage: ud_viz_imgui [replay.ud] [--shapes file.udshapes]   (shapes hot-reload on save)
// Controls: Space=play/pause  ←/→=step  U=union  G=god  0/1=faction view  scroll=zoom  Q=quit
//           R=replay this frame's shots  T=toggle static shot lines while paused
//           Z=toggle claim zones (territory)

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>

#include "config.hpp"
#include "vision.hpp"
#include "entity.hpp"
#include "frame.hpp"
#include "grid.hpp"
#include "replay.hpp"
#include "replay_io.hpp"
#include "shape_model.hpp"
#include "stats.hpp"
#include "territory.hpp"
#include "wincheck.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
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

static constexpr ImVec4 kOpenBg   = {0.14f, 0.14f, 0.18f, 1.0f};
static constexpr ImVec4 kAsteroid = {0.19f, 0.17f, 0.15f, 1.0f}; // dust under the rocks
static constexpr ImVec4 kNebula   = {0.17f, 0.13f, 0.26f, 1.0f}; // ground under the gas
static constexpr ImVec4 kResource = {0.24f, 0.20f, 0.14f, 1.0f}; // ground under the crystals
static constexpr ImVec4 kCargo    = {1.0f, 0.72f, 0.18f, 1.0f}; // Drone hold bar
static constexpr ImVec4 kShield   = {0.35f, 0.85f, 1.0f, 1.0f};  // shield bar, inside the HP bar
static constexpr float  kGhostAlpha = 0.45f; // a structure still under construction

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
  bool     fogged{false};
  float    rock{0.0f}; // asteroid brightness to draw at; 0 = no asteroid
  float    cargo{0.0f}; // Drone hold fill, 0..1
  float    crystal{0.0f}; // resource-node brightness to draw at; 0 = no node
  float    built{1.0f};   // construction progress, 0..1; below 1 it draws as a ghost
  float    gas{0.0f};     // nebula brightness to draw at; 0 = no nebula
  int32_t  shield{0}, max_shield{0};
};

// ---- Cell data builder ----------------------------------------------------

// Structures never move; turn them toward the map centre so bases face each other.
static int structure_facing(const game::Map& map, int32_t x, int32_t y) {
  int facing = facing_from_step(map.width / 2 - x, map.height / 2 - y);
  return facing < 0 ? 0 : facing;
}

// view: -1 = god (everything), -2 = union of both factions, 0/1 = that faction.
// Line of sight this frame: what units and structures can be shown. Terrain is public (every
// bot gets the whole map at init), so out of sight it is only dimmed.
static bool tile_visible(const game::Map& map, const game::FrameState& fr, int view, int32_t x,
                         int32_t y) {
  if (view == -1) return true;
  for (const auto& u : fr.units) {
    if (view >= 0 && static_cast<int>(u.faction.value) != view) continue;
    if (game::sees_tile(map, u.pos, game::stats_for(u.type).sight, {x, y})) return true;
  }
  for (const auto& s : fr.structures) {
    if (view >= 0 && static_cast<int>(s.faction.value) != view) continue;
    if (s.hp > 0 && game::sees_tile(map, s.pos, game::cfg::STRUCTURE_SIGHT, {x, y})) return true;
  }
  return false;
}

static constexpr float kOutOfSightDim = 0.45f;

static ImVec4 terrain_bg(const game::Map& map, int32_t x, int32_t y) {
  switch (map.tile_at({x, y}).terrain) {
    case game::Terrain::Asteroid:     return kAsteroid;
    case game::Terrain::Nebula:       return kNebula;
    case game::Terrain::ResourceNode: return kResource;
    default:                          return kOpenBg;
  }
}

static float cargo_fill(const game::UnitFrame& u) {
  return std::max(static_cast<float>(u.cargo_energy) / game::cfg::DRONE_CARGO_ENERGY,
                  static_cast<float>(u.cargo_alloy) / game::cfg::DRONE_CARGO_ALLOY);
}

static CellData make_cell(const game::Map& map, const game::FrameState& fr,
                           const std::vector<uint8_t>& facings, int view, int32_t x, int32_t y) {
  if (!tile_visible(map, fr, view, x, y)) {
    CellData fog{dim_vec(terrain_bg(map, x, y), kOutOfSightDim)};
    fog.fogged = true;
    return fog;
  }

  const ImVec4 bg = terrain_bg(map, x, y);

  for (const auto& s : fr.structures) {
    if (s.pos.x != x || s.pos.y != y) continue;
    char g = s.type == game::StructureType::CommandCore ? 'C' :
             s.type == game::StructureType::Factory     ? 'T' : 'N';
    ImVec4 fg = s.faction.value == 0 ? kFactionA : kFactionB;
    CellData cd{bg, g, false, game::UnitType::Drone, shape_index(s.type), fg, s.hp, s.max_hp,
                structure_facing(map, x, y), s.id.value, true};
    if (s.build_ticks_left > 0)
      cd.built = 1.0f - static_cast<float>(s.build_ticks_left) / game::cfg::BUILD_TICKS;
    return cd;
  }

  int top = -1;
  for (size_t i = 0; i < fr.units.size(); ++i)
    if (fr.units[i].pos.x == x && fr.units[i].pos.y == y) top = static_cast<int>(i);
  if (top >= 0) {
    const auto& u = fr.units[top];
    ImVec4 fg = u.faction.value == 0 ? kFactionA : kFactionB;
    int facing = top < static_cast<int>(facings.size()) ? facings[top] : 0;
    CellData cd{bg, '\0', true, u.type, shape_index(u.type), fg, u.hp, u.max_hp, facing,
                u.id.value};
    cd.cargo = cargo_fill(u);
    cd.shield = u.shield;
    cd.max_shield = u.max_shield;
    return cd;
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
  bool   claim = false; // Tracer: a Claim Node fired, so kClaimFx styles it
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
  uint32_t          forfeit_total[game::MAX_FACTIONS]{}; // stalled ticks per side, whole replay
  std::string       shapes_path = UD_DEFAULT_SHAPES_PATH;
  std::filesystem::file_time_type shapes_mtime{};
  double            shapes_checked_t = 0.0;
  uint32_t          tick_cap = 0;
  bool              show_zones = true;
  int               zones_frame = -1;     // frame `zones` was computed for
  std::vector<int8_t> zones;              // territory owner per tile (territory_owners)
  game::TerritoryState zone_pct;
} g;

// Recomputed from the frame's live Claim Nodes with the engine's own rule, so the zones drawn
// are exactly the territory the win check counted.
static void update_zones() {
  if (g.zones_frame == g.cur) return;
  std::vector<game::ClaimAnchor> claims;
  for (const auto& s : g.frames[static_cast<size_t>(g.cur)].structures)
    if (s.type == game::StructureType::ClaimNode && s.hp > 0 && s.build_ticks_left == 0)
      claims.push_back({s.pos, s.faction.value});
  g.zones = game::territory_owners(g.map, claims);
  g.zone_pct = game::territory_pct(g.map, g.zones);
  g.zones_frame = g.cur;
}

static int8_t zone_at(int32_t x, int32_t y) {
  if (x < 0 || y < 0 || x >= g.map.width || y >= g.map.height) return -1;
  return g.zones[static_cast<size_t>(y) * static_cast<size_t>(g.map.width) +
                 static_cast<size_t>(x)];
}

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
      if (fc >= 0 && !sh.attacker.is_null()) fired[sh.attacker] = fc;
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
    uint64_t fingerprint = 0;
    g.map    = map_from_log(log);
    g.frames = game::replay_frames(log, &fingerprint);
    if (fingerprint != log.expected_hash)
      fprintf(stderr, "warning: hash mismatch — replay may not reproduce original match\n");
    compute_unit_facings();
    g.fx     = FxState{};
    for (auto& n : g.forfeit_total) n = 0;
    for (const auto& f : g.frames)
      for (int i = 0; i < game::MAX_FACTIONS; ++i)
        if (f.forfeits[static_cast<size_t>(i)] != game::TickFault::None) ++g.forfeit_total[i];
    g.cur    = 0;
    g.total  = static_cast<int>(g.frames.size());
    g.tick_cap    = log.tick_cap;
    g.zones_frame = -1;
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
  return tile_visible(g.map, fr, g.view, sh.from.x, sh.from.y) ||
         tile_visible(g.map, fr, g.view, sh.to.x, sh.to.y);
}

// A Claim Node's shot: a thin, quick zap.
static constexpr WeaponFx kClaimFx = {0.12, 1, 0.00, 0.06f, 0.55f, 0.5f, false, false};

static bool claim_shot(const game::ShotEvent& sh) { return !sh.attacker_struct.is_null(); }

static WeaponFx shot_fx(const game::ShotEvent& sh) {
  return claim_shot(sh) ? kClaimFx : weapon_fx(sh.attacker_type);
}

static int shooter_shape(const game::ShotEvent& sh) {
  return claim_shot(sh) ? shape_index(game::StructureType::ClaimNode)
                        : shape_index(sh.attacker_type);
}

static int32_t splash_radius(const game::ShotEvent& sh) {
  return game::stats_for(sh.attacker_type).splash_radius;
}

// The shooter's facing on `frame` (it turned toward its target). A shooter killed in
// the same tick isn't in the frame, so fall back to the shot direction.
static int shooter_facing(int frame, const game::ShotEvent& sh) {
  if (claim_shot(sh)) return structure_facing(g.map, sh.from.x, sh.from.y);
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
    const WeaponFx w = shot_fx(sh);
    const double t0 = now + (sh.first_strike ? 0.0 : kFxDelayNormal) * k;
    impact[i]   = t0 + (w.travel + w.stagger * (w.bolts - 1)) * k;
    last_impact = std::max(last_impact, impact[i]);
    if (!shot_visible(fr, sh)) continue;

    const ImVec4 fc = sh.faction.value == 0 ? kFactionA : kFactionB;
    const ImVec4 tc = mix_white(fc, sh.attacker_type == game::UnitType::Frigate ? 0.65f : 0.45f);
    const int sshape = shooter_shape(sh), sface = shooter_facing(frame, sh);
    for (int b = 0; b < w.bolts; ++b) {
      Effect e{FxKind::Tracer, sh.attacker_type, sh.from, sh.to, tc, t0 + b * w.stagger * k,
               w.travel * k};
      e.shooter_shape = sshape; e.shooter_facing = sface; e.claim = claim_shot(sh);
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
    if (!tile_visible(g.map, fr, g.view, kl.pos.x, kl.pos.y)) continue;
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
      const ImVec2 a = muzzle_at(sh.from, shooter_shape(sh), shooter_facing(g.cur, sh));
      const ImVec2 b = ctr(sh.to, impact);
      if (shot_fx(sh).arc) {
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
      const WeaponFx w = e.claim ? kClaimFx : weapon_fx(e.weapon);
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
      if (ImGui::IsKeyPressed(ImGuiKey_Z)) g.show_zones = !g.show_zones;
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
  ImGui::Checkbox("Zones", &g.show_zones);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(65.0f);
  ImGui::SliderFloat("px##z", &g.cs, 16.0f, 128.0f, "%.0f");
  ImGui::SameLine();
  ImGui::TextColored(kFactionA, "A  e:%-4d a:%d",
      fr.resources[0].energy, fr.resources[0].alloy);
  ImGui::SameLine();
  ImGui::TextColored(kFactionB, "B  e:%-4d a:%d",
      fr.resources[1].energy, fr.resources[1].alloy);

  // A side that forfeited this tick had its commands discarded: its units stood still.
  for (int i = 0; i < game::MAX_FACTIONS; ++i) {
    const auto fault = fr.forfeits[static_cast<size_t>(i)];
    if (fault == game::TickFault::None) continue;
    ImGui::SameLine();
    ImGui::TextColored({1.0f, 0.3f, 0.25f, 1.0f}, " | %s STALLED (%s) [%u in replay]",
                       i == 0 ? "A" : "B", game::tick_fault_name(fault), g.forfeit_total[i]);
  }

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

// ---- Asteroids --------------------------------------------------------------
//
// One to three lumpy boulders per tile. Shapes come from a hash of the tile position: they
// vary across the field but never change between frames.

static constexpr ImVec4 kRockBase   = {0.40f, 0.36f, 0.31f, 1.0f};
static constexpr ImVec4 kRockLit    = {0.56f, 0.51f, 0.44f, 1.0f};
static constexpr ImVec4 kRockShadow = {0.22f, 0.19f, 0.17f, 1.0f};
static constexpr int    kRockPoints = 11;

static uint32_t tile_hash(int32_t x, int32_t y, uint32_t salt) {
  uint32_t h = static_cast<uint32_t>(x) * 0x9E3779B1u ^ static_cast<uint32_t>(y) * 0x85EBCA77u ^
               salt * 0xC2B2AE3Du;
  h ^= h >> 15;
  h *= 0x2C1B3C6Du;
  h ^= h >> 12;
  return h;
}

// Uniform in [0, 1) from the hash stream for this tile.
struct TileRand {
  int32_t x, y;
  uint32_t n = 0;
  float next() { return static_cast<float>(tile_hash(x, y, n++) & 0xFFFF) / 65536.0f; }
};

static void draw_boulder(ImDrawList* dl, ImVec2 c, float rx, float ry, float bright,
                         TileRand& rnd) {
  auto col = [&](ImVec4 v) { return u32(dim_vec(v, bright)); };
  dl->AddEllipseFilled({c.x + rx * 0.15f, c.y + ry * 0.55f}, {rx * 1.05f, ry * 0.45f},
                       u32({0.0f, 0.0f, 0.0f, 0.35f * bright}));

  ImVec2 body[kRockPoints], lit[kRockPoints];
  const float phase = rnd.next() * 6.2832f;
  for (int i = 0; i < kRockPoints; ++i) {
    const float a = phase + 6.2832f * static_cast<float>(i) / kRockPoints;
    const float r = 0.78f + 0.22f * rnd.next();
    body[i] = {c.x + std::cos(a) * rx * r, c.y + std::sin(a) * ry * r};
    // The lit face: the same outline shrunk toward the upper left.
    lit[i] = {c.x - rx * 0.16f + std::cos(a) * rx * r * 0.68f,
              c.y - ry * 0.22f + std::sin(a) * ry * r * 0.62f};
  }
  dl->AddConcavePolyFilled(body, kRockPoints, col(kRockShadow));
  ImVec2 mid[kRockPoints];
  for (int i = 0; i < kRockPoints; ++i)
    mid[i] = {(body[i].x * 0.55f + lit[i].x * 0.45f), (body[i].y * 0.55f + lit[i].y * 0.45f)};
  dl->AddConcavePolyFilled(mid, kRockPoints, col(kRockBase));
  dl->AddConcavePolyFilled(lit, kRockPoints, col(kRockLit));

  const int craters = 1 + static_cast<int>(rnd.next() * 2.0f);
  for (int i = 0; i < craters; ++i) {
    const ImVec2 p = {c.x + (rnd.next() - 0.6f) * rx * 0.9f, c.y + (rnd.next() - 0.6f) * ry * 0.8f};
    const float cr = rx * (0.10f + 0.10f * rnd.next());
    dl->AddEllipseFilled(p, {cr, cr * 0.6f}, col(kRockShadow));
    dl->AddEllipse({p.x + cr * 0.15f, p.y + cr * 0.12f}, {cr, cr * 0.6f}, col(kRockLit), 0.0f, 0,
                   std::max(1.0f, rx * 0.04f));
  }
  dl->AddPolyline(body, kRockPoints, u32({0.0f, 0.0f, 0.0f, 0.55f * bright}), ImDrawFlags_Closed,
                  std::max(1.0f, rx * 0.05f));
}

static void draw_asteroid(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2, int32_t x, int32_t y,
                          float bright) {
  TileRand rnd{x, y};
  const int count = 1 + static_cast<int>(rnd.next() * 3.0f);
  struct Rock { ImVec2 c; float s; };
  Rock rocks[3];
  for (int i = 0; i < count; ++i) {
    // The first rock is big and central; extras are smaller and pushed toward the edges.
    const float s = i == 0 ? 0.62f + 0.22f * rnd.next() : 0.28f + 0.18f * rnd.next();
    const float spread = i == 0 ? 0.15f : 0.55f;
    rocks[i] = {{ctr.x + (rnd.next() * 2.0f - 1.0f) * cw2 * spread,
                 ctr.y + (rnd.next() * 2.0f - 1.0f) * ch2 * spread},
                s};
  }
  // Back to front, so nearer rocks overlap farther ones.
  std::sort(rocks, rocks + count, [](const Rock& a, const Rock& b) { return a.c.y < b.c.y; });
  for (int i = 0; i < count; ++i) {
    const float rx = cw2 * rocks[i].s;
    const float ry = rx * 0.78f; // taller than the ground's 2:1, so they read as raised
    draw_boulder(dl, {rocks[i].c.x, rocks[i].c.y - ry * 0.45f}, rx, ry, bright, rnd);
  }
}

// Resource nodes are clusters of amber crystal shards: the same hue as a Drone's cargo bar, so
// what a Drone carries visibly comes from them.
static constexpr ImVec4 kCrystalLit    = {1.00f, 0.86f, 0.42f, 1.0f};
static constexpr ImVec4 kCrystalBase   = {0.95f, 0.62f, 0.14f, 1.0f};
static constexpr ImVec4 kCrystalShadow = {0.55f, 0.30f, 0.06f, 1.0f};
static constexpr ImVec4 kCrystalGlow   = {1.00f, 0.70f, 0.20f, 0.22f};

// One shard: a pointed prism leaning by `lean`, its left face in shadow and its right face lit.
static void draw_shard(ImDrawList* dl, ImVec2 base, float w, float h, float lean, float bright) {
  auto col = [&](ImVec4 v) { return u32(dim_vec(v, bright)); };
  const float shoulder = 0.72f;
  const ImVec2 bl = {base.x - w * 0.5f, base.y};
  const ImVec2 bm = {base.x, base.y + w * 0.22f}; // front edge, a little lower: reads as 3D
  const ImVec2 br = {base.x + w * 0.5f, base.y};
  const ImVec2 sl = {bl.x + lean * h * shoulder, bl.y - h * shoulder};
  const ImVec2 sm = {bm.x + lean * h * shoulder, bm.y - h * shoulder};
  const ImVec2 sr = {br.x + lean * h * shoulder, br.y - h * shoulder};
  const ImVec2 tip = {base.x + lean * h, base.y - h};

  const ImVec2 left_face[4] = {bl, bm, sm, sl};
  const ImVec2 right_face[4] = {bm, br, sr, sm};
  dl->AddConvexPolyFilled(left_face, 4, col(kCrystalBase));
  dl->AddConvexPolyFilled(right_face, 4, col(kCrystalLit));
  dl->AddTriangleFilled(sl, sm, tip, col(kCrystalShadow));
  dl->AddTriangleFilled(sm, sr, tip, col(kCrystalBase));

  const ImVec2 outline[6] = {bl, bm, br, sr, tip, sl};
  const float lw = std::max(1.0f, w * 0.08f);
  dl->AddPolyline(outline, 6, u32({0.15f, 0.07f, 0.0f, 0.8f * bright}), ImDrawFlags_Closed, lw);
  dl->AddLine({bm.x + w * 0.06f, bm.y - h * 0.08f}, {sm.x + w * 0.06f, sm.y - h * 0.02f},
              u32({1.0f, 0.97f, 0.85f, 0.8f * bright}), lw);
}

static void draw_crystals(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2, int32_t x, int32_t y,
                          float bright) {
  dl->AddEllipseFilled({ctr.x, ctr.y}, {cw2 * 0.62f, ch2 * 0.62f},
                       u32({kCrystalGlow.x, kCrystalGlow.y, kCrystalGlow.z,
                            kCrystalGlow.w * bright}));
  TileRand rnd{x, y, 101};
  struct Shard { ImVec2 base; float h, w, lean; };
  Shard shards[4];
  const int count = 3 + static_cast<int>(rnd.next() * 2.0f);
  for (int i = 0; i < count; ++i) {
    // The first shard is the tall centrepiece; the rest are shorter and splay outward.
    const float ox = i == 0 ? 0.0f : (rnd.next() * 2.0f - 1.0f) * 0.42f;
    const float oy = i == 0 ? 0.0f : (rnd.next() * 2.0f - 1.0f) * 0.38f;
    const float h = cw2 * (i == 0 ? 0.95f + 0.25f * rnd.next() : 0.45f + 0.30f * rnd.next());
    shards[i] = {{ctr.x + ox * cw2, ctr.y + oy * ch2 + ch2 * 0.15f}, h,
                 cw2 * (i == 0 ? 0.30f : 0.20f + 0.06f * rnd.next()),
                 i == 0 ? (rnd.next() - 0.5f) * 0.12f : ox * 0.55f};
  }
  std::sort(shards, shards + count,
            [](const Shard& a, const Shard& b) { return a.base.y < b.base.y; });
  for (int i = 0; i < count; ++i)
    draw_shard(dl, shards[i].base, shards[i].w, shards[i].h, shards[i].lean, bright);
}

// Nebulae are drifting gas: overlapping translucent puffs that spill past their tile, so a
// patch reads as one cloud. Drawn twice: thick under units, then a thin veil over everything,
// so a ship inside looks like it is in the gas.
static constexpr ImVec4 kGasDeep  = {0.46f, 0.24f, 0.70f, 1.0f};
static constexpr ImVec4 kGasLight = {0.42f, 0.55f, 0.92f, 1.0f};
static constexpr int    kGasPuffs = 6;
static constexpr int    kGasRings = 4;
static constexpr float  kGasDrift = 0.07f;  // fraction of a tile the puffs wander
static constexpr float  kGasSpeed = 0.22f;  // radians per second
static constexpr float  kGasUnder = 0.55f;  // puff opacity beneath units (summed over rings)
static constexpr float  kGasVeil  = 0.22f;  // ...and over them

static void draw_gas(ImDrawList* dl, ImVec2 ctr, float cw2, float ch2, int32_t x, int32_t y,
                     float bright, double now, float opacity) {
  TileRand rnd{x, y, 202};
  for (int i = 0; i < kGasPuffs; ++i) {
    const float phase = rnd.next() * 6.2832f;
    const float t = static_cast<float>(now) * kGasSpeed + phase;
    const float ox = rnd.next() * 2.0f - 1.0f;
    const float oy = rnd.next() * 2.0f - 1.0f;
    const ImVec2 c = {ctr.x + (ox * 0.5f + std::sin(t) * kGasDrift) * cw2,
                      ctr.y + (oy * 0.5f + std::cos(t * 0.8f) * kGasDrift) * ch2};
    const float rx = cw2 * (0.55f + 0.30f * rnd.next());
    const float k = rnd.next();
    const ImVec4 tone = {kGasDeep.x + (kGasLight.x - kGasDeep.x) * k,
                         kGasDeep.y + (kGasLight.y - kGasDeep.y) * k,
                         kGasDeep.z + (kGasLight.z - kGasDeep.z) * k, 1.0f};
    const float a = opacity * bright * (0.75f + 0.25f * std::sin(t * 1.3f));
    // Stacked rings, faint and wide to dense and small: the puff thins out at its edge.
    for (int ring = 0; ring < kGasRings; ++ring) {
      const float s = 1.0f - 0.22f * static_cast<float>(ring);
      const float lift = 0.06f * static_cast<float>(ring);
      dl->AddEllipseFilled({c.x - rx * 0.08f * ring, c.y - rx * 0.05f * ring},
                           {rx * s, rx * s * 0.55f},
                           u32({tone.x + lift, tone.y + lift, tone.z + lift * 0.5f,
                                a / kGasRings}));
    }
  }
}

// ---- Claim zones ------------------------------------------------------------

static constexpr float kZoneTint     = 0.30f; // faction colour mixed into an owned tile
static constexpr float kZoneFogTint  = 0.18f; // ...into a fogged one
static constexpr float kZoneEdgeInset = 0.86f; // edges drawn inside the tile, so a neighbour's
                                               // fill can't cover them

static ImVec4 faction_color(int8_t f) { return f == 0 ? kFactionA : kFactionB; }

static ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w};
}

// A faction knows where its own claims are, so in its own view its territory shows through
// fog; the enemy's shows only where it can see.
static bool zone_shown(int8_t owner, const CellData& cd) {
  if (!g.show_zones || owner < 0) return false;
  return !cd.fogged || g.view < 0 || g.view == owner;
}

static void tint_zone(CellData& cd, int32_t x, int32_t y) {
  const int8_t owner = zone_at(x, y);
  if (!zone_shown(owner, cd)) return;
  cd.bg = mix(cd.bg, faction_color(owner), cd.fogged ? kZoneFogTint : kZoneTint);
}

// Outline each tile edge where the owner changes, in this tile's owner's colour: a border
// between two zones shows both colours side by side.
static void draw_zone_edges(ImDrawList* dl, ImVec2 ctr, ImVec2 top, ImVec2 right, ImVec2 bottom,
                            ImVec2 left, const CellData& cd, int32_t x, int32_t y) {
  const int8_t owner = zone_at(x, y);
  if (!zone_shown(owner, cd)) return;
  auto in = [&](ImVec2 p) {
    return ImVec2{ctr.x + (p.x - ctr.x) * kZoneEdgeInset, ctr.y + (p.y - ctr.y) * kZoneEdgeInset};
  };
  // DIRS order (N, E, S, W) → the diamond edge shared with that neighbour.
  const ImVec2 edges[4][2] = {{top, right}, {right, bottom}, {bottom, left}, {left, top}};
  const ImVec4 fc = faction_color(owner);
  const ImU32 col = u32({fc.x, fc.y, fc.z, cd.fogged ? 0.45f : 0.9f});
  const float w = std::max(1.0f, g.cs / 32.0f);
  for (int i = 0; i < 4; ++i) {
    const game::Vec2 n = game::Vec2{x, y} + game::DIRS[i];
    if (zone_at(n.x, n.y) == owner) continue;
    dl->AddLine(in(edges[i][0]), in(edges[i][1]), col, w);
  }
}

// Territory held against the threshold that wins on this tick (it falls as the match goes on).
static void draw_territory_bar(const game::FrameState& fr) {
  ImGui::Text("Territory");
  ImGui::SameLine();
  ImGui::TextColored(kFactionA, "A %u%%", g.zone_pct.pct_faction[0]);
  ImGui::SameLine();
  ImGui::TextColored(kFactionB, "B %u%%", g.zone_pct.pct_faction[1]);
  ImGui::SameLine();
  ImGui::TextDisabled("| %d%% wins now | Z: zones %s", game::win_threshold(fr.tick, g.tick_cap),
                      g.show_zones ? "on" : "off");
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
  draw_zone_edges(dl, ctr, top, right, bottom, left, cd, x, y);
  if (cd.rock > 0.0f) draw_asteroid(dl, ctr, cw2, ch2, x, y, cd.rock);
  if (cd.crystal > 0.0f) draw_crystals(dl, ctr, cw2, ch2, x, y, cd.crystal);
  if (cd.gas > 0.0f) draw_gas(dl, ctr, cw2, ch2, x, y, cd.gas, glfwGetTime(), kGasUnder);

  if (cd.shape >= 0) {
    const float flash = hit_flash(cd.is_struct, cd.id, {x, y}, glfwGetTime());
    ImVec4 col = mix_white(cd.fg, 0.75f * flash);
    if (cd.built < 1.0f) col.w = kGhostAlpha;
    draw_unit_from_def(dl, ctr, cw2, ch2, g.shape_defs[cd.shape], u32(col), cd.facing);
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

    // Just inside the HP bar, toward the tile centre (the edge's inward normal is (-1,-2)/√5).
    if (cd.max_shield > 0) {
      const float  sr  = static_cast<float>(cd.shield) / static_cast<float>(cd.max_shield);
      const ImVec2 off = {-0.447f * bw * 1.2f, -0.894f * bw * 1.2f};
      const ImVec2 a   = {right.x + off.x, right.y + off.y};
      const ImVec2 b   = {bottom.x + off.x, bottom.y + off.y};
      dl->AddLine(a, b, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
      if (sr > 0.0f)
        dl->AddLine(a, {a.x + sr * (b.x - a.x), a.y + sr * (b.y - a.y)}, u32(kShield), bw);
    }
  }

  // Construction progress sits where a Drone's cargo would (structures carry none).
  if (cd.built < 1.0f && g.cs >= 32.0f) {
    ImVec2 b_end = {left.x + cd.built * (bottom.x - left.x),
                    left.y + cd.built * (bottom.y - left.y)};
    float  bw    = std::max(2.0f, ch2 * 0.3f);
    dl->AddLine(left, bottom, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
    dl->AddLine(left, b_end, u32(mix_white(cd.fg, 0.5f)), bw);
  }

  // The hold mirrors the HP bar on the opposite lower edge. Too small for a bar, a pip.
  if (cd.cargo > 0.0f) {
    if (g.cs >= 32.0f) {
      ImVec2 c_end = {left.x + cd.cargo * (bottom.x - left.x),
                      left.y + cd.cargo * (bottom.y - left.y)};
      float  bw    = std::max(2.0f, ch2 * 0.3f);
      dl->AddLine(left, bottom, u32({0.08f, 0.08f, 0.08f, 1.0f}), bw);
      dl->AddLine(left, c_end, u32(kCargo), bw);
    } else {
      dl->AddCircleFilled({(left.x + bottom.x) * 0.5f, (left.y + bottom.y) * 0.5f},
                          std::max(1.5f, ch2 * 0.25f), u32(kCargo));
    }
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
    for (int32_t x = 0; x < g.map.width; ++x) {
      CellData cd = make_cell(g.map, fr, g.unit_facing[g.cur], g.view, x, y);
      if (g.map.tile_at({x, y}).terrain == game::Terrain::Asteroid)
        cd.rock = cd.fogged ? kOutOfSightDim : 1.0f;
      if (g.map.tile_at({x, y}).terrain == game::Terrain::ResourceNode)
        cd.crystal = cd.fogged ? kOutOfSightDim : 1.0f;
      if (g.map.tile_at({x, y}).terrain == game::Terrain::Nebula)
        cd.gas = cd.fogged ? kOutOfSightDim : 1.0f;
      tint_zone(cd, x, y);
      draw_tile_cell(dl, tile_ctr(x, y), cw2, ch2, cd, x, y, fr);
    }

  for (int32_t y = 0; y < g.map.height; ++y)
    for (int32_t x = 0; x < g.map.width; ++x) {
      if (g.map.tile_at({x, y}).terrain != game::Terrain::Nebula) continue;
      const bool seen = tile_visible(g.map, fr, g.view, x, y);
      draw_gas(dl, tile_ctr(x, y), cw2, ch2, x, y, seen ? 1.0f : kOutOfSightDim, glfwGetTime(),
               kGasVeil);
    }

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
      if (sel->max_shield > 0) {
        ImGui::Text("Shld ");
        ImGui::SameLine();
        ImGui::TextColored(kShield, "%d / %d", sel->shield, sel->max_shield);
      }
      ImGui::Text("Pos  %d, %d", sel->pos.x, sel->pos.y);
      if (sel->type == game::UnitType::Drone)
        ImGui::TextColored(sel->carrying() ? kCargo : ImVec4{0.6f, 0.6f, 0.6f, 1.0f},
                           "Cargo  E %d/%d  A %d/%d", sel->cargo_energy,
                           game::cfg::DRONE_CARGO_ENERGY, sel->cargo_alloy,
                           game::cfg::DRONE_CARGO_ALLOY);
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
      if (sel->build_ticks_left > 0)
        ImGui::TextColored({0.9f, 0.85f, 0.4f, 1.0f}, "Building  %d/%d  (needs a Drone within %d)",
                           game::cfg::BUILD_TICKS - sel->build_ticks_left,
                           game::cfg::BUILD_TICKS, game::placement_range(sel->type));
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
      update_zones();
      draw_playback_bar(fr);
      draw_territory_bar(fr);
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
