// Usage: ud_viz_imgui [replay.ud] [--shapes file.udshapes]   (shapes hot-reload on save)
//        ud_viz_imgui replay.ud --screenshot out.ppm [--frame N] [--play SECONDS]
//                               [--size WxH] [--zoom PX] [--center X,Y] [--select X,Y]
//                               [--view V] [--no-hud]
//        ud_viz_imgui --dump-shapes file.udshapes   (write the built-in shapes and exit)
// Controls: Space=play/pause  ←/→=step  U=both sides' sight  G=no fog  0/1=one side's sight
//           drag=pan  scroll=zoom  F=fit the map  C=auto camera  Z=claim zones  H=hide the HUD
//           R=replay this frame's shots  T=toggle static shot lines while paused  Q=quit

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

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
#include "version.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// The HUD's typeface: CMake bakes in the path of Dear ImGui's Roboto; the browser build embeds
// it. Without the file the viewer falls back to ImGui's built-in font.
#ifndef UD_FONT_PATH
#define UD_FONT_PATH "/font.ttf"
#endif

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

// ---- Clock ------------------------------------------------------------------
//
// Everything animated reads this, set once per UI frame: wall time, or a stepped clock when
// rendering a screenshot, so an image of a moment mid-fight comes out the same every time.

static double g_now = 0.0;
static double g_dt  = 0.0;

// ---- Color palette --------------------------------------------------------

static constexpr ImVec4 kSpaceTop  = {0.020f, 0.024f, 0.050f, 1.0f};
static constexpr ImVec4 kSpaceLow  = {0.045f, 0.030f, 0.075f, 1.0f};
static constexpr ImVec4 kOpenBg    = {0.058f, 0.074f, 0.120f, 1.0f};
static constexpr ImVec4 kAsteroid  = {0.070f, 0.068f, 0.082f, 1.0f}; // dust under the rocks
static constexpr ImVec4 kNebula    = {0.105f, 0.070f, 0.180f, 1.0f}; // ground under the gas
static constexpr ImVec4 kResource  = {0.135f, 0.115f, 0.070f, 1.0f}; // ground under the crystals
static constexpr ImVec4 kGridLine  = {0.36f, 0.56f, 0.95f, 0.11f};
static constexpr ImVec4 kRim       = {0.36f, 0.62f, 1.00f, 1.0f};  // the platform's edge
static constexpr ImVec4 kCargo     = {1.00f, 0.86f, 0.30f, 1.0f};  // Drone hold
static constexpr ImVec4 kShield    = {0.62f, 0.90f, 1.00f, 1.0f};  // shield bar, over the HP bar
static constexpr ImVec4 kHot       = {1.00f, 0.90f, 0.62f, 1.0f};  // explosions: white-hot
static constexpr ImVec4 kPanel     = {0.030f, 0.045f, 0.085f, 0.86f};
static constexpr ImVec4 kPanelLine = {0.36f, 0.56f, 0.95f, 0.35f};
static constexpr ImVec4 kText      = {0.86f, 0.91f, 1.00f, 1.0f};
static constexpr ImVec4 kTextDim   = {0.50f, 0.58f, 0.74f, 1.0f};
static constexpr ImVec4 kAccent    = {0.45f, 0.70f, 1.00f, 1.0f};  // a HUD button that is on
static constexpr float  kGhostAlpha = 0.45f; // a structure still under construction

static ImU32  u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
static ImU32  rgba(const ImVec4& c, float a) { return u32({c.x, c.y, c.z, c.w * a}); }
static ImVec4 dim_vec(ImVec4 c, float f) { return {c.x*f, c.y*f, c.z*f, c.w}; }
static ImVec4 mix(ImVec4 a, ImVec4 b, float t) {
  return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w};
}
static ImVec4 mix_white(ImVec4 c, float t) { return mix(c, {1.0f, 1.0f, 1.0f, c.w}, t); }
static ImVec4 hp_color(float ratio) {
  return ratio > 0.5f
      ? ImVec4{0.2f + (1.0f - ratio) * 1.6f, 0.95f, 0.35f, 1.0f}
      : ImVec4{1.0f, ratio * 1.7f, 0.15f, 1.0f};
}
static ImVec4 faction_color(int f) { return team_color(f); }
// The bright tint of a team's colour: its lights, its shots, the rim of its hulls.
static ImVec4 faction_light(int f) { return mix_white(team_color(f), 0.55f); }

static float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

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
  // Hit: the entity that flashes white. Burst: the victim (also flashes). Tracer: the target.
  game::UnitId      unit{};
  game::StructureId structure{};
  int32_t           radius = 0; // Splash: everything this close to `to` flashes
  // Tracer/Muzzle: the shooter, so shots leave from its muzzle point.
  game::UnitId      shooter{};
  int               shooter_shape  = -1;
  int               shooter_team   = 0;
  int               shooter_facing = 0;
  // Burst only: the victim, drawn until the explosion covers it. The frame being
  // shown is post-combat, so without this the target vanishes before it is hit.
  int    ghost_shape  = -1;
  int    ghost_team   = 0;
  int    ghost_facing = 0;
  bool   ghost_moved  = false; // it was still gliding in from `ghost_from` when it died
  game::Vec2 ghost_from{};
  bool   claim = false; // Tracer: a Claim Node fired, so kClaimFx styles it
};

struct FxState {
  std::vector<Effect> live;
  int    seen_frame  = -1;    // last g.cur observed
  double changed_t   = 0.0;   // when g.cur last changed
  bool   pending     = false; // seen_frame's effects not yet spawned
  bool   scrubbing   = false; // the timeline is being dragged
  bool   show_static = true;  // faint lines for the current frame's shots while paused
};

// ---- App state ------------------------------------------------------------

enum class SelType : uint8_t { Unit, Structure, Terrain };

// One per frame, worked out when a replay loads: what the timeline draws.
struct FrameStat {
  uint16_t shots = 0;
  uint8_t  lost_units[2]{};
  uint8_t  lost_structs[2]{};
  bool     lost_core[2]{};
  uint8_t  territory[2]{};
  uint8_t  threshold = 0;
};

// A unit as drawn this instant: between tiles while it moves, mid-turn while it swings round.
struct DrawUnit {
  const game::UnitFrame* u;
  float x, y;       // tile space
  float turn;       // facing, in quarter turns
  float fade;       // 0..1: a new unit fades in
  bool  moving;
  ImVec2 heading;   // screen-space unit vector of travel; only if moving
};

struct Rect {
  ImVec2 a, b;
  Rect() : a(0.0f, 0.0f), b(0.0f, 0.0f) {}
  Rect(ImVec2 lo, ImVec2 hi) : a(lo), b(hi) {}
  bool has(ImVec2 p) const { return p.x >= a.x && p.x < b.x && p.y >= a.y && p.y < b.y; }
  float w() const { return b.x - a.x; }
  float h() const { return b.y - a.y; }
};

struct App {
  bool   loaded  = false;
  std::string error;
  game::Map map;
  std::vector<game::FrameState> frames;
  std::vector<FrameStat> stats;
  int    max_shots = 1;
  int    cur     = 0;
  int    total   = 0;
  int    view    = -2;
  bool   playing = false;
  float  fps     = 15.0f;
  double last_t  = 0.0; // when the frame shown was stepped to, while playing
  char   path_buf[512] = {};
  std::string names[2] = {"TEAM A", "TEAM B"};

  // Camera: the tile at the middle of the free part of the window, and the tile width in
  // pixels. Eased toward the targets, which the auto camera or the mouse set.
  float  cam_x = 0.0f, cam_y = 0.0f, cs = 48.0f;
  float  cam_tx = 0.0f, cam_ty = 0.0f, cs_t = 48.0f;
  bool   autocam = true;
  bool   cam_snap = true;        // jump to the targets: on load, and on a resize
  float  zoom_arg = 0.0f;        // --zoom: fixes the tile width
  bool   select_arg = false;     // --select: picks what stands on a tile, once it is shown
  bool   center_arg = false;     // --center: fixes the tile looked at
  float  center_x = 0.0f, center_y = 0.0f;
  Rect   win, safe;              // the window, and the part of it the HUD leaves free
  ImVec2 iso_org{};              // screen position of tile (0, 0)'s centre
  bool   dragging = false, drag_moved = false;
  ImVec2 drag_from{};

  // Motion between frames: from `anim_prev` (the frame before `cur`, or -1 for none) over
  // `anim_dur` seconds from `anim_t0`.
  int    anim_prev = -1;
  double anim_t0 = 0.0, anim_dur = 0.0;
  float  anim_a = 1.0f; // 0..1 this UI frame
  std::vector<DrawUnit> draw_units; // sorted by id, like the frame's units

  bool              has_sel  = false;
  SelType           sel_type = SelType::Unit;
  game::UnitId      sel_id{};
  game::StructureId sel_struct_id{};
  game::Vec2        sel_tile{};
  ShapeSet          shapes;
  // unit_facing[f][i]: facing of frames[f].units[i], from its last move.
  std::vector<std::vector<uint8_t>> unit_facing;
  FxState           fx;
  std::string       shapes_path = UD_DEFAULT_SHAPES_PATH;
  std::filesystem::file_time_type shapes_mtime{};
  double            shapes_checked_t = 0.0;
  uint32_t          tick_cap = 0;
  bool              show_zones = true;
  bool              chrome = true;        // the HUD; a page embedding us may draw its own
  int               zones_frame = -1;     // frame `zones` was computed for
  std::vector<int8_t> zones;              // territory owner per tile (territory_owners)
  game::TerritoryState zone_pct;
  int               vis_frame = -1, vis_view = -99;
  std::vector<uint8_t> vis;               // per tile: in sight, for the view shown

  std::vector<GlowReq> glows;             // lights to add over the scene this UI frame
  ImFont *font_ui = nullptr, *font_big = nullptr, *font_small = nullptr;
} g;

static float cw2() { return g.cs * 0.5f; }
static float ch2() { return g.cs * 0.25f; }
static ImVec2 to_screen(float x, float y, float z = 0.0f) {
  return {g.iso_org.x + (x - y) * cw2(), g.iso_org.y + (x + y) * ch2() - z * ch2()};
}
static ImVec2 to_tile(ImVec2 s) {
  const float fdx = (s.x - g.iso_org.x) / cw2(), fdy = (s.y - g.iso_org.y) / ch2();
  return {(fdx + fdy) * 0.5f, (fdy - fdx) * 0.5f};
}

static size_t tile_index(int32_t x, int32_t y) {
  return static_cast<size_t>(y) * static_cast<size_t>(g.map.width) + static_cast<size_t>(x);
}
static bool in_map(int32_t x, int32_t y) {
  return x >= 0 && y >= 0 && x < g.map.width && y < g.map.height;
}

// Structures never move; turn them toward the map centre so bases face each other.
static int structure_facing(const game::Map& map, int32_t x, int32_t y) {
  int facing = facing_from_step(map.width / 2 - x, map.height / 2 - y);
  return facing < 0 ? 0 : facing;
}

// ---- Line of sight ----------------------------------------------------------

// view: -1 = no fog (everything), -2 = both factions' sight, 0/1 = that faction's.
// What units and structures can be shown this frame. Terrain is public (every bot gets the
// whole map at init), so out of sight it is only dimmed. Stamped once per frame and view from
// each entity's sight, with the engine's own rule.
static void update_vis() {
  if (g.vis_frame == g.cur && g.vis_view == g.view) return;
  g.vis_frame = g.cur;
  g.vis_view = g.view;
  const size_t n = static_cast<size_t>(g.map.width) * static_cast<size_t>(g.map.height);
  g.vis.assign(n, g.view == -1 ? 1 : 0);
  if (g.view == -1) return;
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  auto stamp = [&](game::Vec2 pos, int32_t sight) {
    for (int32_t y = std::max(0, pos.y - sight); y <= std::min(g.map.height - 1, pos.y + sight);
         ++y)
      for (int32_t x = std::max(0, pos.x - sight); x <= std::min(g.map.width - 1, pos.x + sight);
           ++x)
        if (game::sees_tile(g.map, pos, sight, {x, y}))
          g.vis[tile_index(x, y)] = 1;
  };
  for (const auto& u : fr.units)
    if (g.view < 0 || static_cast<int>(u.faction.value) == g.view)
      stamp(u.pos, game::stats_for(u.type).sight);
  for (const auto& s : fr.structures)
    if (s.hp > 0 && (g.view < 0 || static_cast<int>(s.faction.value) == g.view))
      stamp(s.pos, game::structure_sight(s.type));
}

static bool tile_visible(int32_t x, int32_t y) {
  return in_map(x, y) && g.vis[tile_index(x, y)] != 0;
}

static constexpr float kOutOfSightDim = 0.58f;

static float cargo_fill(const game::UnitFrame& u) {
  return std::max(static_cast<float>(u.cargo_energy) / game::cfg::DRONE_CARGO_ENERGY,
                  static_cast<float>(u.cargo_alloy) / game::cfg::DRONE_CARGO_ALLOY);
}

// ---- Territory --------------------------------------------------------------

static std::vector<game::ClaimAnchor> frame_claims(const game::FrameState& fr) {
  std::vector<game::ClaimAnchor> claims;
  for (const auto& s : fr.structures)
    if (s.type == game::StructureType::ClaimNode && s.hp > 0 && s.build_ticks_left == 0)
      claims.push_back({s.pos, s.faction.value});
  return claims;
}

// Recomputed from the frame's live Claim Nodes with the engine's own rule, so the zones drawn
// are exactly the territory the win check counted.
static void update_zones() {
  if (g.zones_frame == g.cur) return;
  g.zones = game::territory_owners(g.map, frame_claims(g.frames[static_cast<size_t>(g.cur)]));
  g.zone_pct = game::territory_pct(g.map, g.zones);
  g.zones_frame = g.cur;
}

static int8_t zone_at(int32_t x, int32_t y) {
  return in_map(x, y) ? g.zones[tile_index(x, y)] : static_cast<int8_t>(-1);
}

// ---- Loading ----------------------------------------------------------------

static void g_focus_reset(); // the auto camera's subject, defined with the camera

static bool load_shape_defs() {
  bool ok = load_shapes(g.shapes, g.shapes_path.c_str());
  if (!ok) make_default_shapes(g.shapes);
  std::error_code ec;
  g.shapes_mtime = std::filesystem::last_write_time(g.shapes_path, ec);
  return ok;
}

// Polls rather than using inotify so it works the same everywhere; twice a second is
// plenty for "save in the editor, glance at the viewer".
static void poll_shape_reload() {
#ifndef __EMSCRIPTEN__
  if (g_now - g.shapes_checked_t < 0.5) return;
  g.shapes_checked_t = g_now;
  std::error_code ec;
  auto mt = std::filesystem::last_write_time(g.shapes_path, ec);
  if (ec || mt == g.shapes_mtime) return;
  load_shape_defs();
  fprintf(stderr, "Reloaded shapes from %s\n", g.shapes_path.c_str());
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

// The timeline's data. Territory is the engine's count, recomputed only on the frames where
// the set of finished claims changed.
static void compute_stats() {
  g.stats.assign(g.frames.size(), {});
  g.max_shots = 1;
  std::vector<game::ClaimAnchor> claims;
  game::TerritoryState pct{};
  bool have = false;
  for (size_t f = 0; f < g.frames.size(); ++f) {
    const auto& fr = g.frames[f];
    FrameStat& st = g.stats[f];
    st.shots = static_cast<uint16_t>(std::min<size_t>(fr.shots.size(), 0xFFFF));
    g.max_shots = std::max(g.max_shots, static_cast<int>(st.shots));
    for (const auto& k : fr.kills) {
      if (k.faction.value > 1) continue;
      const uint32_t s = k.faction.value;
      if (!k.unit.is_null()) {
        if (st.lost_units[s] < 255) ++st.lost_units[s];
      } else {
        if (st.lost_structs[s] < 255) ++st.lost_structs[s];
        if (k.structure_type == game::StructureType::CommandCore) st.lost_core[s] = true;
      }
    }
    auto now_claims = frame_claims(fr);
    const bool same = have && now_claims.size() == claims.size() &&
        std::equal(now_claims.begin(), now_claims.end(), claims.begin(),
                   [](const game::ClaimAnchor& a, const game::ClaimAnchor& b) {
                     return a.pos == b.pos && a.faction == b.faction;
                   });
    if (!same) {
      claims = std::move(now_claims);
      pct = game::territory_pct(g.map, game::territory_owners(g.map, claims));
      have = true;
    }
    st.territory[0] = static_cast<uint8_t>(std::min<uint32_t>(pct.pct_faction[0], 100));
    st.territory[1] = static_cast<uint8_t>(std::min<uint32_t>(pct.pct_faction[1], 100));
    st.threshold = static_cast<uint8_t>(game::win_threshold(fr.tick, g.tick_cap));
  }
}

static std::string upper(std::string s) {
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
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
    g.tick_cap = log.tick_cap;
    compute_unit_facings();
    compute_stats();
    g.names[0] = log.name_a.empty() ? "TEAM A" : upper(log.name_a);
    g.names[1] = log.name_b.empty() ? "TEAM B" : upper(log.name_b);
    g.fx     = FxState{};
    g.cur    = 0;
    g.total  = static_cast<int>(g.frames.size());
    g.zones_frame = -1;
    g.vis_frame   = -1;
    g.anim_prev   = -1;
    g.has_sel     = false;
    g.cam_snap    = true;
    g.autocam     = true;
    g_focus_reset();
    g.loaded = true;
    fprintf(stderr, "Loaded: %zu ticks on %dx%d map\n",
            log.ticks.size(), log.map_w, log.map_h);
  } catch (const std::exception& e) {
    g.loaded = false;
    g.error  = e.what();
    fprintf(stderr, "error: %s\n", e.what());
  }
}

