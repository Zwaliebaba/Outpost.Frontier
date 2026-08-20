#include "pch.h"
#include "CppUnitTest.h"

#include "EconomyDef.h"
#include "EconomyParse.h"
#include "OrderMessages.h"
#include "Orders.h"
#include "ShipClass.h"
#include "SiteEpoch.h"
#include "SiteField.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "Validate.h"
#include "World.h"
#include "WorldHash.h"
#include "WorldRegistry.h"

#include "ByteReader.h"
#include "ByteWriter.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * Mining in the sim, and the site ledger (Build Order E2, ADR-024 §3d/§4).
 *
 * The subject is an economy that has to be **exact and reproducible**: a unit of
 * ore is a number two machines have to agree about a day later, and every way
 * this could go wrong is quiet. A cluster that loses a unit to rounding is a
 * field nobody can finish; a yield that draws from the world's generator is a
 * replay that stops reproducing a thousand ticks after the divergence; a ledger
 * that forgets a teardown is a station full of ore that was never mined.
 *
 * So the suite is arranged by what it is protecting rather than by file: the
 * field's arithmetic, the order's refusals, the cycle in the tick, and the
 * ledger at the universe layer.
 *
 * **The content is the committed content.** Every economy here is parsed from
 * `GameData/Economy/Economy.json` -- U1's lesson, and the reason E1a's two
 * invisible bugs were caught. Where a test shortens the cycle it says so and
 * changes exactly that one number, so the litres, the pools and the hold sizes
 * under test are the ones that ship.
 */

namespace GameLogicTests
{

namespace
{

/*
 * The committed file, found by walking up from wherever the test host runs.
 *
 * The same loader `EconomyParseTests` has, and duplicated rather than shared
 * for the reason the Dependency Map gives: `GameLogicTests` may reference only
 * GameLogic and NeuronCore, so there is no place to put a shared test helper
 * that is not a new project. Twelve lines is cheaper than that.
 */
[[nodiscard]] EconomyDef CommittedEconomy()
{
  static constexpr const char* CANDIDATES[] = {
    "GameData/Economy/Economy.json",
    "../GameData/Economy/Economy.json",
    "../../GameData/Economy/Economy.json",
    "../../../GameData/Economy/Economy.json",
    "../../../../GameData/Economy/Economy.json",
  };

  for (const char* candidate : CANDIDATES)
  {
    std::ifstream file(candidate, std::ios::binary);
    if (!file)
    {
      continue;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string text = buffer.str();

    EconomyDef economy;
    std::vector<EconomyDiagnostic> diagnostics;
    Assert::IsTrue(ParseEconomy(text, economy, diagnostics), L"the committed economy does not parse");
    return economy;
  }

  Assert::Fail(L"the committed Economy.json was not found from the test working directory");
  return {};
}

/*
 * The same economy with the clock wound in.
 *
 * A cycle is 40 seconds and a Miner's ore hold is 400 Ferro-Chroma units, so
 * filling one honestly is 27,000 ticks -- which is a fine thing for a soak and a
 * poor thing for eighty tests. Only `cycleSeconds` moves: every litre, every
 * pool and every hold under test is the shipping number, because those are what
 * these tests are about. `TheCommittedCycleIsEightHundredTicks` is what keeps
 * the real one honest.
 */
[[nodiscard]] EconomyDef FastEconomy()
{
  EconomyDef economy = CommittedEconomy();
  economy.mining.cycleSeconds = 1;
  return economy;
}

/// A field built by hand, so a test can say "one cluster, thirty units of
/// Ferro-Chroma" and mean it. The bake's own fields are thousands of units
/// across a dozen clusters, which is the right subject for the ledger tests and
/// the wrong one for an arithmetic boundary.
[[nodiscard]] SiteField MakeField(SiteArchetype _archetype, std::uint8_t _grade,
                                  std::initializer_list<std::initializer_list<std::uint32_t>> _clusters)
{
  SiteField field;
  field.archetype = _archetype;
  field.grade = _grade;
  field.fieldRadiusCm = 500000; // 5 km, the authored minimum.
  for (const std::initializer_list<std::uint32_t>& cluster : _clusters)
  {
    SiteCluster& target = field.clusters[field.clusterCount];
    // Spread along a line inside the field, far enough apart that a fleet sent
    // to one is not standing in another.
    target.xCm = static_cast<std::int32_t>(field.clusterCount) * 40000;
    target.yCm = 0;
    std::uint8_t ore = 0;
    for (const std::uint32_t units : cluster)
    {
      target.remainingUnits[ore] = units;
      ++ore;
    }
    ++field.clusterCount;
  }
  return field;
}

/// The anchor id every hand-built world uses. Any non-invalid value does; a
/// named one keeps the orders readable.
constexpr AnchorId TEST_SITE = 7;

/*
 * A grid standing in a field, with `_miners` Miners and `_escorts` Interceptors
 * on it.
 *
 * The world is told its anchor, its economy and its field the way the registry
 * tells a real one -- so what is under test is the same path a shard walks, with
 * the universe left out because none of it is about where the grid sits.
 */
[[nodiscard]] std::vector<ShipId> Populate(World& _world, std::uint32_t _miners, std::uint32_t _escorts)
{
  std::vector<ShipId> ships;
  ShipId next = 1;
  for (std::uint32_t index = 0; index < _miners + _escorts; ++index)
  {
    ShipSpawn spawn;
    spawn.hullClass = index < _miners ? HullClass::Miner : HullClass::Interceptor;
    spawn.wing = 1;
    spawn.xMetres = 100.0f + static_cast<float>(index) * 60.0f;
    spawn.yMetres = 100.0f;
    const ShipId id = _world.Spawn(spawn, next);
    Assert::IsTrue(id != INVALID_SHIP_ID, L"the fixture could not spawn a ship");
    ships.push_back(id);
    ++next;
  }
  return ships;
}

[[nodiscard]] OrderSubmit MineOrder(std::span<const ShipId> _ships, OreFilter _filter, std::uint32_t _seq = 1)
{
  OrderSubmit order;
  order.orderSeq = _seq;
  order.kind = OrderKind::Mine;
  order.oreFilter = _filter;
  order.queueMode = QueueMode::Replace;
  for (const ShipId ship : _ships)
  {
    Assert::IsTrue(order.AddShip(ship), L"the fixture built an order past the ship cap");
  }
  return order;
}

/// Ticks until every group has left the table, or the cap runs out. The cap is
/// generous and its expiry is a failure: a mining order that never finishes is
/// exactly the defect these tests exist to catch.
std::uint32_t RunUntilIdle(World& _world, std::uint32_t _fromTick, std::uint32_t _capTicks)
{
  std::uint32_t tick = _fromTick;
  for (std::uint32_t step = 0; step < _capTicks; ++step)
  {
    ++tick;
    _world.Tick(tick);
    if (_world.Groups().empty() && _world.PendingOrderCount() == 0)
    {
      return tick;
    }
  }
  Assert::Fail(L"a mining order never finished");
  return tick;
}

/*
 * Ticks a shard until a mining order has actually produced something, or the
 * cap runs out.
 *
 * The cap is generous because the *journey* is: a baked field sits up to twelve
 * kilometres off its anchor and a Miner crosses that at 115 m/s, so a wing is
 * two thousand ticks from its first cycle however short the cycle is made. A
 * fixed tick count would pass or fail on which site the bake happened to place
 * first, which is the kind of test that is green until somebody reseeds.
 */
std::uint32_t RunUntilLedger(WorldRegistry& _registry, AnchorId _site, std::uint32_t _fromTick, std::uint32_t _capTicks)
{
  std::uint32_t tick = _fromTick;
  for (std::uint32_t step = 0; step < _capTicks; ++step)
  {
    _registry.Tick(++tick);
    if (_registry.Ledger(_site) != nullptr)
    {
      return tick;
    }
  }
  Assert::Fail(L"a mining wing never landed a single cycle on the ledger");
  return tick;
}

[[nodiscard]] std::uint32_t UsedOreLitres(const World& _world, std::uint32_t _slot, const EconomyDef& _economy)
{
  std::uint32_t used = 0;
  for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
  {
    used += _world.Cargo()[_slot].oreUnits[ore] * _economy.ores[ore].unitVolumeLitres;
  }
  return used;
}

/// A small universe that actually has fields in it, baked from the committed
/// distribution so the sites under test are the sites the shard would have.
[[nodiscard]] UniverseDef SiteUniverse(const EconomyDef& _economy)
{
  UniverseGenConfig config;
  config.regionCount = 2;
  config.constellationsPerRegion = 2;
  config.systemCount = 12;
  UniverseDef universe;
  Assert::IsTrue(GenerateUniverse(config, _economy.sites, universe), L"the site universe would not bake");
  return universe;
}

/// The first site the bake placed, in anchor order. Any one will do; the first
/// one is the one two runs of this suite agree about.
[[nodiscard]] AnchorId FirstSite(const UniverseDef& _universe)
{
  for (const SolarSystem& system : _universe.systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Site)
      {
        return anchor.id;
      }
    }
  }
  Assert::Fail(L"the baked universe has no mining sites");
  return INVALID_ID;
}

} // namespace

