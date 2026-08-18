#include "pch.h"
#include "CppUnitTest.h"

#include "Universe.h"
#include "UniverseParse.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace Game;

/*
 * The universe definition (Build Order S5b, ADR-009).
 *
 * Everything here runs on bytes held in this file. `ParseUniverse` takes text,
 * not a path, so these tests need no filesystem, no fixtures and nothing to
 * clean up -- which is the practical payoff of ADR-009 §7 keeping file IO in
 * the hosts.
 */

namespace GameLogicTests
{

namespace
{

/// A complete, valid universe. Tests edit one thing about it and re-parse, so
/// each case says exactly what it is about.
constexpr const char* VALID_UNIVERSE = R"json({
  "name": "Test",
  "start": { "system": 1, "station": 1 },
  "regions": [ { "id": 1, "name": "Reach" } ],
  "systems": [
    {
      "id": 1,
      "region": 1,
      "name": "Vesta-3",
      "centre": { "x": 4200000000, "y": -1750000000 },
      "celestials": [
        { "id": 1, "kind": "star",   "name": "Vesta",   "position": { "x": 4200000000, "y": -1750000000 }, "radiusMetres": 696000000 },
        { "id": 2, "kind": "planet", "name": "Kessler", "position": { "x": 4310000000, "y": -1750000000 }, "radiusMetres": 6051000 }
      ],
      "stations": [
        { "id": 1, "name": "Anchorage", "position": { "x": 4309400000, "y": -1749880000 } }
      ]
    }
  ]
})json";

[[nodiscard]] UniverseDef ParseOrFail(const char* _text)
{
  UniverseDef universe;
  std::vector<UniverseDiagnostic> diagnostics;
  if (!ParseUniverse(_text, universe, diagnostics))
  {
    const std::string message = diagnostics.empty() ? "no diagnostic" : diagnostics.front().Text();
    Assert::Fail(std::wstring(message.begin(), message.end()).c_str());
  }
  return universe;
}

/// Asserts the text is refused, and that it said something about it. A parser
/// that returns false with an empty diagnostic list is a parser nobody can use.
void AssertRejected(const char* _text, const wchar_t* _what)
{
  UniverseDef universe;
  std::vector<UniverseDiagnostic> diagnostics;
  Assert::IsFalse(ParseUniverse(_text, universe, diagnostics), _what);
  Assert::IsFalse(diagnostics.empty(), _what);
  Assert::IsFalse(diagnostics.front().message.empty(), _what);
  Assert::IsTrue(universe.systems.empty(), L"a refused parse must not hand back a half-read universe");
}

} // namespace