static void open_dialog() {
  std::string picked = pick_file();
  if (picked.empty()) return;
  strncpy(g.path_buf, picked.c_str(), sizeof(g.path_buf) - 1);
  try_load(g.path_buf);
}

// ---- Stepping through frames ------------------------------------------------

static constexpr double kStepGlide = 0.22; // seconds a single step takes to glide

// Every change of frame goes through here. Stepping one frame on (playing, or the → key)
// glides the units there; anything else (a jump, a step back) just shows the frame.
static void set_frame(int f, bool glide) {
  f = std::clamp(f, 0, std::max(0, g.total - 1));
  if (f == g.cur) return;
  if (glide && f == g.cur + 1) {
    g.anim_prev = g.cur;
    g.anim_t0   = g_now;
    g.anim_dur  = g.playing ? 1.0 / static_cast<double>(g.fps) : kStepGlide;
  } else {
    g.anim_prev = -1;
  }
  g.cur = f;
}

static void set_playing(bool on) {
  if (on && g.loaded && g.cur + 1 >= g.total) set_frame(0, false); // play again from the start
  g.playing = on && g.loaded;
  g.last_t  = g_now;
}

static const game::UnitFrame* find_unit(const std::vector<game::UnitFrame>& units,
                                        game::UnitId id, size_t* index = nullptr) {
  auto it = std::lower_bound(units.begin(), units.end(), id,
                             [](const game::UnitFrame& u, game::UnitId v) { return u.id < v; });
  if (it == units.end() || it->id != id) {
    // Frames list units in id order; a hand-built one that doesn't still gets found.
    it = std::find_if(units.begin(), units.end(),
                      [&](const game::UnitFrame& u) { return u.id == id; });
    if (it == units.end()) return nullptr;
  }
  if (index) *index = static_cast<size_t>(it - units.begin());
  return &*it;
}

static float turn_between(int from, int to, float t) {
  const int diff = ((to - from + 6) & 3) - 2; // -2..1: the short way round
  return static_cast<float>(from) + static_cast<float>(diff) * t;
}

// Where every unit of the frame shown is drawn this instant.
static void build_draw_units() {
  g.draw_units.clear();
  if (!g.loaded) return;
  const bool anim = g.anim_prev >= 0 && g.anim_prev + 1 == g.cur;
  g.anim_a = anim ? clamp01(static_cast<float>((g_now - g.anim_t0) / std::max(1e-6, g.anim_dur)))
                  : 1.0f;
  const float a = g.anim_a;
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  const auto& facings = g.unit_facing[static_cast<size_t>(g.cur)];
  const game::FrameState* prev = anim ? &g.frames[static_cast<size_t>(g.anim_prev)] : nullptr;
  g.draw_units.reserve(fr.units.size());
  for (size_t i = 0; i < fr.units.size(); ++i) {
    const auto& u = fr.units[i];
    DrawUnit d{&u, static_cast<float>(u.pos.x), static_cast<float>(u.pos.y),
               static_cast<float>(facings[i]), 1.0f, false, {}};
    size_t pi = 0;
    const game::UnitFrame* was = prev ? find_unit(prev->units, u.id, &pi) : nullptr;
    if (was && a < 1.0f) {
      d.x = static_cast<float>(was->pos.x) + static_cast<float>(u.pos.x - was->pos.x) * a;
      d.y = static_cast<float>(was->pos.y) + static_cast<float>(u.pos.y - was->pos.y) * a;
      d.turn = turn_between(g.unit_facing[static_cast<size_t>(g.anim_prev)][pi], facings[i],
                            clamp01(a * 3.0f));
      if (was->pos != u.pos) {
        const float dx = static_cast<float>(u.pos.x - was->pos.x);
        const float dy = static_cast<float>(u.pos.y - was->pos.y);
        const ImVec2 sv = {(dx - dy) * 2.0f, dx + dy}; // screen direction, at 2:1
        const float len = std::sqrt(sv.x * sv.x + sv.y * sv.y);
        d.moving  = true;
        d.heading = {sv.x / len, sv.y / len};
      }
    } else if (prev && !was) {
      d.fade = a;
    }
    g.draw_units.push_back(d);
  }
}

static const DrawUnit* find_draw_unit(game::UnitId id) {
  auto it = std::lower_bound(g.draw_units.begin(), g.draw_units.end(), id,
                             [](const DrawUnit& d, game::UnitId v) { return d.u->id < v; });
  if (it != g.draw_units.end() && it->u->id == id) return &*it;
  for (const auto& d : g.draw_units)
    if (d.u->id == id) return &d;
  return nullptr;
}

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
static constexpr double kFxScrubSettle = 0.10; // the timeline must rest this long before spawning
// Combat follows movement within a tick, so the guns open up part-way through the glide.
static constexpr double kFxGlideLead   = 0.45;

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

static bool shot_visible(const game::ShotEvent& sh) {
  return tile_visible(sh.from.x, sh.from.y) || tile_visible(sh.to.x, sh.to.y);
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
  size_t i = 0;
  if (find_unit(units, sh.attacker, &i)) return g.unit_facing[static_cast<size_t>(frame)][i];
  const int fc = facing_from_step(sh.to.x - sh.from.x, sh.to.y - sh.from.y);
  return fc < 0 ? 0 : fc;
}

static void spawn_fx(int frame, double now) {
  const auto& fr = g.frames[static_cast<size_t>(frame)];
  const double k = g.playing ? std::min(1.0, kFxMaxTicks / g.fps / kFxTotal) : 1.0;
  const bool gliding = g.anim_prev >= 0 && g.anim_prev + 1 == frame && g.anim_a < 1.0f;
  const double lead = gliding ? kFxGlideLead * g.anim_dur : 0.0;

  // Impact time per shot, so each explosion waits for the shot that caused it.
  std::vector<double> impact(fr.shots.size());
  double last_impact = now + lead;
  for (size_t i = 0; i < fr.shots.size(); ++i) {
    const auto& sh  = fr.shots[i];
    const WeaponFx w = shot_fx(sh);
    const double t0 = now + lead + (sh.first_strike ? 0.0 : kFxDelayNormal) * k;
    impact[i]   = t0 + (w.travel + w.stagger * (w.bolts - 1)) * k;
    last_impact = std::max(last_impact, impact[i]);
    if (!shot_visible(sh)) continue;

    const int team = sh.faction.value == 0 ? 0 : 1;
    const ImVec4 tc = mix_white(faction_color(team),
                                sh.attacker_type == game::UnitType::Frigate ? 0.55f : 0.35f);
    const int sshape = shooter_shape(sh), sface = shooter_facing(frame, sh);
    auto from_shooter = [&](Effect& e) {
      e.shooter = sh.attacker;
      e.shooter_shape = sshape;
      e.shooter_team = team;
      e.shooter_facing = sface;
    };
    for (int b = 0; b < w.bolts; ++b) {
      Effect e{FxKind::Tracer, sh.attacker_type, sh.from, sh.to, tc, t0 + b * w.stagger * k,
               w.travel * k};
      from_shooter(e);
      e.unit = sh.target_unit;
      e.claim = claim_shot(sh);
      g.fx.live.push_back(e);
    }
    if (w.beam) {
      Effect e{FxKind::Muzzle, sh.attacker_type, sh.from, sh.to, tc, t0, kFxMuzzle * k,
               w.hit_size};
      from_shooter(e);
      g.fx.live.push_back(e);
    }
    if (sh.splash) {
      Effect e{FxKind::Splash, sh.attacker_type, sh.to, sh.to, kHot, impact[i], kFxSplash * k};
      e.radius = splash_radius(sh);
      g.fx.live.push_back(e);
    } else {
      Effect e{FxKind::Hit, sh.attacker_type, sh.to, sh.to, tc, impact[i], kFxHit * k,
               w.hit_size};
      e.unit      = sh.target_unit;
      e.structure = sh.target_struct;
      g.fx.live.push_back(e);
    }
  }

  for (const auto& kl : fr.kills) {
    if (!tile_visible(kl.pos.x, kl.pos.y)) continue;
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
    const int team = kl.faction.value == 0 ? 0 : 1;
    // The blast carries the loser's colour: it shows at a glance whose ship that was.
    Effect e{FxKind::Burst, game::UnitType::Drone, kl.pos, kl.pos, faction_color(team), t,
             kFxBurst * (0.7 + 0.3 * size) * k, size};
    e.unit      = kl.unit;
    e.structure = kl.structure;
    const bool is_unit = !kl.unit.is_null();
    e.ghost_shape = is_unit ? shape_index(kl.unit_type) : shape_index(kl.structure_type);
    e.ghost_team  = team;
    if (is_unit && frame > 0) {
      const auto& prev = g.frames[static_cast<size_t>(frame - 1)];
      size_t i = 0;
      if (const game::UnitFrame* was = find_unit(prev.units, kl.unit, &i)) {
        e.ghost_facing = g.unit_facing[static_cast<size_t>(frame - 1)][i];
        e.ghost_moved  = gliding && was->pos != kl.pos;
        e.ghost_from   = was->pos;
      }
    } else if (!is_unit) {
      e.ghost_facing = structure_facing(g.map, kl.pos.x, kl.pos.y);
    }
    g.fx.live.push_back(e);
  }
}

// Called once per UI frame, after input and playback have settled g.cur.
static void update_fx() {
  auto& fx = g.fx;
  if (g.cur != fx.seen_frame) {
    const bool continuing = g.playing && g.cur == fx.seen_frame + 1;
    if (!continuing) fx.live.clear();
    fx.seen_frame = g.cur;
    fx.changed_t  = g_now;
    fx.pending    = true;
  }
  if (fx.pending && !(fx.scrubbing && g_now - fx.changed_t < kFxScrubSettle)) {
    spawn_fx(g.cur, g_now);
    fx.pending = false;
  }
  std::erase_if(fx.live, [&](const Effect& e) { return g_now > e.start + e.dur; });
}

// 0..1: how strongly an entity should flash white right now from hits on it.
static float hit_flash(bool is_struct, uint32_t id, game::Vec2 pos) {
  float best = 0.0f;
  for (const auto& e : g.fx.live) {
    if ((e.kind != FxKind::Hit && e.kind != FxKind::Splash) || g_now < e.start ||
        g_now > e.start + e.dur)
      continue;
    const bool match =
        e.kind == FxKind::Splash ? game::manhattan(pos, e.to) <= e.radius
        : is_struct              ? !e.structure.is_null() && e.structure.value == id
                                 : !e.unit.is_null() && e.unit.value == id;
    if (match)
      best = std::max(best, 1.0f - static_cast<float>((g_now - e.start) / e.dur));
  }
  return best;
}

static ImVec2 arc_point(ImVec2 a, ImVec2 b, float p, float h) {
  return {a.x + (b.x - a.x) * p, a.y + (b.y - a.y) * p - h * 4.0f * p * (1.0f - p)};
}

// ---- Light ------------------------------------------------------------------
//
// Lights are drawn additively over the finished scene: they brighten what is under them
// rather than paint over it, which is what makes a bolt or a beacon look like it shines.

static void blend_additive(const ImDrawList*, const ImDrawCmd*) {
  glBlendFunc(GL_SRC_ALPHA, GL_ONE);
}

static void begin_lights(ImDrawList* dl) { dl->AddCallback(blend_additive, nullptr); }
static void end_lights(ImDrawList* dl) {
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
}

// A soft round light: brightest at the middle, fading to nothing at `r`. Two rings of a fan
// with the colour's alpha set per vertex, so the falloff is smooth at any size.
static void glow(ImDrawList* dl, ImVec2 c, float r, ImVec4 col, float a) {
  if (r < 0.75f || a <= 0.003f) return;
  const int seg = std::clamp(static_cast<int>(r * 0.5f) + 8, 10, 56);
  const ImU32 c_mid = rgba(col, std::min(1.0f, 0.55f * a));
  const ImU32 c_in  = rgba(col, std::min(1.0f, 0.20f * a));
  const ImU32 c_out = rgba(col, 0.0f);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  dl->PrimReserve(seg * 9, 1 + 2 * seg);
  const unsigned int base = dl->_VtxCurrentIdx;
  dl->PrimWriteVtx(c, uv, c_mid);
  for (int i = 0; i < seg; ++i) {
    const float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(seg);
    const ImVec2 d = {std::cos(ang), std::sin(ang)};
    dl->PrimWriteVtx({c.x + d.x * r * 0.42f, c.y + d.y * r * 0.42f}, uv, c_in);
    dl->PrimWriteVtx({c.x + d.x * r, c.y + d.y * r}, uv, c_out);
  }
  auto idx = [&](unsigned int v) { dl->PrimWriteIdx(static_cast<ImDrawIdx>(base + v)); };
  for (int i = 0; i < seg; ++i) {
    const unsigned int in0 = 1 + 2 * static_cast<unsigned int>(i);
    const unsigned int in1 = 1 + 2 * static_cast<unsigned int>((i + 1) % seg);
    idx(0); idx(in0); idx(in1);
    idx(in0); idx(in0 + 1); idx(in1 + 1);
    idx(in0); idx(in1 + 1); idx(in1);
  }
}

static void glow_line(ImDrawList* dl, ImVec2 a, ImVec2 b, float w, ImVec4 col, float alpha) {
  dl->AddLine(a, b, rgba(col, 0.14f * alpha), w * 4.5f);
  dl->AddLine(a, b, rgba(col, 0.40f * alpha), w * 2.0f);
  dl->AddLine(a, b, rgba(mix_white(col, 0.7f), alpha), std::max(1.0f, w * 0.8f));
}

static ImVec2 ground_of(game::UnitId id, game::Vec2 tile) {
  if (!id.is_null())
    if (const DrawUnit* d = find_draw_unit(id)) return {d->x, d->y};
  return {static_cast<float>(tile.x), static_cast<float>(tile.y)};
}

static const UnitShapeDef& shape_def(int team, int shape) { return g.shapes.d[team & 1][shape]; }

// How far above the tile a ship rides: a slow bob, out of step from ship to ship.
static float hover_lift(uint32_t id) {
  return 0.07f + 0.05f * std::sin(static_cast<float>(g_now) * 2.1f + static_cast<float>(id) * 1.7f);
}

// The victims of this frame's kills, as they were, until the blast covers them.
static void draw_fx_ghosts(ImDrawList* dl) {
  for (const auto& e : g.fx.live) {
    if (e.ghost_shape < 0 || g_now > e.start + e.dur * 0.15) continue;
    const bool is_struct = e.unit.is_null();
    const float f = hit_flash(is_struct, is_struct ? e.structure.value : e.unit.value, e.to);
    ImVec2 at = {static_cast<float>(e.to.x), static_cast<float>(e.to.y)};
    if (e.ghost_moved && g.anim_a < 1.0f)
      at = {static_cast<float>(e.ghost_from.x) +
                static_cast<float>(e.to.x - e.ghost_from.x) * g.anim_a,
            static_cast<float>(e.ghost_from.y) +
                static_cast<float>(e.to.y - e.ghost_from.y) * g.anim_a};
    ShapePaint paint;
    paint.lift = is_struct ? 0.0f : hover_lift(e.unit.value);
    paint.edge = rgba(faction_light(e.ghost_team), 0.7f);
    paint.light = u32(mix_white(faction_color(e.ghost_team), 0.8f));
    draw_unit_from_def(dl, to_screen(at.x, at.y), cw2(), ch2(),
                       shape_def(e.ghost_team, e.ghost_shape),
                       u32(mix_white(faction_color(e.ghost_team), 0.75f * f)),
                       static_cast<float>(e.ghost_facing), paint);
  }
}

// Paused, with nothing in flight: a faint line for each of the frame's shots.
static void draw_fx_static(ImDrawList* dl) {
  const bool idle = !g.playing && g.fx.live.empty() && !g.fx.pending;
  if (!idle || !g.fx.show_static) return;
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  const float impact = 0.5f;
  for (const auto& sh : fr.shots) {
    if (!shot_visible(sh)) continue;
    const int team = sh.faction.value == 0 ? 0 : 1;
    const ImVec4 fc = faction_light(team);
    const ImVec2 from = ground_of(sh.attacker, sh.from);
    const ImVec2 a = shape_muzzle(shape_def(team, shooter_shape(sh)), to_screen(from.x, from.y),
                                  cw2(), ch2(), static_cast<float>(shooter_facing(g.cur, sh)));
    const ImVec2 to = ground_of(sh.target_unit, sh.to);
    const ImVec2 b = to_screen(to.x, to.y, impact);
    if (shot_fx(sh).arc) {
      ImVec2 pts[13];
      const float h = ch2() * (1.2f + 0.45f * static_cast<float>(game::manhattan(sh.from, sh.to)));
      for (int i = 0; i <= 12; ++i) pts[i] = arc_point(a, b, static_cast<float>(i) / 12.0f, h);
      dl->AddPolyline(pts, 13, rgba(fc, 0.45f), 0, 1.5f);
    } else {
      dl->AddLine(a, b, rgba(fc, 0.45f), 1.5f);
    }
    dl->AddCircleFilled(b, std::max(2.0f, ch2() * 0.12f), rgba(fc, 0.6f));
  }
}

