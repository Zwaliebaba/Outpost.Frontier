#include "pch.h"

#include "CppUnitTest.h"

#include "Relevance.h"
#include "ShipClass.h"
#include "Universe.h"
#include "UniverseParse.h"
#include "World.h"
#include "WorldRegistry.h"

#include "Wire.h"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * U3d-a: the ranking (ADR-022 §4).
 *
 * The accept this file is written against is the build order's, and its first
 * clause is the one worth restating because it is the clause a "good enough"
 * ranking quietly fails: **a total order with no duplicates and no omissions.**
 * A ranked list that lost a ship is a ship the engine can never choose to send,
 * and no amount of budget makes it appear -- so the omission tests come first
 * here, ahead of the tiering that is the interesting part.
 *
 * Everything below is device-free and socket-free by construction. Ranking is a
 * read over dense arrays, so this suite is the whole of U3d-a's proof.
 */

namespace GameLogicTests
{

namespace
{

[[nodiscard]] const UniverseDef& Universe()
{
  static const UniverseDef universe = []
  {
    static constexpr const char* CANDIDATES[] = {
      "GameData/Universe/Frontier.json",
      "../GameData/Universe/Frontier.json",
      "../../GameData/Universe/Frontier.json",
      "../../../GameData/Universe/Frontier.json",
      "../../../../GameData/Universe/Frontier.json",
    };
    for (const char* path : CANDIDATES)
    {
      std::ifstream file(path, std::ios::binary);
      if (!file)
      {
        continue;
      }
      std::ostringstream text;
      text << file.rdbuf();
      UniverseDef parsed;
      std::vector<UniverseDiagnostic> diagnostics;
      if (ParseUniverse(text.str(), parsed, diagnostics))
      {
        return parsed;
      }
    }
    Assert::Fail(L"the committed Frontier.json was not found from the test working directory");
    return UniverseDef{};
  }();
  return universe;
}

/*
 * A planet's grid, because it is the one kind that spins up with nothing on it.
 *
 * Ranking is about *which* ships lead, so a fixture that arrived with authored
 * occupants would make every count in this file an assertion about the bake.
 */
[[nodiscard]] AnchorId EmptyGrid()
{
  for (const SolarSystem& system : Universe().systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Planet)
      {
        return anchor.id;
      }
    }
  }
  return INVALID_ID;
}

/// A station's grid, for the landmark tier: the bake puts a `Structure` on it.
[[nodiscard]] AnchorId StationGrid()
{
  for (const SolarSystem& system : Universe().systems)
  {
    for (const Anchor& anchor : system.anchors)
    {
      if (anchor.kind == AnchorKind::Station)
      {
        return anchor.id;
      }
    }
  }
  return INVALID_ID;
}

void MakeRegistry(WorldRegistry& _registry)
{
  RegistryConfig config;
  config.sessionSeed = 0x03D4u;
  _registry.Reset(&Universe(), nullptr, config);
}

[[nodiscard]] ShipSpawn At(float _x, float _y, HullClass _hull = HullClass::Interceptor)
{
  ShipSpawn spawn;
  spawn.hullClass = _hull;
  spawn.wing = 1;
  spawn.xMetres = _x;
  spawn.yMetres = _y;
  return spawn;
}

constexpr Neuron::PlayerId ONE = 1;
constexpr Neuron::PlayerId TWO = 2;

[[nodiscard]] RelevanceQuery Looking(Neuron::PlayerId _viewer, AnchorId _grid, float _focusX = 0.0f, float _focusY = 0.0f,
                                     float _halfExtent = 1000.0f)
{
  RelevanceQuery query;
  query.viewer = _viewer;
  query.grid = _grid;
  query.focusXMetres = _focusX;
  query.focusYMetres = _focusY;
  query.viewHalfExtentMetres = _halfExtent;
  return query;
}

/// Where a ship came in the ranking, or `npos`. The whole suite is about
/// *order*, so almost every assertion is a comparison of two of these.
[[nodiscard]] std::size_t PlaceOf(const std::vector<ShipId>& _ranked, ShipId _shipId)
{
  const auto found = std::find(_ranked.begin(), _ranked.end(), _shipId);
  return found == _ranked.end() ? std::string::npos : static_cast<std::size_t>(std::distance(_ranked.begin(), found));
}

} // namespace

/*
 * The first clause of the accept, and the one that must hold whatever else
 * does: the ranking is a permutation of the grid.
 */