TEST_CLASS(UniverseParseTests)
{
public:
  TEST_METHOD(ReadsEveryFieldItWasGiven)
  {
    const UniverseDef universe = ParseOrFail(VALID_UNIVERSE);

    Assert::AreEqual(std::string("Test"), universe.name);
    Assert::AreEqual<std::size_t>(1, universe.regions.size());
    Assert::AreEqual<std::size_t>(1, universe.systems.size());

    const SolarSystem& system = universe.systems.front();
    Assert::AreEqual<std::uint16_t>(1, system.id);
    Assert::AreEqual<std::uint16_t>(1, system.region);
    Assert::AreEqual(std::string("Vesta-3"), system.name);
    Assert::AreEqual<std::int64_t>(4200000000, system.centre.x);
    Assert::AreEqual<std::int64_t>(-1750000000, system.centre.y);

    Assert::AreEqual<std::size_t>(2, system.celestials.size());
    Assert::IsTrue(system.celestials[0].kind == CelestialKind::Star);
    Assert::IsTrue(system.celestials[1].kind == CelestialKind::Planet);
    Assert::AreEqual<std::int64_t>(696000000, system.celestials[0].radiusMetres);

    Assert::AreEqual<std::size_t>(1, system.stations.size());
    Assert::AreEqual(std::string("Anchorage"), system.stations[0].name);
    Assert::IsTrue(system.gates.empty(), L"gates are optional and this file has none");
  }

  TEST_METHOD(CoordinatesKeepEveryDigitPastTwoToTheFiftyThree)
  {
    // The reason ADR-012 §C7 demanded exact int64 from the parser. A double
    // would round this, and a station would end up metres from where it was
    // authored with nothing to show for it.
    constexpr std::int64_t BEYOND_DOUBLE = 9007199254740993; // 2^53 + 1
    const std::string text = std::string(R"json({
      "name": "Far", "start": { "system": 1, "station": 1 },
      "regions": [ { "id": 1, "name": "R" } ],
      "systems": [ { "id": 1, "region": 1, "name": "S", "centre": { "x": 0, "y": 0 },
        "stations": [ { "id": 1, "name": "St", "position": { "x": 9007199254740993, "y": -9007199254740993 } } ] } ]
    })json");

    const UniverseDef universe = ParseOrFail(text.c_str());
    Assert::AreEqual<std::int64_t>(BEYOND_DOUBLE, universe.systems[0].stations[0].position.x);
    Assert::AreEqual<std::int64_t>(-BEYOND_DOUBLE, universe.systems[0].stations[0].position.y);
  }

  TEST_METHOD(CommentsAndTrailingCommasAreAccepted)
  {
    // Hand-authored content: ADR-012 §C5's two relaxations exist for exactly
    // this file, so a universe that uses them must parse.
    constexpr const char* text = R"json({
      // The universe.
      "name": "Test",
      "start": { "system": 1, "station": 1 },
      "regions": [ { "id": 1, "name": "Reach" }, ],
      "systems": [
        { "id": 1, "region": 1, "name": "S", "centre": { "x": 0, "y": 0 },
          "stations": [ { "id": 1, "name": "St", "position": { "x": 0, "y": 0 } }, ] }, /* trailing */
      ],
    })json";
    const UniverseDef universe = ParseOrFail(text);
    Assert::AreEqual<std::size_t>(1, universe.systems.size());
  }

  TEST_METHOD(MalformedContentIsRefusedWithADiagnostic)
  {
    AssertRejected("{ not json", L"malformed JSON");
    AssertRejected("[]", L"a root that is not an object");
    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[],"systems":[]})json", L"a universe with no systems");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":2,"name":"s","centre":{"x":0,"y":0},
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"a system naming a region that is not declared");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":9},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"a start pointing at a station that is not there");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},
      "stations":[{"id":1,"name":"a","position":{"x":0,"y":0}},{"id":1,"name":"b","position":{"x":1,"y":0}}]}]})json",
                   L"two stations sharing an id");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},"stations":[]}]})json",
                   L"a system with no station -- there would be nowhere to anchor a grid");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":1.5,"y":0},
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"a fractional coordinate, which is a rounded coordinate");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},
      "stations":[{"id":0,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"id 0, which is reserved for 'nothing'");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},"colour":"red",
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"an unknown key, which in content means the halves may read different things");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},
      "celestials":[{"id":1,"kind":"comet","name":"c","position":{"x":0,"y":0},"radiusMetres":1}],
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"a celestial kind outside the closed set");

    AssertRejected(R"json({"name":"x","start":{"system":1,"station":1},"regions":[{"id":1,"name":"r"}],
      "systems":[{"id":1,"region":1,"name":"s","centre":{"x":0,"y":0},
      "gates":[{"id":1,"name":"g","to":9,"position":{"x":0,"y":0}}],
      "stations":[{"id":1,"name":"st","position":{"x":0,"y":0}}]}]})json",
                   L"a gate leading to a system that does not exist");
  }

  TEST_METHOD(EveryProblemIsReportedInOnePass)
  {
    // ADR-012 §C8: authoring a universe means getting several things wrong at
    // once, and a parser that surfaces them one run at a time turns fixing them
    // into several runs.
    constexpr const char* text = R"json({
      "name": "x", "start": { "system": 1, "station": 1 },
      "regions": [ { "id": 1, "name": "r" } ],
      "systems": [ { "id": 1, "region": 1, "name": "s", "centre": { "x": 0, "y": 0 },
        "celestials": [ { "id": 1, "kind": "comet", "name": "c", "position": { "x": 0, "y": 0 }, "radiusMetres": 1 } ],
        "stations": [ { "id": 1, "name": "a", "position": { "x": 0, "y": 0 } },
                      { "id": 1, "name": "b", "position": { "x": 1, "y": 0 } } ] } ]
    })json";

    UniverseDef universe;
    std::vector<UniverseDiagnostic> diagnostics;
    Assert::IsFalse(ParseUniverse(text, universe, diagnostics));
    Assert::IsTrue(diagnostics.size() >= 2, L"both the bad celestial kind and the duplicate station id should be reported");
  }

  TEST_METHOD(ADiagnosticSaysWhereToLook)
  {
    constexpr const char* text = R"json({
      "name": "x", "start": { "system": 1, "station": 1 },
      "regions": [ { "id": 1, "name": "r" } ],
      "systems": [ { "id": 1, "region": 1, "name": "s", "centre": { "x": 0, "y": 0 },
        "stations": [ { "id": 1, "name": "a", "position": { "x": 0, "y": 0 } },
                      { "id": 1, "name": "b", "position": { "x": 1, "y": 0 } } ] } ]
    })json";

    UniverseDef universe;
    std::vector<UniverseDiagnostic> diagnostics;
    Assert::IsFalse(ParseUniverse(text, universe, diagnostics));

    const UniverseDiagnostic& diagnostic = diagnostics.front();
    Assert::IsTrue(diagnostic.line > 0, L"a diagnostic carries the line it was found on");
    Assert::IsTrue(diagnostic.path.find("stations") != std::string::npos, L"and the JSON path to the thing to edit");

    const std::string text8 = diagnostic.Text("Frontier.json");
    Assert::IsTrue(text8.find("Frontier.json") != std::string::npos, L"and names the file when it is given one");
  }
};