/*
 * The field's own arithmetic (ADR-024 §4b, §4c).
 *
 * Pure functions over a salt and a pool, which is what lets the client draw the
 * rocks the sim counts against without a byte of them crossing the wire. Every
 * property here is one both halves depend on.
 */
TEST_CLASS(SiteFieldTests)
{
public:
  TEST_METHOD(TheSplitIsExactToTheUnit)
  {
    const EconomyDef economy = CommittedEconomy();
    const std::uint32_t pools[ORE_COUNT] = {4000, 1234, 7};

    // Across many salts, because "exact" is a claim about the algorithm and a
    // single layout could be exact by luck.
    for (std::uint32_t salt = 1; salt <= 64; ++salt)
    {
      const SiteField field = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 3, 600000, salt, pools);
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        Assert::AreEqual<std::uint32_t>(pools[ore], field.Remaining(static_cast<OreId>(ore)),
                                        L"the split lost or invented units");
      }
    }
  }

  TEST_METHOD(TheClusterCountStaysInsideTheAuthoredRange)
  {
    const EconomyDef economy = CommittedEconomy();
    const std::uint32_t pools[ORE_COUNT] = {1000, 0, 0};
    for (std::uint32_t salt = 1; salt <= 64; ++salt)
    {
      const SiteField field = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 1, 600000, salt, pools);
      Assert::IsTrue(field.clusterCount >= economy.sites.clusterCountMin, L"too few clusters");
      Assert::IsTrue(field.clusterCount <= economy.sites.clusterCountMax, L"too many clusters");
      Assert::IsTrue(field.clusterCount <= MAX_SITE_CLUSTERS, L"more clusters than there is storage for");
    }
  }

  TEST_METHOD(EveryClusterSitsInsideTheField)
  {
    const EconomyDef economy = CommittedEconomy();
    const std::uint32_t pools[ORE_COUNT] = {1000, 500, 250};
    constexpr std::int64_t RADIUS_CM = 600000;
    for (std::uint32_t salt = 1; salt <= 64; ++salt)
    {
      const SiteField field =
        BuildSiteField(economy.sites, SiteArchetype::NebulaPocket, 2, static_cast<std::int32_t>(RADIUS_CM), salt, pools);
      for (std::uint8_t index = 0; index < field.clusterCount; ++index)
      {
        const auto x = static_cast<std::int64_t>(field.clusters[index].xCm);
        const auto y = static_cast<std::int64_t>(field.clusters[index].yCm);
        Assert::IsTrue(x * x + y * y <= RADIUS_CM * RADIUS_CM, L"a cluster is outside the rocks");
      }
    }
  }

  TEST_METHOD(TheSaltIsTheWholeLayout)
  {
    const EconomyDef economy = CommittedEconomy();
    const std::uint32_t pools[ORE_COUNT] = {2000, 1000, 500};
    const SiteField a = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 3, 600000, 0xABCDu, pools);
    const SiteField again = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 3, 600000, 0xABCDu, pools);
    const SiteField b = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 3, 600000, 0xABCEu, pools);

    Assert::AreEqual<unsigned>(a.clusterCount, again.clusterCount, L"one salt built two fields");
    for (std::uint8_t index = 0; index < a.clusterCount; ++index)
    {
      Assert::AreEqual<std::int32_t>(a.clusters[index].xCm, again.clusters[index].xCm, L"one salt moved a cluster");
      Assert::AreEqual<std::uint32_t>(a.clusters[index].remainingUnits[0], again.clusters[index].remainingUnits[0],
                                      L"one salt split a pool two ways");
    }

    // And the next epoch reshuffles it, which is the whole point of the salt
    // being folded with the epoch (ADR-024 §3d): yesterday's scouting is stale.
    bool moved = a.clusterCount != b.clusterCount;
    for (std::uint8_t index = 0; index < a.clusterCount && index < b.clusterCount && !moved; ++index)
    {
      moved = a.clusters[index].xCm != b.clusters[index].xCm || a.clusters[index].yCm != b.clusters[index].yCm;
    }
    Assert::IsTrue(moved, L"a new epoch left the field exactly where it was");
  }

  TEST_METHOD(AnOreTheSiteDoesNotHoldPlacesNothing)
  {
    const EconomyDef economy = CommittedEconomy();
    const std::uint32_t pools[ORE_COUNT] = {3000, 0, 0};
    const SiteField field = BuildSiteField(economy.sites, SiteArchetype::FerrousField, 2, 600000, 99u, pools);
    Assert::AreEqual<std::uint32_t>(0, field.Remaining(OreId::Astracite), L"an absent ore was placed anyway");
    Assert::AreEqual<std::uint32_t>(3000, field.TotalRemaining(), L"the field holds something nobody authored");
  }

  TEST_METHOD(TheRichestClusterHonoursTheFilterAndBreaksTiesLow)
  {
    // Cluster 0: 10 FC. Cluster 1: 10 FC (a tie). Cluster 2: 50 Nebulite.
    const SiteField field = MakeField(SiteArchetype::FerrousField, 2, {{10, 0, 0}, {10, 0, 0}, {0, 0, 50}});

    Assert::AreEqual<unsigned>(2, RichestCluster(field, OreFilter::Any), L"Any should take the biggest pile");
    Assert::AreEqual<unsigned>(0, RichestCluster(field, OreFilter::FerroChroma), L"a tie should keep the lower index");
    Assert::AreEqual<unsigned>(2, RichestCluster(field, OreFilter::Nebulite), L"the filter was ignored");
    Assert::AreEqual<unsigned>(MAX_SITE_CLUSTERS, RichestCluster(field, OreFilter::Astracite),
                              L"an ore the field does not hold should offer no cluster");
  }

  TEST_METHOD(ClusterOreTakesTheRichestTheFilterAccepts)
  {
    const SiteField field = MakeField(SiteArchetype::FerrousField, 2, {{5, 40, 9}});

    OreId ore = OreId::FerroChroma;
    Assert::IsTrue(ClusterOre(field, 0, OreFilter::Any, ore), L"a cluster with ore in it offered none");
    Assert::IsTrue(ore == OreId::Astracite, L"Any should take the richest ore present");

    Assert::IsTrue(ClusterOre(field, 0, OreFilter::Nebulite, ore), L"the filtered ore is present");
    Assert::IsTrue(ore == OreId::Nebulite, L"the filter was ignored");

    const SiteField empty = MakeField(SiteArchetype::FerrousField, 2, {{0, 0, 0}});
    Assert::IsFalse(ClusterOre(empty, 0, OreFilter::Any, ore), L"an empty cluster offered an ore");
  }

  TEST_METHOD(TheCommittedCycleIsEightHundredTicks)
  {
    // 40 seconds at 20 Hz, computed the integer way. `40 / 0.05f` is the
    // arithmetic this constant exists to avoid (ADR-016 §5's own lesson).
    const EconomyDef economy = CommittedEconomy();
    const SiteField field = MakeField(SiteArchetype::FerrousField, 2, {{100, 0, 0}});
    Assert::AreEqual<std::uint32_t>(800, MiningCycleTicks(economy, field, 0), L"the committed cycle is not 40 seconds");
  }

  TEST_METHOD(RadiationStretchesTheCycleByTheWholeStack)
  {
    const EconomyDef economy = CommittedEconomy();
    const SiteHazard& hazard = economy.Archetype(SiteArchetype::IrradiatedBelt).hazard;

    // Grade I scales the hazard by half, so ten stacks are 10 * 3 * 50 / 100 =
    // 15 per cent -- computed over the *total* stack rather than floored per
    // stack, which would have rounded each of the ten to one.
    const SiteField gentle = MakeField(SiteArchetype::IrradiatedBelt, 1, {{0, 100, 0}});
    const std::uint32_t base = economy.mining.cycleSeconds * 20;
    Assert::AreEqual<std::uint32_t>(base, MiningCycleTicks(economy, gentle, 0), L"a clean miner pays the base cycle");
    Assert::AreEqual<std::uint32_t>(base * 115 / 100, MiningCycleTicks(economy, gentle, 10),
                                    L"grade I radiation is not scaled the way the table says");

    const SiteField fierce = MakeField(SiteArchetype::IrradiatedBelt, 5, {{0, 100, 0}});
    Assert::IsTrue(MiningCycleTicks(economy, fierce, 10) > MiningCycleTicks(economy, gentle, 10),
                   L"a grade V belt should bite harder than a grade I");
    Assert::IsTrue(hazard.cycleSlowPerStackPct > 0, L"the fixture assumes the belt slows cycles");

    // And a field with no radiation never pays it, however many stacks a ship
    // is carrying in from somewhere else.
    const SiteField clean = MakeField(SiteArchetype::FerrousField, 5, {{100, 0, 0}});
    Assert::AreEqual<std::uint32_t>(base, MiningCycleTicks(economy, clean, 40), L"a ferrous field charged for radiation");
  }

  TEST_METHOD(NebulaHeatAndDampingAreGraded)
  {
    const EconomyDef economy = CommittedEconomy();
    const SiteHazard& hazard = economy.Archetype(SiteArchetype::NebulaPocket).hazard;

    const SiteField faded = MakeField(SiteArchetype::NebulaPocket, 1, {{0, 0, 100}});
    const SiteField deep = MakeField(SiteArchetype::NebulaPocket, 5, {{0, 0, 100}});
    Assert::AreEqual<unsigned>(hazard.heatPerCycle * 50 / 100, CycleHeat(economy, faded), L"grade I heat is unscaled");
    Assert::AreEqual<unsigned>(hazard.heatPerCycle * 300 / 100, CycleHeat(economy, deep), L"grade V heat is unscaled");

    // Damping is *already* per grade in the content, so it is not scaled a
    // second time -- doing so would dampen a grade V pocket to nothing.
    Assert::AreEqual<unsigned>(hazard.sensorDampingPctByGrade[0], SensorDampingPct(economy, faded),
                              L"the faded pocket does not damp by its own row");
    Assert::AreEqual<unsigned>(hazard.sensorDampingPctByGrade[4], SensorDampingPct(economy, deep),
                              L"the deep pocket does not damp by its own row");

    const SiteField ferrous = MakeField(SiteArchetype::FerrousField, 3, {{100, 0, 0}});
    Assert::AreEqual<unsigned>(100, SensorDampingPct(economy, ferrous), L"a field with no nebula dampened anyway");
    Assert::AreEqual<unsigned>(0, CycleHeat(economy, ferrous), L"a field with no nebula heated anyway");
  }

  TEST_METHOD(TheLedgerRoundTripsWhatIsLeft)
  {
    SiteField field = MakeField(SiteArchetype::FerrousField, 2, {{10, 20, 30}, {40, 0, 0}});
    SiteLedger ledger;
    ledger.anchor = TEST_SITE;
    StoreSiteField(field, ledger);
    Assert::AreEqual<std::uint32_t>(field.TotalRemaining(), ledger.TotalRemaining(), L"the ledger lost units");

    // Rebuilt fresh, then overlaid: the shape comes from the bake and the
    // numbers from the ledger, which is exactly what a spin-up does.
    SiteField rebuilt = MakeField(SiteArchetype::FerrousField, 2, {{999, 999, 999}, {999, 999, 999}});
    ApplySiteLedger(ledger, rebuilt);
    for (std::uint8_t index = 0; index < field.clusterCount; ++index)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        Assert::AreEqual<std::uint32_t>(field.clusters[index].remainingUnits[ore],
                                        rebuilt.clusters[index].remainingUnits[ore], L"the overlay lost a number");
      }
    }
  }
};