// Every effect in flight. Call between begin_lights and end_lights.
static void draw_fx_lights(ImDrawList* dl) {
  const float c2 = cw2(), h2 = ch2();
  auto arc_h  = [&](game::Vec2 a, game::Vec2 b) {
    return h2 * (1.2f + 0.45f * static_cast<float>(game::manhattan(a, b)));
  };
  const float impact = 0.5f; // ch2 units above the target tile, roughly hull top
  auto muzzle_at = [&](const Effect& e) {
    const ImVec2 at = ground_of(e.shooter, e.from);
    float turn = static_cast<float>(e.shooter_facing);
    if (!e.shooter.is_null())
      if (const DrawUnit* d = find_draw_unit(e.shooter)) turn = d->turn;
    const float lift = e.shooter.is_null() ? 0.0f : hover_lift(e.shooter.value);
    const ImVec2 c = to_screen(at.x, at.y, lift);
    return e.shooter_shape >= 0
             ? shape_muzzle(shape_def(e.shooter_team, e.shooter_shape), c, c2, h2, turn)
             : ImVec2{c.x, c.y - kDefaultMuzzleZ * h2};
  };

  for (const auto& e : g.fx.live) {
    if (g_now < e.start) continue;
    const float p = static_cast<float>(std::clamp((g_now - e.start) / e.dur, 0.0, 1.0));
    const float q = 1.0f - p;
    switch (e.kind) {
    case FxKind::Tracer: {
      const WeaponFx w = e.claim ? kClaimFx : weapon_fx(e.weapon);
      const ImVec2 a = muzzle_at(e);
      const ImVec2 to = ground_of(e.unit, e.to);
      const ImVec2 b = to_screen(to.x, to.y, impact);
      const float  wd = std::max(1.2f, h2 * w.width);
      if (p < 0.35f) glow(dl, a, h2 * 0.55f, e.col, 1.0f - p / 0.35f); // muzzle flash
      if (w.arc) {
        const float top = arc_h(e.from, e.to);
        ImVec2 trail[8];
        for (int i = 0; i < 8; ++i)
          trail[i] =
            arc_point(a, b, std::max(0.0f, p - 0.24f * (1.0f - static_cast<float>(i) / 7.0f)), top);
        dl->AddPolyline(trail, 8, rgba(e.col, 0.18f), 0, wd * 3.0f);
        dl->AddPolyline(trail, 8, rgba(e.col, 0.55f), 0, wd);
        const ImVec2 shell = trail[7];
        glow(dl, shell, wd * 5.0f, e.col, 1.0f);
        dl->AddCircleFilled(shell, wd * 1.1f, rgba(mix_white(e.col, 0.8f), 1.0f));
        break;
      }
      const float head = 1.0f - q * q; // ease-out
      const float tail = std::max(0.0f, head - w.tail);
      const ImVec2 hp = {a.x + (b.x - a.x) * head, a.y + (b.y - a.y) * head};
      const ImVec2 tp = {a.x + (b.x - a.x) * tail, a.y + (b.y - a.y) * tail};
      auto bolt = [&](ImVec2 off) {
        const ImVec2 t2{tp.x + off.x, tp.y + off.y}, h2p{hp.x + off.x, hp.y + off.y};
        glow_line(dl, t2, h2p, wd, e.col, 1.0f);
        glow(dl, h2p, wd * 3.5f, e.col, 0.9f);
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
      const ImVec2 c = muzzle_at(e);
      const float r = h2 * 0.9f * e.size * (0.6f + 0.6f * p);
      glow(dl, c, r, e.col, 1.4f * q);
      dl->AddCircleFilled(c, r * 0.22f, rgba({1, 1, 1, 1}, q));
      break;
    }
    case FxKind::Hit: {
      const ImVec2 to = ground_of(e.unit, e.to);
      const ImVec2 c = to_screen(to.x, to.y, impact);
      const float r = h2 * (0.35f + 0.75f * p) * e.size;
      glow(dl, c, r * 1.6f, e.col, 1.2f * q);
      dl->AddEllipse(c, {r * 1.2f, r * 0.84f}, rgba(mix_white(e.col, 0.5f), 0.8f * q), 0.0f, 0,
                     1.5f);
      for (int i = 0; i < 4; ++i) { // sparks thrown off the plating
        const float ang = 1.1f + 1.5708f * static_cast<float>(i) +
                          static_cast<float>(e.to.x * 3 + e.to.y);
        const float r0 = r * 0.5f, r1 = r * (1.2f + 0.9f * p);
        dl->AddLine({c.x + std::cos(ang) * r0, c.y + std::sin(ang) * r0 * 0.6f},
                    {c.x + std::cos(ang) * r1, c.y + std::sin(ang) * r1 * 0.6f},
                    rgba(mix_white(e.col, 0.6f), 0.7f * q), 1.2f);
      }
      break;
    }
    case FxKind::Splash: {
      // Light up every tile in the blast, friendlies included, then a ground ring.
      for (int32_t dy = -e.radius; dy <= e.radius; ++dy)
        for (int32_t dx = -e.radius; dx <= e.radius; ++dx) {
          if (std::abs(dx) + std::abs(dy) > e.radius) continue;
          const ImVec2 c =
            to_screen(static_cast<float>(e.to.x + dx), static_cast<float>(e.to.y + dy));
          const ImVec2 quad[4] = {
            {c.x, c.y - h2}, {c.x + c2, c.y}, {c.x, c.y + h2}, {c.x - c2, c.y}};
          dl->AddConvexPolyFilled(quad, 4, rgba({1.0f, 0.55f, 0.18f, 1.0f}, 0.34f * q * q));
        }
      const ImVec2 c = to_screen(static_cast<float>(e.to.x), static_cast<float>(e.to.y));
      const float rx = c2 * (0.6f + (static_cast<float>(e.radius) + 0.6f) * p);
      dl->AddEllipse(c, {rx, rx * 0.5f}, rgba(e.col, 0.9f * q), 0.0f, 0, 2.5f);
      const ImVec2 up = to_screen(static_cast<float>(e.to.x), static_cast<float>(e.to.y), impact);
      glow(dl, up, c2 * (1.0f + 1.4f * p), {1.0f, 0.62f, 0.25f, 1.0f}, 1.6f * q);
      if (p < 0.35f) glow(dl, up, c2 * 0.9f, {1, 1, 1, 1}, 2.0f * (1.0f - p / 0.35f));
      break;
    }
    case FxKind::Burst: {
      const ImVec2 c = to_screen(static_cast<float>(e.to.x), static_cast<float>(e.to.y), impact);
      const float r = h2 * (0.4f + 1.1f * p) * e.size;
      glow(dl, c, r * 2.2f, mix(kHot, e.col, p), 1.8f * q);                         // fireball
      if (p < 0.3f) glow(dl, c, r * 1.3f, {1, 1, 1, 1}, 2.2f * (1.0f - p / 0.3f)); // flash
      dl->AddEllipse(c, {r * 1.25f, r * 0.8f}, rgba(e.col, 0.9f * q), 0.0f, 0, 2.0f);
      const int sparks = e.size > 1.5f ? 14 : 9;
      for (int i = 0; i < sparks; ++i) {
        const float ang = 6.2831853f * static_cast<float>(i) / static_cast<float>(sparks) + 0.3f +
                          static_cast<float>(e.to.x + e.to.y * 5);
        const float reach = 1.1f + 0.5f * static_cast<float>((i * 7) % 5) / 4.0f;
        const float r0 = r * 0.5f, r1 = r * 1.5f * reach;
        dl->AddLine({c.x + std::cos(ang) * r0, c.y + std::sin(ang) * r0 * 0.6f},
                    {c.x + std::cos(ang) * r1, c.y + std::sin(ang) * r1 * 0.6f},
                    rgba(i & 1 ? e.col : kHot, 0.85f * q), 1.5f);
      }
      if (e.size > 1.8f) { // structures: a slower ground shockwave
        const ImVec2 g0 = to_screen(static_cast<float>(e.to.x), static_cast<float>(e.to.y));
        const float sr = c2 * (0.5f + 1.8f * p) * (e.size / 2.0f);
        dl->AddEllipse(g0, {sr, sr * 0.5f}, rgba(mix_white(e.col, 0.3f), 0.7f * q), 0.0f, 0, 3.0f);
      }
      break;
    }
    }
  }
}

// ---- Backdrop ---------------------------------------------------------------

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

static constexpr int kStars = 260;

// Deep space behind the board: a gradient, two faint clouds, and stars in three layers that
// slide past at different rates as the camera moves.
static void draw_backdrop(ImDrawList* dl) {
  const ImVec2 a = g.win.a, b = g.win.b;
  dl->AddRectFilledMultiColor(a, b, u32(kSpaceTop), u32(kSpaceTop), u32(kSpaceLow), u32(kSpaceLow));
  const float w = g.win.w(), h = g.win.h();
  if (w < 2.0f || h < 2.0f) return;
  const float t = static_cast<float>(g_now);
  const ImVec2 drift = {(g.cam_x - g.cam_y) * g.cs * 0.5f, (g.cam_x + g.cam_y) * g.cs * 0.25f};

  begin_lights(dl);
  struct Cloud { float x, y, r; ImVec4 col; };
  const Cloud clouds[] = {{0.22f, 0.30f, 0.46f, {0.30f, 0.16f, 0.62f, 1.0f}},
                          {0.80f, 0.72f, 0.52f, {0.08f, 0.30f, 0.52f, 1.0f}}};
  for (const Cloud& c : clouds) {
    const ImVec2 at = {a.x + c.x * w - drift.x * 0.03f, a.y + c.y * h - drift.y * 0.03f};
    // Many faint discs, each a little smaller: no one edge shows.
    for (int i = 0; i < 18; ++i)
      dl->AddCircleFilled(at, c.r * std::max(w, h) * (1.0f - 0.05f * static_cast<float>(i)),
                          rgba(c.col, 0.011f), 48);
  }
  TileRand rnd{7, 13};
  for (int i = 0; i < kStars; ++i) {
    const float sx = rnd.next(), sy = rnd.next(), k = rnd.next(), ph = rnd.next() * 6.2832f;
    const int layer = i % 3;
    const float par = 0.02f + 0.03f * static_cast<float>(layer);
    float x = std::fmod(sx * w - drift.x * par, w);
    float y = std::fmod(sy * h - drift.y * par, h);
    if (x < 0) x += w;
    if (y < 0) y += h;
    const float tw = 0.55f + 0.45f * std::sin(t * (0.6f + 1.6f * k) + ph);
    const ImVec4 col = k < 0.18f ? ImVec4{1.0f, 0.82f, 0.62f, 1.0f}
                     : k < 0.40f ? ImVec4{0.62f, 0.80f, 1.0f, 1.0f}
                                 : ImVec4{0.90f, 0.93f, 1.0f, 1.0f};
    const float r = 0.6f + 0.5f * static_cast<float>(layer) + (k > 0.94f ? 0.9f : 0.0f);
    dl->AddCircleFilled({a.x + x, a.y + y}, r,
                        rgba(col, (0.30f + 0.22f * static_cast<float>(layer)) * tw), 6);
    if (k > 0.94f) glow(dl, {a.x + x, a.y + y}, r * 5.0f, col, 0.7f * tw);
  }
  end_lights(dl);
}

// ---- Asteroids --------------------------------------------------------------
//
// One to three lumpy boulders per tile. Shapes come from a hash of the tile position: they
// vary across the field but never change between frames.

static constexpr ImVec4 kRockBase   = {0.30f, 0.31f, 0.40f, 1.0f};
static constexpr ImVec4 kRockLit    = {0.50f, 0.54f, 0.68f, 1.0f};
static constexpr ImVec4 kRockShadow = {0.13f, 0.13f, 0.20f, 1.0f};
static constexpr int    kRockPoints = 11;

static void draw_boulder(ImDrawList* dl, ImVec2 c, float rx, float ry, float bright,
                         TileRand& rnd) {
  auto col = [&](ImVec4 v) { return u32(dim_vec(v, bright)); };
  dl->AddEllipseFilled({c.x + rx * 0.15f, c.y + ry * 0.55f}, {rx * 1.05f, ry * 0.45f},
                       u32({0.0f, 0.0f, 0.0f, 0.40f * bright}));

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

static void draw_asteroid(ImDrawList* dl, ImVec2 ctr, int32_t x, int32_t y, float bright) {
  const float c2 = cw2(), h2 = ch2();
  TileRand rnd{x, y};
  const int count = 1 + static_cast<int>(rnd.next() * 3.0f);
  struct Rock { ImVec2 c; float s; };
  Rock rocks[3];
  for (int i = 0; i < count; ++i) {
    // The first rock is big and central; extras are smaller and pushed toward the edges.
    const float s = i == 0 ? 0.62f + 0.22f * rnd.next() : 0.28f + 0.18f * rnd.next();
    const float spread = i == 0 ? 0.15f : 0.55f;
    rocks[i] = {{ctr.x + (rnd.next() * 2.0f - 1.0f) * c2 * spread,
                 ctr.y + (rnd.next() * 2.0f - 1.0f) * h2 * spread},
                s};
  }
  // Back to front, so nearer rocks overlap farther ones.
  std::sort(rocks, rocks + count, [](const Rock& a, const Rock& b) { return a.c.y < b.c.y; });
  for (int i = 0; i < count; ++i) {
    const float rx = c2 * rocks[i].s;
    const float ry = rx * 0.78f; // taller than the ground's 2:1, so they read as raised
    draw_boulder(dl, {rocks[i].c.x, rocks[i].c.y - ry * 0.45f}, rx, ry, bright, rnd);
  }
}

// Resource nodes are clusters of gold crystal shards: the same hue as a Drone's cargo, so
// what a Drone carries visibly comes from them.
static constexpr ImVec4 kCrystalLit    = {1.00f, 0.95f, 0.55f, 1.0f};
static constexpr ImVec4 kCrystalBase   = {1.00f, 0.78f, 0.20f, 1.0f};
static constexpr ImVec4 kCrystalShadow = {0.62f, 0.40f, 0.06f, 1.0f};

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

static void draw_crystals(ImDrawList* dl, ImVec2 ctr, int32_t x, int32_t y, float bright) {
  const float c2 = cw2(), h2 = ch2();
  TileRand rnd{x, y, 101};
  struct Shard { ImVec2 base; float h, w, lean; };
  Shard shards[4];
  const int count = 3 + static_cast<int>(rnd.next() * 2.0f);
  for (int i = 0; i < count; ++i) {
    // The first shard is the tall centrepiece; the rest are shorter and splay outward.
    const float ox = i == 0 ? 0.0f : (rnd.next() * 2.0f - 1.0f) * 0.42f;
    const float oy = i == 0 ? 0.0f : (rnd.next() * 2.0f - 1.0f) * 0.38f;
    const float h = c2 * (i == 0 ? 0.95f + 0.25f * rnd.next() : 0.45f + 0.30f * rnd.next());
    shards[i] = {{ctr.x + ox * c2, ctr.y + oy * h2 + h2 * 0.15f}, h,
                 c2 * (i == 0 ? 0.30f : 0.20f + 0.06f * rnd.next()),
                 i == 0 ? (rnd.next() - 0.5f) * 0.12f : ox * 0.55f};
  }
  std::sort(shards, shards + count,
            [](const Shard& a, const Shard& b) { return a.base.y < b.base.y; });
  for (int i = 0; i < count; ++i)
    draw_shard(dl, shards[i].base, shards[i].w, shards[i].h, shards[i].lean, bright);
  const float pulse = 0.75f + 0.25f * std::sin(static_cast<float>(g_now) * 1.7f +
                                               static_cast<float>(x * 3 + y * 5));
  g.glows.push_back({{ctr.x, ctr.y - c2 * 0.35f}, c2 * 1.5f,
                     rgba(kCrystalBase, 0.75f * bright * pulse)});
}

// Nebulae are drifting gas: overlapping translucent puffs that spill past their tile, so a
// patch reads as one cloud. Drawn twice: thick under units, then a thin veil over everything,
// so a ship inside looks like it is in the gas.
static constexpr ImVec4 kGasDeep  = {0.50f, 0.22f, 0.80f, 1.0f};
static constexpr ImVec4 kGasLight = {0.36f, 0.50f, 1.00f, 1.0f};
static constexpr int    kGasPuffs = 6;
static constexpr int    kGasRings = 4;
static constexpr float  kGasDrift = 0.07f;  // fraction of a tile the puffs wander
static constexpr float  kGasSpeed = 0.22f;  // radians per second
static constexpr float  kGasUnder = 0.50f;  // puff opacity beneath units (summed over rings)
static constexpr float  kGasVeil  = 0.22f;  // ...and over them

static void draw_gas(ImDrawList* dl, ImVec2 ctr, int32_t x, int32_t y, float bright,
                     float opacity) {
  const float c2 = cw2(), h2 = ch2();
  TileRand rnd{x, y, 202};
  // Far out, a tile is a few pixels: fewer puffs carry the same cloud.
  const int puffs = g.cs < 14.0f ? 2 : g.cs < 26.0f ? 4 : kGasPuffs;
  const int rings = g.cs < 14.0f ? 2 : kGasRings;
  for (int i = 0; i < puffs; ++i) {
    const float phase = rnd.next() * 6.2832f;
    const float t = static_cast<float>(g_now) * kGasSpeed + phase;
    const float ox = rnd.next() * 2.0f - 1.0f;
    const float oy = rnd.next() * 2.0f - 1.0f;
    const ImVec2 c = {ctr.x + (ox * 0.5f + std::sin(t) * kGasDrift) * c2,
                      ctr.y + (oy * 0.5f + std::cos(t * 0.8f) * kGasDrift) * h2};
    const float rx = c2 * (0.55f + 0.30f * rnd.next());
    const float k = rnd.next();
    const ImVec4 tone = mix(kGasDeep, kGasLight, k);
    const float a = opacity * bright * (0.75f + 0.25f * std::sin(t * 1.3f)) *
                    static_cast<float>(kGasPuffs) / static_cast<float>(puffs);
    // Stacked rings, faint and wide to dense and small: the puff thins out at its edge.
    for (int ring = 0; ring < rings; ++ring) {
      const float s = 1.0f - 0.22f * static_cast<float>(ring);
      const float lift = 0.06f * static_cast<float>(ring);
      dl->AddEllipseFilled({c.x - rx * 0.08f * static_cast<float>(ring),
                            c.y - rx * 0.05f * static_cast<float>(ring)},
                           {rx * s, rx * s * 0.55f},
                           u32({tone.x + lift, tone.y + lift, tone.z + lift * 0.5f,
                                a / static_cast<float>(rings)}),
                           0.0f, g.cs < 26.0f ? 10 : 0);
    }
  }
}

// ---- The board ----------------------------------------------------------------

static constexpr float kZoneTint      = 0.20f; // faction colour mixed into an owned tile
static constexpr float kZoneFogTint   = 0.13f; // ...into a fogged one
static constexpr float kZoneEdgeInset = 0.90f; // edges drawn inside the tile, so the two sides
                                               // of a border lie side by side
static constexpr float kLodShapes     = 13.0f; // below this tile width, units are markers

// A faction knows where its own claims are, so in its own view its territory shows through
// fog; the enemy's shows only where it can see.
static bool zone_shown(int8_t owner, bool fogged) {
  if (!g.show_zones || owner < 0) return false;
  return !fogged || g.view < 0 || g.view == owner;
}

static ImVec4 terrain_bg(int32_t x, int32_t y) {
  ImVec4 c;
  switch (g.map.tile_at({x, y}).terrain) {
    case game::Terrain::Asteroid:     c = kAsteroid; break;
    case game::Terrain::Nebula:       c = kNebula; break;
    case game::Terrain::ResourceNode: c = kResource; break;
    default:                          c = kOpenBg; break;
  }
  // Deck plating: every tile a shade off its neighbours.
  return dim_vec(c, 0.90f + 0.20f * static_cast<float>(tile_hash(x, y, 77) & 0xFF) / 255.0f);
}

struct TileSpan {
  int32_t x0, y0, x1, y1; // inclusive
};

// The tiles that can reach into the window: the tile-space box round its four corners, padded
// for what stands tall or spills over a tile's edge.
static TileSpan visible_span() {
  const ImVec2 c[4] = {to_tile(g.win.a), to_tile({g.win.b.x, g.win.a.y}), to_tile(g.win.b),
                       to_tile({g.win.a.x, g.win.b.y})};
  float x0 = c[0].x, x1 = c[0].x, y0 = c[0].y, y1 = c[0].y;
  for (const ImVec2& p : c) {
    x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
    y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
  }
  const int32_t pad = 3;
  return {std::max(0, static_cast<int32_t>(std::floor(x0)) - pad),
          std::max(0, static_cast<int32_t>(std::floor(y0)) - pad),
          std::min(g.map.width - 1, static_cast<int32_t>(std::ceil(x1)) + pad),
          std::min(g.map.height - 1, static_cast<int32_t>(std::ceil(y1)) + pad)};
}

static bool on_screen(ImVec2 p, float margin) {
  return p.x > g.win.a.x - margin && p.x < g.win.b.x + margin && p.y > g.win.a.y - margin &&
         p.y < g.win.b.y + margin;
}

struct GlowLine {
  ImVec2 a, b;
  ImVec4 col;
  float  w, alpha;
};

// The slab the map sits on: a lit rim, and two faces falling away into the dark.
static void draw_platform(ImDrawList* dl, std::vector<GlowLine>& lines) {
  const float w = static_cast<float>(g.map.width), h = static_cast<float>(g.map.height);
  const ImVec2 top = to_screen(-0.5f, -0.5f), right = to_screen(w - 0.5f, -0.5f);
  const ImVec2 bottom = to_screen(w - 0.5f, h - 0.5f), left = to_screen(-0.5f, h - 0.5f);
  const float drop = std::max(6.0f, ch2() * 2.2f);
  const ImU32 face = u32({0.030f, 0.040f, 0.075f, 1.0f}),
              dark = u32({0.012f, 0.016f, 0.034f, 1.0f});
  const ImVec2 l2 = {left.x, left.y + drop}, b2 = {bottom.x, bottom.y + drop},
               r2 = {right.x, right.y + drop};
  dl->PrimReserve(12, 8);
  const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
  // A quad shaded from its top edge to its bottom edge.
  auto quad = [&](ImVec2 p0, ImVec2 p1, ImVec2 p2, ImVec2 p3, ImU32 c_top, ImU32 c_low) {
    const ImDrawIdx i0 = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 1));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 2));
    dl->PrimWriteIdx(i0); dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 2));
    dl->PrimWriteIdx(static_cast<ImDrawIdx>(i0 + 3));
    dl->PrimWriteVtx(p0, uv, c_top); dl->PrimWriteVtx(p1, uv, c_top);
    dl->PrimWriteVtx(p2, uv, c_low); dl->PrimWriteVtx(p3, uv, c_low);
  };
  quad(left, bottom, b2, l2, face, dark);
  quad(bottom, right, r2, b2, dim_col(face, 0.75f), dark);
  const ImVec2 rim[4] = {top, right, bottom, left};
  for (int i = 0; i < 4; ++i) {
    dl->AddLine(rim[i], rim[(i + 1) & 3], rgba(kRim, 0.55f), 1.5f);
    lines.push_back({rim[i], rim[(i + 1) & 3], kRim, 2.2f, 0.55f});
  }
}