TEST_CLASS(UniverseHashTests)
{
public:
  TEST_METHOD(FormattingAndKeyOrderDoNotChangeIt)
  {
    // ADR-012 §D: the hash is over the *parsed content*, so a reformat, a
    // comment or a reordered object is not a content change. If it were, every
    // whitespace edit would refuse every client.
    const UniverseDef reference = ParseOrFail(VALID_UNIVERSE);

    constexpr const char* reordered = R"json({
      // reformatted, re-ordered, and commented
      "systems": [ { "stations": [ { "position": { "y": -1749880000, "x": 4309400000 }, "name": "Anchorage", "id": 1 } ],
        "celestials": [
          { "radiusMetres": 6051000, "position": { "x": 4310000000, "y": -1750000000 }, "name": "Kessler", "kind": "planet", "id": 2 },
          { "radiusMetres": 696000000, "position": { "y": -1750000000, "x": 4200000000 }, "name": "Vesta", "kind": "star", "id": 1 } ],
        "centre": { "y": -1750000000, "x": 4200000000 }, "name": "Vesta-3", "region": 1, "id": 1 } ],
      "regions": [ { "name": "Reach", "id": 1 } ],
      "start": { "station": 1, "system": 1 },
      "name": "Test"
    })json";

    Assert::AreEqual(ComputeUniverseHash(reference), ComputeUniverseHash(ParseOrFail(reordered)));
  }

  TEST_METHOD(ReorderingEntitiesDoesNotChangeIt)
  {
    // The hash walks ids, not file order, so moving a system up the array is
    // not a content change. Authors reorder files; they should not have to
    // re-ship clients for it.
    UniverseDef universe = ParseOrFail(VALID_UNIVERSE);
    const std::uint64_t before = ComputeUniverseHash(universe);

    std::swap(universe.systems[0].celestials[0], universe.systems[0].celestials[1]);
    Assert::AreEqual(before, ComputeUniverseHash(universe));
  }

  TEST_METHOD(RealContentChangesDoChangeIt)
  {
    const UniverseDef reference = ParseOrFail(VALID_UNIVERSE);
    const std::uint64_t before = ComputeUniverseHash(reference);

    {
      UniverseDef moved = reference;
      moved.systems[0].stations[0].position.x += 1; // One metre.
      Assert::AreNotEqual(before, ComputeUniverseHash(moved), L"moving a station one metre is a content change");
    }
    {
      UniverseDef renamed = reference;
      renamed.systems[0].name = "Vesta-4";
      Assert::AreNotEqual(before, ComputeUniverseHash(renamed), L"renaming a system is a content change");
    }
    {
      UniverseDef renumbered = reference;
      renumbered.systems[0].celestials[1].id = 9;
      Assert::AreNotEqual(before, ComputeUniverseHash(renumbered), L"renumbering an entity is a content change");
    }
    {
      UniverseDef removed = reference;
      removed.systems[0].celestials.pop_back();
      Assert::AreNotEqual(before, ComputeUniverseHash(removed), L"removing a celestial is a content change");
    }
    {
      UniverseDef restarted = reference;
      restarted.start.station = 2;
      Assert::AreNotEqual(before, ComputeUniverseHash(restarted), L"starting somewhere else is a content change");
    }
    {
      UniverseDef retitled = reference;
      retitled.name = "Test2";
      Assert::AreNotEqual(before, ComputeUniverseHash(retitled), L"the universe's own name is content too");
    }
  }

  TEST_METHOD(ItIsStableAcrossRuns)
  {
    // The handshake compares hashes computed by two different processes, so
    // "the same twice" is the entire requirement.
    Assert::AreEqual(ComputeUniverseHash(ParseOrFail(VALID_UNIVERSE)), ComputeUniverseHash(ParseOrFail(VALID_UNIVERSE)));
  }
};