/*
 * The order's refusals, and the sequence they come in (ADR-024 §4a, ADR-018 D9).
 *
 * The reason is what the player reads, so an order that breaks two rules has to
 * say the same one on both machines -- which is what the check-order contract in
 * `GAME_SCHEMA_TEXT` is for and what the pairs below hold.
 */
TEST_CLASS(MineValidationTests)
{
public:
  TEST_METHOD(AMineAwayFromAFieldIsNotAtSite)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    const std::vector<ShipId> ships = Populate(world, 2, 0);

    // No `SetSite`: a station's grid, a gate's grid, open space.
    const OrderVerdict verdict = world.SubmitOrder(MineOrder(ships, OreFilter::Any));
    Assert::IsFalse(verdict.accepted, L"a Mine was accepted where there is no field");
    Assert::IsTrue(verdict.reason == OrderReason::NotAtSite, L"the wrong reason for mining nowhere");
  }

  TEST_METHOD(AMineWithNoMinerIsRefused)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{500, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 0, 3);

    const OrderVerdict verdict = world.SubmitOrder(MineOrder(ships, OreFilter::Any));
    Assert::IsFalse(verdict.accepted, L"a wing of escorts was sent mining");
    Assert::IsTrue(verdict.reason == OrderReason::NoMinerInOrder, L"the wrong reason for a Minerless order");
  }

  TEST_METHOD(AMixedOrderIsLegal)
  {
    // The null-sec fantasy, and it falls out of the formation solve for free
    // (ADR-024 §4a, ruling R4): escorts are members in every sense but cycling.
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{500, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 4);

    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"a mixed order was refused");
  }

  TEST_METHOD(AMineIsReplaceOnly)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{500, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 2, 0);

    OrderSubmit order = MineOrder(ships, OreFilter::Any);
    order.queueMode = QueueMode::Append;
    const OrderVerdict verdict = world.SubmitOrder(order);
    Assert::IsFalse(verdict.accepted, L"a Mine was queued behind a movement plan");
    Assert::IsTrue(verdict.reason == OrderReason::InvalidQueueMode, L"the wrong reason for a queued verb");
  }

  TEST_METHOD(TheSiteCheckRunsBeforeTheSelectionChecks)
  {
    // Both wrong at once: no field here *and* nothing that can mine. The
    // contract says the place is answered first, so the player is told where to
    // go rather than what to bring.
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    const std::vector<ShipId> ships = Populate(world, 0, 2);

    const OrderVerdict verdict = world.SubmitOrder(MineOrder(ships, OreFilter::Any));
    Assert::IsTrue(verdict.reason == OrderReason::NotAtSite, L"the check order put the selection before the place");
  }

  TEST_METHOD(NoMinerIsAnsweredBeforeAFullHold)
  {
    // A view built by hand, because the pair being tested is a *sequence* and
    // the world cannot easily be put in a state that breaks both at once.
    const EconomyDef economy = CommittedEconomy();
    const ShipId ids[] = {1, 2};
    const ShipMark marks[] = {{0, 0, HullClass::Interceptor}, {0, 0, HullClass::Interceptor}};
    const std::uint32_t room[] = {0, 0};

    ValidationView view;
    view.shipIds = ids;
    view.shipMarks = marks;
    view.siteAnchor = TEST_SITE;
    view.oreHoldFreeLitres = room;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      view.oreUnitLitres[ore] = economy.ores[ore].unitVolumeLitres;
    }

    OrderSubmit order = MineOrder(std::span<const ShipId>{ids}, OreFilter::Any);
    Assert::IsTrue(ValidateOrder(view, order).reason == OrderReason::NoMinerInOrder,
                   L"a full hold answered before an absent Miner");
  }

  TEST_METHOD(EveryHoldFullIsRefused)
  {
    const EconomyDef economy = CommittedEconomy();
    const ShipId ids[] = {1, 2};
    const ShipMark marks[] = {{0, 0, HullClass::Miner}, {0, 0, HullClass::Miner}};
    const std::uint32_t none[] = {0, 0};
    const std::uint32_t someRoom[] = {0, 1000};

    ValidationView view;
    view.shipIds = ids;
    view.shipMarks = marks;
    view.siteAnchor = TEST_SITE;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      view.oreUnitLitres[ore] = economy.ores[ore].unitVolumeLitres;
    }

    const OrderSubmit order = MineOrder(std::span<const ShipId>{ids}, OreFilter::Any);

    view.oreHoldFreeLitres = none;
    Assert::IsTrue(ValidateOrder(view, order).reason == OrderReason::HoldFull, L"a wing with no room was sent to work");

    // One Miner with room is enough: the order is for the wing, and the ships
    // that can still take ore are the ones that will.
    view.oreHoldFreeLitres = someRoom;
    Assert::IsTrue(ValidateOrder(view, order).accepted, L"one Miner with room should carry the order");
  }

  TEST_METHOD(AViewWithoutCargoSkipsTheHoldCheck)
  {
    /*
     * The client's half until E3 replicates cargo. Bouncing every Mine for want
     * of a number the client is not sent would make the feature unusable from
     * the surface that offers it; the authority still applies the check, and
     * that asymmetry is designed rather than a hole (ADR-005 §4).
     */
    const EconomyDef economy = CommittedEconomy();
    const ShipId ids[] = {1};
    const ShipMark marks[] = {{0, 0, HullClass::Miner}};

    ValidationView view;
    view.shipIds = ids;
    view.shipMarks = marks;
    view.siteAnchor = TEST_SITE;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      view.oreUnitLitres[ore] = economy.ores[ore].unitVolumeLitres;
    }
    // No `oreHoldFreeLitres`.
    Assert::IsTrue(ValidateOrder(view, MineOrder(std::span<const ShipId>{ids}, OreFilter::Any)).accepted,
                   L"a view that cannot answer the hold question refused instead of deferring");
  }

  TEST_METHOD(TheOreFilterCrossesTheWire)
  {
    const ShipId ids[] = {11, 12, 13};
    OrderSubmit sent = MineOrder(std::span<const ShipId>{ids}, OreFilter::Nebulite, 42);
    sent.formation = FormationId::Claw;

    std::uint8_t bytes[MAX_ORDER_SUBMIT_BYTES] = {};
    Neuron::ByteWriter writer{std::span<std::uint8_t>{bytes}};
    Assert::IsTrue(WriteOrderSubmit(sent, writer), L"the order would not fit its own cap");

    Neuron::ByteReader reader{std::span<const std::uint8_t>{bytes, writer.BytesWritten()}};
    OrderSubmit received;
    Assert::IsTrue(ReadOrderSubmit(reader, received), L"the order would not read back");
    Assert::IsTrue(received.oreFilter == OreFilter::Nebulite, L"the ore filter did not survive the wire");
    Assert::IsTrue(received.kind == OrderKind::Mine, L"the kind did not survive the wire");
    Assert::IsTrue(received.formation == FormationId::Claw, L"a Mine still carries a formation for its escorts");
    Assert::AreEqual<std::uint16_t>(3, received.shipCount, L"the selection did not survive the wire");
  }

  TEST_METHOD(AnOreFilterOutsideTheEnumIsMalformed)
  {
    /*
     * The one field the decoder refuses on. `kind` and `formation` are cast
     * through because they have reason codes a player reads; a filter has none,
     * because every value it defines is legal everywhere -- so a byte outside it
     * is a schema disagreement rather than a refusal owed to anyone.
     */
    const ShipId ids[] = {5};
    const OrderSubmit sent = MineOrder(std::span<const ShipId>{ids}, OreFilter::Any);

    std::uint8_t bytes[MAX_ORDER_SUBMIT_BYTES] = {};
    Neuron::ByteWriter writer{std::span<std::uint8_t>{bytes}};
    Assert::IsTrue(WriteOrderSubmit(sent, writer), L"the order would not fit");
    const std::size_t written = writer.BytesWritten();
    bytes[written - 1] = 9; // A fourth ore, from a build that does not exist.

    Neuron::ByteReader reader{std::span<const std::uint8_t>{bytes, written}};
    OrderSubmit received;
    Assert::IsFalse(ReadOrderSubmit(reader, received), L"an unknown ore filter was accepted");
  }

  TEST_METHOD(TheCommandSurfaceOffersMine)
  {
    Assert::IsTrue(OrderKindHasContent(OrderKind::Mine), L"Mine is simulated and should not be greyed");
    Assert::IsTrue(std::string_view{OrderKindParameterName(OrderKind::Mine)} == "Ore Filter",
                   L"a Mine varies by ore, and the surface has to be able to say so");
    Assert::IsTrue(std::find(std::begin(ORDER_KIND_IDS), std::end(ORDER_KIND_IDS), OrderKind::Mine) !=
                     std::end(ORDER_KIND_IDS),
                   L"the command row would not draw Mine");
    for (const OreFilter filter : ORE_FILTER_IDS)
    {
      Assert::IsNotNull(OreFilterName(filter), L"a filter the player cannot name is a control they cannot use");
    }
  }
};

