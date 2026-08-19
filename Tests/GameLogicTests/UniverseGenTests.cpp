#include "pch.h"
#include "CppUnitTest.h"

#include "ShipClass.h"
#include "Universe.h"
#include "UniverseGen.h"
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

[[nodiscard]] UniverseDef Bake(const UniverseGenConfig& _config)
{
  UniverseDef universe;
  Assert::IsTrue(GenerateUniverse(_config, universe), L"the bake refused a config it should accept");
  return universe;
}

/// Squared distance with the deltas shifted down first -- the universe plane is
/// wider than int64 can square. Mirrors what the generator does, for the same
/// reason it does it.
[[nodiscard]] std::int64_t DistanceSquared(const UniversePos& _a, const UniversePos& _b, std::int32_t _shift)
{
  const std::int64_t dx = (_a.x - _b.x) >> _shift;
  const std::int64_t dy = (_a.y - _b.y) >> _shift;
  return dx * dx + dy * dy;
}

} // namespace

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
    // The strategic map's stated requirement, made measurable: every system is
    // nearer its own constellation's centre than any other's. The generator
    // guarantees it by construction (a system sits inside a radius, centres are
    // more than twice that radius apart), so a failure here means one of those
    // two constants moved without the other.
    const UniverseDef universe = Bake(SmallConfig());

    std::vector<UniversePos> centre(universe.constellations.size() + 1);
    std::vector<std::uint32_t> members(universe.constellations.size() + 1, 0);
    for (const SolarSystem& system : universe.systems)
    {
      centre[system.constellation].x += system.centre.x >> 8;
      centre[system.constellation].y += system.centre.y >> 8;
      ++members[system.constellation];
    }
    for (std::size_t index = 1; index < centre.size(); ++index)
    {
      Assert::IsTrue(members[index] > 0, L"a constellation has no systems");
      centre[index].x = (centre[index].x / members[index]) << 8;
      centre[index].y = (centre[index].y / members[index]) << 8;
    }

    for (const SolarSystem& system : universe.systems)
    {
      const std::int64_t own = DistanceSquared(system.centre, centre[system.constellation], 24);
      for (std::size_t index = 1; index < centre.size(); ++index)
      {
        if (index == system.constellation)
        {
          continue;
        }
        Assert::IsTrue(own < DistanceSquared(system.centre, centre[index], 24),
                       L"a system sits nearer another constellation's centre than its own");
      }
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
        Assert::IsTrue(idBlocks.insert(anchor.occupantIdBase).second, L"two anchors share an occupant id block (ADR-018 D6a)");
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
      }
    }
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
    Assert::IsTrue(GenerateUniverse(UniverseGenConfig{}, universe), L"the committed recipe does not bake");
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

} // namespace GameLogicTests
