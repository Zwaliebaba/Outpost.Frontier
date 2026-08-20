#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "Universe.h"
#include "UniverseGen.h"
#include "UniverseRoute.h"
#include "SiteEpoch.h"
#include "UniverseParse.h"
#include "Validate.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * The bake's invariants (Build Order U1).
 *
 * These are the acceptance criteria, written as a suite rather than as a
 * checklist someone runs: the bake produces content that ships, so "the gate
 * graph is connected" has to be a thing the build knows rather than a thing the
 * generator's author believed.
 *
 * Almost every test here runs a **small** universe -- a few hundred systems --
 * because the properties are structural and hold at any size, and because a
 * suite that took the full 2,500-system bake on every run would be a suite
 * people learn to skip. The full-scale bake is exercised once, in
 * `TheCommittedScaleBakesAndSurvivesItsOwnRoundTrip`.
 */

namespace GameLogicTests
{

namespace
{

/// A universe small enough to be quick and large enough to have structure.
[[nodiscard]] UniverseGenConfig SmallConfig()
{
  UniverseGenConfig config;
  config.regionCount = 6;
  config.constellationsPerRegion = 4;
  config.systemCount = 180;
  return config;
}

/*
 * The site content the bake consumes (ADR-024 §7's division of custody).
 *
 * Built in code rather than parsed from a literal, and deliberately: the shape
 * of `Economy.json` is `EconomyParseTests`' subject, and repeating a whole
 * economy document here would make every site test fail for parsing reasons
 * too. What this suite is about is what the *bake* does with the numbers.
 *
 * The values mirror the shipped file closely enough that its guarantees are
 * exercised -- a High-Sec grade cap on pockets, a per-band composition where
 * only a pocket carries Nebulite in High-Sec -- because those are exactly the
 * clauses the bake has to reconcile.
 */
[[nodiscard]] SitesInfo TestSites()
{
  SitesInfo sites;
  sites.regenSeconds = 86400;
  sites.warpInStandoffMetres = 2000;
  sites.fieldRadiusMinMetres = 5000;
  sites.fieldRadiusMaxMetres = 12000;
  sites.arrivalSpreadPctOfField = 40;
  sites.clusterCountMin = 6;
  sites.clusterCountMax = 12;

  const std::uint32_t pools[SITE_GRADE_COUNT] = {4000, 8000, 16000, 32000, 64000};
  const std::uint32_t hazard[SITE_GRADE_COUNT] = {50, 100, 150, 200, 300};
  for (std::uint8_t grade = 0; grade < SITE_GRADE_COUNT; ++grade)
  {
    sites.grades[grade].poolUnits = pools[grade];
    sites.grades[grade].hazardScalePct = hazard[grade];
  }

  // FC / AC / NB per band, in `OreId` order. Nebulite is absent from High-Sec
  // except in a pocket, which is what makes the faded-pocket guarantee the only
  // way a High-Sec region covers all three ores.
  const std::uint8_t composition[SITE_ARCHETYPE_COUNT][SECURITY_BAND_COUNT][ORE_COUNT] = {
      {{90, 10, 0}, {80, 15, 5}, {70, 20, 10}},
      {{25, 75, 0}, {15, 75, 10}, {10, 75, 15}},
      {{15, 10, 75}, {5, 15, 80}, {5, 10, 85}}};
  for (std::uint8_t archetype = 0; archetype < SITE_ARCHETYPE_COUNT; ++archetype)
  {
    for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        sites.archetypes[archetype].compositionPct[band][ore] = composition[archetype][band][ore];
      }
    }
  }
  sites.archetypes[static_cast<std::uint8_t>(SiteArchetype::NebulaPocket)]
      .gradeCapByBand[static_cast<std::uint8_t>(SecurityBand::High)] = 1;

  SiteDistributionInfo& distribution = sites.distribution;
  distribution.highSecurityFloor = 60;
  distribution.lowSecurityFloor = 25;
  const std::uint8_t counts[SECURITY_BAND_COUNT][2] = {{70, 30}, {50, 50}, {30, 70}};
  const std::uint8_t weights[SECURITY_BAND_COUNT][SITE_ARCHETYPE_COUNT] = {{65, 25, 10}, {40, 35, 25}, {25, 40, 35}};
  const std::uint8_t gradeMin[SECURITY_BAND_COUNT] = {1, 2, 3};
  const std::uint8_t gradeMax[SECURITY_BAND_COUNT] = {2, 4, 5};
  const std::uint8_t sameCap[SECURITY_BAND_COUNT] = {2, 2, 3};
  for (std::uint8_t band = 0; band < SECURITY_BAND_COUNT; ++band)
  {
    distribution.siteCountWeights[band][0] = counts[band][0];
    distribution.siteCountWeights[band][1] = counts[band][1];
    for (std::uint8_t archetype = 0; archetype < SITE_ARCHETYPE_COUNT; ++archetype)
    {
      distribution.archetypeWeights[band][archetype] = weights[band][archetype];
    }
    distribution.gradeMin[band] = gradeMin[band];
    distribution.gradeMax[band] = gradeMax[band];
    distribution.maxSameArchetypePerSystem[band] = sameCap[band];
  }
  distribution.highSecFirstSiteArchetype = SiteArchetype::FerrousField;
  distribution.fadedPocketSystemsPerHighRegion = 1;
  distribution.minRegionOreGrade = 2;
  return sites;
}

/// Which band a system's security puts it in, by the same thresholds the bake
/// used. A test that re-derived them from its own literals would be checking
/// that two numbers were typed the same way twice.
[[nodiscard]] std::uint8_t TestBand(const SitesInfo& _sites, std::uint8_t _security)
{
  if (_security >= _sites.distribution.highSecurityFloor)
  {
    return static_cast<std::uint8_t>(SecurityBand::High);
  }
  if (_security >= _sites.distribution.lowSecurityFloor)
  {
    return static_cast<std::uint8_t>(SecurityBand::Low);
  }
  return static_cast<std::uint8_t>(SecurityBand::Null);
}

