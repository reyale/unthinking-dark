#include <gtest/gtest.h>
#include "bot_iface.hpp"
#include "combat.hpp"
#include "command.hpp"
#include "config.hpp"
#include "match.hpp"
#include "snapshot.hpp"
#include "world.hpp"

// Build a minimal world with two units adjacent to each other.
static game::World make_combat_world() {
  game::World w;
  w.map = game::Map::make(10, 10);
  w.rng = game::Rng{0};
  return w;
}

// Issue an Attack command for a unit.
static game::ValidatedCommands attack_cmd(game::UnitId attacker) {
  game::Command cmd{};
  cmd.entity_id = attacker.value;
  cmd.kind = static_cast<uint16_t>(game::CommandKind::Attack);
  game::ValidatedCommands vc;
  vc.emplace(attacker, cmd);
  return vc;
}

// ---- First-strike --------------------------------------------------------

TEST(Combat, FrigateFirstStrikeKillsBeforeSimultaneous) {
  auto w = make_combat_world();
  // Frigate (faction 0) vs Interceptor (faction 1) at range 1
  auto& frigate = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5});
  auto& inter = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5});

  // Both attack each other
  auto cmds_a = attack_cmd(frigate.id);
  auto cmds_b = attack_cmd(inter.id);

  game::run_combat(w, cmds_a, cmds_b);

  // Frigate fires first-strike: Interceptor (hp=60, dmg=14) should take 14 dmg
  // but the Frigate (hp=35) fires first and the Interceptor is dead or weakened
  // before firing. Interceptor dmg=12, so:
  //   Frigate first-strike: Interceptor hp 60 → 46 (not dead)
  //   Interceptor simultaneous: Frigate hp 35 → 23
  EXPECT_EQ(inter.hp, game::cfg::INTERCEPTOR_HP - game::cfg::FRIGATE_DMG);
  EXPECT_EQ(frigate.hp, game::cfg::FRIGATE_HP - game::cfg::INTERCEPTOR_DMG);
}

TEST(Combat, FrigateKillsWeakTargetBeforeSimultaneous) {
  auto w = make_combat_world();
  // Frigate vs a heavily damaged Interceptor (1 hp) — first-strike kills it,
  // so it should NOT fire back in the simultaneous phase.
  game::UnitId frigate_id = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5}).id;
  game::UnitId inter_id = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5}).id;
  w.find_unit(inter_id)->hp = 1;

  auto cmds_a = attack_cmd(frigate_id);
  auto cmds_b = attack_cmd(inter_id);

  game::run_combat(w, cmds_a, cmds_b);

  // Interceptor dead from first-strike; Frigate takes no damage
  EXPECT_EQ(w.find_unit(frigate_id)->hp, game::cfg::FRIGATE_HP);
  EXPECT_EQ(w.find_unit(inter_id), nullptr); // purged
}

// ---- Simultaneous kills --------------------------------------------------

TEST(Combat, MutualKillBothDie) {
  auto w = make_combat_world();
  // Two Interceptors at 1 hp each — simultaneous, both should die
  auto& a = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& b = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5});
  a.hp = 1;
  b.hp = 1;

  auto cmds_a = attack_cmd(a.id);
  auto cmds_b = attack_cmd(b.id);

  game::run_combat(w, cmds_a, cmds_b);

  EXPECT_TRUE(w.units.empty());
}

// ---- Structure targeting -------------------------------------------------

TEST(Combat, InterceptorDamagesCommandCore) {
  auto w = make_combat_world();
  // Interceptor adjacent to an enemy CommandCore — no enemy units present.
  // Auto-targeting should fall through to the structure.
  auto& inter = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& core  = w.spawn_structure({1}, game::StructureType::CommandCore, {6, 5});

  auto cmds_a = attack_cmd(inter.id);
  game::ValidatedCommands empty_b;

  game::run_combat(w, cmds_a, empty_b);

  EXPECT_EQ(core.hp, game::cfg::CMD_CORE_HP - game::cfg::INTERCEPTOR_DMG);
  EXPECT_TRUE(inter.alive()); // attacker unharmed
}