TEST_CLASS(GridAnchorTests)
{
public:
  TEST_METHOD(TheStartAnchorIsTheStartStation)
  {
    const UniverseDef universe = ParseOrFail(VALID_UNIVERSE);
    const GridAnchor anchor = universe.StartAnchor();

    Assert::AreEqual<std::uint16_t>(1, anchor.system);
    Assert::AreEqual<std::int64_t>(4309400000, anchor.origin.x);
    Assert::AreEqual<std::int64_t>(-1749880000, anchor.origin.y);
  }

  TEST_METHOD(AnchorPlusLocalReconstructsTheExactPosition)
  {
    // ADR-009 §2's promise, as a property. Integer throughout, so "without
    // loss" means bit-exact rather than close enough.
    const UniversePos anchor{4309400000, -1749880000};
    const std::int64_t offsets[] = {0, 1, -1, 7, -7, 999, -999, 12345, -12345, GRID_HALF_EXTENT_METRES, -GRID_HALF_EXTENT_METRES};

    for (const std::int64_t dx : offsets)
    {
      for (const std::int64_t dy : offsets)
      {
        const UniversePos position{anchor.x + dx, anchor.y + dy};

        LocalOffsetCm local;
        Assert::IsTrue(LocalFromUniverse(anchor, position, local), L"a position on the grid converts");
        Assert::IsTrue(IsWholeMetres(local), L"a whole-metre placement is a whole number of centimetres");
        Assert::AreEqual<std::int64_t>(dx * 100, local.x);
        Assert::AreEqual<std::int64_t>(dy * 100, local.y);
        Assert::IsTrue(UniverseFromLocal(anchor, local) == position, L"anchor + local is the position it came from");
      }
    }
  }

  TEST_METHOD(OffGridPositionsAreRefusedRatherThanWrapped)
  {
    const UniversePos anchor{4309400000, -1749880000};
    LocalOffsetCm local;

    Assert::IsTrue(LocalFromUniverse(anchor, UniversePos{anchor.x + GRID_HALF_EXTENT_METRES, anchor.y}, local),
                   L"the grid edge is on the grid");
    Assert::IsFalse(LocalFromUniverse(anchor, UniversePos{anchor.x + GRID_HALF_EXTENT_METRES + 1, anchor.y}, local),
                    L"one metre past it is not");
    Assert::IsFalse(LocalFromUniverse(anchor, UniversePos{0, 0}, local), L"another part of the system is not");
  }

  TEST_METHOD(TheEndsOfThePlaneDoNotWrapIntoAFalseAccept)
  {
    // The subtraction this guards is the one that would be undefined: two
    // positions at opposite ends of an int64 plane. A wrapped difference that
    // happened to land inside the grid would place something 18 quintillion
    // metres away as if it were next door.
    constexpr std::int64_t LOWEST = std::numeric_limits<std::int64_t>::min();
    constexpr std::int64_t HIGHEST = std::numeric_limits<std::int64_t>::max();

    LocalOffsetCm local;
    Assert::IsFalse(LocalFromUniverse(UniversePos{LOWEST, LOWEST}, UniversePos{HIGHEST, HIGHEST}, local),
                    L"opposite ends of the plane are refused");
    Assert::IsFalse(LocalFromUniverse(UniversePos{HIGHEST, HIGHEST}, UniversePos{LOWEST, LOWEST}, local), L"and the other way round");

    // An anchor at the very edge still works for things genuinely beside it.
    Assert::IsTrue(LocalFromUniverse(UniversePos{LOWEST, LOWEST}, UniversePos{LOWEST + 5, LOWEST + 5}, local));
    Assert::IsTrue(UniverseFromLocal(UniversePos{LOWEST, LOWEST}, local) == (UniversePos{LOWEST + 5, LOWEST + 5}));
    Assert::IsTrue(LocalFromUniverse(UniversePos{HIGHEST, HIGHEST}, UniversePos{HIGHEST - 5, HIGHEST - 5}, local));
  }

  TEST_METHOD(LocalMetresMatchTheCentimetres)
  {
    const UniversePos anchor{1000, 2000};
    const DirectX::XMFLOAT2 metres = LocalMetresFromUniverse(anchor, UniversePos{1250, 1875});

    Assert::AreEqual(250.0f, metres.x, 1e-3f);
    Assert::AreEqual(-125.0f, metres.y, 1e-3f);
  }
};

} // namespace GameLogicTests