[[nodiscard]] UniverseDef Bake(const UniverseGenConfig& _config)
{
  UniverseDef universe;
  Assert::IsTrue(GenerateUniverse(_config, TestSites(), universe), L"the bake refused a config it should accept");
  return universe;
}

/*
 * Squared distance with the deltas shifted down first -- the universe plane is
 * wider than int64 can square. Mirrors what the generator does, for the same
 * reason it does it.
 *
 * **The shift is chosen by the widest pair the comparison can reach, not by
 * the pair you have in mind.** This is worth saying because getting it wrong
 * does not throw or assert: 1.7e18 metres shifted by 24 and squared is 1e22,
 * which wraps, and a wrapped comparison reports that a system on one side of
 * the universe is nearer a constellation on the other than its own. That is
 * exactly what a first version of `ConstellationsCluster` reported, and the
 * bake it accused was correct.
 *
 * Any comparison that ranges over *all* constellations is universe-scale, even
 * though the answer is always a local one.
 */
constexpr std::int32_t SHIFT_UNIVERSE = 30;

[[nodiscard]] std::int64_t DistanceSquared(const UniversePos& _a, const UniversePos& _b, std::int32_t _shift)
{
  const std::int64_t dx = (_a.x - _b.x) >> _shift;
  const std::int64_t dy = (_a.y - _b.y) >> _shift;
  return dx * dx + dy * dy;
}

} // namespace

TEST_CLASS(UniverseRouteTests)
{
public:
  TEST_METHOD(ARouteToWhereYouAreIsOneSystemAndNoJumps)
  {
    const UniverseDef universe = Bake(SmallConfig());
    const SystemId here = universe.systems.front().id;

    const Route route = SolveRoute(universe, here, here);
    Assert::AreEqual<std::size_t>(1, route.systems.size());
    Assert::AreEqual<std::size_t>(0, route.Jumps());
    Assert::AreEqual<std::uint32_t>(here, route.systems.front());
  }

  TEST_METHOD(EverySystemReachesEveryOtherBecauseTheBakeConnectedThem)
  {
    /*
     * U1's connectivity invariant, asked from the other side. The bake promises
     * a connected gate graph; this asks the planner, which is what a SET
     * DESTINATION actually runs, and would catch a graph that is connected only
     * in the direction the generator happened to walk it.
     */
    const UniverseDef universe = Bake(SmallConfig());
    const SystemId from = universe.systems.front().id;

    for (const SolarSystem& system : universe.systems)
    {
      const Route route = SolveRoute(universe, from, system.id);
      Assert::IsFalse(route.systems.empty(), L"a system nothing reaches");
      Assert::AreEqual<std::uint32_t>(from, route.systems.front());
      Assert::AreEqual<std::uint32_t>(system.id, route.systems.back());
    }
  }

  TEST_METHOD(EveryStepOfARouteIsAGateThatExists)
  {
    // A plan the fleet cannot fly is worse than no plan: it is a line on the
    // map that ends somewhere nobody can get to.
    const UniverseDef universe = Bake(SmallConfig());
    const SystemId from = universe.systems.front().id;
    const SystemId to = universe.systems.back().id;

    const Route route = SolveRoute(universe, from, to);
    Assert::IsTrue(route.Jumps() > 0, L"the two ends of the table should not be neighbours");

    for (std::size_t step = 0; step + 1 < route.systems.size(); ++step)
    {
      const SolarSystem* system = universe.FindSystem(route.systems[step]);
      Assert::IsNotNull(system);
      const SystemId next = route.systems[step + 1];
      const bool linked = std::any_of(system->gates.begin(), system->gates.end(),
                                      [next](const Gate& _gate) { return _gate.toSystem == next; });
      Assert::IsTrue(linked, L"the route stepped between two systems with no gate between them");
    }
  }

  TEST_METHOD(TheSameQuestionGivesTheSameRouteTwice)
  {
    /*
     * Two equally short routes must be *the same* route on the server and the
     * client, or the line drawn on the map is not the line the fleet flies.
     * That is what the tie-break on system id buys, and it is a promise worth a
     * test because a gate table walked in its own order would look right until
     * two clients disagreed.
     */
    const UniverseDef universe = Bake(SmallConfig());
    const SystemId from = universe.systems.front().id;
    const SystemId to = universe.systems.back().id;

    const Route first = SolveRoute(universe, from, to);
    const Route second = SolveRoute(universe, from, to);
    Assert::IsTrue(first.systems == second.systems);
  }

  TEST_METHOD(ARouteToNowhereIsEmptyRatherThanWrong)
  {
    const UniverseDef universe = Bake(SmallConfig());
    const SystemId from = universe.systems.front().id;

    Assert::IsTrue(SolveRoute(universe, from, 60000).systems.empty());
    Assert::IsTrue(SolveRoute(universe, 60000, from).systems.empty());
  }

  TEST_METHOD(SearchFindsSystemsByNameAndCase)
  {
    const UniverseDef universe = Bake(SmallConfig());
    const std::string& name = universe.systems.front().name;
    Assert::IsFalse(name.empty());

    const std::vector<SystemId> exact = FindSystems(universe, name);
    Assert::IsFalse(exact.empty());
    Assert::IsTrue(std::find(exact.begin(), exact.end(), universe.systems.front().id) != exact.end());

    // Case-folded, because a player typing into a search box is not typing a
    // content identifier.
    std::string shouted = name;
    for (char& letter : shouted)
    {
      letter = letter >= 'a' && letter <= 'z' ? static_cast<char>(letter - 'a' + 'A') : letter;
    }
    Assert::IsTrue(FindSystems(universe, shouted) == exact, L"case changed the answer");

    // An empty box is not a search for everything.
    Assert::IsTrue(FindSystems(universe, "").empty());
  }

  TEST_METHOD(SearchIsCappedAndOrderedSoOneLetterCannotFloodTheScreen)
  {
    const UniverseDef universe = Bake(SmallConfig());

    // The bake names systems `ROOT-N`, so a single letter matches everything.
    const std::vector<SystemId> flood = FindSystems(universe, "-", 5);
    Assert::AreEqual<std::size_t>(5, flood.size());
    Assert::IsTrue(std::is_sorted(flood.begin(), flood.end()), L"the same query has to give the same list");
  }
};