TEST_CLASS(RelevanceTotalOrderTests)
{
public:
  TEST_METHOD(EveryShipOnTheGridAppearsExactlyOnce)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    std::vector<ShipId> spawned;
    for (int index = 0; index < 24; ++index)
    {
      // Deliberately mixed: some owned, some foreign, some near, some far, so
      // every tier is populated and the permutation claim is not being made
      // about one homogeneous block.
      const auto offset = static_cast<float>(index) * 250.0f;
      const Neuron::PlayerId owner = (index % 3 == 0) ? ONE : TWO;
      spawned.push_back(registry.Spawn(grid, At(offset, offset * 0.5f), owner));
    }

    const World* world = registry.Borrow(grid);
    Assert::IsNotNull(world);

    std::vector<ShipId> ranked;
    RankRelevance(registry, *world, Looking(ONE, grid), ranked);

    Assert::AreEqual(static_cast<std::size_t>(world->ShipCount()), ranked.size(), L"the ranking changed the population");

    std::vector<ShipId> sorted = ranked;
    std::sort(sorted.begin(), sorted.end());
    Assert::IsTrue(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end(), L"a ship was ranked twice");

    for (const ShipId ship : spawned)
    {
      Assert::AreNotEqual(std::string::npos, PlaceOf(ranked, ship), L"a ship was ranked nowhere");
    }
  }

  TEST_METHOD(AnEmptyGridRanksEmptyAndClearsWhatWasThere)
  {
    /*
     * The out-parameter is cleared and refilled, not appended to. A ranking
     * that grew across ticks would send the engine ids for ships that had left
     * the grid, which is the ghost `leftInterest` exists to prevent -- arriving
     * from the wrong end.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();
    const World* world = registry.Borrow(grid);
    Assert::IsNotNull(world);
    Assert::AreEqual<std::uint32_t>(0, world->ShipCount(), L"the planet fixture was not empty");

    std::vector<ShipId> ranked{7, 8, 9};
    RankRelevance(registry, *world, Looking(ONE, grid), ranked);
    Assert::AreEqual<std::size_t>(0, ranked.size());
  }

  TEST_METHOD(TheRankingIsAFunctionOfTheQueryAlone)
  {
    /*
     * Two calls with the same query give the same answer, ties included.
     *
     * The tie-break on ship id is what makes this true: without it two ships at
     * the same distance would order by whatever `std::sort` did with them, and
     * a ranking nobody can predict is a ranking nobody can test. Four ships are
     * placed at the same distance on purpose.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();
    for (int index = 0; index < 4; ++index)
    {
      const float sign = (index % 2 == 0) ? 1.0f : -1.0f;
      const float other = (index < 2) ? 0.0f : 1.0f;
      (void)registry.Spawn(grid, At(sign * 3000.0f * (1.0f - other), sign * 3000.0f * other), TWO);
    }

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> first;
    std::vector<ShipId> second;
    RankRelevance(registry, *world, Looking(ONE, grid), first);
    RankRelevance(registry, *world, Looking(ONE, grid), second);
    Assert::IsTrue(first == second, L"equidistant ships ranked differently on two identical queries");
  }
};

/// The tiers themselves (ADR-022 §4's table).
TEST_CLASS(RelevanceTierTests)
{
public:
  TEST_METHOD(AnOwnedShipAcrossTheGridOutranksANeutralUnderTheCursor)
  {
    /*
     * The accept's headline clause, and the one the guarantee rests on
     * (ADR-022 §5a): tier 0 is not "nearby", it is *yours*. A pre-check runs
     * off ids from the snapshot, so a culled owned ship is a ship the client
     * cannot pre-check an order for -- which is BounceParity failing on the
     * player's own fleet.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    const ShipId mineFarAway = registry.Spawn(grid, At(18000.0f, 18000.0f), ONE);
    const ShipId theirsUnderCursor = registry.Spawn(grid, At(5.0f, 5.0f), TWO);

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> ranked;
    RankRelevance(registry, *world, Looking(ONE, grid), ranked);

    Assert::IsTrue(PlaceOf(ranked, mineFarAway) < PlaceOf(ranked, theirsUnderCursor),
                   L"a neutral hull under the cursor outranked the commander's own fleet");
  }

  TEST_METHOD(ASelectedForeignShipIsTierZero)
  {
    /*
     * §5a's second cause: selection, order footprints and the puck are all
     * drawn from replicated state, so culling a selected ship makes the
     * player's own selection blink. It does not matter whose ship it is.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    const ShipId mine = registry.Spawn(grid, At(100.0f, 0.0f), ONE);
    const ShipId theirsFarAway = registry.Spawn(grid, At(19000.0f, 19000.0f), TWO);
    const ShipId theirsNearer = registry.Spawn(grid, At(2000.0f, 0.0f), TWO);

    const World* world = registry.Borrow(grid);
    const ShipId selection[] = {theirsFarAway};

    RelevanceQuery query = Looking(ONE, grid);
    query.selection = selection;

    std::vector<ShipId> ranked;
    RankRelevance(registry, *world, query, ranked);

    Assert::IsTrue(PlaceOf(ranked, theirsFarAway) < PlaceOf(ranked, theirsNearer),
                   L"a selected ship did not reach tier 0");
    // And it is genuinely the selection doing it: unselected, the same hull
    // sits behind the nearer one.
    query.selection = {};
    RankRelevance(registry, *world, query, ranked);
    Assert::IsTrue(PlaceOf(ranked, theirsNearer) < PlaceOf(ranked, theirsFarAway),
                   L"the fixture did not actually depend on the selection");
    Assert::IsTrue(PlaceOf(ranked, mine) < PlaceOf(ranked, theirsNearer));
  }

  TEST_METHOD(AStructureIsTierZeroForACommanderWhoDoesNotOwnIt)
  {
    /*
     * Landmarks (§4). There are one or two per grid and they are what the
     * player navigates by; a station that flickered out of interest would take
     * their sense of where they are with it. Authored occupants belong to
     * nobody, so this is exactly the case where "tier 0 means owned" would be
     * wrong.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = StationGrid();
    const World* world = registry.Borrow(grid);
    Assert::IsNotNull(world, L"the station grid did not spin up");

    ShipId structure = INVALID_SHIP_ID;
    for (std::size_t slot = 0; slot < world->Ids().size(); ++slot)
    {
      if (static_cast<HullClass>(world->Classes()[slot]) == HullClass::Structure)
      {
        structure = world->Ids()[slot];
        break;
      }
    }
    Assert::AreNotEqual<std::uint32_t>(INVALID_SHIP_ID, structure, L"the station fixture had no structure on it");
    Assert::AreEqual<std::uint32_t>(Neuron::INVALID_PLAYER_ID, registry.OwnerOf(structure),
                                    L"the fixture's structure was owned, which is not the case under test");

    // A commander's own ship, near the focus, and a foreign one nearer still --
    // so the structure has to beat something to prove anything.
    (void)registry.Spawn(grid, At(50.0f, 50.0f), ONE);
    const ShipId theirs = registry.Spawn(grid, At(10.0f, 10.0f), TWO);

    std::vector<ShipId> ranked;
    RankRelevance(registry, *world, Looking(ONE, grid), ranked);
    Assert::IsTrue(PlaceOf(ranked, structure) < PlaceOf(ranked, theirs), L"the grid's landmark was not tier 0");
  }

  TEST_METHOD(InsideTheCameraOutranksOutsideIt)
  {
    /*
     * Tier 1 against tier 2. Both are nearest-first, so the tier is the only
     * thing that can be under test here -- and the extent is a box rather than
     * a radius, which is what the corner ship checks: it is further away than
     * the ship beyond the edge, and it is still on screen.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    const ShipId inCorner = registry.Spawn(grid, At(990.0f, 990.0f), TWO);   // ~1400 m away, on screen.
    const ShipId justBeyond = registry.Spawn(grid, At(1100.0f, 0.0f), TWO);  // 1100 m away, off screen.

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> ranked;
    RankRelevance(registry, *world, Looking(ONE, grid, 0.0f, 0.0f, 1000.0f), ranked);

    Assert::IsTrue(PlaceOf(ranked, inCorner) < PlaceOf(ranked, justBeyond),
                   L"a corner of the player's own screen ranked below empty space beside it");
  }

  TEST_METHOD(NearerToFocusLeadsWithinATier)
  {
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    const ShipId nearTheOrigin = registry.Spawn(grid, At(4200.0f, 0.0f), TWO);
    const ShipId furtherOut = registry.Spawn(grid, At(6000.0f, 0.0f), TWO);

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> ranked;

    // Both outside a tight camera, so both are tier 2 and distance is the only
    // thing ordering them.
    RankRelevance(registry, *world, Looking(ONE, grid, 0.0f, 0.0f, 100.0f), ranked);
    Assert::IsTrue(PlaceOf(ranked, nearTheOrigin) < PlaceOf(ranked, furtherOut));

    // Now move the camera past both of them. The ranking must follow the focus
    // rather than the plane's origin, so the order inverts.
    RankRelevance(registry, *world, Looking(ONE, grid, 6500.0f, 0.0f, 100.0f), ranked);
    Assert::IsTrue(PlaceOf(ranked, furtherOut) < PlaceOf(ranked, nearTheOrigin),
                   L"the ranking read the origin instead of the focus");
  }

  TEST_METHOD(ARelationshipIsViewerRelative)
  {
    /*
     * The same hull is `Own` to one commander and `Neutral` to the next, which
     * is why relationship is computed per query and not stored on the world
     * (ADR-022 §8b). `Allied` and `Hostile` have no producer until the combat
     * phase, which is recorded on the enum rather than hidden here.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();
    const ShipId ship = registry.Spawn(grid, At(0.0f, 0.0f), ONE);

    Assert::IsTrue(RelationshipOf(registry, ONE, ship) == Relationship::Own);
    Assert::IsTrue(RelationshipOf(registry, TWO, ship) == Relationship::Neutral);
    Assert::IsTrue(RelationshipOf(registry, Neuron::INVALID_PLAYER_ID, ship) == Relationship::Neutral,
                   L"an anonymous viewer owned something");
  }
};

/// Tier 2's round-robin (ADR-022 §4), which is the clause that turns
/// "nearest first" from a permanent ranking into a cadence.
TEST_CLASS(RelevanceRoundRobinTests)
{
public:
  TEST_METHOD(EveryTierTwoShipReachesTheFrontWithinABoundedNumberOfTicks)
  {
    /*
     * The accept's own words. Without the rotation the far tail of a busy grid
     * sits below the budget line every tick and updates *never*, which is the
     * outcome §4's "rather than never" is written against.
     *
     * The bound asserted is the tier's own size: rotating by `tick % count`
     * means every ship leads its tier once per `count` ticks, which is what
     * makes the bound a property of the rule rather than of a lucky stride.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    constexpr int POPULATION = 12;
    std::vector<ShipId> outsiders;
    for (int index = 0; index < POPULATION; ++index)
    {
      // All foreign and all well outside a tight camera, so every one of them
      // is tier 2 and the rotation is the only thing ordering them.
      outsiders.push_back(registry.Spawn(grid, At(5000.0f + static_cast<float>(index) * 400.0f, 0.0f), TWO));
    }

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> ledTheTier;
    std::vector<ShipId> ranked;
    for (std::uint32_t tick = 0; tick < POPULATION; ++tick)
    {
      RelevanceQuery query = Looking(ONE, grid, 0.0f, 0.0f, 10.0f);
      query.tick = tick;
      RankRelevance(registry, *world, query, ranked);
      Assert::AreEqual<std::size_t>(POPULATION, ranked.size());
      ledTheTier.push_back(ranked.front());
    }

    std::sort(ledTheTier.begin(), ledTheTier.end());
    std::sort(outsiders.begin(), outsiders.end());
    Assert::IsTrue(ledTheTier == outsiders, L"some tier 2 ship never reached the front of its tier");
  }

  TEST_METHOD(TheRotationNeverDisturbsTierZero)
  {
    /*
     * The guarantee is not a cadence (§5a). An owned ship must lead on *every*
     * tick, not on its turn -- so the rotation is scoped to tier 2 and this is
     * the assertion that says so.
     */
    WorldRegistry registry;
    MakeRegistry(registry);
    const AnchorId grid = EmptyGrid();

    const ShipId mine = registry.Spawn(grid, At(9000.0f, 9000.0f), ONE);
    for (int index = 0; index < 8; ++index)
    {
      (void)registry.Spawn(grid, At(200.0f + static_cast<float>(index) * 100.0f, 0.0f), TWO);
    }

    const World* world = registry.Borrow(grid);
    std::vector<ShipId> ranked;
    for (std::uint32_t tick = 0; tick < 40; ++tick)
    {
      RelevanceQuery query = Looking(ONE, grid, 0.0f, 0.0f, 10.0f);
      query.tick = tick;
      RankRelevance(registry, *world, query, ranked);
      Assert::AreEqual<std::size_t>(0, PlaceOf(ranked, mine), L"the commander's own ship lost the lead to the rotation");
    }
  }
};

} // namespace GameLogicTests