static void draw_tiles(ImDrawList* dl, const TileSpan& span, std::vector<GlowLine>& lines) {
  const float c2 = cw2(), h2 = ch2();
  const ImDrawListFlags saved = dl->Flags;
  // Thousands of touching quads: without anti-aliasing they meet cleanly, and cost a third.
  dl->Flags &= ~(ImDrawListFlags_AntiAliasedFill | ImDrawListFlags_AntiAliasedLines);
  const bool grid = g.cs >= 14.0f;
  const ImU32 grid_col = u32(kGridLine);
  for (int32_t y = span.y0; y <= span.y1; ++y)
    for (int32_t x = span.x0; x <= span.x1; ++x) {
      const ImVec2 ctr = to_screen(static_cast<float>(x), static_cast<float>(y));
      if (!on_screen(ctr, g.cs)) continue;
      const bool fogged = !tile_visible(x, y);
      ImVec4 bg = terrain_bg(x, y);
      if (fogged) bg = dim_vec(bg, kOutOfSightDim);
      const int8_t owner = zone_at(x, y);
      if (zone_shown(owner, fogged))
        bg = mix(bg, faction_color(owner), fogged ? kZoneFogTint : kZoneTint);
      const ImVec2 top = {ctr.x, ctr.y - h2}, right = {ctr.x + c2, ctr.y};
      const ImVec2 bottom = {ctr.x, ctr.y + h2}, left = {ctr.x - c2, ctr.y};
      dl->AddQuadFilled(top, right, bottom, left, u32(bg));
      if (grid) {
        dl->AddLine(top, right, grid_col, 1.0f);
        dl->AddLine(top, left, grid_col, 1.0f);
      }
    }
  dl->Flags = saved;

  // Outline each tile edge where the owner changes, in this tile's owner's colour: a border
  // between two zones shows both colours side by side.
  if (!g.show_zones) return;
  const float pulse = 0.80f + 0.20f * std::sin(static_cast<float>(g_now) * 1.6f);
  const float lw = std::max(1.0f, g.cs / 30.0f);
  for (int32_t y = span.y0; y <= span.y1; ++y)
    for (int32_t x = span.x0; x <= span.x1; ++x) {
      const int8_t owner = zone_at(x, y);
      const bool fogged = !tile_visible(x, y);
      if (!zone_shown(owner, fogged)) continue;
      const ImVec2 ctr = to_screen(static_cast<float>(x), static_cast<float>(y));
      if (!on_screen(ctr, g.cs)) continue;
      auto in = [&](ImVec2 p) {
        return ImVec2{ctr.x + (p.x - ctr.x) * kZoneEdgeInset,
                      ctr.y + (p.y - ctr.y) * kZoneEdgeInset};
      };
      const ImVec2 top = {ctr.x, ctr.y - h2}, right = {ctr.x + c2, ctr.y};
      const ImVec2 bottom = {ctr.x, ctr.y + h2}, left = {ctr.x - c2, ctr.y};
      // DIRS order (N, E, S, W) → the diamond edge shared with that neighbour.
      const ImVec2 edges[4][2] = {{top, right}, {right, bottom}, {bottom, left}, {left, top}};
      const ImVec4 fc = faction_color(owner);
      const float a = fogged ? 0.40f : 0.95f;
      for (int i = 0; i < 4; ++i) {
        const game::Vec2 n = game::Vec2{x, y} + game::DIRS[i];
        if (zone_at(n.x, n.y) == owner) continue;
        const ImVec2 p0 = in(edges[i][0]), p1 = in(edges[i][1]);
        dl->AddLine(p0, p1, rgba(mix_white(fc, 0.25f), a), lw);
        if (!fogged) lines.push_back({p0, p1, fc, lw * 2.6f, 0.50f * pulse});
      }
    }
}

// How big a marker stands in for each thing when the map is too small for shapes.
static float marker_size(game::UnitType t) {
  switch (t) {
  case game::UnitType::Drone:       return 0.16f;
  case game::UnitType::Interceptor: return 0.18f;
  case game::UnitType::Frigate:     return 0.24f;
  case game::UnitType::Artillery:   return 0.24f;
  case game::UnitType::Battleship:  return 0.32f;
  }
  return 0.2f;
}

// A bar floating over a unit: HP, with the shield as a pale cap over it.
static void draw_bars(ImDrawList* dl, ImVec2 at, float w, int32_t hp, int32_t max_hp,
                      int32_t shield, int32_t max_shield) {
  if (max_hp <= 0) return;
  const float h = std::max(3.0f, g.cs * 0.07f);
  const ImVec2 a = {at.x - w * 0.5f, at.y - h};
  const float ratio = clamp01(static_cast<float>(hp) / static_cast<float>(max_hp));
  dl->AddRectFilled({a.x - 1.0f, a.y - 1.0f}, {a.x + w + 1.0f, a.y + h + 1.0f},
                    IM_COL32(0, 0, 0, 190), 2.0f);
  dl->AddRectFilled(a, {a.x + w * ratio, a.y + h}, u32(hp_color(ratio)), 1.5f);
  if (max_shield > 0 && shield > 0) {
    const float sr = clamp01(static_cast<float>(shield) / static_cast<float>(max_shield));
    dl->AddRectFilled({a.x, a.y - h * 0.9f - 1.0f}, {a.x + w * sr, a.y - 1.0f}, u32(kShield), 1.5f);
  }
}

static bool is_selected(const game::UnitFrame& u) {
  return g.has_sel && g.sel_type == SelType::Unit && g.sel_id == u.id;
}
static bool is_selected(const game::StructureFrame& s) {
  return g.has_sel && g.sel_type == SelType::Structure && g.sel_struct_id == s.id;
}

// A ring on the ground that marks the selection: it breathes, and carries four ticks.
static void draw_sel_ring(ImDrawList* dl, ImVec2 ctr, float scale) {
  const float c2 = cw2() * scale, h2 = ch2() * scale;
  const float k = 1.0f + 0.06f * std::sin(static_cast<float>(g_now) * 5.0f);
  const ImU32 col = IM_COL32(255, 255, 255, 230);
  dl->AddEllipse(ctr, {c2 * 0.62f * k, h2 * 0.62f * k}, col, 0.0f, 0, 1.6f);
  for (int i = 0; i < 4; ++i) {
    const float a = 0.7854f + 1.5708f * static_cast<float>(i);
    const ImVec2 d = {std::cos(a), std::sin(a)};
    dl->AddLine({ctr.x + d.x * c2 * 0.70f * k, ctr.y + d.y * h2 * 0.70f * k},
                {ctr.x + d.x * c2 * 0.92f * k, ctr.y + d.y * h2 * 0.92f * k}, col, 2.0f);
  }
}

static void draw_unit(ImDrawList* dl, const DrawUnit& d) {
  const game::UnitFrame& u = *d.u;
  const int team = u.faction.value == 0 ? 0 : 1;
  const float c2 = cw2(), h2 = ch2();
  const ImVec2 ctr = to_screen(d.x, d.y);
  if (!on_screen(ctr, g.cs * 2.0f)) return;
  const ImVec4 fc = faction_color(team), light = faction_light(team);
  const float flash = hit_flash(false, u.id.value, u.pos);

  if (is_selected(u)) draw_sel_ring(dl, ctr, 1.0f);

  if (g.cs < kLodShapes) {
    // Too small for a hull: a lit marker, sized by class, says whose it is and how heavy.
    const float r = std::max(1.6f, g.cs * marker_size(u.type));
    dl->AddCircleFilled(ctr, r + 1.0f, IM_COL32(0, 0, 0, 200), 10);
    dl->AddCircleFilled(ctr, r, u32(mix_white(fc, 0.25f + 0.6f * flash)), 10);
    g.glows.push_back({ctr, r * 3.2f, rgba(fc, 0.8f * d.fade)});
    return;
  }

  // The team's ring on the deck under every ship: whose it is, at a glance, whatever the hull.
  dl->AddEllipse(ctr, {c2 * 0.50f, h2 * 0.50f}, rgba(fc, 0.55f * d.fade), 0.0f, 0,
                 std::max(1.0f, g.cs / 34.0f));

  const UnitShapeDef& def = shape_def(team, shape_index(u.type));
  const float lift = hover_lift(u.id.value);
  ShapePaint paint;
  paint.lift = lift;
  paint.alpha = d.fade;
  paint.edge = rgba(light, 0.75f);
  paint.light = u32(mix_white(fc, 0.62f));
  paint.glows = &g.glows;
  paint.glow_col = u32(fc);
  draw_unit_from_def(dl, ctr, c2, h2, def, u32(mix_white(fc, 0.75f * flash)), d.turn, paint);

  // Exhausts burn all the time, and streak out behind a ship under way.
  const float flicker = 0.80f + 0.20f * std::sin(static_cast<float>(g_now) * 31.0f +
                                                 static_cast<float>(u.id.value));
  for (const ShapePoint& e : def.engines) {
    const ImVec2 r = rotate_turn(d.turn, {e.x, e.y});
    const ImVec2 at = iso_project(ctr, c2, h2, r.x, r.y, e.z + lift);
    g.glows.push_back({at, h2 * (d.moving ? 0.75f : 0.45f) * flicker, rgba(light, 0.9f * d.fade)});
    if (d.moving) {
      const float len = c2 * 0.95f * flicker;
      const ImVec2 tail = {at.x - d.heading.x * len, at.y - d.heading.y * len};
      const ImVec2 mid = {at.x - d.heading.x * len * 0.4f, at.y - d.heading.y * len * 0.4f};
      g.glows.push_back({mid, h2 * 0.45f, rgba(fc, 0.8f * d.fade)});
      g.glows.push_back({tail, h2 * 0.30f, rgba(fc, 0.5f * d.fade)});
    }
  }

  // A hold with something in it glows gold above a Drone.
  const float cargo = cargo_fill(u);
  if (cargo > 0.0f) {
    const ImVec2 at = to_screen(d.x, d.y, 1.05f + lift);
    const float r = std::max(1.5f, h2 * (0.10f + 0.10f * cargo));
    dl->AddCircleFilled(at, r, u32(kCargo), 8);
    g.glows.push_back({at, r * 3.5f, rgba(kCargo, 0.9f)});
  }
}