TEST_CLASS(UniverseGenTests)
{
public:
  TEST_METHOD(TheSameRecipeBakesTheSameUniverseTwice)
  {
    // The accept's first line, and the reason the generator may not touch a
    // clock, an allocator's addresses, or a float.
    const UniverseGenConfig config = SmallConfig();
    std::string first;
    std::string second;
    Assert::IsTrue(WriteUniverseJson(Bake(config), first));
    Assert::IsTrue(WriteUniverseJson(Bake(config), second));
    Assert::IsTrue(first == second, L"two bakes of one recipe produced different bytes");

    UniverseGenConfig nudged = config;
    nudged.seed ^= 1u;
    std::string third;
    Assert::IsTrue(WriteUniverseJson(Bake(nudged), third));
    Assert::IsTrue(first != third, L"changing the seed changed nothing, so the seed is not being used");
  }

  TEST_METHOD(EverySystemNameIsUniqueAndSaysWhereItIs)
  {
    const UniverseDef universe = Bake(SmallConfig());
    std::unordered_set<std::string> names;
    for (const SolarSystem& system : universe.systems)
    {
      Assert::IsTrue(names.insert(system.name).second, L"two systems share a name");
      Assert::IsFalse(system.name.empty());
    }

    std::unordered_set<std::string> regionNames;
    for (const Region& region : universe.regions)
    {
      Assert::IsTrue(regionNames.insert(region.name).second, L"two regions share a name");
    }
    std::unordered_set<std::string> constellationNames;
    for (const Constellation& constellation : universe.constellations)
    {
      Assert::IsTrue(constellationNames.insert(constellation.name).second, L"two constellations share a name");
    }
  }

  TEST_METHOD(TheGateGraphIsConnectedAndSymmetric)
  {
    const UniverseDef universe = Bake(SmallConfig());

    // Symmetry: every edge is a pair, one record in each system, each naming
    // the other. A one-sided gate is a door that only opens outward.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> seen;
    for (const SolarSystem& system : universe.systems)
    {
      Assert::IsTrue(system.gates.size() <= MAX_GATES_PER_SYSTEM, L"a system grew more gates than the cap allows");
      Assert::IsTrue(!system.gates.empty(), L"a system has no gates at all, so it cannot be reached");
      for (const Gate& gate : system.gates)
      {
        const SolarSystem* other = universe.FindSystem(gate.toSystem);
        Assert::IsNotNull(other, L"a gate leads to a system that does not exist");
        const bool mirrored = std::any_of(other->gates.begin(), other->gates.end(),
                                          [&system](const Gate& _back) { return _back.toSystem == system.id; });
        Assert::IsTrue(mirrored, L"a gate has no partner in the system it leads to");
      }
    }

    // Connectivity, by union-find over the edges. The generator builds a
    // spanning tree first, so this is checking the construction rather than
    // hoping the extra edges happened to join everything.
    std::vector<std::uint32_t> parent(universe.systems.size() + 1);
    for (std::uint32_t index = 0; index < parent.size(); ++index)
    {
      parent[index] = index;
    }
    const std::function<std::uint32_t(std::uint32_t)> find = [&parent](std::uint32_t _x)
    {
      while (parent[_x] != _x)
      {
        parent[_x] = parent[parent[_x]];
        _x = parent[_x];
      }
      return _x;
    };
    for (const SolarSystem& system : universe.systems)
    {
      for (const Gate& gate : system.gates)
      {
        parent[find(system.id)] = find(gate.toSystem);
      }
    }
    std::uint32_t roots = 0;
    for (std::uint32_t index = 1; index < parent.size(); ++index)
    {
      roots += find(index) == index ? 1u : 0u;
    }
    Assert::AreEqual(1u, roots, L"the universe is in more than one piece");
  }

  TEST_METHOD(ConstellationsCluster)
  {
    /*
     * The strategic map's stated requirement, made measurable: every system is
     * nearer its own constellation's **placed** centre than any other's. The
     * generator guarantees it by construction -- a system sits inside a radius,
     * and centres are more than twice that radius apart -- so a failure here
     * means one of those two constants moved without the other.
     *
     * Against the *placed* centre, not the centroid of the systems drawn into
     * it. That distinction is why `Constellation` carries its centre: with a
     * handful of systems sampled from a much larger lattice, the centroid
     * drifts far enough to break a property the placement never broke, and the
     * first version of this test failed for exactly that reason.
     */
    const UniverseDef universe = Bake(SmallConfig());

    std::vector<UniversePos> centre(universe.constellations.size() + 1);
    for (const Constellation& constellation : universe.constellations)
    {
      centre[constellation.id] = constellation.centre;
    }

    std::vector<std::uint32_t> members(universe.constellations.size() + 1, 0);
    for (const SolarSystem& system : universe.systems)
    {
      ++members[system.constellation];
      const std::int64_t own = DistanceSquared(system.centre, centre[system.constellation], SHIFT_UNIVERSE);
      for (const Constellation& other : universe.constellations)
      {
        if (other.id == system.constellation)
        {
          continue;
        }
        Assert::IsTrue(own < DistanceSquared(system.centre, other.centre, SHIFT_UNIVERSE),
                       L"a system sits nearer another constellation's centre than its own");
      }
    }
    for (const Constellation& constellation : universe.constellations)
    {
      Assert::IsTrue(members[constellation.id] > 0, L"a constellation has no systems");
    }
  }

  TEST_METHOD(SecurityStaysInsideItsRegionsBand)
  {
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      const Region& region = universe.regions[system.region - 1];
      Assert::AreEqual(static_cast<std::uint16_t>(system.region), static_cast<std::uint16_t>(region.id));
      Assert::IsTrue(system.security >= region.securityFloor && system.security <= region.securityCeiling,
                     L"a system's security escaped its region's band");
    }
  }

  TEST_METHOD(EveryStationOrbitsAPlanetAtTheStandoff)
  {
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      Assert::IsTrue(!system.stations.empty() && system.stations.size() <= 2, L"a system has the wrong number of stations");
      Assert::IsTrue(system.celestials.size() >= 3, L"a system needs a star and at least two planets");
      Assert::IsTrue(system.celestials[0].kind == CelestialKind::Star, L"the first celestial is the star");

      for (const Station& station : system.stations)
      {
        bool orbits = false;
        for (std::size_t index = 1; index < system.celestials.size(); ++index)
        {
          const std::int64_t dx = station.position.x - system.celestials[index].position.x;
          const std::int64_t dy = station.position.y - system.celestials[index].position.y;
          const std::int64_t distanceSq = dx * dx + dy * dy;
          // The standoff, to the metre, with two metres of rounding slack from
          // the fixed-point angle table.
          orbits = orbits || (distanceSq <= (STATION_STANDOFF_METRES + 2) * (STATION_STANDOFF_METRES + 2) &&
                              distanceSq >= (STATION_STANDOFF_METRES - 2) * (STATION_STANDOFF_METRES - 2));
        }
        Assert::IsTrue(orbits, L"a station orbits nothing");
      }
    }
  }

  TEST_METHOD(EveryAnchorIsSomewhereAFleetCanArrive)
  {
    const UniverseDef universe = Bake(SmallConfig());
    const float structureContact = ShipClass(HullClass::Structure).collisionRadiusMetres;
    constexpr std::int64_t GRID_BOUND_CM = GRID_HALF_EXTENT_METRES * 100;

    std::unordered_set<AnchorId> anchorIds;
    std::unordered_set<std::uint32_t> idBlocks;
    for (const SolarSystem& system : universe.systems)
    {
      Assert::IsTrue(!system.anchors.empty(), L"a system has nowhere to warp to");
      for (const Anchor& anchor : system.anchors)
      {
        Assert::IsTrue(anchorIds.insert(anchor.id).second, L"two anchors share an id, and a warp order carries only an id");
        if (anchor.occupantCount > 0)
        {
          // Only anchors that author something hold a block, so only they can
          // collide -- and their ids must fit the u16 window `ShipId` keeps
          // until the delta cluster widens it (ADR-018 D6).
          Assert::IsTrue(idBlocks.insert(anchor.occupantIdBase).second, L"two anchors share an occupant id block (ADR-018 D6a)");
          Assert::IsTrue(anchor.occupantIdBase >= AUTHORED_SHIP_ID_BASE, L"an authored id block starts below the authored space");
          Assert::IsTrue(anchor.occupantIdBase + anchor.occupantCount < DYNAMIC_SHIP_ID_BASE,
                         L"authored ids have run into the dynamic id space (ADR-018 D6a)");
        }
        Assert::AreEqual(static_cast<std::uint16_t>(system.id), static_cast<std::uint16_t>(anchor.system));

        // Inside the grid, or the formation solve centres on somewhere the
        // simulation cannot represent.
        Assert::IsTrue(std::abs(static_cast<std::int64_t>(anchor.warpInPoint.x)) < GRID_BOUND_CM &&
                           std::abs(static_cast<std::int64_t>(anchor.warpInPoint.y)) < GRID_BOUND_CM,
                       L"a warp-in point sits outside its own grid");
        Assert::IsTrue(anchor.arrivalSpreadRadiusCm > 0, L"an anchor reserves no room for arrival contention (ADR-018 D18)");

        const std::int64_t warpX = anchor.warpInPoint.x;
        const std::int64_t warpY = anchor.warpInPoint.y;
        if (anchor.kind == AnchorKind::Station)
        {
          // ADR-017's invariant, against ADR-018 D7's *base* radius: a fleet
          // that warps to a station can dock on arrival. The footprint-derived
          // radius only ever widens this, so the base is what the bake clears.
          constexpr std::int64_t DOCK_CM = DOCK_RADIUS_METRES * 100;
          Assert::IsTrue(warpX * warpX + warpY * warpY <= DOCK_CM * DOCK_CM,
                         L"a station's warp-in point is outside its own dock radius");

          const std::int64_t undockX = anchor.undockPoint.x;
          const std::int64_t undockY = anchor.undockPoint.y;
          const std::int64_t contactCm = static_cast<std::int64_t>(structureContact) * 100;
          Assert::IsTrue(undockX * undockX + undockY * undockY > contactCm * contactCm,
                         L"an undock point is inside the structure it undocks from");
          Assert::IsTrue(std::abs(undockX) < GRID_BOUND_CM && std::abs(undockY) < GRID_BOUND_CM,
                         L"an undock point sits outside its own grid");
          Assert::AreEqual(static_cast<std::uint16_t>(1), anchor.occupantCount, L"a station anchor should author its structure");
        }
        else if (anchor.kind == AnchorKind::Gate)
        {
          /*
           * ADR-016 §5's invariant, and U4's half of the one above it: "warp-in
           * lands inside that radius, so the common case chains seamlessly". A
           * fleet that warps to a gate to make a hop must arrive able to take
           * it, or every routed crossing is a jump plus a crawl.
           */
          constexpr std::int64_t JUMP_CM = JUMP_RADIUS_METRES * 100;
          Assert::IsTrue(warpX * warpX + warpY * warpY <= JUMP_CM * JUMP_CM,
                         L"a gate's warp-in point is outside its own jump radius");

          // And outside the gate itself, for the reason an undock point is
          // outside its station: a fleet must not arrive in contact.
          const auto gateContactCm = static_cast<std::int64_t>(ShipClass(HullClass::Gate).collisionRadiusMetres) * 100;
          Assert::IsTrue(warpX * warpX + warpY * warpY > gateContactCm * gateContactCm,
                         L"a gate's warp-in point is inside the structure it arrives beside");

          Assert::AreEqual(static_cast<std::uint16_t>(1), anchor.occupantCount, L"a gate anchor should author its gate");
        }
      }
    }
  }

  TEST_METHOD(EveryGateLeadsToOneThatLeadsBack)
  {
    /*
     * The pairing U4 jumps across, asserted from the anchor side because that
     * is the side a warp order names (ADR-016 §3). Three things have to hold at
     * once for a crossing to be a crossing rather than a one-way trip: the pair
     * exists, it is in the system the gate says it leads to, and it names this
     * one back.
     */
    const UniverseDef universe = Bake(SmallConfig());

    std::uint32_t checked = 0;
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Gate)
        {
          continue;
        }
        const AnchorId paired = universe.PairedGateAnchor(anchor.id);
        Assert::IsTrue(paired != INVALID_ID, L"a gate leads nowhere");

        const Anchor* far = universe.FindAnchor(paired);
        Assert::IsNotNull(far, L"a gate leads to an anchor nobody authored");
        Assert::IsTrue(far->kind == AnchorKind::Gate);
        Assert::IsTrue(far->system != system.id, L"a gate leads back into its own system");
        Assert::AreEqual<std::uint16_t>(anchor.id, universe.PairedGateAnchor(paired), L"the pairing is not symmetric");
        ++checked;
      }
    }
    Assert::IsTrue(checked > 0, L"the test universe has no gates, so this proved nothing");

    // And a non-gate has no far side, which is what makes `jumpAnchor` invalid
    // on every grid that is not a gate's.
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Gate)
        {
          Assert::AreEqual<std::uint16_t>(INVALID_ID, universe.PairedGateAnchor(anchor.id));
        }
      }
    }
  }

  TEST_METHOD(TheAuthoredIdSpaceFitsTheWindowAtTheCommittedScale)
  {
    /*
     * The arithmetic `ANCHOR_ID_BLOCK` states, checked against the recipe that
     * produced the committed file rather than against the small config the rest
     * of this suite bakes (ADR-018 D6a).
     *
     * It is the *worst case* rather than a measurement, so it holds without
     * baking 2,500 systems in a unit test: a system authors at most two
     * stations and `MAX_GATES_PER_SYSTEM` gates. This is what U4 had to move --
     * a block of eight fitted while stations were the only anchors authoring
     * anything, and stopped fitting the moment 6,000 gates joined them.
     */
    const UniverseGenConfig committed;
    const std::uint32_t worstAnchors = static_cast<std::uint32_t>(committed.systemCount) * (2u + MAX_GATES_PER_SYSTEM);
    const std::uint32_t worstIds = worstAnchors * ANCHOR_ID_BLOCK;

    Assert::IsTrue(AUTHORED_SHIP_ID_BASE + worstIds < DYNAMIC_SHIP_ID_BASE,
                   L"authored ids at the committed scale would run into the dynamic id space");

    /*
     * The bake also refuses rather than wrapping if a recipe ever does exhaust
     * the window -- which the arithmetic above cannot promise on its own, since
     * the gate degree cap is allowed to bend where connectivity needs it.
     *
     * That branch is deliberately not exercised here. Reaching it needs roughly
     * 8,700 systems, and the extra-edge pass is quadratic in system count, so
     * the test would cost more seconds than the guard costs instructions. What
     * *is* exercised is the posture it belongs to: a recipe that cannot be
     * satisfied is refused whole, never half-baked.
     */
    UniverseGenConfig impossible = SmallConfig();
    impossible.systemCount = 0;
    UniverseDef empty;
    Assert::IsFalse(GenerateUniverse(impossible, TestSites(), empty), L"a recipe with fewer systems than constellations is not satisfiable");
    Assert::IsTrue(empty.systems.empty());
  }

  TEST_METHOD(TheStartIsVestaThreeAndTheStartersAreValid)
  {
    const UniverseDef universe = Bake(SmallConfig());

    // The curated insert: hand-authored content keeps its geometry and the
    // galaxy grows around it.
    Assert::AreEqual(std::string("Vesta-3"), universe.systems[0].name);
    Assert::IsTrue(universe.systems[0].starter);
    Assert::AreEqual(std::string("Vesta"), universe.systems[0].celestials[0].name);
    Assert::AreEqual(std::string("Kessler"), universe.systems[0].celestials[1].name);
    Assert::AreEqual(std::string("Halgren"), universe.systems[0].celestials[2].name);
    Assert::AreEqual(std::string("Vesta-3 Anchorage"), universe.systems[0].stations[0].name);

    Assert::IsNotNull(universe.FindStation(universe.start.system, universe.start.station), L"the start names no station");

    std::uint32_t starters = 0;
    for (const SolarSystem& system : universe.systems)
    {
      if (system.starter)
      {
        ++starters;
        Assert::IsTrue(!system.stations.empty(), L"a starter system has nowhere to start");
      }
    }
    Assert::AreEqual(SmallConfig().starterSystemCount, static_cast<std::uint16_t>(starters));
  }

  TEST_METHOD(TheBakeSurvivesItsOwnRoundTrip)
  {
    // The bake writes what the game reads. If those two disagree the content
    // ships broken and the diagnostic points at the file rather than the tool.
    const UniverseDef baked = Bake(SmallConfig());
    std::string json;
    Assert::IsTrue(WriteUniverseJson(baked, json));

    UniverseDef reloaded;
    std::vector<UniverseDiagnostic> diagnostics;
    if (!ParseUniverse(json, reloaded, diagnostics))
    {
      std::string message = "the baked universe does not parse: ";
      message += diagnostics.empty() ? "no diagnostic" : diagnostics.front().Text("(baked)");
      Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
    }
    Assert::AreEqual(ComputeUniverseHash(baked), ComputeUniverseHash(reloaded), L"the round trip lost content");

    std::string again;
    Assert::IsTrue(WriteUniverseJson(reloaded, again));
    Assert::IsTrue(json == again, L"re-serialising a parsed universe changed it");
  }

  TEST_METHOD(TheHashNoticesEveryKindOfContentChange)
  {
    // The content hash is the fail-closed handshake's input (ADR-004 §3), so a
    // field it does not cover is a field two halves can disagree about
    // silently. Anchors matter most: a warp order names one.
    const UniverseDef baked = Bake(SmallConfig());
    const std::uint64_t reference = ComputeUniverseHash(baked);

    UniverseDef moved = baked;
    moved.systems[0].anchors[0].warpInPoint.x += 1;
    Assert::AreNotEqual(reference, ComputeUniverseHash(moved), L"moving a warp-in point one centimetre is a content change");

    UniverseDef renamed = baked;
    renamed.constellations[0].name += "!";
    Assert::AreNotEqual(reference, ComputeUniverseHash(renamed), L"renaming a constellation is a content change");

    UniverseDef retuned = baked;
    retuned.systems[0].security = static_cast<std::uint8_t>(retuned.systems[0].security ^ 1u);
    Assert::AreNotEqual(reference, ComputeUniverseHash(retuned), L"a system's security is content");

    UniverseDef restarted = baked;
    restarted.systems[1].starter = !restarted.systems[1].starter;
    Assert::AreNotEqual(reference, ComputeUniverseHash(restarted), L"where a commander may start is content");

    UniverseDef reblocked = baked;
    reblocked.systems[0].anchors[0].occupantIdBase += 1;
    Assert::AreNotEqual(reference, ComputeUniverseHash(reblocked), L"an occupant id block is content (ADR-018 D6a)");
  }

  TEST_METHOD(TheCommittedScaleBakesAndSurvivesItsOwnRoundTrip)
  {
    // Once, at the real size, because the properties above are structural but
    // the *arithmetic* is not: 2,500 systems is where an id space or an int64
    // square would overflow if one were going to.
    UniverseDef universe;
    Assert::IsTrue(GenerateUniverse(UniverseGenConfig{}, TestSites(), universe), L"the committed recipe does not bake");
    Assert::AreEqual(static_cast<std::size_t>(UniverseGenConfig{}.systemCount), universe.systems.size());

    std::size_t anchors = 0;
    std::size_t gates = 0;
    for (const SolarSystem& system : universe.systems)
    {
      anchors += system.anchors.size();
      gates += system.gates.size();
    }
    // ADR-016's "~2.4 average" is a design number, so it is asserted as a band
    // rather than as a value: the extra-edge pass aims at it and the degree cap
    // is allowed to win.
    Assert::IsTrue(gates >= universe.systems.size() * 2, L"the universe is more sparsely gated than ADR-016 asks");
    Assert::IsTrue(gates <= universe.systems.size() * 3, L"the universe grew more gates than ADR-016 asks");
    Assert::IsTrue(anchors > universe.systems.size() * 4, L"2,500 systems should afford far more than four anchors each");

    std::string json;
    Assert::IsTrue(WriteUniverseJson(universe, json));
    UniverseDef reloaded;
    std::vector<UniverseDiagnostic> diagnostics;
    Assert::IsTrue(ParseUniverse(json, reloaded, diagnostics), L"the committed-scale bake does not parse");
    Assert::AreEqual(ComputeUniverseHash(universe), ComputeUniverseHash(reloaded));
  }
};