/*
 * The cycle in the tick (ADR-024 §4b).
 *
 * Hardness rather than luck, exact to the litre, and three exits that are all
 * per-ship and all visible.
 */
TEST_CLASS(MiningCycleTests)
{
public:
  TEST_METHOD(ACompletedCycleCreditsTheHoldAndDebitsTheCluster)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{500, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::FerroChroma)).accepted, L"the order was refused");

    const std::uint32_t perCycle = economy.mining.unitsPerCycle[0];
    for (std::uint32_t tick = 1; tick <= 400; ++tick)
    {
      world.Tick(tick);
    }

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    const std::uint32_t carried = world.Cargo()[slot].Units(OreId::FerroChroma);
    Assert::IsTrue(carried > 0, L"nothing was mined in twenty seconds of one-second cycles");
    Assert::AreEqual<std::uint32_t>(0, carried % perCycle, L"a cycle yielded something other than its whole yield");
    Assert::AreEqual<std::uint32_t>(500 - carried, world.Site().Remaining(OreId::FerroChroma),
                                    L"the cluster and the hold disagree about what was taken");

    // And the cycle clock is running, so the next credit is already scheduled
    // rather than the ship having quietly stopped.
    Assert::IsTrue(world.MiningStates()[slot].cycleEndTick > 0, L"the Miner stopped after one cycle");
  }

  TEST_METHOD(TheYieldIsCappedByWhatIsLeft)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});

    // Twenty units against a twelve-unit cycle: one full cycle, then eight.
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{20, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::FerroChroma)).accepted, L"the order was refused");

    RunUntilIdle(world, 0, 4000);
    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    Assert::AreEqual<std::uint32_t>(20, world.Cargo()[slot].Units(OreId::FerroChroma), L"the last partial cycle was lost");
    Assert::AreEqual<std::uint32_t>(0, world.Site().TotalRemaining(), L"the cluster was not worked out");
  }

  TEST_METHOD(AnExhaustedClusterEndsTheOrderAndLeavesTheRestOfTheField)
  {
    /*
     * The second exit (ADR-024 §4b): the *cluster* runs out and the order
     * completes, with the rest of the field untouched. The wing does not hop to
     * the next pile on its own -- that would be movement the player did not
     * order, and the client feeds the next Mine.
     */
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});

    // The filter picks the richest cluster, so the Nebulite pile is the one
    // worked and the Ferro-Chroma pile is the one left standing.
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{5000, 0, 0}, {0, 0, 24}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Nebulite)).accepted, L"the order was refused");

    RunUntilIdle(world, 0, 4000);
    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    Assert::AreEqual<std::uint32_t>(24, world.Cargo()[slot].Units(OreId::Nebulite), L"the cluster was not emptied");
    Assert::AreEqual<std::uint32_t>(0, world.Site().Remaining(OreId::Nebulite), L"the worked cluster still holds ore");
    Assert::AreEqual<std::uint32_t>(5000, world.Site().Remaining(OreId::FerroChroma),
                                    L"the wing wandered to a cluster nobody sent it to");
  }

  TEST_METHOD(AnEmptyFieldCompletesTheOrderAtOnce)
  {
    /*
     * An empty field is not an error, it is news (ADR-024 §4a). The order is
     * accepted and finishes, so the client raises the toast a completed order
     * already raises rather than telling the player they did something wrong.
     */
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{0, 0, 500}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);

    // Ferro-Chroma is what the filter asks for; this field holds only Nebulite.
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::FerroChroma)).accepted,
                   L"an order at a field with the wrong ore should be accepted, not refused");
    world.Tick(1);
    Assert::AreEqual<std::size_t>(1, world.Groups().size(), L"the order should be in the table to be reported Done");
    Assert::IsTrue(world.Groups()[0].state == OrderState::Done, L"the order did not complete on the spot");
  }

  TEST_METHOD(AFullHoldStopsTheShipExactlyAtTheLitre)
  {
    /*
     * The boundary the accept list names: a hold whose remaining litres are not
     * a multiple of any ore's unit volume.
     *
     * A mixed load is what produces it. Nebulite is 250 litres and Ferro-Chroma
     * is 300 against a 120,000-litre ore hold, and an `Any` order takes
     * whichever ore the cluster is richest in each cycle -- so the two
     * interleave and the hold ends a stranded remainder short of full. It *is*
     * full, because the smallest thing there is to put in it no longer fits,
     * and the field still holds ore it cannot take.
     */
    const EconomyDef economy = FastEconomy();
    const std::uint32_t hold = economy.Cargo(HullClass::Miner).oreHoldLitres;
    const std::uint32_t nebulite = economy.ores[static_cast<std::uint8_t>(OreId::Nebulite)].unitVolumeLitres;
    const std::uint32_t ferro = economy.ores[static_cast<std::uint8_t>(OreId::FerroChroma)].unitVolumeLitres;
    Assert::AreEqual<std::uint32_t>(120000, hold, L"this test is written against the committed hold");

    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{100, 0, 477}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"the order was refused");

    RunUntilIdle(world, 0, 40000);
    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");

    const std::uint32_t carriedNebulite = world.Cargo()[slot].Units(OreId::Nebulite);
    const std::uint32_t carriedFerro = world.Cargo()[slot].Units(OreId::FerroChroma);
    Assert::IsTrue(carriedNebulite > 0 && carriedFerro > 0, L"the boundary needs a mixed load to be interesting");

    const std::uint32_t used = carriedNebulite * nebulite + carriedFerro * ferro;
    Assert::AreEqual<std::uint32_t>(used, UsedOreLitres(world, slot, economy), L"the manifest does not add up");
    Assert::AreEqual<std::uint32_t>(hold - used, world.OreHoldFreeLitres(slot), L"the free-space answer drifted");
    Assert::IsTrue(used <= hold, L"the hold took on more than it holds");

    // Smaller than the smallest thing there is to put in it -- full to the
    // litre -- and larger than nothing, which is what makes this a boundary
    // rather than an exact division.
    std::uint32_t smallest = 0xffffffffu;
    for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
    {
      smallest = std::min(smallest, economy.ores[ore].unitVolumeLitres);
    }
    Assert::IsTrue(world.OreHoldFreeLitres(slot) < smallest, L"the hold stopped with room for another unit in it");
    Assert::IsTrue(world.OreHoldFreeLitres(slot) > 0, L"this boundary is only interesting when it does not divide");
    Assert::IsTrue(world.Site().TotalRemaining() > 0, L"the field should have ore the full hold cannot take");
  }

  TEST_METHOD(EscortsDoNotMine)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{500, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 3);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"the mixed order was refused");

    for (std::uint32_t tick = 1; tick <= 400; ++tick)
    {
      world.Tick(tick);
    }

    for (std::size_t index = 1; index < ships.size(); ++index)
    {
      std::uint32_t slot = 0;
      Assert::IsTrue(world.FindSlot(ships[index], slot), L"an escort vanished");
      Assert::AreEqual<std::uint32_t>(0, world.Cargo()[slot].Units(OreId::FerroChroma), L"an Interceptor mined");
      Assert::AreEqual<std::uint32_t>(0, world.MiningStates()[slot].cycleEndTick, L"an escort ran a cycle");
    }

    std::uint32_t minerSlot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], minerSlot), L"the Miner vanished");
    Assert::IsTrue(world.Cargo()[minerSlot].Units(OreId::FerroChroma) > 0, L"the Miner did not work");
  }

  TEST_METHOD(TheGeneratorIsUntouched)
  {
    /*
     * A draw here would desync a replay, so the yield is `min` of three
     * integers and nothing else (ADR-024 §4b). The generator's state is the
     * evidence: if anything in mining ever reaches for randomness, this is what
     * says so on the tick it happens.
     */
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(0xC0FFEEu);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::IrradiatedBelt, 3, {{0, 400, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 3, 2);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"the order was refused");

    const std::uint64_t state = world.Random().State();
    const std::uint64_t increment = world.Random().Increment();
    RunUntilIdle(world, 0, 40000);
    Assert::IsTrue(world.Random().State() == state, L"mining drew from the world's generator");
    Assert::IsTrue(world.Random().Increment() == increment, L"mining reseeded the world's generator");

    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    Assert::IsTrue(world.Cargo()[slot].Units(OreId::Astracite) > 0, L"the scenario mined nothing to be sure about");
  }

  TEST_METHOD(TwoRunsOfOneScenarioAreBitIdentical)
  {
    /*
     * Cycles, a hold filling, a cluster running dry and several Miners racing
     * each other for the same rocks -- all in one script, run twice, hashed.
     * The world hash covers the manifests, the cycle clocks, the hazard
     * counters and the field, so a divergence anywhere in this slice lands here.
     */
    const EconomyDef economy = FastEconomy();
    const auto run = [&economy](std::uint64_t& _outHash, std::uint32_t& _outCarried)
    {
      World world;
      world.Reset(0x5EEDu);
      world.SetEconomy(&economy);
      world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
      world.SetSite(MakeField(SiteArchetype::IrradiatedBelt, 4, {{60, 900, 0}, {0, 40, 0}}), 3);
      const std::vector<ShipId> ships = Populate(world, 4, 2);
      Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"the order was refused");

      const std::uint32_t ended = RunUntilIdle(world, 0, 40000);
      Assert::IsTrue(ended > 0, L"the script did nothing");

      std::uint32_t slot = 0;
      Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
      _outCarried = world.Cargo()[slot].Units(OreId::Astracite);
      _outHash = ComputeWorldHash(world);
    };

    std::uint64_t first = 0;
    std::uint64_t second = 0;
    std::uint32_t carriedFirst = 0;
    std::uint32_t carriedSecond = 0;
    run(first, carriedFirst);
    run(second, carriedSecond);

    Assert::IsTrue(carriedFirst > 0, L"the script mined nothing, so it proves nothing");
    Assert::AreEqual<std::uint32_t>(carriedFirst, carriedSecond, L"two runs mined different amounts");
    Assert::IsTrue(first == second, L"two runs of one script produced different worlds");
  }

  TEST_METHOD(RadiationAccruesInTheFieldAndShedsOutside)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::IrradiatedBelt, 3, {{0, 5000, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Astracite)).accepted, L"the order was refused");

    std::uint32_t tick = 0;
    for (; tick < 600; ++tick)
    {
      world.Tick(tick + 1);
    }
    std::uint32_t slot = 0;
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    const std::uint8_t stacked = world.MiningStates()[slot].radiationStacks;
    Assert::IsTrue(stacked > 0, L"working an irradiated belt cost nothing");

    /*
     * Now send it out of the rocks. A stack sheds per `stackDecaySeconds`
     * outside the field and never inside it, which is what makes leaving the
     * escape rather than a wait.
     */
    OrderSubmit away;
    away.orderSeq = 2;
    away.kind = OrderKind::Move;
    away.target.xCm = 1500 * 100; // Well outside the 5 km field.
    Assert::IsTrue(away.AddShip(ships[0]), L"the fixture built a bad order");
    away.target.xCm = 15000 * 100;
    Assert::IsTrue(world.SubmitOrder(away).accepted, L"the move away was refused");

    const std::uint32_t decayTicks = economy.Archetype(SiteArchetype::IrradiatedBelt).hazard.stackDecaySeconds * 20;
    for (std::uint32_t step = 0; step < decayTicks * 2 + 4000; ++step)
    {
      world.Tick(++tick);
    }
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    Assert::IsTrue(world.MiningStates()[slot].radiationStacks < stacked, L"leaving the field shed nothing");
  }

  TEST_METHOD(NebulaHeatLocksTheLaserOut)
  {
    /*
     * 34 heat a cycle against a threshold of 100 at grade II: the third cycle
     * locks the laser out, and 60 seconds at 2 a second more than clears it.
     * Heat sheds only while the laser is *off*, which is what makes the
     * authored numbers mean what §3b says they mean.
     */
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::NebulaPocket, 2, {{0, 0, 5000}}), 0);
    const std::vector<ShipId> ships = Populate(world, 1, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Nebulite)).accepted, L"the order was refused");

    std::uint32_t slot = 0;
    bool lockedOut = false;
    for (std::uint32_t tick = 1; tick <= 600 && !lockedOut; ++tick)
    {
      world.Tick(tick);
      Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
      lockedOut = world.MiningStates()[slot].lockoutUntilTick > tick;
    }
    Assert::IsTrue(lockedOut, L"a nebula pocket never locked the laser out");

    // And the lockout ends, rather than grounding the ship for good.
    for (std::uint32_t tick = 601; tick <= 4000; ++tick)
    {
      world.Tick(tick);
    }
    Assert::IsTrue(world.FindSlot(ships[0], slot), L"the Miner vanished");
    Assert::IsTrue(world.Cargo()[slot].Units(OreId::Nebulite) > 0, L"nothing was mined through the lockout");
    Assert::AreEqual<unsigned>(economy.Archetype(SiteArchetype::NebulaPocket).hazard.sensorDampingPctByGrade[1],
                              world.SensorDampingPct(), L"the grid does not report the pocket's damping");
  }

  TEST_METHOD(AWorkingOrderReportsAClusterEta)
  {
    const EconomyDef economy = FastEconomy();
    World world;
    world.Reset(1);
    world.SetEconomy(&economy);
    world.SetAnchor(TEST_SITE, INVALID_SHIP_ID, {});
    world.SetSite(MakeField(SiteArchetype::FerrousField, 2, {{5000, 0, 0}}), 0);
    const std::vector<ShipId> ships = Populate(world, 2, 0);
    Assert::IsTrue(world.SubmitOrder(MineOrder(ships, OreFilter::Any)).accepted, L"the order was refused");

    float working = -1.0f;
    for (std::uint32_t tick = 1; tick <= 600; ++tick)
    {
      world.Tick(tick);
      if (!world.Groups().empty() && world.Groups()[0].legIndex >= world.Groups()[0].legCount)
      {
        working = world.LegEtaSeconds(world.Groups()[0]);
        break;
      }
    }
    Assert::IsTrue(working >= 0.0f, L"a working mining order reported no ETA at all");
    Assert::IsTrue(working > 0.0f, L"a field with five thousand units reported nothing left to do");
  }
};

