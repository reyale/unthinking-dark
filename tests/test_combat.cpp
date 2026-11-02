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

  // The Frigate's first strike breaks the Interceptor's shield and dents its hull without
  // killing it, so it fires back in the simultaneous phase, into the Frigate's shield.
  using namespace game::cfg;
  static_assert(FRIGATE_DMG > INTERCEPTOR_SHIELD &&
                FRIGATE_DMG < INTERCEPTOR_SHIELD + INTERCEPTOR_HP);
  static_assert(INTERCEPTOR_DMG < FRIGATE_SHIELD);
  EXPECT_EQ(inter.shield, 0);
  EXPECT_EQ(inter.hp, INTERCEPTOR_HP + INTERCEPTOR_SHIELD - FRIGATE_DMG);
  EXPECT_EQ(frigate.shield, FRIGATE_SHIELD - INTERCEPTOR_DMG);
  EXPECT_EQ(frigate.hp, FRIGATE_HP);
}

TEST(Combat, FrigateKillsWeakTargetBeforeSimultaneous) {
  auto w = make_combat_world();
  // Frigate vs a heavily damaged Interceptor (1 hp) — first-strike kills it,
  // so it should NOT fire back in the simultaneous phase.
  game::UnitId frigate_id = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5}).id;
  game::UnitId inter_id = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5}).id;
  w.find_unit(inter_id)->hp = 1;
  w.find_unit(inter_id)->shield = 0;

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
  // Two Interceptors at 1 hp, shields down — simultaneous, both should die
  auto& a = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& b = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5});
  a.hp = 1;
  b.hp = 1;
  a.shield = 0;
  b.shield = 0;

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

// ---- Claim Nodes fire, weakly -----------------------------------------------

// Fights it out with every attacker attacking until the claim or all of them are dead.
static bool claim_survives(int32_t interceptors) {
  auto w = make_combat_world();
  const game::StructureId claim =
      w.spawn_structure({1}, game::StructureType::ClaimNode, {5, 5}).id;
  game::ValidatedCommands cmds, empty;
  const game::Vec2 spots[] = {{3, 5}, {5, 3}, {7, 5}};
  for (int32_t i = 0; i < interceptors; ++i)
    cmds.merge(attack_cmd(w.spawn_unit({0}, game::UnitType::Interceptor, spots[i]).id));
  while (w.find_structure(claim) && !w.units.empty())
    game::run_combat(w, cmds, empty);
  return w.find_structure(claim) != nullptr;
}

TEST(Combat, FullStrengthClaimSeesOffALoneScoutButNotTwo) {
  EXPECT_TRUE(claim_survives(1));
  EXPECT_FALSE(claim_survives(2));
}

TEST(Combat, ClaimShootsNearestEnemyUnitInRangeOnly) {
  auto w = make_combat_world();
  w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  w.spawn_structure({1}, game::StructureType::Factory, {5, 6});     // structures: never
  auto& own  = w.spawn_unit({0}, game::UnitType::Frigate, {4, 5});  // own: never
  auto& far  = w.spawn_unit({1}, game::UnitType::Frigate, {5, 8});  // distance 3: out of range
  auto& near = w.spawn_unit({1}, game::UnitType::Frigate, {6, 6});  // distance 2
  auto& tie  = w.spawn_unit({1}, game::UnitType::Frigate, {7, 5});  // distance 2, higher id
  game::ValidatedCommands empty;
  game::CombatLog log;
  game::run_combat(w, empty, empty, &log);
  const auto& st = game::stats_for(game::UnitType::Frigate);
  const int32_t full = st.hp + st.shield;
  auto pool = [](const game::Unit& u) { return u.hp + u.shield; };
  EXPECT_EQ(pool(near), full - game::cfg::CLAIM_ATTACK_DMG);
  EXPECT_EQ(pool(tie), full);
  EXPECT_EQ(pool(far), full);
  EXPECT_EQ(pool(own), full);
  ASSERT_EQ(log.shots.size(), 1u);
  EXPECT_TRUE(log.shots[0].attacker.is_null());
  EXPECT_FALSE(log.shots[0].attacker_struct.is_null());
  EXPECT_EQ(log.shots[0].target_unit, near.id);
}

TEST(Combat, UnfinishedClaimDoesNotShoot) {
  auto w = make_combat_world();
  auto& c = w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  c.build_ticks_left = 5;
  auto& e = w.spawn_unit({1}, game::UnitType::Frigate, {6, 5});
  game::ValidatedCommands empty;
  game::run_combat(w, empty, empty);
  EXPECT_EQ(e.hp, game::stats_for(game::UnitType::Frigate).hp);
}

TEST(Combat, ClaimCannotShootIntoANebulaItCannotSee) {
  auto w = make_combat_world();
  w.map.set_terrain({7, 5}, game::Terrain::Nebula);
  w.spawn_structure({0}, game::StructureType::ClaimNode, {5, 5});
  auto& e = w.spawn_unit({1}, game::UnitType::Frigate, {7, 5}); // distance 2, hidden
  game::ValidatedCommands empty;
  game::run_combat(w, empty, empty);
  EXPECT_EQ(e.hp, game::stats_for(game::UnitType::Frigate).hp);
}