static void draw_unit_bars(ImDrawList* dl, const DrawUnit& d) {
  const game::UnitFrame& u = *d.u;
  if (g.cs < 20.0f) return;
  const bool hurt = u.hp < u.max_hp || u.shield < u.max_shield;
  if (!hurt && !is_selected(u)) return;
  const float tall = u.type == game::UnitType::Battleship ? 2.1f : 1.45f;
  draw_bars(dl, to_screen(d.x, d.y, tall), cw2() * 0.85f, u.hp, u.max_hp, u.shield, u.max_shield);
}

static void draw_structure(ImDrawList* dl, const game::StructureFrame& s) {
  const int team = s.faction.value == 0 ? 0 : 1;
  const float c2 = cw2(), h2 = ch2();
  const ImVec2 ctr = to_screen(static_cast<float>(s.pos.x), static_cast<float>(s.pos.y));
  if (!on_screen(ctr, g.cs * 3.0f)) return;
  const ImVec4 fc = faction_color(team), light = faction_light(team);
  const float flash = hit_flash(true, s.id.value, s.pos);
  const bool building = s.build_ticks_left > 0;
  const float built = building
      ? 1.0f - static_cast<float>(s.build_ticks_left) / static_cast<float>(game::cfg::BUILD_TICKS)
      : 1.0f;

  if (is_selected(s)) draw_sel_ring(dl, ctr, 1.25f);

  if (g.cs < kLodShapes) {
    // A square where ships are discs; the Command Core is the big one with a white heart.
    const bool core = s.type == game::StructureType::CommandCore;
    const float r = std::max(2.2f, g.cs * (core ? 0.52f : 0.30f));
    const ImVec2 q[4] = {
      {ctr.x, ctr.y - r}, {ctr.x + r, ctr.y}, {ctr.x, ctr.y + r}, {ctr.x - r, ctr.y}};
    dl->AddConvexPolyFilled(q, 4, rgba(mix_white(fc, 0.6f * flash), building ? kGhostAlpha : 1.0f));
    dl->AddPolyline(q, 4, IM_COL32(0, 0, 0, 220), ImDrawFlags_Closed, 1.0f);
    if (core) dl->AddCircleFilled(ctr, r * 0.35f, IM_COL32(255, 255, 255, 235), 8);
    if (!building) g.glows.push_back({ctr, r * (core ? 4.0f : 2.6f), rgba(fc, 0.9f)});
    return;
  }

  const UnitShapeDef& def = shape_def(team, shape_index(s.type));
  // A beacon's light swells and fades; an unfinished structure shines nothing.
  const float pulse = 0.72f + 0.28f * std::sin(static_cast<float>(g_now) * 2.4f +
                                               static_cast<float>(s.id.value));
  ShapePaint paint;
  paint.alpha = building ? kGhostAlpha : 1.0f;
  paint.edge = rgba(light, 0.75f);
  paint.light = u32(mix_white(fc, 0.42f + 0.28f * pulse));
  paint.glow_col = rgba(fc, pulse);
  paint.glows = building ? nullptr : &g.glows;
  draw_unit_from_def(dl, ctr, c2, h2, def, u32(mix_white(fc, 0.75f * flash)),
                     static_cast<float>(structure_facing(g.map, s.pos.x, s.pos.y)), paint);

  if (building) { // a ring on the deck that closes as the work is done
    const int seg = 28;
    const int n = std::max(1, static_cast<int>(built * static_cast<float>(seg)));
    dl->PathClear();
    for (int i = 0; i <= n; ++i) {
      const float a = -1.5708f + 6.2832f * static_cast<float>(i) / static_cast<float>(seg);
      dl->PathLineTo({ctr.x + std::cos(a) * c2 * 0.62f, ctr.y + std::sin(a) * h2 * 0.62f});
    }
    dl->PathStroke(rgba(light, 0.95f), 0, std::max(1.5f, g.cs / 22.0f));
  }
}

static void draw_structure_bars(ImDrawList* dl, const game::StructureFrame& s) {
  // Claims wear down by themselves all match: only real damage is worth a bar.
  const bool hurt = s.hp * 10 < s.max_hp * 9;
  if (g.cs < 20.0f || !(hurt || is_selected(s)) || s.build_ticks_left > 0) return;
  const float tall = s.type == game::StructureType::CommandCore ? 2.7f : 1.9f;
  draw_bars(dl, to_screen(static_cast<float>(s.pos.x), static_cast<float>(s.pos.y), tall),
            cw2() * 1.0f, s.hp, s.max_hp, 0, 0);
}

// Everything that stands up from the deck, back to front.
struct Standing {
  float depth;
  uint8_t kind; // 0 rocks, 1 crystals, 2 structure, 3 unit
  int32_t a, b;
};

static void draw_world(ImDrawList* dl) {
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  const TileSpan span = visible_span();
  static std::vector<GlowLine> lines;
  static std::vector<Standing> standing;
  lines.clear();
  standing.clear();
  g.glows.clear();

  draw_platform(dl, lines);
  draw_tiles(dl, span, lines);

  for (int32_t y = span.y0; y <= span.y1; ++y)
    for (int32_t x = span.x0; x <= span.x1; ++x) {
      const game::Terrain t = g.map.tile_at({x, y}).terrain;
      if (t == game::Terrain::Open) continue;
      const ImVec2 ctr = to_screen(static_cast<float>(x), static_cast<float>(y));
      if (!on_screen(ctr, g.cs * 1.5f)) continue;
      const float depth = static_cast<float>(x + y);
      if (t == game::Terrain::Nebula)
        draw_gas(dl, ctr, x, y, tile_visible(x, y) ? 1.0f : kOutOfSightDim, kGasUnder);
      else
        standing.push_back(
          {depth, static_cast<uint8_t>(t == game::Terrain::Asteroid ? 0 : 1), x, y});
    }
  for (size_t i = 0; i < fr.structures.size(); ++i) {
    const auto& s = fr.structures[i];
    if (tile_visible(s.pos.x, s.pos.y))
      standing.push_back(
        {static_cast<float>(s.pos.x + s.pos.y) + 0.01f, 2, static_cast<int32_t>(i), 0});
  }
  for (size_t i = 0; i < g.draw_units.size(); ++i) {
    const DrawUnit& d = g.draw_units[i];
    if (tile_visible(d.u->pos.x, d.u->pos.y))
      standing.push_back({d.x + d.y + 0.02f, 3, static_cast<int32_t>(i), 0});
  }
  std::stable_sort(standing.begin(), standing.end(),
                   [](const Standing& a, const Standing& b) { return a.depth < b.depth; });

  if (g.has_sel && g.sel_type == SelType::Terrain) {
    const ImVec2 c = to_screen(static_cast<float>(g.sel_tile.x), static_cast<float>(g.sel_tile.y));
    dl->AddQuad({c.x, c.y - ch2()}, {c.x + cw2(), c.y}, {c.x, c.y + ch2()}, {c.x - cw2(), c.y},
                IM_COL32(200, 230, 255, 220), 2.0f);
  }

  const bool small = g.cs < kLodShapes;
  for (const Standing& it : standing) {
    switch (it.kind) {
    case 0:
    case 1: {
      const float bright = tile_visible(it.a, it.b) ? 1.0f : kOutOfSightDim;
      const ImVec2 ctr = to_screen(static_cast<float>(it.a), static_cast<float>(it.b));
      if (it.kind == 0) {
        if (small) { // a pebble where a boulder field would be a smear
          const float r = g.cs * 0.34f;
          dl->AddEllipseFilled({ctr.x, ctr.y - r * 0.3f}, {r, r * 0.75f},
                               u32(dim_vec(kRockBase, bright)), 0.0f, 8);
          dl->AddEllipseFilled({ctr.x - r * 0.2f, ctr.y - r * 0.5f}, {r * 0.55f, r * 0.4f},
                               u32(dim_vec(kRockLit, bright)), 0.0f, 8);
        } else {
          draw_asteroid(dl, ctr, it.a, it.b, bright);
        }
      } else if (small) {
        const float r = g.cs * 0.26f;
        dl->AddTriangleFilled({ctr.x - r, ctr.y}, {ctr.x + r, ctr.y}, {ctr.x, ctr.y - r * 2.4f},
                              u32(dim_vec(kCrystalBase, bright)));
        g.glows.push_back({{ctr.x, ctr.y - r}, r * 3.0f, rgba(kCrystalBase, 0.7f * bright)});
      } else {
        draw_crystals(dl, ctr, it.a, it.b, bright);
      }
      break;
    }
    case 2: draw_structure(dl, fr.structures[static_cast<size_t>(it.a)]); break;
    case 3: draw_unit(dl, g.draw_units[static_cast<size_t>(it.a)]); break;
    }
  }
  draw_fx_ghosts(dl);

  for (int32_t y = span.y0; y <= span.y1; ++y)
    for (int32_t x = span.x0; x <= span.x1; ++x) {
      if (g.map.tile_at({x, y}).terrain != game::Terrain::Nebula) continue;
      const ImVec2 ctr = to_screen(static_cast<float>(x), static_cast<float>(y));
      if (on_screen(ctr, g.cs * 1.5f))
        draw_gas(dl, ctr, x, y, tile_visible(x, y) ? 1.0f : kOutOfSightDim, kGasVeil);
    }

  draw_fx_static(dl);

  begin_lights(dl);
  for (const GlowLine& l : lines) {
    dl->AddLine(l.a, l.b, rgba(l.col, 0.16f * l.alpha), l.w * 3.0f);
    dl->AddLine(l.a, l.b, rgba(l.col, 0.42f * l.alpha), l.w);
  }
  for (const GlowReq& q : g.glows) glow(dl, q.c, q.r, ImGui::ColorConvertU32ToFloat4(q.col), 1.0f);
  draw_fx_lights(dl);
  end_lights(dl);

  for (const DrawUnit& d : g.draw_units)
    if (tile_visible(d.u->pos.x, d.u->pos.y)) draw_unit_bars(dl, d);
  for (const auto& s : fr.structures)
    if (tile_visible(s.pos.x, s.pos.y)) draw_structure_bars(dl, s);
}

// ---- Camera -----------------------------------------------------------------

static constexpr float kMinCs = 5.0f, kMaxCs = 128.0f;
static constexpr float kAutoMinCs = 26.0f; // the auto camera never shows tiles smaller than this
                                           // if it can follow the fight instead

// The tile width at which the whole map fits the free part of the window.
static float fit_cs() {
  const float span = static_cast<float>(g.map.width + g.map.height) + 1.5f;
  const float by_w = g.safe.w() * 2.0f / span;
  const float by_h = (g.safe.h() - 20.0f) * 4.0f / (span + 3.0f); // room for what stands tall
  return std::clamp(std::min(by_w, by_h), kMinCs, 96.0f);
}

static void fit_map() {
  g.cam_tx = (static_cast<float>(g.map.width) - 1.0f) * 0.5f;
  g.cam_ty = (static_cast<float>(g.map.height) - 1.0f) * 0.5f;
  g.cs_t   = g.zoom_arg > 0.0f ? g.zoom_arg : fit_cs();
  if (g.center_arg) {
    g.cam_tx = g.center_x;
    g.cam_ty = g.center_y;
  }
}

// The auto camera's subject on a map too big to read whole: the hottest fight. Found from the
// shots and kills of the last few ticks and the next many (a replay knows what is coming, so
// the camera can be there first).
struct Focus {
  bool   on = false;
  float  x = 0.0f, y = 0.0f;
  double since = 0.0;   // when the camera turned to it
  int    frame = -1;    // the frame it was last worked out for
} g_focus;

static constexpr int    kFocusBack = 5, kFocusAhead = 18; // ticks either side of now
static constexpr int    kFocusReach = 9;       // tiles round a point that count as one fight
static constexpr float  kFocusMinHeat = 4.0f;  // less than this is not worth leaving the map for
static constexpr float kFocusKeep = 0.6f; // stay while the fight has this much of the best's heat
static constexpr double kFocusHold = 1.2;      // seconds before the camera may turn elsewhere
static constexpr float  kFocusCs = 38.0f;      // tile width when on a fight
static constexpr size_t kFocusMaxEvents = 500;

static void update_focus() {
  if (g_focus.frame == g.cur) return;
  g_focus.frame = g.cur;
  struct Event { game::Vec2 p; float w; };
  static std::vector<Event> ev;
  ev.clear();
  const int from = std::max(0, g.cur - kFocusBack), to = std::min(g.total - 1, g.cur + kFocusAhead);
  for (int f = from; f <= to; ++f) {
    const auto& fr = g.frames[static_cast<size_t>(f)];
    for (const auto& sh : fr.shots) ev.push_back({sh.to, 1.0f});
    for (const auto& k : fr.kills)
      ev.push_back({k.pos, !k.unit.is_null() ? 4.0f
                           : k.structure_type == game::StructureType::CommandCore ? 30.0f : 10.0f});
  }
  auto heat_at = [&](float x, float y, ImVec2* centre) {
    float w = 0.0f, sx = 0.0f, sy = 0.0f;
    for (const Event& e : ev) {
      if (std::fabs(static_cast<float>(e.p.x) - x) + std::fabs(static_cast<float>(e.p.y) - y) >
          static_cast<float>(kFocusReach))
        continue;
      w += e.w;
      sx += e.w * static_cast<float>(e.p.x);
      sy += e.w * static_cast<float>(e.p.y);
    }
    if (centre && w > 0.0f) *centre = {sx / w, sy / w};
    return w;
  };
  float best = 0.0f;
  ImVec2 best_at{};
  const size_t stride = std::max<size_t>(1, ev.size() / kFocusMaxEvents);
  for (size_t i = 0; i < ev.size(); i += stride) {
    ImVec2 c{};
    const float w = heat_at(static_cast<float>(ev[i].p.x), static_cast<float>(ev[i].p.y), &c);
    if (w > best) { best = w; best_at = c; }
  }
  if (best < kFocusMinHeat) {
    g_focus.on = false;
    return;
  }
  if (g_focus.on) {
    // Stay with a fight that is still going, following it as it moves, rather than cut away
    // to one that is only a little bigger.
    ImVec2 c{};
    const float here = heat_at(g_focus.x, g_focus.y, &c);
    if (here >= kFocusKeep * best || (here > 0.0f && g_now - g_focus.since < kFocusHold)) {
      g_focus.x = c.x;
      g_focus.y = c.y;
      return;
    }
  }
  g_focus.on = true;
  g_focus.x = best_at.x;
  g_focus.y = best_at.y;
  g_focus.since = g_now;
}

static void update_camera() {
  if (!g.loaded) return;
  if (g.autocam) {
    fit_map();
    // A map too big to read whole: go to the fight, and pull back out in a lull.
    if (g.zoom_arg <= 0.0f && !g.center_arg && g.cs_t < kAutoMinCs) {
      update_focus();
      if (g_focus.on) {
        g.cs_t = std::max(g.cs_t, kFocusCs);
        g.cam_tx = g_focus.x;
        g.cam_ty = g_focus.y;
      }
    }
  }
  const float k =
    g.cam_snap ? 1.0f : 1.0f - std::exp(-static_cast<float>(g_dt) * (g.autocam ? 2.2f : 14.0f));
  g.cam_x += (g.cam_tx - g.cam_x) * k;
  g.cam_y += (g.cam_ty - g.cam_y) * k;
  g.cs    += (g.cs_t - g.cs) * k;
  g.cam_snap = false;
  const ImVec2 mid = {(g.safe.a.x + g.safe.b.x) * 0.5f, (g.safe.a.y + g.safe.b.y) * 0.5f};
  g.iso_org = {mid.x - (g.cam_x - g.cam_y) * cw2(), mid.y - (g.cam_x + g.cam_y) * ch2()};
}

static void g_focus_reset() { g_focus = Focus{}; }

// Zoom about a point on screen: the tile under it stays under it.
static void zoom_at(ImVec2 at, float factor) {
  const ImVec2 mid = {(g.safe.a.x + g.safe.b.x) * 0.5f, (g.safe.a.y + g.safe.b.y) * 0.5f};
  const float old_cs = g.cs_t;
  const float cs = std::clamp(old_cs * factor, kMinCs, kMaxCs);
  // Offsets from the camera tile scale with 1/cs: keep the pointed-at tile where it is.
  const float fdx = (at.x - mid.x) / (old_cs * 0.5f), fdy = (at.y - mid.y) / (old_cs * 0.25f);
  const ImVec2 off = {(fdx + fdy) * 0.5f, (fdy - fdx) * 0.5f};
  const float s = 1.0f - old_cs / cs;
  g.cam_tx += off.x * s;
  g.cam_ty += off.y * s;
  g.cs_t = cs;
  g.autocam = false;
}

// ---- HUD --------------------------------------------------------------------
//
// Drawn straight onto the window's draw list over the board, as a broadcast overlay: a
// scoreboard on top, the timeline and controls below. `g.chrome` off hides all of it, for a
// page that draws its own.

struct Hud {
  Rect top, bottom;
  Rect sel; // the selection panel, as it was last frame
} g_hud;

static float text_w(ImFont* f, const char* s) {
  return f->CalcTextSizeA(f->FontSize, FLT_MAX, 0.0f, s).x;
}
static void text(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* s) {
  dl->AddText(f, f->FontSize, p, col, s);
}
static void text_right(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* s) {
  text(dl, f, {p.x - text_w(f, s), p.y}, col, s);
}
static void text_mid(ImDrawList* dl, ImFont* f, ImVec2 p, ImU32 col, const char* s) {
  text(dl, f, {p.x - text_w(f, s) * 0.5f, p.y}, col, s);
}