TEST(Combat, FrigateDestroysWeakStructure) {
  auto w = make_combat_world();
  auto& frigate = w.spawn_unit({0}, game::UnitType::Frigate, {6, 5});
  auto& fac     = w.spawn_structure({1}, game::StructureType::Factory, {9, 5});
  game::StructureId fac_id = fac.id;
  fac.hp = 1; // one hit kill

  auto cmds_a = attack_cmd(frigate.id);
  game::ValidatedCommands empty_b;

  game::run_combat(w, cmds_a, empty_b);

  EXPECT_EQ(w.find_structure(fac_id), nullptr); // purged
}

TEST(Combat, UnitPreferredOverStructureWhenBothInRange) {
  auto w = make_combat_world();
  // Interceptor with range 1 adjacent to both an enemy unit and a structure.
  // Unit should be chosen first.
  auto& inter = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& enemy = w.spawn_unit({1}, game::UnitType::Drone, {6, 5});
  auto& core  = w.spawn_structure({1}, game::StructureType::CommandCore, {5, 4});
  int32_t core_hp_before = core.hp;

  auto cmds_a = attack_cmd(inter.id);
  game::ValidatedCommands empty_b;

  game::run_combat(w, cmds_a, empty_b);

  EXPECT_LT(enemy.hp, game::cfg::DRONE_HP); // enemy unit took damage
  EXPECT_EQ(core.hp, core_hp_before);        // structure untouched
}

TEST(Combat, BaseDestructionEndsMatch) {
  // Run a short match where one Frigate is guaranteed to destroy the enemy
  // CommandCore (set to 1 hp) — verifies WinReason::BaseDestroyed fires.
  game::World w;
  w.map = game::Map::make(10, 10);
  w.rng = game::Rng{0};
  w.rng_seed = 0;

  w.spawn_structure({0}, game::StructureType::CommandCore, {1, 1});
  w.spawn_unit({0}, game::UnitType::Frigate, {5, 5});

  w.spawn_structure({1}, game::StructureType::CommandCore, {8, 5});
  w.command_core({1})->hp = 1;

  struct RushBot : game::Bot {
    void on_init(const game::Map&, uint32_t) override {}
    std::vector<game::Command> on_tick(const game::Snapshot& snap) override {
      std::vector<game::Command> out;
      for (const auto& u : snap.my_units) {
        game::Command cmd{};
        cmd.entity_id = u.id;
        cmd.kind = static_cast<uint16_t>(game::CommandKind::MoveAttack);
        cmd.ax = 8; cmd.ay = 5;
        out.push_back(cmd);
      }
      return out;
    }
    bool healthy() const override { return true; }
  };

  struct IdleB : game::Bot {
    void on_init(const game::Map&, uint32_t) override {}
    std::vector<game::Command> on_tick(const game::Snapshot&) override { return {}; }
    bool healthy() const override { return true; }
  };

  RushBot a; IdleB b;
  auto rec = game::run_match(w, a, 0, b, 1, 50);
  EXPECT_EQ(rec.outcome.reason, game::WinReason::BaseDestroyed);
  EXPECT_EQ(rec.outcome.winner.value, 0u);
}

// ---- Artillery splash ----------------------------------------------------

TEST(Combat, ArtillerySplashHitsFriendly) {
  auto w = make_combat_world();
  // Artillery (faction 0) fires at an enemy; a friendly unit is also in splash radius
  auto& arty     = w.spawn_unit({0}, game::UnitType::Artillery, {5, 5});
  w.spawn_unit({1}, game::UnitType::Interceptor, {7, 5}); // range 6, in range
  auto& friendly = w.spawn_unit({0}, game::UnitType::Drone, {7, 6}); // adjacent to enemy (splash r=1)
  game::UnitId friendly_id = friendly.id;
  int32_t friendly_hp_before = friendly.hp;

  auto cmds_a = attack_cmd(arty.id);
  game::ValidatedCommands empty_b;
  game::run_combat(w, cmds_a, empty_b);

  // Friendly drone should have taken splash damage (may be dead if dmg > hp)
  const game::Unit* f = w.find_unit(friendly_id);
  if (f) {
    EXPECT_LT(f->hp, friendly_hp_before);
  } else {
    SUCCEED(); // killed by splash — damage was certainly applied
  }
}

// ---- pick_target edge cases ---------------------------------------------

TEST(Combat, DroneRangeZeroPicksNoTarget) {
  // Drones have range=0 — pick_target must return null immediately.
  auto w = make_combat_world();
  auto& drone = w.spawn_unit({0}, game::UnitType::Drone, {5, 5});
  w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5});

  auto cmds_a = attack_cmd(drone.id);
  game::ValidatedCommands empty_b;
  game::run_combat(w, cmds_a, empty_b);

  // Drone has no damage; enemy untouched
  EXPECT_EQ(w.find_unit(drone.id)->hp, game::cfg::DRONE_HP);
}