// ---- Shields -------------------------------------------------------------

TEST(Shields, ShipsSpawnWithFullShieldsAndDronesHaveNone) {
  auto w = make_combat_world();
  EXPECT_EQ(w.spawn_unit({0}, game::UnitType::Frigate, {1, 1}).shield, game::cfg::FRIGATE_SHIELD);
  EXPECT_EQ(w.spawn_unit({0}, game::UnitType::Drone, {2, 2}).shield, 0);
}

TEST(Shields, ShieldTakesTheHitAndOnlyTheOverflowReachesTheHull) {
  using namespace game::cfg;
  static_assert(ARTILLERY_DMG > FRIGATE_SHIELD);
  auto w = make_combat_world();
  auto& art = w.spawn_unit({0}, game::UnitType::Artillery, {1, 5});
  auto& fr = w.spawn_unit({1}, game::UnitType::Frigate, {5, 5});
  auto cmds = attack_cmd(art.id);
  game::ValidatedCommands empty;
  game::run_combat(w, cmds, empty);
  EXPECT_EQ(fr.shield, 0);
  EXPECT_EQ(fr.hp, FRIGATE_HP - (ARTILLERY_DMG - FRIGATE_SHIELD));
}

// Ships fire only when ordered, so after one ordered shot, empty commands are quiet ticks.
TEST(Shields, RegenerateOnlyAfterTheDelayAndStopAtFull) {
  using namespace game::cfg;
  auto w = make_combat_world();
  auto& in = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& fr = w.spawn_unit({1}, game::UnitType::Frigate, {6, 5});
  auto cmds = attack_cmd(in.id);
  game::ValidatedCommands empty;
  game::run_combat(w, cmds, empty);
  const int32_t hit = FRIGATE_SHIELD - INTERCEPTOR_DMG;
  EXPECT_EQ(fr.shield, hit); // a unit hit this tick doesn't regenerate
  EXPECT_EQ(fr.shield_wait, SHIELD_REGEN_DELAY - 1);
  for (int32_t t = 1; t < SHIELD_REGEN_DELAY; ++t) {
    game::run_combat(w, empty, empty);
    EXPECT_EQ(fr.shield, hit) << "quiet tick " << t;
  }
  game::run_combat(w, empty, empty);
  EXPECT_EQ(fr.shield, hit + FRIGATE_SHIELD_REGEN);
  for (int32_t t = 0; t < FRIGATE_SHIELD; ++t)
    game::run_combat(w, empty, empty);
  EXPECT_EQ(fr.shield, FRIGATE_SHIELD);
  EXPECT_EQ(fr.hp, FRIGATE_HP);
}

TEST(Shields, AFullyAbsorbedHitStillRestartsTheWait) {
  using namespace game::cfg;
  auto w = make_combat_world();
  auto& in = w.spawn_unit({0}, game::UnitType::Interceptor, {5, 5});
  auto& fr = w.spawn_unit({1}, game::UnitType::Frigate, {6, 5});
  auto cmds = attack_cmd(in.id);
  game::ValidatedCommands empty;
  game::run_combat(w, cmds, empty);
  for (int32_t t = 1; t < SHIELD_REGEN_DELAY; ++t)
    game::run_combat(w, empty, empty);
  game::run_combat(w, cmds, empty); // the tick it would have regenerated
  EXPECT_EQ(fr.shield, FRIGATE_SHIELD - 2 * INTERCEPTOR_DMG);
  EXPECT_EQ(fr.shield_wait, SHIELD_REGEN_DELAY - 1);
}

TEST(Shields, FriendlySplashCountsAsAHit) {
  using namespace game::cfg;
  auto w = make_combat_world();
  auto& art = w.spawn_unit({0}, game::UnitType::Artillery, {1, 5});
  w.spawn_unit({1}, game::UnitType::Frigate, {5, 5});
  auto& mate = w.spawn_unit({0}, game::UnitType::Battleship, {5, 6});
  auto cmds = attack_cmd(art.id);
  game::ValidatedCommands empty;
  game::run_combat(w, cmds, empty);
  EXPECT_EQ(mate.shield, BATTLESHIP_SHIELD - ARTILLERY_DMG);
  EXPECT_EQ(mate.shield_wait, SHIELD_REGEN_DELAY - 1);
}

TEST(Shields, SnapshotShowsOwnShieldAndWaitAndEnemyShield) {
  auto w = make_combat_world();
  auto& mine = w.spawn_unit({0}, game::UnitType::Frigate, {5, 5});
  auto& theirs = w.spawn_unit({1}, game::UnitType::Interceptor, {6, 5});
  mine.shield = 7;
  mine.shield_wait = 3;
  theirs.shield = 2;
  const game::Snapshot snap = game::build_snapshot(w, 0, 100);
  ASSERT_EQ(snap.my_units.size(), 1u);
  EXPECT_EQ(snap.my_units[0].shield, 7);
  EXPECT_EQ(snap.my_units[0].shield_wait, 3);
  ASSERT_EQ(snap.visible_enemies.size(), 1u);
  EXPECT_EQ(snap.visible_enemies[0].shield, 2);
}