static void panel(ImDrawList* dl, const Rect& r) {
  dl->AddRectFilled(r.a, r.b, u32(kPanel), 8.0f);
  dl->AddRect(r.a, r.b, u32(kPanelLine), 8.0f, 0, 1.0f);
}

enum class Icon : uint8_t { None, Play, Pause, Prev, Next, Restart, Open, Fit };

static void draw_icon(ImDrawList* dl, Icon icon, ImVec2 c, float s, ImU32 col) {
  switch (icon) {
  case Icon::Play:
    dl->AddTriangleFilled({c.x - s * 0.55f, c.y - s * 0.75f}, {c.x + s * 0.80f, c.y},
                          {c.x - s * 0.55f, c.y + s * 0.75f}, col);
    break;
  case Icon::Pause:
    dl->AddRectFilled({c.x - s * 0.62f, c.y - s * 0.70f}, {c.x - s * 0.16f, c.y + s * 0.70f}, col,
                      1.0f);
    dl->AddRectFilled({c.x + s * 0.16f, c.y - s * 0.70f}, {c.x + s * 0.62f, c.y + s * 0.70f}, col,
                      1.0f);
    break;
  case Icon::Prev:
  case Icon::Next: {
    const float d = icon == Icon::Next ? 1.0f : -1.0f;
    const ImVec2 tri[3] = {{c.x - d * s * 0.55f, c.y - s * 0.6f}, {c.x + d * s * 0.30f, c.y},
                           {c.x - d * s * 0.55f, c.y + s * 0.6f}};
    fill_convex(dl, tri, 3, col);
    dl->AddRectFilled({c.x + d * s * 0.36f - s * 0.10f, c.y - s * 0.6f},
                      {c.x + d * s * 0.36f + s * 0.10f, c.y + s * 0.6f}, col);
    break;
  }
  case Icon::Restart: {
    dl->AddRectFilled({c.x - s * 0.66f, c.y - s * 0.6f}, {c.x - s * 0.46f, c.y + s * 0.6f}, col);
    const ImVec2 tri[3] = {{c.x + s * 0.60f, c.y - s * 0.6f}, {c.x + s * 0.60f, c.y + s * 0.6f},
                           {c.x - s * 0.34f, c.y}};
    fill_convex(dl, tri, 3, col);
    break;
  }
  case Icon::Open:
    dl->AddRect({c.x - s * 0.7f, c.y - s * 0.35f}, {c.x + s * 0.7f, c.y + s * 0.6f}, col, 1.5f, 0,
                1.5f);
    dl->AddLine({c.x - s * 0.7f, c.y - s * 0.35f}, {c.x - s * 0.45f, c.y - s * 0.65f}, col, 1.5f);
    dl->AddLine({c.x - s * 0.45f, c.y - s * 0.65f}, {c.x - s * 0.05f, c.y - s * 0.65f}, col, 1.5f);
    dl->AddLine({c.x - s * 0.05f, c.y - s * 0.65f}, {c.x + s * 0.15f, c.y - s * 0.35f}, col, 1.5f);
    break;
  case Icon::Fit:
    for (int i = 0; i < 4; ++i) {
      const float sx = i & 1 ? 1.0f : -1.0f, sy = i & 2 ? 1.0f : -1.0f;
      const ImVec2 k = {c.x + sx * s * 0.7f, c.y + sy * s * 0.6f};
      dl->AddLine(k, {k.x - sx * s * 0.4f, k.y}, col, 1.6f);
      dl->AddLine(k, {k.x, k.y - sy * s * 0.4f}, col, 1.6f);
    }
    break;
  case Icon::None:
    break;
  }
}

// A HUD button: an icon, a label, or both. `on` lights it in `accent`.
static bool hud_button(ImDrawList* dl, const char* id, Rect r, Icon icon, const char* label,
                       bool on, ImVec4 accent, const char* tip) {
  ImGui::SetCursorScreenPos(r.a);
  const bool clicked = ImGui::InvisibleButton(id, {r.w(), r.h()});
  const bool hover = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
  const ImVec4 base = {0.10f, 0.14f, 0.24f, 0.92f};
  ImVec4 bg = on ? mix(base, accent, 0.42f) : base;
  if (hover) bg = mix_white(bg, 0.10f);
  if (held) bg = mix_white(bg, 0.20f);
  dl->AddRectFilled(r.a, r.b, u32(bg), 5.0f);
  dl->AddRect(r.a, r.b, on ? rgba(accent, 0.95f) : rgba(kPanelLine, hover ? 2.0f : 1.0f), 5.0f, 0,
              1.0f);
  const ImU32 fg = u32(on ? mix_white(accent, 0.75f) : hover ? kText : mix(kTextDim, kText, 0.5f));
  const ImVec2 c = {(r.a.x + r.b.x) * 0.5f, (r.a.y + r.b.y) * 0.5f};
  if (icon != Icon::None && label) {
    draw_icon(dl, icon, {r.a.x + r.h() * 0.55f, c.y}, r.h() * 0.24f, fg);
    text(dl, g.font_small, {r.a.x + r.h() * 1.0f, c.y - g.font_small->FontSize * 0.5f}, fg, label);
  } else if (icon != Icon::None) {
    draw_icon(dl, icon, c, r.h() * 0.26f, fg);
  } else if (label) {
    text_mid(dl, g.font_small, {c.x, c.y - g.font_small->FontSize * 0.5f}, fg, label);
  }
  if (hover && tip) ImGui::SetTooltip("%s", tip);
  return clicked;
}

struct SideCount {
  int ships = 0, drones = 0, claims = 0, building = 0, factories = 0;
};

static void count_sides(const game::FrameState& fr, SideCount side[2]) {
  for (const auto& u : fr.units) {
    if (u.hp <= 0 || u.faction.value > 1) continue;
    (u.type == game::UnitType::Drone ? side[u.faction.value].drones
                                     : side[u.faction.value].ships)++;
  }
  for (const auto& st : fr.structures) {
    if (st.hp <= 0 || st.faction.value > 1) continue;
    SideCount& sd = side[st.faction.value];
    if (st.type == game::StructureType::ClaimNode)
      (st.build_ticks_left == 0 ? sd.claims : sd.building)++;
    else if (st.type == game::StructureType::Factory)
      sd.factories++;
  }
}

// The scoreboard: each side's name and holdings at the ends, and between them the territory
// race: both sides' share growing toward the middle, and the marks they must reach to win
// (they close in as the match goes on).
static void draw_scoreboard(ImDrawList* dl, const game::FrameState& fr) {
  const Rect& r = g_hud.top;
  panel(dl, r);
  SideCount side[2];
  count_sides(fr, side);
  const int thr = game::win_threshold(fr.tick, g.tick_cap);
  const float pad = 16.0f;
  const bool wide = r.w() > 760.0f;

  for (int f = 0; f < 2; ++f) {
    const ImVec4 fc = faction_color(f);
    // A wash of the team's colour in from the panel's end, and a solid bar down it.
    const float x0 = f == 0 ? r.a.x : r.b.x - 150.0f, x1 = x0 + 150.0f;
    const ImU32 lit = rgba(fc, 0.22f), off = rgba(fc, 0.0f);
    dl->AddRectFilledMultiColor({x0, r.a.y + 1.0f}, {x1, r.b.y - 1.0f}, f == 0 ? lit : off,
                                f == 0 ? off : lit, f == 0 ? off : lit, f == 0 ? lit : off);
    const float bx = f == 0 ? r.a.x : r.b.x - 5.0f;
    dl->AddRectFilled({bx, r.a.y + 8.0f}, {bx + 5.0f, r.b.y - 8.0f}, u32(fc), 2.0f);

    char buf[96];
    const float tx = f == 0 ? r.a.x + pad : r.b.x - pad;
    auto put = [&](ImFont* font, float y, ImU32 col, const char* s) {
      if (f == 0) text(dl, font, {tx, y}, col, s);
      else text_right(dl, font, {tx, y}, col, s);
    };
    std::string name = g.names[f];
    const float room = wide ? 250.0f : 150.0f;
    while (name.size() > 3 && text_w(g.font_ui, name.c_str()) > room) name.pop_back();
    put(g.font_ui, r.a.y + 9.0f, u32(mix_white(fc, 0.35f)), name.c_str());
    snprintf(buf, sizeof(buf), "E %d    A %d", fr.resources[f].energy, fr.resources[f].alloy);
    put(g.font_small, r.a.y + 34.0f, u32(kText), buf);
    if (wide)
      snprintf(buf, sizeof(buf), "%d ships   %d drones   %d claims   %d factories", side[f].ships,
               side[f].drones, side[f].claims, side[f].factories);
    else
      snprintf(buf, sizeof(buf), "%d ships  %d claims", side[f].ships, side[f].claims);
    put(g.font_small, r.a.y + 53.0f, u32(kTextDim), buf);
  }

  // The race.
  const float inset = wide ? 330.0f : 200.0f;
  const Rect bar = {{r.a.x + inset, r.a.y + 38.0f}, {r.b.x - inset, r.a.y + 54.0f}};
  if (bar.w() < 60.0f) return;
  const float pct[2] = {static_cast<float>(g.zone_pct.pct_faction[0]),
                        static_cast<float>(g.zone_pct.pct_faction[1])};
  dl->AddRectFilled(bar.a, bar.b, IM_COL32(8, 12, 24, 255), 4.0f);
  const float wa = bar.w() * clamp01(pct[0] / 100.0f), wb = bar.w() * clamp01(pct[1] / 100.0f);
  if (wa > 1.0f) dl->AddRectFilled(bar.a, {bar.a.x + wa, bar.b.y}, u32(kFactionA), 4.0f);
  if (wb > 1.0f) dl->AddRectFilled({bar.b.x - wb, bar.a.y}, bar.b, u32(kFactionB), 4.0f);
  dl->AddRect(bar.a, bar.b, u32(kPanelLine), 4.0f, 0, 1.0f);
  begin_lights(dl);
  if (wa > 1.0f) glow(dl, {bar.a.x + wa, (bar.a.y + bar.b.y) * 0.5f}, 16.0f, kFactionA, 1.2f);
  if (wb > 1.0f) glow(dl, {bar.b.x - wb, (bar.a.y + bar.b.y) * 0.5f}, 16.0f, kFactionB, 1.2f);
  end_lights(dl);
  // The winning marks: a side wins when its fill reaches its own.
  const float tx = bar.w() * static_cast<float>(thr) / 100.0f;
  for (int f = 0; f < 2; ++f) {
    const float x = f == 0 ? bar.a.x + tx : bar.b.x - tx;
    dl->AddLine({x, bar.a.y - 4.0f}, {x, bar.b.y + 4.0f}, IM_COL32(255, 255, 255, 235), 2.0f);
    dl->AddTriangleFilled({x - 4.0f, bar.a.y - 9.0f}, {x + 4.0f, bar.a.y - 9.0f},
                          {x, bar.a.y - 3.0f}, u32(faction_light(f)));
  }
  char buf[64];
  const float num_y = (bar.a.y + bar.b.y) * 0.5f - g.font_big->FontSize * 0.5f;
  snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct[0]));
  text_right(dl, g.font_big, {bar.a.x - 12.0f, num_y}, u32(mix_white(kFactionA, 0.3f)), buf);
  snprintf(buf, sizeof(buf), "%d%%", static_cast<int>(pct[1]));
  text(dl, g.font_big, {bar.b.x + 12.0f, num_y}, u32(mix_white(kFactionB, 0.3f)), buf);
  const float mid = (bar.a.x + bar.b.x) * 0.5f;
  snprintf(buf, sizeof(buf), "TICK %u / %u", fr.tick, g.tick_cap);
  text_mid(dl, g.font_ui, {mid, r.a.y + 7.0f}, u32(kText), buf);
  snprintf(buf, sizeof(buf), "TERRITORY   %d%% WINS", thr);
  text_mid(dl, g.font_small, {mid, bar.b.y + 5.0f}, u32(kTextDim), buf);
}

// The match at a glance, and the way to move through it: how hard the fighting was (the
// columns), each side's territory and the winning mark over time (the lines), and what was
// lost when (the ticks along the top; a Command Core is the tall one).
static void draw_timeline(ImDrawList* dl, const Rect& r) {
  dl->AddRectFilled(r.a, r.b, IM_COL32(6, 9, 20, 235), 4.0f);
  const int n = g.total;
  const int cols = std::max(1, static_cast<int>(r.w()));
  auto x_of = [&](int f) {
    return r.a.x + (n > 1 ? r.w() * static_cast<float>(f) / static_cast<float>(n - 1) : 0.0f);
  };
  const float h = r.h();
  const ImDrawListFlags saved = dl->Flags;
  dl->Flags &= ~ImDrawListFlags_AntiAliasedFill;
  for (int c = 0; c < cols; ++c) {
    const int f0 = static_cast<int>(static_cast<int64_t>(c) * n / cols);
    const int f1 = std::max(f0 + 1, static_cast<int>(static_cast<int64_t>(c + 1) * n / cols));
    int shots = 0;
    for (int f = f0; f < f1 && f < n; ++f)
      shots = std::max<int>(shots, g.stats[static_cast<size_t>(f)].shots);
    if (shots == 0) continue;
    const float k = std::sqrt(static_cast<float>(shots) / static_cast<float>(g.max_shots));
    const float x = r.a.x + static_cast<float>(c);
    dl->AddRectFilled({x, r.b.y - h * 0.9f * k}, {x + 1.0f, r.b.y},
                      rgba({0.75f, 0.85f, 1.0f, 1.0f}, 0.20f + 0.25f * k));
  }
  dl->Flags = saved;

  static std::vector<ImVec2> line;
  auto plot = [&](int which, ImU32 col, float w) {
    line.clear();
    const int step = std::max(1, n / cols);
    for (int f = 0; f < n; f += step) {
      const FrameStat& st = g.stats[static_cast<size_t>(f)];
      const float v = which < 2 ? st.territory[which] : st.threshold;
      line.push_back({x_of(f), r.b.y - 2.0f - (h - 4.0f) * v / 100.0f});
    }
    if (line.size() > 1) dl->AddPolyline(line.data(), static_cast<int>(line.size()), col, 0, w);
  };
  plot(2, IM_COL32(255, 255, 255, 110), 1.0f);
  plot(0, rgba(kFactionA, 0.95f), 1.6f);
  plot(1, rgba(kFactionB, 0.95f), 1.6f);

  for (int f = 0; f < n; ++f) {
    const FrameStat& st = g.stats[static_cast<size_t>(f)];
    for (int s = 0; s < 2; ++s) {
      if (!st.lost_structs[s]) continue;
      const float x = x_of(f), len = st.lost_core[s] ? h * 0.55f : h * 0.24f;
      dl->AddLine({x, r.a.y}, {x, r.a.y + len}, rgba(faction_color(s), 0.95f),
                  st.lost_core[s] ? 2.5f : 1.5f);
    }
  }

  // The part already played is lit; the playhead rides its edge.
  const float px = x_of(g.cur);
  dl->AddRectFilled(r.a, {px, r.b.y}, IM_COL32(120, 170, 255, 26), 4.0f);
  dl->AddRect(r.a, r.b, u32(kPanelLine), 4.0f, 0, 1.0f);
  dl->AddLine({px, r.a.y - 3.0f}, {px, r.b.y + 3.0f}, IM_COL32(255, 255, 255, 255), 2.0f);
  dl->AddCircleFilled({px, r.a.y - 3.0f}, 4.5f, IM_COL32(255, 255, 255, 255), 12);
  begin_lights(dl);
  glow(dl, {px, (r.a.y + r.b.y) * 0.5f}, 22.0f, {0.6f, 0.8f, 1.0f, 1.0f}, 0.9f);
  end_lights(dl);

  ImGui::SetCursorScreenPos({r.a.x, r.a.y - 6.0f});
  ImGui::InvisibleButton("##timeline", {r.w(), r.h() + 12.0f});
  g.fx.scrubbing = ImGui::IsItemActive();
  if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
    const float k = clamp01((ImGui::GetMousePos().x - r.a.x) / std::max(1.0f, r.w()));
    const int f = static_cast<int>(std::round(k * static_cast<float>(n - 1)));
    if (ImGui::IsItemActive()) {
      g.playing = false;
      set_frame(f, false);
    } else {
      const FrameStat& st = g.stats[static_cast<size_t>(f)];
      ImGui::SetTooltip("tick %u\n%s %d%%   %s %d%%", g.frames[static_cast<size_t>(f)].tick,
                        g.names[0].c_str(), st.territory[0], g.names[1].c_str(), st.territory[1]);
    }
  }
}

static constexpr float kSpeeds[] = {8.0f, 15.0f, 30.0f, 60.0f};
static const char* const kSpeedNames[] = {"\xC2\xBD\xC3\x97", "1\xC3\x97", "2\xC3\x97",
                                          "4\xC3\x97"}; // ½× 1× 2× 4×