/*
 * Sites: what the bake owes the economy (ADR-024 §3, build order E1b).
 *
 * The distribution numbers are content, so these check the *guarantees* rather
 * than the rolls -- a suite that asserted "65% of High-Sec sites are ferrous"
 * would fail the day someone retuned a weight, which is a thing the file is
 * meant to allow.
 */
TEST_CLASS(UniverseSiteTests)
{
public:
  TEST_METHOD(EverySystemHasTwoOrThreeSites)
  {
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      std::size_t sites = 0;
      for (const Anchor& anchor : system.anchors)
      {
        sites += anchor.kind == AnchorKind::Site ? 1u : 0u;
      }
      Assert::IsTrue(sites >= 2 && sites <= 3, L"ADR-024 says exactly 2 to 3 mining areas per system");
    }
  }

  TEST_METHOD(ASiteAuthorsNoOccupantsAndSoCostsNoShipId)
  {
    /*
     * The property that lets ~6,250 anchors join without going near the window
     * U4 measured into refusal: rocks are not entities (ADR-024 §4c), so a site
     * takes no occupant block at all. If this ever stops being true the bake
     * starts competing with stations and gates for the 32,767 authored ids.
     */
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        Assert::AreEqual(0u, static_cast<unsigned>(anchor.occupantCount), L"a site authored an occupant");
        Assert::AreEqual(0u, static_cast<unsigned>(anchor.occupantIdBase), L"a site took an occupant id block");
      }
    }
  }

  TEST_METHOD(AFieldAndItsApproachFitTheGridAtEveryEpoch)
  {
    /*
     * The invariant the moving bearing makes interesting.
     *
     * A grid is 40 km **across** -- `GRID_HALF_EXTENT_METRES` is 20,000 -- so
     * the field, the standoff outside it and the arrival arc all have to fit
     * inside 20 km of the anchor. None of the three is a function of the
     * bearing, which is exactly why the property holds for every epoch; this
     * sweeps epochs anyway, because "it cannot depend on the bearing" is the
     * kind of reasoning worth one loop of evidence.
     */
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());
    constexpr std::int64_t BOUND_CM = GRID_HALF_EXTENT_METRES * 100;

    std::uint32_t checked = 0;
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        const std::int64_t reach = static_cast<std::int64_t>(anchor.site.fieldRadiusCm) +
                                   static_cast<std::int64_t>(sites.warpInStandoffMetres) * 100 + anchor.arrivalSpreadRadiusCm;
        Assert::IsTrue(reach <= BOUND_CM, L"a field, its standoff and its arrival arc do not fit the grid");

        for (std::uint32_t epoch = 0; epoch < 8; ++epoch)
        {
          const SitePlacement placement =
              SiteEpochPlacement(system.centre, anchor.id, anchor.site, epoch, sites.warpInStandoffMetres);
          const std::int64_t x = placement.warpInPoint.x;
          const std::int64_t y = placement.warpInPoint.y;
          Assert::IsTrue(x * x + y * y <= BOUND_CM * BOUND_CM, L"an epoch put the warp-in point outside the grid");
        }
        ++checked;
      }
    }
    Assert::IsTrue(checked > 0, L"the test universe has no sites, so this proved nothing");
  }

  TEST_METHOD(TheBakedPlacementIsEpochZero)
  {
    // The file carries a real position rather than a placeholder, so anything
    // reading it without knowing epochs exist still sees a sane universe.
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        const SitePlacement zero = SiteEpochPlacement(system.centre, anchor.id, anchor.site, 0, sites.warpInStandoffMetres);
        Assert::AreEqual(zero.origin.x, anchor.origin.x, L"the baked origin is not the epoch-0 placement");
        Assert::AreEqual(zero.origin.y, anchor.origin.y, L"the baked origin is not the epoch-0 placement");
        Assert::AreEqual(zero.warpInPoint.x, anchor.warpInPoint.x, L"the baked warp-in is not epoch 0's");
        Assert::AreEqual(zero.warpInPoint.y, anchor.warpInPoint.y, L"the baked warp-in is not epoch 0's");
        Assert::AreEqual(static_cast<unsigned>(zero.warpInFacingTurns16), static_cast<unsigned>(anchor.warpInFacingTurns16),
                         L"the baked facing is not epoch 0's");
      }
    }
  }

  TEST_METHOD(AFieldReFormsBetweenEpochsAndEachEpochReproducesExactly)
  {
    /*
     * Ruling R7's whole point, as two properties rather than one: the field has
     * to *move* (or scouting never goes stale) and each epoch has to be
     * *reproducible* (or two machines disagree about where the ore is).
     */
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());
    std::uint32_t checked = 0;
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        const SitePlacement first = SiteEpochPlacement(system.centre, anchor.id, anchor.site, 3, sites.warpInStandoffMetres);
        const SitePlacement again = SiteEpochPlacement(system.centre, anchor.id, anchor.site, 3, sites.warpInStandoffMetres);
        Assert::AreEqual(first.origin.x, again.origin.x, L"an epoch does not reproduce");
        Assert::AreEqual(first.origin.y, again.origin.y, L"an epoch does not reproduce");
        Assert::AreEqual(first.layoutSalt, again.layoutSalt, L"a rock layout does not reproduce");

        const SitePlacement next = SiteEpochPlacement(system.centre, anchor.id, anchor.site, 4, sites.warpInStandoffMetres);
        Assert::IsTrue(next.origin.x != first.origin.x || next.origin.y != first.origin.y, L"the field did not move");
        Assert::IsTrue(next.layoutSalt != first.layoutSalt, L"the rock layout did not reshuffle");
        ++checked;
        break;
      }
      if (checked >= 16)
      {
        break;
      }
    }
    Assert::IsTrue(checked > 0, L"the test universe has no sites, so this proved nothing");
  }

  TEST_METHOD(TheEpochStaggerSpreadsBoundariesAcrossTheDay)
  {
    /*
     * Without the stagger every field in the shard re-forms on one tick, which
     * is a visible hitch and a predictable minute to be waiting in. The claim
     * is only that boundaries *differ*, not how they are spread -- the
     * distribution is the generator's business, not the design's.
     */
    constexpr std::uint32_t EPOCH_TICKS = 1728000; // One day at 20 Hz.
    std::unordered_set<std::uint32_t> boundaries;
    for (AnchorId anchor = 1; anchor <= 64; ++anchor)
    {
      std::uint32_t boundary = 0;
      while (boundary < EPOCH_TICKS && SiteEpochIndex(boundary, anchor, EPOCH_TICKS) == SiteEpochIndex(0, anchor, EPOCH_TICKS))
      {
        boundary += 4096;
      }
      boundaries.insert(boundary);
    }
    Assert::IsTrue(boundaries.size() > 8, L"the epoch stagger puts too many sites on one boundary");

    // A shard configured never to regenerate sits in epoch 0 forever rather
    // than dividing by zero.
    Assert::AreEqual(0u, SiteEpochIndex(999999, 7, 0));
  }

  TEST_METHOD(AHighSecSystemsFirstSiteIsTheNewPlayerFloor)
  {
    // ADR-024 §3c's on-ramp. It has to survive the faded-pocket repair, which
    // is the interaction that actually broke when E1b was written: the repair
    // converted whichever site it found first, and six regions lost their floor.
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      if (TestBand(sites, system.security) != static_cast<std::uint8_t>(SecurityBand::High))
      {
        continue;
      }
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        Assert::IsTrue(anchor.site.archetype == sites.distribution.highSecFirstSiteArchetype,
                       L"a High-Sec system's first mining field is not the hazard-free one");
        break;
      }
    }
  }

  TEST_METHOD(EveryHighSecRegionHoldsAFadedPocket)
  {
    // Ruling 1b, made measurable: Tier 1 stays craftable in every band while no
    // market exists to buy Nebulite from.
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());

    std::unordered_set<std::uint16_t> highRegions;
    std::unordered_set<std::uint16_t> withPocket;
    for (const SolarSystem& system : universe.systems)
    {
      if (TestBand(sites, system.security) != static_cast<std::uint8_t>(SecurityBand::High))
      {
        continue;
      }
      highRegions.insert(system.region);
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind == AnchorKind::Site && anchor.site.archetype == SiteArchetype::NebulaPocket)
        {
          withPocket.insert(system.region);
        }
      }
    }
    Assert::IsTrue(!highRegions.empty(), L"the test universe has no High-Sec regions, so this proved nothing");
    for (const std::uint16_t region : highRegions)
    {
      Assert::IsTrue(withPocket.count(region) == 1, L"a High-Sec region has nowhere at all to mine Nebulite");
    }
  }

  TEST_METHOD(EveryRegionCoversAllThreeOres)
  {
    /*
     * The coverage guarantee, with the precedence that makes it satisfiable: an
     * archetype's band cap beats the coverage floor. In High-Sec only a pocket
     * carries Nebulite and a pocket is capped at grade I, so a floor of II
     * applied blindly would be a promise the content cannot keep.
     */
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());

    std::unordered_set<std::uint32_t> covered; // region * ORE_COUNT + ore
    std::unordered_set<std::uint16_t> regions;
    for (const SolarSystem& system : universe.systems)
    {
      regions.insert(system.region);
      const std::uint8_t band = TestBand(sites, system.security);
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        const std::uint8_t archetypeCap = sites.archetypes[static_cast<std::uint8_t>(anchor.site.archetype)].gradeCapByBand[band];
        const std::uint8_t cap =
            archetypeCap == 0 ? sites.distribution.gradeMax[band] : std::min(archetypeCap, sites.distribution.gradeMax[band]);
        const std::uint8_t floor = std::min(sites.distribution.minRegionOreGrade, cap);
        if (anchor.site.grade < floor)
        {
          continue;
        }
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          if (anchor.site.poolUnits[ore] > 0)
          {
            covered.insert(static_cast<std::uint32_t>(system.region) * ORE_COUNT + ore);
          }
        }
      }
    }
    for (const std::uint16_t region : regions)
    {
      for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
      {
        Assert::IsTrue(covered.count(static_cast<std::uint32_t>(region) * ORE_COUNT + ore) == 1,
                       L"a region cannot supply one of the three ores at all");
      }
    }
  }

  TEST_METHOD(GradesRespectTheBandRangeAndTheArchetypeCap)
  {
    const SitesInfo sites = TestSites();
    const UniverseDef universe = Bake(SmallConfig());
    for (const SolarSystem& system : universe.systems)
    {
      const std::uint8_t band = TestBand(sites, system.security);
      for (const Anchor& anchor : system.anchors)
      {
        if (anchor.kind != AnchorKind::Site)
        {
          continue;
        }
        const std::uint8_t archetypeCap = sites.archetypes[static_cast<std::uint8_t>(anchor.site.archetype)].gradeCapByBand[band];
        const std::uint8_t cap =
            archetypeCap == 0 ? sites.distribution.gradeMax[band] : std::min(archetypeCap, sites.distribution.gradeMax[band]);
        Assert::IsTrue(anchor.site.grade >= 1 && anchor.site.grade <= cap, L"a site rolled a grade its band and archetype forbid");
        Assert::IsTrue(anchor.site.orbitRingRadiusMetres > 0, L"a field rides no orbit ring");

        std::uint32_t total = 0;
        for (const std::uint32_t pool : anchor.site.poolUnits)
        {
          total += pool;
        }
        Assert::IsTrue(total > 0, L"a mining field holds no ore at all");
      }
    }
  }

  TEST_METHOD(NoSiteContentBakesNoSites)
  {
    // What makes `SitesInfo{}` a usable argument for a suite that is not about
    // the economy -- and what stops a forgotten economy reading off the end of
    // the grade table.
    UniverseDef universe;
    Assert::IsTrue(GenerateUniverse(SmallConfig(), SitesInfo{}, universe), L"a site-free bake was refused");
    for (const SolarSystem& system : universe.systems)
    {
      for (const Anchor& anchor : system.anchors)
      {
        Assert::IsTrue(anchor.kind != AnchorKind::Site, L"a bake with no site content produced a site");
      }
    }
  }

  TEST_METHOD(SitesRoundTripThroughTheFileUnchanged)
  {
    const UniverseDef universe = Bake(SmallConfig());
    std::string json;
    Assert::IsTrue(WriteUniverseJson(universe, json), L"the universe would not serialise");

    UniverseDef reloaded;
    std::vector<UniverseDiagnostic> diagnostics;
    Assert::IsTrue(ParseUniverse(json, reloaded, diagnostics), L"a universe with sites does not parse back");
    Assert::AreEqual(ComputeUniverseHash(universe), ComputeUniverseHash(reloaded),
                     L"a site did not survive the round trip -- so a field would mean something different on each half");
  }
};

} // namespace GameLogicTests
