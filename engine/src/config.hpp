#pragma once
#include <cstdint>

// All §12 balance constants and v1 hard caps live here — nowhere else.
// Every value tagged (v1 placeholder) is expected to change during calibration.
namespace game::cfg {

// ---- Map ---------------------------------------------------------------
inline constexpr int32_t MAP_MAX_W = 128;
inline constexpr int32_t MAP_MAX_H = 128;
inline constexpr int32_t MAP_DEFAULT_W = 80; // v1 placeholder
inline constexpr int32_t MAP_DEFAULT_H = 80; // v1 placeholder

// ---- Match -------------------------------------------------------------
inline constexpr int32_t TICK_CAP = 3000;     // v1 placeholder
inline constexpr int32_t UNIT_HARD_CAP = 256; // per side; bounds fuel/memory worst-case
// Per side, all structure types. Bounds the snapshot like UNIT_HARD_CAP; Build and
// DeployClaim are refused at the cap.
inline constexpr int32_t STRUCTURE_HARD_CAP = 128; // v1 placeholder
// Every structure sees this far (Manhattan), so a base isn't blind without units nearby.
inline constexpr int32_t STRUCTURE_SIGHT = 2; // v1 placeholder
// A nebula tile, and anything on it, is seen only from within NEBULA_SIGHT (Manhattan), and a
// unit there can only be targeted by a faction that sees it. Units inside see out normally.
inline constexpr int32_t NEBULA_SIGHT = 1;

// ---- Win condition / territory -----------------------------------------
// threshold(g) = THRESH_A - THRESH_B * g^2,  g = tick / TICK_CAP
inline constexpr int32_t THRESH_A = 78;   // % at g=0  (v1 placeholder)
inline constexpr int32_t THRESH_B = 27;   // convex drop (v1 placeholder)
inline constexpr int32_t THRESH_MIN = 51; // floor sanity (should equal A-B)

inline constexpr int32_t CLAIM_INFLUENCE = 12; // tiles radius, ~2–3% of 80×80 (v1 placeholder)

// Claim hp: eff_hp = base*(1-g) + floor, drives late-game volatility
inline constexpr int32_t CLAIM_HP_BASE = 200; // v1 placeholder
inline constexpr int32_t CLAIM_HP_FLOOR = 1;
// Construction: a deployed claim starts at CLAIM_BUILD_START_HP and needs CLAIM_BUILD_TICKS
// ticks with an own Drone within CLAIM_DEPLOY_RANGE (it waits, still standing, while none is).
// Its hp rises with each tick of work. Until finished it holds no territory, is no drop-off
// and adds no Drone-cap slot.
inline constexpr int32_t CLAIM_BUILD_TICKS = 20;
inline constexpr int32_t CLAIM_BUILD_START_HP = 10;
// Claims defend themselves, very weakly: each finished claim shoots one enemy unit within
// CLAIM_ATTACK_RANGE per tick (nearest it can see, ties → lower id). Enough to see off a lone
// Interceptor at full claim hp; two of them, or anything that out-ranges it, still take it.
inline constexpr int32_t CLAIM_ATTACK_DMG = 1;
inline constexpr int32_t CLAIM_ATTACK_RANGE = 2;

// ---- Unit stats (v1 placeholders — iterate fast) -----------------------
inline constexpr int32_t DRONE_HP = 8;
inline constexpr int32_t DRONE_DMG = 0;   // Drones are economy units; they do not attack
inline constexpr int32_t DRONE_RANGE = 0;
inline constexpr int32_t DRONE_SIGHT = 3;
inline constexpr int32_t DRONE_SPEED = 1;

inline constexpr int32_t INTERCEPTOR_HP = 15;
inline constexpr int32_t INTERCEPTOR_DMG = 5;
inline constexpr int32_t INTERCEPTOR_RANGE = 2;
inline constexpr int32_t INTERCEPTOR_SIGHT = 3;
inline constexpr int32_t INTERCEPTOR_SPEED = 2;

inline constexpr int32_t FRIGATE_HP = 65;
inline constexpr int32_t FRIGATE_DMG = 12;
inline constexpr int32_t FRIGATE_RANGE = 3;
inline constexpr int32_t FRIGATE_SIGHT = 3;
inline constexpr int32_t FRIGATE_SPEED = 1;

inline constexpr int32_t ARTILLERY_HP = 25;
inline constexpr int32_t ARTILLERY_DMG = 22;
inline constexpr int32_t ARTILLERY_RANGE = 6;
inline constexpr int32_t ARTILLERY_SIGHT = 2;
inline constexpr int32_t ARTILLERY_SPEED = 1;
inline constexpr int32_t ARTILLERY_SPLASH_RADIUS = 1;

inline constexpr int32_t BATTLESHIP_HP = 150;
inline constexpr int32_t BATTLESHIP_DMG = 35;
inline constexpr int32_t BATTLESHIP_RANGE = 4;
inline constexpr int32_t BATTLESHIP_SIGHT = 5;
inline constexpr int32_t BATTLESHIP_SPEED = 1;

// ---- Structure stats ---------------------------------------------------
inline constexpr int32_t CMD_CORE_HP = 600;
inline constexpr int32_t FACTORY_HP = 150;

// ---- Economy (v1 placeholders — tune in sim) ---------------------------
// Harvest yield per Drone per tick, once however many nodes it touches. It goes into the
// Drone's cargo and is only income once dropped at its own Command Core or a Claim Node
// within DEPOSIT_RANGE (Manhattan). A full Drone harvests nothing; a dead one loses its load.
inline constexpr int32_t HARVEST_ENERGY = 2;
inline constexpr int32_t HARVEST_ALLOY = 1;
inline constexpr int32_t DRONE_CARGO_ENERGY = 10;
inline constexpr int32_t DRONE_CARGO_ALLOY = 5;
inline constexpr int32_t DEPOSIT_RANGE = 1;

// Drone cap: this many per living Claim Node, plus per living Command Core. Drones queued in a
// Core count. Losing a claim never kills Drones; it only blocks producing more.
inline constexpr int32_t DRONES_PER_CLAIM = 1;
inline constexpr int32_t DRONES_PER_CORE = 1;

// Production costs (energy, alloy)
inline constexpr int32_t DRONE_COST_E = 5,        DRONE_COST_A = 0;
inline constexpr int32_t INTERCEPTOR_COST_E = 10, INTERCEPTOR_COST_A = 5;
inline constexpr int32_t FRIGATE_COST_E = 40,     FRIGATE_COST_A = 5;
inline constexpr int32_t ARTILLERY_COST_E = 10,   ARTILLERY_COST_A = 25;
inline constexpr int32_t BATTLESHIP_COST_E = 120, BATTLESHIP_COST_A = 15;
inline constexpr int32_t FACTORY_COST_E = 0,      FACTORY_COST_A = 50;
inline constexpr int32_t CLAIM_COST_E = 0,        CLAIM_COST_A = 20;
// A Drone deploys a Claim Node onto a tile within this Manhattan distance of itself.
// v1 placeholder.
inline constexpr int32_t CLAIM_DEPLOY_RANGE = 2;
// A Drone builds a Factory on a tile within this Manhattan distance of itself. v1 placeholder.
inline constexpr int32_t BUILD_RANGE = 1;
inline constexpr int32_t PRODUCTION_TICKS = 3; // Factory build time, every ship type (v1 placeholder)

// ---- WASM budgets (v1 placeholders — tune in sim) ----------------------
inline constexpr uint64_t FUEL_STARTUP = 1'000'000'000ULL;
inline constexpr uint64_t FUEL_PER_TICK = 10'000'000ULL;
inline constexpr uint32_t MEMORY_CAP_MB = 64;

// ---- Player names ------------------------------------------------------
inline constexpr uint32_t MAX_PLAYER_NAME_LEN = 32;

// ---- Draw condition ----------------------------------------------------
// At tick cap, if |territory_pct[0] - territory_pct[1]| <= this, it's a draw.
inline constexpr int32_t DRAW_TERRITORY_MARGIN = 5; // percentage points (v1 placeholder)

// ---- ABI ---------------------------------------------------------------
// TODO - have it generated by wireformat - see notes in abi.hpp
inline constexpr uint32_t ABI_VERSION = 12;

} // namespace game::cfg