static void draw_controls(ImDrawList* dl) {
  const Rect& r = g_hud.bottom;
  panel(dl, r);
  const float pad = 12.0f;
  draw_timeline(dl, {{r.a.x + pad, r.a.y + 14.0f}, {r.b.x - pad, r.a.y + 46.0f}});

  const float y0 = r.a.y + 54.0f, bh = 28.0f;
  float x = r.a.x + pad;
  auto next = [&](float w) {
    const Rect b = {{x, y0}, {x + w, y0 + bh}};
    x += w + 5.0f;
    return b;
  };
  if (hud_button(dl, "##restart", next(32.0f), Icon::Restart, nullptr, false, kAccent,
                 "Back to the start")) {
    g.playing = false;
    set_frame(0, false);
  }
  if (hud_button(dl, "##prev", next(32.0f), Icon::Prev, nullptr, false, kAccent,
                 "Step back  (Left)")) {
    g.playing = false;
    set_frame(g.cur - 1, false);
  }
  if (hud_button(dl, "##play", next(52.0f), g.playing ? Icon::Pause : Icon::Play, nullptr,
                 g.playing, kAccent, "Play / pause  (Space)"))
    set_playing(!g.playing);
  if (hud_button(dl, "##next", next(32.0f), Icon::Next, nullptr, false, kAccent,
                 "Step on  (Right)")) {
    g.playing = false;
    set_frame(g.cur + 1, true);
  }
  x += 8.0f;
  for (int i = 0; i < 4; ++i)
    if (hud_button(dl, kSpeedNames[i], next(34.0f), Icon::None, kSpeedNames[i],
                   std::fabs(g.fps - kSpeeds[i]) < 0.5f, kAccent, "Playback speed"))
      g.fps = kSpeeds[i];

  // From the right: what is shown.
  float xr = r.b.x - pad;
  auto prev = [&](float w) {
    xr -= w;
    const Rect b = {{xr, y0}, {xr + w, y0 + bh}};
    xr -= 5.0f;
    return b;
  };
#ifndef __EMSCRIPTEN__
  if (hud_button(dl, "##open", prev(32.0f), Icon::Open, nullptr, false, kAccent, "Open a replay"))
    open_dialog();
#endif
  if (hud_button(dl, "##fit", prev(32.0f), Icon::Fit, nullptr, false, kAccent,
                 "Fit the map  (F)")) {
    g.autocam = false;
    fit_map();
  }
  if (hud_button(dl, "AUTO CAM", prev(82.0f), Icon::None, "AUTO CAM", g.autocam, kAccent,
                 "The camera frames the map and follows the fighting  (C)"))
    g.autocam = !g.autocam;
  if (hud_button(dl, "ZONES", prev(62.0f), Icon::None, "ZONES", g.show_zones, kAccent,
                 "Claimed territory  (Z)"))
    g.show_zones = !g.show_zones;
  xr -= 8.0f;
  const bool narrow = r.w() < 900.0f;
  const std::string label_a = narrow ? "A" : g.names[0].substr(0, 11);
  const std::string label_b = narrow ? "B" : g.names[1].substr(0, 11);
  const float side_w = narrow ? 30.0f : 96.0f;
  if (hud_button(dl, "##viewB", prev(side_w), Icon::None, label_b.c_str(), g.view == 1, kFactionB,
                 "Only what this side sees  (1)"))
    g.view = 1;
  if (hud_button(dl, "##viewA", prev(side_w), Icon::None, label_a.c_str(), g.view == 0, kFactionA,
                 "Only what this side sees  (0)"))
    g.view = 0;
  if (hud_button(dl, "BOTH", prev(52.0f), Icon::None, "BOTH", g.view == -2, kAccent,
                 "What either side sees  (U)"))
    g.view = -2;
  if (hud_button(dl, "NO FOG", prev(64.0f), Icon::None, "NO FOG", g.view == -1, kAccent,
                 "Everything  (G)"))
    g.view = -1;
  if (!narrow) text_right(dl, g.font_small, {xr - 4.0f, y0 + 7.0f}, u32(kTextDim), "SIGHT");
}

// What was just lost, newest first, each line fading as it ages: the losing side's colour, the
// count, the type. A structure's loss is the headline.
static void draw_kill_feed(ImDrawList* dl) {
  struct Line { int age; int side; std::string what; int n; bool big; };
  std::vector<Line> lines;
  const int window = 45;
  for (int f = g.cur; f > std::max(0, g.cur - window) && lines.size() < 7; --f) {
    const auto& fr = g.frames[static_cast<size_t>(f)];
    const size_t first = lines.size();
    for (const auto& k : fr.kills) {
      if (k.faction.value > 1 || (g.view >= 0 && !tile_visible(k.pos.x, k.pos.y))) continue;
      const bool is_unit = !k.unit.is_null();
      const char* name = is_unit ? game::unit_type_name(k.unit_type)
                                 : game::structure_type_name(k.structure_type);
      bool merged = false;
      for (size_t i = first; i < lines.size(); ++i)
        if (lines[i].side == static_cast<int>(k.faction.value) && lines[i].what == name) {
          ++lines[i].n;
          merged = true;
        }
      if (!merged)
        lines.push_back({g.cur - f, static_cast<int>(k.faction.value), name, 1, !is_unit});
    }
  }
  float y = g_hud.top.b.y + 14.0f;
  const float xr = g.win.b.x - 14.0f;
  for (const Line& l : lines) {
    const float a = 1.0f - static_cast<float>(l.age) / static_cast<float>(window);
    char buf[96];
    if (l.n > 1) snprintf(buf, sizeof(buf), "%d \xC3\x97 %s", l.n, l.what.c_str());
    else snprintf(buf, sizeof(buf), "%s", l.what.c_str());
    ImFont* font = l.big ? g.font_ui : g.font_small;
    const float w = text_w(font, buf) + 44.0f, h = font->FontSize + 8.0f;
    const ImVec4 fc = faction_color(l.side);
    dl->AddRectFilled({xr - w, y}, {xr, y + h}, rgba(kPanel, a), 4.0f);
    dl->AddRectFilled({xr - 5.0f, y}, {xr, y + h}, rgba(fc, a), 2.0f);
    // A cross in the loser's colour: this was destroyed.
    const ImVec2 c = {xr - w + 14.0f, y + h * 0.5f};
    dl->AddLine({c.x - 4.0f, c.y - 4.0f}, {c.x + 4.0f, c.y + 4.0f}, rgba(fc, a), 2.0f);
    dl->AddLine({c.x - 4.0f, c.y + 4.0f}, {c.x + 4.0f, c.y - 4.0f}, rgba(fc, a), 2.0f);
    text(dl, font, {xr - w + 26.0f, y + 4.0f}, rgba(l.big ? mix_white(fc, 0.5f) : kText, a), buf);
    y += h + 4.0f;
  }
}

static void draw_result(ImDrawList* dl, const game::FrameState& fr) {
  if (!fr.result.has_value()) return;
  const auto& res = *fr.result;
  const bool draw = res.reason == game::WinReason::Draw;
  const int w = draw ? -1 : (res.winner.value == 0 ? 0 : 1);
  const ImVec4 fc = draw ? ImVec4{0.95f, 0.90f, 0.45f, 1.0f} : faction_color(w);
  const std::string why =
      game::result_reason(res, fr.forfeits, {g.names[0].c_str(), g.names[1].c_str()});
  const std::string head = draw ? std::string("DRAW") : g.names[w] + " WINS";
  const float tw = std::max(text_w(g.font_big, head.c_str()), text_w(g.font_ui, why.c_str()));
  const ImVec2 c = {(g.safe.a.x + g.safe.b.x) * 0.5f, g.safe.a.y + 58.0f};
  const Rect r = {{c.x - tw * 0.5f - 36.0f, c.y - 34.0f}, {c.x + tw * 0.5f + 36.0f, c.y + 42.0f}};
  begin_lights(dl);
  glow(dl, c, r.w() * 0.62f, fc, 0.9f);
  end_lights(dl);
  dl->AddRectFilled(r.a, r.b, u32({0.02f, 0.03f, 0.06f, 0.90f}), 8.0f);
  dl->AddRect(r.a, r.b, rgba(fc, 0.95f), 8.0f, 0, 2.0f);
  dl->AddRectFilled({r.a.x + 12.0f, r.b.y - 6.0f}, {r.b.x - 12.0f, r.b.y - 3.0f}, u32(fc), 2.0f);
  text_mid(dl, g.font_big, {c.x, c.y - 28.0f}, u32(mix_white(fc, 0.35f)), head.c_str());
  text_mid(dl, g.font_ui, {c.x, c.y + 8.0f}, u32(kText), why.c_str());
}

static void layout_hud() {
  const float w = g.win.w(), h = g.win.h();
  g.safe = g.win;
  g_hud.top = Rect();
  g_hud.bottom = Rect();
  if (!g.chrome || !g.loaded) return;
  const float tw = std::min(w - 20.0f, 1040.0f);
  g_hud.top = {{g.win.a.x + (w - tw) * 0.5f, g.win.a.y + 10.0f},
               {g.win.a.x + (w + tw) * 0.5f, g.win.a.y + 86.0f}};
  g_hud.bottom = {{g.win.a.x + 10.0f, g.win.a.y + h - 100.0f},
                  {g.win.b.x - 10.0f, g.win.a.y + h - 10.0f}};
  g.safe = {{g.win.a.x, g_hud.top.b.y + 6.0f}, {g.win.b.x, g_hud.bottom.a.y - 6.0f}};
}

static void draw_hud(ImDrawList* dl, const game::FrameState& fr) {
  if (!g.chrome) return;
  draw_kill_feed(dl);
  draw_scoreboard(dl, fr);
  draw_controls(dl);
  draw_result(dl, fr);
}

// Before a replay is loaded: the title, and how to get one in.
static void draw_welcome(ImDrawList* dl) {
  const ImVec2 c = {(g.win.a.x + g.win.b.x) * 0.5f, (g.win.a.y + g.win.b.y) * 0.5f};
  begin_lights(dl);
  glow(dl, {c.x, c.y - 40.0f}, 320.0f, {0.25f, 0.45f, 1.0f, 1.0f}, 0.5f);
  end_lights(dl);
  text_mid(dl, g.font_big, {c.x, c.y - 70.0f}, u32(kText), "UNTHINKING DARK");
  if (!g.chrome) return;
  text_mid(dl, g.font_ui, {c.x, c.y - 26.0f}, u32(kTextDim), "Drop a .ud replay here to watch it");
  if (!g.error.empty())
    text_mid(dl, g.font_ui, {c.x, c.y + 48.0f}, IM_COL32(255, 110, 110, 255), g.error.c_str());
#ifndef __EMSCRIPTEN__
  if (hud_button(dl, "##open0", {{c.x - 74.0f, c.y + 6.0f}, {c.x + 74.0f, c.y + 36.0f}}, Icon::Open,
                 "OPEN REPLAY", false, kAccent, nullptr))
    open_dialog();
#endif
}

// ---- Input ------------------------------------------------------------------

static void select_at(ImVec2 mouse) {
  const ImVec2 t = to_tile(mouse);
  const int32_t tx = static_cast<int32_t>(std::round(t.x)),
                ty = static_cast<int32_t>(std::round(t.y));
  g.has_sel = false;
  if (!in_map(tx, ty)) return;
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  if (tile_visible(tx, ty)) {
    for (const auto& u : fr.units)
      if (u.pos.x == tx && u.pos.y == ty) {
        g.has_sel = true; g.sel_type = SelType::Unit; g.sel_id = u.id;
        return;
      }
    for (const auto& s : fr.structures)
      if (s.pos.x == tx && s.pos.y == ty) {
        g.has_sel = true; g.sel_type = SelType::Structure; g.sel_struct_id = s.id;
        return;
      }
  }
  g.has_sel = true; g.sel_type = SelType::Terrain; g.sel_tile = {tx, ty};
}

static void handle_input(const ImGuiIO& io, GLFWwindow* win) {
  const ImVec2 mouse = io.MousePos;
  // The whole window is one ImGui window, so ImGui's own "wants the mouse" is always set:
  // the board takes the mouse wherever no panel is.
  const bool on_hud = g.chrome && (g_hud.top.has(mouse) || g_hud.bottom.has(mouse) ||
                                   (g.has_sel && g_hud.sel.has(mouse)));
  const bool over_board = g.loaded && !on_hud && g.win.has(mouse);

  if (over_board && io.MouseWheel != 0.0f) zoom_at(mouse, std::pow(1.12f, io.MouseWheel));
  if (over_board && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    g.dragging = true;
    g.drag_moved = false;
    g.drag_from = mouse;
  }
  if (g.dragging) {
    const ImVec2 d = {mouse.x - g.drag_from.x, mouse.y - g.drag_from.y};
    if (!g.drag_moved && d.x * d.x + d.y * d.y > 16.0f) g.drag_moved = true;
    if (g.drag_moved) {
      // Drag the board with the pointer: the camera moves the other way.
      const float fdx = d.x / cw2(), fdy = d.y / ch2();
      g.cam_tx = g.cam_x = g.cam_x - (fdx + fdy) * 0.5f;
      g.cam_ty = g.cam_y = g.cam_y - (fdy - fdx) * 0.5f;
      g.cs_t = g.cs;
      g.drag_from = mouse;
      g.autocam = false;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      if (!g.drag_moved && g.loaded) select_at(mouse);
      g.dragging = false;
    }
  }

  if (io.WantTextInput) return;
  if (g.loaded) {
    if (ImGui::IsKeyPressed(ImGuiKey_Space)) set_playing(!g.playing);
    if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) { g.playing = false; set_frame(g.cur + 1, true); }
    if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) { g.playing = false; set_frame(g.cur - 1, false); }
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
      g.fx.live.clear();
      spawn_fx(g.cur, g_now);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_T)) g.fx.show_static = !g.fx.show_static;
    if (ImGui::IsKeyPressed(ImGuiKey_Z)) g.show_zones = !g.show_zones;
    if (ImGui::IsKeyPressed(ImGuiKey_C)) g.autocam = !g.autocam;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) { g.autocam = false; fit_map(); }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) g.has_sel = false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_H)) g.chrome = !g.chrome;
  if (ImGui::IsKeyPressed(ImGuiKey_U)) g.view = -2;
  if (ImGui::IsKeyPressed(ImGuiKey_G)) g.view = -1;
  if (ImGui::IsKeyPressed(ImGuiKey_0)) g.view = 0;
  if (ImGui::IsKeyPressed(ImGuiKey_1)) g.view = 1;
  if (ImGui::IsKeyPressed(ImGuiKey_Q)) glfwSetWindowShouldClose(win, GLFW_TRUE);
}

// ---- Selection panel --------------------------------------------------------

// Selected unit/structure drawn large on its own tile, in faction colour and
// facing the same way it does on the map.
static void draw_portrait(int team, int shape, float facing) {
  const ImVec2 size = {176.0f, 124.0f};
  const ImVec2 p0   = ImGui::GetCursorScreenPos();
  ImGui::Dummy(size);
  ImDrawList* dl  = ImGui::GetWindowDrawList();
  const float c2 = 54.0f, h2 = 27.0f;
  const ImVec2 c  = {p0.x + size.x * 0.5f, p0.y + size.y * 0.72f};
  const ImVec4 fc = faction_color(team);
  dl->AddRectFilled(p0, {p0.x + size.x, p0.y + size.y}, IM_COL32(6, 9, 20, 255), 4.0f);
  ImVec2 top{c.x, c.y - h2}, right{c.x + c2, c.y}, bottom{c.x, c.y + h2}, left{c.x - c2, c.y};
  dl->AddQuadFilled(top, right, bottom, left, u32(mix(kOpenBg, fc, 0.18f)));
  dl->AddQuad(top, right, bottom, left, rgba(fc, 0.7f), 1.2f);
  dl->PushClipRect(p0, {p0.x + size.x, p0.y + size.y}, true);
  ShapePaint paint;
  paint.edge = rgba(faction_light(team), 0.75f);
  paint.light = u32(mix_white(fc, 0.8f));
  draw_unit_from_def(dl, c, c2, h2, shape_def(team, shape), u32(fc), facing, paint);
  dl->PopClipRect();
}

static void stat_row(const char* label, const char* fmt, ...) IM_FMTARGS(2);
static void stat_row(const char* label, const char* fmt, ...) {
  ImGui::TextColored(kTextDim, "%s", label);
  ImGui::SameLine(84.0f);
  va_list args;
  va_start(args, fmt);
  ImGui::TextV(fmt, args);
  va_end(args);
}