/*
 * The ledger at the universe layer (ADR-024 §3d, ADR-018 D2/D8).
 *
 * "Worlds forget, ledgers do not" as a test rather than a sentence.
 */
TEST_CLASS(SiteLedgerTests)
{
public:
  TEST_METHOD(APristineSiteHasNoLedger)
  {
    const EconomyDef economy = CommittedEconomy();
    const UniverseDef universe = SiteUniverse(economy);
    const AnchorId site = FirstSite(universe);

    WorldRegistry registry;
    RegistryConfig config;
    config.sessionSeed = 0x51DEu;
    registry.Reset(&universe, &economy, config);

    Assert::IsNotNull(registry.Borrow(site), L"a site anchor would not spin up");
    Assert::IsNull(registry.Ledger(site), L"an untouched field should carry no durable state at all");

    // And the world it spun up is holding the bake's own pools.
    const World* world = registry.Peek(site);
    Assert::IsNotNull(world, L"the site's grid is not live");
    Assert::IsTrue(world->Site().Exists(), L"the grid was spun up without its field");
    Assert::IsTrue(world->Site().TotalRemaining() > 0, L"a pristine field is empty");
    Assert::AreEqual<std::uint32_t>(world->Site().TotalRemaining(), registry.FieldAt(site).TotalRemaining(),
                                    L"the universe layer and the grid disagree about the field");
  }

  TEST_METHOD(TheLedgerSurvivesTeardownAndRecreate)
  {
    const EconomyDef economy = FastEconomy();
    const UniverseDef universe = SiteUniverse(economy);
    const AnchorId site = FirstSite(universe);

    WorldRegistry registry;
    RegistryConfig config;
    config.sessionSeed = 0x51DEu;
    registry.Reset(&universe, &economy, config);

    World* world = registry.Borrow(site);
    Assert::IsNotNull(world, L"a site anchor would not spin up");
    const std::uint32_t pristine = world->Site().TotalRemaining();

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    const ShipId miner = registry.Spawn(site, spawn);
    Assert::IsTrue(miner != INVALID_SHIP_ID, L"the Miner would not spawn");

    const ShipId ids[] = {miner};
    Assert::IsTrue(registry.Borrow(site)->SubmitOrder(MineOrder(std::span<const ShipId>{ids}, OreFilter::Any)).accepted,
                   L"the mining order was refused");

    std::uint32_t tick = RunUntilLedger(registry, site, 0, 20000);

    const SiteLedger* ledger = registry.Ledger(site);
    Assert::IsNotNull(ledger, L"a worked field left no ledger behind");
    const std::uint32_t left = ledger->TotalRemaining();
    Assert::IsTrue(left < pristine, L"the ledger did not record what was taken");

    /*
     * Now the grid empties and goes. Nothing is saved on the way out, which is
     * the rule -- so what comes back has to be rebuilt from the bake and this
     * ledger, and coming back full would be the failure.
     */
    Assert::IsTrue(registry.Despawn(site, miner), L"the Miner would not leave");
    registry.Tick(++tick);
    Assert::IsNull(registry.Peek(site), L"the empty grid should have been torn down");

    const SiteLedger* afterTeardown = registry.Ledger(site);
    Assert::IsNotNull(afterTeardown, L"the ledger went with the world");
    Assert::AreEqual<std::uint32_t>(left, afterTeardown->TotalRemaining(), L"the ledger changed on teardown");

    const World* recreated = registry.Borrow(site);
    Assert::IsNotNull(recreated, L"the site would not come back");
    Assert::AreEqual<std::uint32_t>(left, recreated->Site().TotalRemaining(),
                                    L"the field came back pristine after being eaten");
  }

  TEST_METHOD(TheLedgerIsInTheRegistryHash)
  {
    const EconomyDef economy = FastEconomy();
    const UniverseDef universe = SiteUniverse(economy);
    const AnchorId site = FirstSite(universe);

    // Two registries: one mines and one does not, and both end with the site's
    // grid torn down -- so the only thing left that could differ is the ledger.
    const auto run = [&universe, &economy, site](bool _mine)
    {
      WorldRegistry registry;
      RegistryConfig config;
      config.sessionSeed = 0x51DEu;
      registry.Reset(&universe, &economy, config);
      Assert::IsNotNull(registry.Borrow(site), L"a site anchor would not spin up");

      std::uint32_t tick = 0;
      if (_mine)
      {
        ShipSpawn spawn;
        spawn.hullClass = HullClass::Miner;
        spawn.wing = 1;
        const ShipId miner = registry.Spawn(site, spawn);
        const ShipId ids[] = {miner};
        Assert::IsTrue(
          registry.Borrow(site)->SubmitOrder(MineOrder(std::span<const ShipId>{ids}, OreFilter::Any)).accepted,
          L"the mining order was refused");
        tick = RunUntilLedger(registry, site, 0, 20000);
        Assert::IsTrue(registry.Despawn(site, miner), L"the Miner would not leave");
      }
      for (std::uint32_t step = 0; step < 4; ++step)
      {
        registry.Tick(++tick);
      }
      Assert::IsNull(registry.Peek(site), L"both runs should end with the grid gone");
      return registry.Hash();
    };

    const std::uint64_t mined = run(true);
    const std::uint64_t untouched = run(false);
    const std::uint64_t again = run(true);
    Assert::IsTrue(mined != untouched, L"a worked field left no trace in the replay domain");
    Assert::IsTrue(mined == again, L"two identical scripts produced different shards");
  }

  TEST_METHOD(AWatchedSiteDoesNotReForm)
  {
    /*
     * The "never torn down under the player's camera" rule, applied to geology
     * (ADR-024 §3d). A grid with presence on it crosses an epoch boundary with
     * its rocks and its emptiness exactly where they were; the re-form waits
     * for the moment there is nobody to do it to.
     */
    EconomyDef economy = FastEconomy();
    economy.sites.regenSeconds = 100; // 2,000 ticks, so a test can cross one.
    const UniverseDef universe = SiteUniverse(economy);
    const AnchorId site = FirstSite(universe);

    WorldRegistry registry;
    RegistryConfig config;
    config.sessionSeed = 0x51DEu;
    registry.Reset(&universe, &economy, config);
    registry.AddViewer(site);

    World* world = registry.Borrow(site);
    Assert::IsNotNull(world, L"a site anchor would not spin up");
    const std::uint32_t pristine = world->Site().TotalRemaining();

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    const ShipId miner = registry.Spawn(site, spawn);
    const ShipId ids[] = {miner};
    Assert::IsTrue(registry.Borrow(site)->SubmitOrder(MineOrder(std::span<const ShipId>{ids}, OreFilter::Any)).accepted,
                   L"the mining order was refused");

    std::uint32_t tick = RunUntilLedger(registry, site, 0, 20000);
    const std::uint32_t eaten = registry.Peek(site)->Site().TotalRemaining();
    Assert::IsTrue(eaten < pristine, L"the fixture did not actually mine anything");

    // Straight across the epoch boundary with the wing still standing in it.
    registry.Despawn(site, miner);
    tick += 2 * economy.sites.regenSeconds * 20;
    registry.Tick(tick);
    Assert::IsNotNull(registry.Peek(site), L"a viewer should hold the grid alive");
    Assert::AreEqual<std::uint32_t>(eaten, registry.Peek(site)->Site().TotalRemaining(),
                                    L"the field re-formed under a watcher");

    // Presence leaves, the grid goes, and the next spin-up is the re-form.
    registry.RemoveViewer(site);
    registry.Tick(++tick);
    Assert::IsNull(registry.Peek(site), L"the grid should go once nobody is holding it");

    const World* reformed = registry.Borrow(site);
    Assert::IsNotNull(reformed, L"the site would not come back");
    Assert::IsTrue(reformed->Site().TotalRemaining() > eaten, L"the epoch did not refill the pool");
    Assert::IsNull(registry.Ledger(site), L"a refilled field should be back to carrying no durable state");
  }

  TEST_METHOD(TheMiningRecordsRideTheBus)
  {
    /*
     * The replay contract extended by one record family rather than forked: a
     * yield is filed during the tick and applied between ticks, in the same
     * `(applyTick, transferId)` order every dock obeys (ADR-018 D17).
     */
    const EconomyDef economy = FastEconomy();
    const UniverseDef universe = SiteUniverse(economy);
    const AnchorId site = FirstSite(universe);

    WorldRegistry registry;
    RegistryConfig config;
    config.sessionSeed = 0x51DEu;
    registry.Reset(&universe, &economy, config);
    Assert::IsNotNull(registry.Borrow(site), L"a site anchor would not spin up");

    ShipSpawn spawn;
    spawn.hullClass = HullClass::Miner;
    spawn.wing = 1;
    const ShipId miner = registry.Spawn(site, spawn);
    const ShipId ids[] = {miner};
    Assert::IsTrue(registry.Borrow(site)->SubmitOrder(MineOrder(std::span<const ShipId>{ids}, OreFilter::Any)).accepted,
                   L"the mining order was refused");

    bool sawFiledRecord = false;
    std::uint32_t tick = 0;
    for (std::uint32_t step = 0; step < 20000 && !sawFiledRecord; ++step)
    {
      registry.Tick(++tick);
      sawFiledRecord = registry.PendingTransferCount() > 0;
    }
    Assert::IsTrue(sawFiledRecord, L"no mining record ever reached the bus");

    tick = RunUntilLedger(registry, site, tick, 20000);
    const SiteLedger* ledger = registry.Ledger(site);
    Assert::IsNotNull(ledger, L"the records never landed on a ledger");

    // What the grid says it has left and what the universe layer says agree,
    // once the bus has drained. The world is a tick ahead while a record is in
    // flight, which is the same one-tick window a dock has.
    registry.Tick(++tick);
    registry.Tick(++tick);
    Assert::AreEqual<std::uint32_t>(registry.Peek(site)->Site().TotalRemaining(), ledger->TotalRemaining(),
                                    L"the grid and the ledger disagree about what is left");
  }
};

} // namespace GameLogicTests