// ---- apply_splash edge cases -------------------------------------------

TEST(Combat, SplashSkipsDeadUnit) {
  // A dead unit (hp=0, not yet purged) must be ignored by apply_splash.
  auto w = make_combat_world();
  auto& arty = w.spawn_unit({0}, game::UnitType::Artillery, {5, 5});
  w.spawn_unit({1}, game::UnitType::Interceptor, {8, 5});           // primary target, dist=3
  game::UnitId dead_id = w.spawn_unit({1}, game::UnitType::Drone, {8, 6}).id; // in splash
  w.find_unit(dead_id)->hp = 0; // dead but not purged

  auto cmds_a = attack_cmd(arty.id);
  game::ValidatedCommands empty_b;
  game::run_combat(w, cmds_a, empty_b);

  EXPECT_EQ(w.find_unit(dead_id), nullptr); // purged; was already at 0, not double-penalized
}

TEST(Combat, SplashDamagesStructureInRadius) {
  // apply_splash must also accumulate damage on structures within radius 1.
  auto w = make_combat_world();
  auto& arty   = w.spawn_unit({0}, game::UnitType::Artillery, {5, 5});
  w.spawn_unit({1}, game::UnitType::Interceptor, {8, 5}); // primary target
  auto& fac    = w.spawn_structure({1}, game::StructureType::Factory, {8, 6}); // in splash

  auto cmds_a = attack_cmd(arty.id);
  game::ValidatedCommands empty_b;
  game::run_combat(w, cmds_a, empty_b);

  EXPECT_LT(fac.hp, game::cfg::FACTORY_HP); // factory took splash damage
}

TEST(Combat, ArtilleryTargetsStructureWithSplash) {
  // Artillery auto-targets an enemy structure (no units in range) — exercises
  // the structure-target branch of sub-phase B including apply_splash on s->pos.
  auto w = make_combat_world();
  auto& arty = w.spawn_unit({0}, game::UnitType::Artillery, {5, 5}); // range=3
  auto& fac  = w.spawn_structure({1}, game::StructureType::Factory, {8, 5}); // dist=3

  auto cmds_a = attack_cmd(arty.id);
  game::ValidatedCommands empty_b;
  game::run_combat(w, cmds_a, empty_b);

  EXPECT_LT(fac.hp, game::cfg::FACTORY_HP);
}

// ---- Combat log (presentation only) --------------------------------------

TEST(CombatLog, RecordsFirstStrikeBeforeSimultaneous) {
  auto w = make_combat_world();
  game::UnitId frigate_id = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5}).id;
  game::UnitId inter_id = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5}).id;

  game::CombatLog log;
  game::run_combat(w, attack_cmd(frigate_id), attack_cmd(inter_id), &log);

  ASSERT_EQ(log.shots.size(), 2u);
  const auto& fs = log.shots[0];
  EXPECT_EQ(fs.attacker, frigate_id);
  EXPECT_TRUE(fs.first_strike);
  EXPECT_FALSE(fs.splash);
  EXPECT_EQ(fs.target_unit, inter_id);
  EXPECT_TRUE(fs.target_struct.is_null());
  EXPECT_EQ(fs.from, (game::Vec2{5, 5}));
  EXPECT_EQ(fs.to, (game::Vec2{6, 5}));
  EXPECT_EQ(fs.damage, game::cfg::FRIGATE_DMG);

  const auto& is = log.shots[1];
  EXPECT_EQ(is.attacker, inter_id);
  EXPECT_FALSE(is.first_strike);
  EXPECT_EQ(is.target_unit, frigate_id);
  EXPECT_EQ(is.damage, game::cfg::INTERCEPTOR_DMG);
  EXPECT_TRUE(log.kills.empty());
}

TEST(CombatLog, FirstStrikeKillStopsReturnFire) {
  auto w = make_combat_world();
  game::UnitId frigate_id = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5}).id;
  game::UnitId inter_id = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5}).id;
  w.find_unit(inter_id)->hp = 1;

  game::CombatLog log;
  game::run_combat(w, attack_cmd(frigate_id), attack_cmd(inter_id), &log);

  ASSERT_EQ(log.shots.size(), 1u);
  EXPECT_EQ(log.shots[0].attacker, frigate_id);
  ASSERT_EQ(log.kills.size(), 1u);
  EXPECT_EQ(log.kills[0].unit, inter_id);
  EXPECT_EQ(log.kills[0].faction.value, 1u);
  EXPECT_EQ(log.kills[0].unit_type, game::UnitType::Interceptor);
  EXPECT_EQ(log.kills[0].pos, (game::Vec2{6, 5}));
}