static void draw_sel_panel() {
  if (!g.loaded || !g.has_sel) return;
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];

  ImGui::SetNextWindowPos({g.win.a.x + 12.0f, g.safe.a.y + 8.0f}, ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {12.0f, 10.0f});
  ImGui::Begin("##sel", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
  ImGui::PushFont(g.font_small);

  auto header = [&](const char* what, int team) {
    ImGui::PushFont(g.font_ui);
    ImGui::TextColored(mix_white(faction_color(team), 0.35f), "%s", upper(what).c_str());
    ImGui::PopFont();
    ImGui::TextColored(kTextDim, "%s", g.names[team].c_str());
  };

  if (g.sel_type == SelType::Unit) {
    const DrawUnit* d = find_draw_unit(g.sel_id);
    if (!d) {
      g.has_sel = false;
    } else {
      const game::UnitFrame* sel = d->u;
      const int team = sel->faction.value == 0 ? 0 : 1;
      header(game::unit_type_name(sel->type), team);
      draw_portrait(team, shape_index(sel->type), d->turn);
      const float ratio = sel->max_hp > 0 ? (float)sel->hp / (float)sel->max_hp : 0.0f;
      ImGui::TextColored(kTextDim, "Hull");
      ImGui::SameLine(84.0f);
      ImGui::TextColored(hp_color(ratio), "%d / %d", sel->hp, sel->max_hp);
      if (sel->max_shield > 0) {
        ImGui::TextColored(kTextDim, "Shield");
        ImGui::SameLine(84.0f);
        ImGui::TextColored(kShield, "%d / %d", sel->shield, sel->max_shield);
      }
      stat_row("Position", "%d, %d", sel->pos.x, sel->pos.y);
      if (sel->type == game::UnitType::Drone) {
        ImGui::TextColored(kTextDim, "Cargo");
        ImGui::SameLine(84.0f);
        ImGui::TextColored(sel->carrying() ? kCargo : kTextDim, "E %d/%d   A %d/%d",
                           sel->cargo_energy, game::cfg::DRONE_CARGO_ENERGY, sel->cargo_alloy,
                           game::cfg::DRONE_CARGO_ALLOY);
      }
      ImGui::Separator();
      const game::UnitStats& st = game::stats_for(sel->type);
      stat_row("Damage", "%d", st.dmg);
      stat_row("Range", "%d", st.range);
      stat_row("Sight", "%d", st.sight);
      stat_row("Speed", "%d", st.speed);
      if (st.first_strike) ImGui::TextColored({1.0f, 0.90f, 0.40f, 1.0f}, "First strike");
      if (st.splash)
        ImGui::TextColored({1.0f, 0.68f, 0.30f, 1.0f}, "Splash, radius %d", st.splash_radius);
    }
  } else if (g.sel_type == SelType::Structure) {
    const game::StructureFrame* sel = nullptr;
    for (const auto& s : fr.structures)
      if (s.id == g.sel_struct_id) { sel = &s; break; }
    if (!sel) {
      g.has_sel = false;
    } else {
      const int team = sel->faction.value == 0 ? 0 : 1;
      header(game::structure_type_name(sel->type), team);
      draw_portrait(team, shape_index(sel->type),
                    static_cast<float>(structure_facing(g.map, sel->pos.x, sel->pos.y)));
      const float ratio = sel->max_hp > 0 ? (float)sel->hp / (float)sel->max_hp : 0.0f;
      ImGui::TextColored(kTextDim, "Hull");
      ImGui::SameLine(84.0f);
      ImGui::TextColored(hp_color(ratio), "%d / %d", sel->hp, sel->max_hp);
      stat_row("Position", "%d, %d", sel->pos.x, sel->pos.y);
      if (sel->build_ticks_left > 0)
        ImGui::TextColored({1.0f, 0.90f, 0.45f, 1.0f}, "Building  %d/%d\n(needs a Drone within %d)",
                           game::cfg::BUILD_TICKS - sel->build_ticks_left,
                           game::cfg::BUILD_TICKS, game::placement_range(sel->type));
    }
  } else {
    const game::Tile& tile = g.map.tile_at(g.sel_tile);
    ImGui::PushFont(g.font_ui);
    ImGui::TextColored(kText, "%s", upper(game::terrain_type_name(tile.terrain)).c_str());
    ImGui::PopFont();
    stat_row("Position", "%d, %d", g.sel_tile.x, g.sel_tile.y);
    stat_row("Passable", "%s", game::is_passable(tile.terrain) ? "yes" : "no");
    if (tile.terrain == game::Terrain::ResourceNode)
      stat_row("Resources", "%d", tile.resource_amount);
    const int8_t owner = zone_at(g.sel_tile.x, g.sel_tile.y);
    if (owner >= 0) {
      ImGui::TextColored(kTextDim, "Held by");
      ImGui::SameLine(84.0f);
      ImGui::TextColored(mix_white(faction_color(owner), 0.35f), "%s", g.names[owner].c_str());
    }
  }

  ImGui::PopFont();
  const ImVec2 p = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
  g_hud.sel = {p, {p.x + sz.x, p.y + sz.y}};
  ImGui::End();
  ImGui::PopStyleVar();
}

// ---- Main -----------------------------------------------------------------

static GLFWwindow* g_win = nullptr;

// --screenshot: render one moment of the replay to an image and exit (native only).
struct Shot {
  std::string path;      // .ppm; empty = off
  int    frame = 0;      // replay frame to show (or to start playing from)
  double play = 0.0;     // seconds of playback to run first, on the stepped clock
  int    w = 1520, h = 860;
  int    settle = 4;     // UI frames to draw first, so ImGui's layout has settled
  bool   done = false;
} g_shot;

static constexpr double kShotStep = 1.0 / 60.0;

#ifndef __EMSCRIPTEN__
static void capture_shot(int fb_w, int fb_h) {
  std::vector<unsigned char> px(static_cast<size_t>(fb_w) * static_cast<size_t>(fb_h) * 3);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels(0, 0, fb_w, fb_h, GL_RGB, GL_UNSIGNED_BYTE, px.data());
  FILE* f = fopen(g_shot.path.c_str(), "wb");
  if (!f) { fprintf(stderr, "error: cannot write %s\n", g_shot.path.c_str()); return; }
  fprintf(f, "P6\n%d %d\n255\n", fb_w, fb_h);
  for (int y = fb_h - 1; y >= 0; --y) // GL rows run bottom-up
    fwrite(px.data() + static_cast<size_t>(y) * static_cast<size_t>(fb_w) * 3, 1,
           static_cast<size_t>(fb_w) * 3, f);
  fclose(f);
}
#endif

static void frame() {
  GLFWwindow* win = g_win;
#ifdef __EMSCRIPTEN__
  // The canvas fills the page: follow its size, so ImGui lays out for what is on screen.
  double css_w = 0, css_h = 0;
  emscripten_get_element_css_size("#canvas", &css_w, &css_h);
  int win_w = 0, win_h = 0;
  glfwGetWindowSize(win, &win_w, &win_h);
  if (static_cast<int>(css_w) != win_w || static_cast<int>(css_h) != win_h)
    glfwSetWindowSize(win, static_cast<int>(css_w), static_cast<int>(css_h));
#endif
  glfwPollEvents();
  ImGuiIO& io = ImGui::GetIO();
  const double t = g_shot.path.empty() ? glfwGetTime() : g_now + kShotStep;
  g_dt  = std::clamp(t - g_now, 0.0, 0.1);
  g_now = t;
  poll_shape_reload();

  if (g.loaded && g.playing) {
    const double step = 1.0 / static_cast<double>(g.fps);
    if (g_now - g.last_t >= step) {
      if (g.cur + 1 < g.total) {
        set_frame(g.cur + 1, true);
        // Keep the beat, unless the page stalled: then pick it up from here.
        g.last_t = g_now - g.last_t > 3.0 * step ? g_now : g.last_t + step;
      } else {
        g.playing = false;
      }
    }
  }

  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();

  const Rect was = g.win;
  g.win = {{0.0f, 0.0f}, io.DisplaySize};
  if (was.w() != g.win.w() || was.h() != g.win.h()) g.cam_snap = true;
  layout_hud();
  handle_input(io, win);
  if (g.loaded) {
    update_vis();
    update_zones();
    build_draw_units();
    update_fx();
  }
  update_camera();
  if (g.loaded && g.select_arg) {
    select_at(to_screen(static_cast<float>(g.sel_tile.x), static_cast<float>(g.sel_tile.y)));
    g.select_arg = false;
  }

  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(io.DisplaySize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
  ImGui::Begin("##ud", nullptr,
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
      ImGuiWindowFlags_NoScrollWithMouse);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  draw_backdrop(dl);
  if (g.loaded) {
    const auto& fr = g.frames[static_cast<size_t>(g.cur)];
    draw_world(dl);
    draw_hud(dl, fr);
  } else {
    draw_welcome(dl);
  }
  ImGui::End();
  ImGui::PopStyleVar();

  if (g.chrome) draw_sel_panel();

  ImGui::Render();
  int fb_w, fb_h;
  glfwGetFramebufferSize(win, &fb_w, &fb_h);
  glViewport(0, 0, fb_w, fb_h);
  glClearColor(kSpaceTop.x, kSpaceTop.y, kSpaceTop.z, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#ifndef __EMSCRIPTEN__
  if (!g_shot.path.empty() && g_now >= g_shot.play && --g_shot.settle <= 0) {
    capture_shot(fb_w, fb_h);
    g_shot.done = true;
  }
#endif
  glfwSwapBuffers(win);
}

#ifdef __EMSCRIPTEN__
// The page writes the replay into the in-memory file system, then calls this.
extern "C" EMSCRIPTEN_KEEPALIVE void load_replay_from_fs() {
  strncpy(g.path_buf, "/replay.ud", sizeof(g.path_buf) - 1);
  try_load("/replay.ud");
}

// What the viewer shows now, as JSON, for a page that draws its own status around the canvas
// (the tournament site's match page polls it). Valid until the next call.
extern "C" EMSCRIPTEN_KEEPALIVE const char* viewer_status() {
  static std::string out;
  if (!g.loaded) {
    out = "{\"loaded\":false}";
    return out.c_str();
  }
  const auto& fr = g.frames[static_cast<size_t>(g.cur)];
  update_zones();
  SideCount side[2];
  count_sides(fr, side);
  char buf[320];
  out.clear();
  snprintf(buf, sizeof(buf),
           "{\"loaded\":true,\"frame\":%d,\"frames\":%d,\"tick\":%u,\"playing\":%s,"
           "\"fps\":%.0f,\"view\":%d,\"zones\":%s,\"threshold\":%d,\"autocam\":%s,\"hud\":%s,"
           "\"sides\":[",
           g.cur, g.total, fr.tick, g.playing ? "true" : "false", g.fps, g.view,
           g.show_zones ? "true" : "false", game::win_threshold(fr.tick, g.tick_cap),
           g.autocam ? "true" : "false", g.chrome ? "true" : "false");
  out += buf;
  for (int f = 0; f < 2; ++f) {
    snprintf(buf, sizeof(buf),
             "%s{\"territory\":%u,\"energy\":%d,\"alloy\":%d,\"ships\":%d,\"drones\":%d,"
             "\"claims\":%d,\"building\":%d,\"factories\":%d}",
             f ? "," : "", g.zone_pct.pct_faction[f], fr.resources[f].energy,
             fr.resources[f].alloy, side[f].ships, side[f].drones, side[f].claims,
             side[f].building, side[f].factories);
    out += buf;
  }
  out += "],\"result\":";
  if (fr.result.has_value()) {
    const auto& res = *fr.result;
    const bool draw = res.reason == game::WinReason::Draw;
    snprintf(buf, sizeof(buf), "{\"winner\":%d,\"text\":\"%s\"}",
             draw ? -1 : static_cast<int>(res.winner.value),
             game::result_reason(res, fr.forfeits, {"A", "B"}).c_str());
    out += buf;
  } else {
    out += "null";
  }
  out += "}";
  return out.c_str();
}

enum ViewerOp : int {
  kPlay = 0, kSeek = 1, kView = 2, kFps = 3, kZones = 4, kChrome = 5, kAutoCam = 6
};

// The page's controls: play (1) / pause (0), seek to a frame, view (-2 both sides' sight, -1 no
// fog, 0/1 a side), frames per second, zones on/off, the HUD on/off, and the auto camera on/off.
extern "C" EMSCRIPTEN_KEEPALIVE void viewer_control(int op, int value) {
  switch (op) {
  case kPlay: set_playing(value != 0); break;
  case kSeek:
    if (g.loaded) {
      g.playing = false;
      set_frame(value, false);
    }
    break;
  case kView: g.view = std::clamp(value, -2, 1); break;
  case kFps: g.fps = static_cast<float>(std::clamp(value, 1, 60)); break;
  case kZones: g.show_zones = value != 0; break;
  case kChrome: g.chrome = value != 0; break;
  case kAutoCam: g.autocam = value != 0; break;
  default: break;
  }
}
#endif

static void load_fonts() {
  ImGuiIO& io = ImGui::GetIO();
  FILE* f = fopen(UD_FONT_PATH, "rb");
  if (!f) {
    fprintf(stderr, "Font: %s not found, using the built-in font\n", UD_FONT_PATH);
    g.font_ui = g.font_big = g.font_small = io.Fonts->AddFontDefault();
    return;
  }
  fclose(f);
  // Latin-1: the speed buttons' ½ and ×, and the kill feed's ×.
  static const ImWchar ranges[] = {0x0020, 0x00FF, 0};
  g.font_small = io.Fonts->AddFontFromFileTTF(UD_FONT_PATH, 14.0f, nullptr, ranges);
  g.font_ui    = io.Fonts->AddFontFromFileTTF(UD_FONT_PATH, 18.0f, nullptr, ranges);
  g.font_big   = io.Fonts->AddFontFromFileTTF(UD_FONT_PATH, 30.0f, nullptr, ranges);
}

static void style_ui() {
  ImGui::StyleColorsDark();
  ImGuiStyle& st = ImGui::GetStyle();
  st.WindowRounding = 8.0f;
  st.PopupRounding = 6.0f;
  st.WindowBorderSize = 1.0f;
  st.Colors[ImGuiCol_WindowBg]  = kPanel;
  st.Colors[ImGuiCol_PopupBg]   = {0.03f, 0.045f, 0.085f, 0.96f};
  st.Colors[ImGuiCol_Border]    = kPanelLine;
  st.Colors[ImGuiCol_Text]      = kText;
  st.Colors[ImGuiCol_Separator] = kPanelLine;
}

int main(int argc, char** argv) {
  if (game::print_version_if_asked(argc, argv, "ud_viz_imgui")) return 0;
  const char* replay_arg = nullptr;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--shapes") == 0 && i + 1 < argc) {
      g.shapes_path = argv[++i];
    } else if (strcmp(argv[i], "--dump-shapes") == 0 && i + 1 < argc) {
      make_default_shapes(g.shapes);
      const bool ok = save_shapes(g.shapes, argv[++i]);
      fprintf(stderr, ok ? "Wrote %s\n" : "error: cannot write %s\n", argv[i]);
      return ok ? 0 : 1;
    } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
      g_shot.path = argv[++i];
    } else if (strcmp(argv[i], "--frame") == 0 && i + 1 < argc) {
      g_shot.frame = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--play") == 0 && i + 1 < argc) {
      g_shot.play = atof(argv[++i]);
    } else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
      sscanf(argv[++i], "%dx%d", &g_shot.w, &g_shot.h);
    } else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
      g.zoom_arg = static_cast<float>(atof(argv[++i]));
    } else if (strcmp(argv[i], "--center") == 0 && i + 1 < argc) {
      g.center_arg = sscanf(argv[++i], "%f,%f", &g.center_x, &g.center_y) == 2;
    } else if (strcmp(argv[i], "--select") == 0 && i + 1 < argc) {
      g.select_arg = sscanf(argv[++i], "%d,%d", &g.sel_tile.x, &g.sel_tile.y) == 2;
    } else if (strcmp(argv[i], "--view") == 0 && i + 1 < argc) {
      g.view = atoi(argv[++i]);
    } else if (strcmp(argv[i], "--no-hud") == 0) {
      g.chrome = false;
    } else if (!replay_arg) {
      replay_arg = argv[i];
    }
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
#ifdef __EMSCRIPTEN__
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3); // WebGL 2
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
  const char* glsl = "#version 300 es";
#else
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
  const char* glsl = "#version 330";
#endif

  if (!g_shot.path.empty()) {
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    if (g.loaded) {
      g.cur = std::clamp(g_shot.frame, 0, g.total - 1);
      g.playing = g_shot.play > 0.0;
    }
  }
  GLFWwindow* win = glfwCreateWindow(g_shot.w, g_shot.h, "Unthinking Dark", nullptr, nullptr);
  if (!win) { glfwTerminate(); fprintf(stderr, "glfwCreateWindow failed\n"); return 1; }
  glfwMakeContextCurrent(win);
  glfwSwapInterval(g_shot.path.empty() ? 1 : 0);

  glfwSetDropCallback(win, [](GLFWwindow*, int count, const char** paths) {
    if (count > 0) {
      strncpy(g.path_buf, paths[0], sizeof(g.path_buf) - 1);
      try_load(paths[0]);
    }
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  style_ui();
  load_fonts();

  ImGui_ImplGlfw_InitForOpenGL(win, true);
  ImGui_ImplOpenGL3_Init(glsl);

  g_now = g_shot.path.empty() ? glfwGetTime() : 0.0;
  g.last_t = g_now;
  g_win = win;

#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop(frame, 0, 1); // never returns
#else
  while (!glfwWindowShouldClose(win) && !g_shot.done)
    frame();
#endif

  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  glfwDestroyWindow(win);
  glfwTerminate();
  return 0;
}