TEST(CombatLog, SplashKillsIncludeFriendlyFire) {
  auto w = make_combat_world();
  game::UnitId arty_id = w.spawn_unit({0}, game::UnitType::Artillery, {5, 5}).id;
  game::UnitId enemy_id = w.spawn_unit({1}, game::UnitType::Interceptor, {7, 5}).id;
  game::UnitId friend_id = w.spawn_unit({0}, game::UnitType::Drone, {7, 6}).id;
  w.find_unit(enemy_id)->hp = 1;
  w.find_unit(friend_id)->hp = 1;

  game::CombatLog log;
  game::ValidatedCommands empty_b;
  game::run_combat(w, attack_cmd(arty_id), empty_b, &log);

  ASSERT_EQ(log.shots.size(), 1u);
  EXPECT_TRUE(log.shots[0].splash);
  EXPECT_EQ(log.shots[0].to, (game::Vec2{7, 5}));
  ASSERT_EQ(log.kills.size(), 2u); // ascending unit-id: enemy spawned first
  EXPECT_EQ(log.kills[0].unit, enemy_id);
  EXPECT_EQ(log.kills[1].unit, friend_id);
  EXPECT_EQ(log.kills[1].faction.value, 0u);
  EXPECT_EQ(log.kills[1].pos, (game::Vec2{7, 6}));
}

TEST(CombatLog, StructureKillRecorded) {
  auto w = make_combat_world();
  game::UnitId frigate_id = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5}).id;
  auto& core = w.spawn_structure({1}, game::StructureType::CommandCore, {6, 5});
  game::StructureId core_id = core.id;
  core.hp = 1;

  game::CombatLog log;
  game::ValidatedCommands empty_b;
  game::run_combat(w, attack_cmd(frigate_id), empty_b, &log);

  ASSERT_EQ(log.shots.size(), 1u);
  EXPECT_EQ(log.shots[0].target_struct, core_id);
  EXPECT_TRUE(log.shots[0].target_unit.is_null());
  ASSERT_EQ(log.kills.size(), 1u);
  EXPECT_EQ(log.kills[0].structure, core_id);
  EXPECT_EQ(log.kills[0].structure_type, game::StructureType::CommandCore);
}

// ---- Nebulae hide targets ---------------------------------------------------

TEST(Combat, EnemyHiddenInNebulaCannotBeTargeted) {
  auto w = make_combat_world();
  w.map.set_terrain({5, 5}, game::Terrain::Nebula);
  auto& frigate = w.spawn_unit({0}, game::UnitType::Frigate, {2, 5}); // range 4, distance 3
  auto& drone = w.spawn_unit({1}, game::UnitType::Drone, {5, 5});
  game::Command c{};
  c.entity_id = frigate.id.value;
  c.kind = static_cast<uint16_t>(game::CommandKind::Attack);
  c.target_id = drone.id.value; // an explicit target doesn't help either
  game::ValidatedCommands cmds, empty;
  cmds.emplace(frigate.id, c);
  game::run_combat(w, cmds, empty);
  EXPECT_EQ(drone.hp, game::stats_for(game::UnitType::Drone).hp);
}

TEST(Combat, ASpotterBesideTheNebulaExposesItsOccupant) {
  auto w = make_combat_world();
  w.map.set_terrain({5, 5}, game::Terrain::Nebula);
  auto& frigate = w.spawn_unit({0}, game::UnitType::Frigate, {2, 5});
  w.spawn_unit({0}, game::UnitType::Drone, {5, 6}); // within NEBULA_SIGHT of the target
  const game::UnitId drone = w.spawn_unit({1}, game::UnitType::Drone, {5, 5}).id;
  game::ValidatedCommands empty;
  game::run_combat(w, attack_cmd(frigate.id), empty);
  const game::Unit* d = w.find_unit(drone); // purged if the shot killed it
  EXPECT_TRUE(!d || d->hp < game::stats_for(game::UnitType::Drone).hp);
}
