#include "pch.h"

#include "UniverseGen.h"

#include "ShipClass.h"

#include "JsonWriter.h"
#include "Random.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace Game
{
namespace
{

/*
 * Angles, in integers.
 *
 * A quarter wave of sine at 2^30, sampled 64 times, mirrored into 256 steps
 * around the circle. Every celestial and every anchor offset is placed with
 * this table, because the bake's output is compared byte for byte across
 * compilers (U1's accept) and `std::sin` is not a promise any two of them make
 * identically.
 *
 * 256 steps is 1.4 degrees, which is finer than anything here needs: an orbit's
 * bearing is flavour, and an anchor's warp-in bearing is a direction to stand
 * off in, not a tolerance to hit.
 */
constexpr std::int32_t SIN_SCALE_BITS = 30;
constexpr std::uint32_t ANGLE_STEPS = 256;

constexpr std::array<std::int32_t, 65> SIN_QUARTER = {
    0,          26350943,   52686014,   78989349,   105245103,  131437462,  157550647,  183568930,
    209476638,  235258165,  260897982,  286380643,  311690799,  336813204,  361732726,  386434353,
    410903207,  435124548,  459083786,  482766489,  506158392,  529245404,  552013618,  574449320,
    596538995,  618269338,  639627258,  660599890,  681174602,  701339000,  721080937,  740388522,
    759250125,  777654384,  795590213,  813046808,  830013654,  846480531,  862437520,  877875009,
    892783698,  907154608,  920979082,  934248793,  946955747,  959092290,  970651112,  981625251,
    992008094,  1001793390, 1010975242, 1019548121, 1027506862, 1034846671, 1041563127, 1047652185,
    1053110176, 1057933813, 1062120190, 1065666786, 1068571464, 1070832474, 1072448455, 1073418433,
    1073741824};

/// sin(2*pi*_step/256), scaled by 2^30.
[[nodiscard]] std::int64_t SinStep(std::uint32_t _step) noexcept
{
  const std::uint32_t step = _step % ANGLE_STEPS;
  const std::uint32_t quadrant = step / 64;
  const std::uint32_t index = step % 64;
  switch (quadrant)
  {
  case 0:
    return SIN_QUARTER[index];
  case 1:
    return SIN_QUARTER[64 - index];
  case 2:
    return -SIN_QUARTER[index];
  default:
    return -SIN_QUARTER[64 - index];
  }
}

[[nodiscard]] std::int64_t CosStep(std::uint32_t _step) noexcept
{
  return SinStep(_step + ANGLE_STEPS / 4);
}

/// `_radius` metres at `_step`/256 of a turn, rounded to the metre. The
/// multiply is int64 throughout: a radius of 30 AU is 4.5e12, and 4.5e12 * 2^30
/// overflows nothing in 64 bits but would silently ruin 32.
[[nodiscard]] UniversePos PolarOffset(std::int64_t _radiusMetres, std::uint32_t _step) noexcept
{
  return UniversePos{(_radiusMetres * CosStep(_step)) >> SIN_SCALE_BITS, (_radiusMetres * SinStep(_step)) >> SIN_SCALE_BITS};
}

[[nodiscard]] UniversePos Offset(const UniversePos& _base, const UniversePos& _delta) noexcept
{
  return UniversePos{_base.x + _delta.x, _base.y + _delta.y};
}

/*
 * Squared distance with the deltas shifted down first.
 *
 * The shift is not a nicety. The universe is ~1.5e18 metres across and int64
 * tops out at 9.2e18, so squaring a raw universe-plane delta overflows before
 * the two systems are even in different regions -- silently, and into a
 * *negative* number, which would make "nearest" mean "furthest". Every caller
 * states the scale it is working at: `SHIFT_UNIVERSE` for anything comparing
 * across the plane, `SHIFT_CONSTELLATION` inside a cluster, `SHIFT_SYSTEM` for
 * in-system geometry where the resolution matters more than the range.
 *
 * Comparisons stay honest under the shift because it is applied to both sides.
 */
constexpr std::int32_t SHIFT_UNIVERSE = 30;
constexpr std::int32_t SHIFT_CONSTELLATION = 24;
constexpr std::int32_t SHIFT_SYSTEM = 10;

[[nodiscard]] std::int64_t DistanceSquared(const UniversePos& _a, const UniversePos& _b, std::int32_t _shift) noexcept
{
  const std::int64_t dx = (_a.x - _b.x) >> _shift;
  const std::int64_t dy = (_a.y - _b.y) >> _shift;
  return dx * dx + dy * dy;
}

/*
 * The name vocabulary (Universe build order D3).
 *
 * Roots are built from syllables rather than listed whole, which is authoring
 * with leverage: 40 x 8 gives 320 roots, every one unique by construction, and
 * uniqueness is what the naming invariant actually needs. A hand-listed 300
 * would be 300 chances to type the same word twice.
 *
 * The syllables are chosen to read as *places* -- hard consonants, few vowels
 * in a row -- so "Kessler Drift" and "Vardan Reach" sit beside the authored
 * Vesta-3 without anyone being able to tell which was written by hand.
 */
constexpr std::array<const char*, 40> ROOT_PREFIX = {"Kess",  "Var",  "Hal",  "Tor",  "Mer",  "Dun",  "Cal",  "Bran",
                                                     "Sev",   "Ald",  "Corv", "Fen",  "Gar",  "Hoth", "Ish",  "Jor",
                                                     "Kal",   "Lorn", "Mal",  "Nym",  "Orr",  "Pel",  "Quar", "Rav",
                                                     "Sol",   "Thal", "Ur",   "Vex",  "Wend", "Xan",  "Yar",  "Zel",
                                                     "Ambr",  "Bex",  "Cind", "Dral", "Eph",  "Fald", "Grim", "Hest"};

constexpr std::array<const char*, 8> ROOT_SUFFIX = {"ler", "dan", "gren", "ith", "mar", "vek", "ros", "tia"};

constexpr std::array<const char*, 8> REGION_FORM = {"Reach", "Expanse", "Verge", "Dominion", "Marches", "Sprawl", "Basin", "Shelf"};

constexpr std::array<const char*, 8> CONSTELLATION_FORM = {"Drift", "Veil",  "Chain",  "Cluster",
                                                           "Span",  "Wreath", "Lattice", "Shoal"};

/// The `_index`-th root of the 320 the vocabulary affords, uppercased or not.
[[nodiscard]] std::string RootWord(std::uint32_t _index, bool _upper)
{
  const char* prefix = ROOT_PREFIX[_index % ROOT_PREFIX.size()];
  const char* suffix = ROOT_SUFFIX[(_index / ROOT_PREFIX.size()) % ROOT_SUFFIX.size()];
  std::string word = std::string(prefix) + suffix;
  if (_upper)
  {
    for (char& letter : word)
    {
      if (letter >= 'a' && letter <= 'z')
      {
        letter = static_cast<char>(letter - 'a' + 'A');
      }
    }
  }
  return word;
}

/*
 * A deterministic permutation of [0, _count), so the vocabulary is spent in an
 * order that does not read as alphabetical without any name being drawn twice.
 * Fisher-Yates over the seeded PCG32 -- the only randomness in this file.
 */
[[nodiscard]] std::vector<std::uint32_t> ShuffledIndices(std::uint32_t _count, Neuron::Pcg32& _rng)
{
  std::vector<std::uint32_t> order(_count);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    order[index] = index;
  }
  for (std::uint32_t index = _count; index > 1; --index)
  {
    const std::uint32_t pick = _rng.NextBelow(index);
    std::swap(order[index - 1], order[pick]);
  }
  return order;
}

/*
 * A signed value in [-_magnitude, +_magnitude].
 *
 * Two draws, not one. The magnitudes here are astronomical -- constellation
 * jitter is 1.5e16 metres -- and a 32-bit draw folded into that range covers
 * four billion of it, so a one-draw version is not "slightly biased", it is a
 * jitter that is negative essentially always. Sixty-four bits and a modulo is
 * exact enough and cannot be got wrong by inspection.
 */
[[nodiscard]] std::int64_t Jitter(std::int64_t _magnitude, Neuron::Pcg32& _rng) noexcept
{
  if (_magnitude <= 0)
  {
    return 0;
  }
  const std::uint64_t draw = (static_cast<std::uint64_t>(_rng.Next()) << 32) | _rng.Next();
  const std::uint64_t span = 2ull * static_cast<std::uint64_t>(_magnitude) + 1ull;
  return static_cast<std::int64_t>(draw % span) - _magnitude;
}

[[nodiscard]] std::int64_t MetresToCm(std::int64_t _metres) noexcept
{
  return _metres * 100;
}

} // namespace

bool GenerateUniverse(const UniverseGenConfig& _config, UniverseDef& _outUniverse)
{
  const std::uint32_t constellationCount =
      static_cast<std::uint32_t>(_config.regionCount) * static_cast<std::uint32_t>(_config.constellationsPerRegion);
  if (_config.regionCount == 0 || _config.constellationsPerRegion == 0 || constellationCount > _config.systemCount)
  {
    return false; // Not content the caller can fix -- a recipe that makes no sense.
  }

  _outUniverse = UniverseDef{};
  _outUniverse.name = _config.name;

  Neuron::Pcg32 rng(_config.seed);

  // Names first, so every later step draws from a settled vocabulary rather
  // than advancing the same stream at a distance.
  const std::uint32_t rootCount = static_cast<std::uint32_t>(ROOT_PREFIX.size() * ROOT_SUFFIX.size());
  const std::vector<std::uint32_t> rootOrder = ShuffledIndices(rootCount, rng);

  // ---- Regions -----------------------------------------------------------
  //
  // On a lattice, because the map's coarsest pinch level wants regions that
  // tile legibly rather than a spray that happens to average out. The security
  // band pattern is fixed rather than random: a universe whose safe space moved
  // with the seed would make the starter systems a lottery.
  const std::uint32_t regionColumns = [&]
  {
    std::uint32_t columns = 1;
    while (columns * columns < _config.regionCount)
    {
      ++columns;
    }
    return columns;
  }();

  std::vector<UniversePos> regionCentre(_config.regionCount);
  _outUniverse.regions.reserve(_config.regionCount);
  for (std::uint16_t index = 0; index < _config.regionCount; ++index)
  {
    const std::int64_t column = index % regionColumns;
    const std::int64_t row = index / regionColumns;
    regionCentre[index] = UniversePos{(column - (regionColumns - 1) / 2) * REGION_PITCH_METRES,
                                      (row - (regionColumns - 1) / 2) * REGION_PITCH_METRES};

    Region region;
    region.id = static_cast<RegionId>(index + 1);
    region.name = RootWord(rootOrder[index], false) + " " + REGION_FORM[index % REGION_FORM.size()];

    // Three bands, in a fixed proportion: a fifth policed, a third lawless, the
    // rest contested. Assigned by index so the shape of the universe's safety
    // is authored even though its names are not.
    const std::uint32_t band = index % 5;
    if (band == 0)
    {
      region.securityFloor = 60;
      region.securityCeiling = 95;
    }
    else if (band == 1 || band == 2)
    {
      region.securityFloor = 25;
      region.securityCeiling = 70;
    }
    else
    {
      region.securityFloor = 0;
      region.securityCeiling = 35;
    }
    _outUniverse.regions.push_back(region);
  }

  // ---- Constellations ----------------------------------------------------
  //
  // A lattice inside each region cell, jittered by less than half the gap it
  // leaves, so `CONSTELLATION_SEPARATION_METRES` holds by construction rather
  // than by rejection -- which is what lets the invariants suite assert the
  // Voronoi property without knowing how any of this works.
  constexpr std::int64_t CONSTELLATION_PITCH = 70'000'000'000'000'000;
  constexpr std::int64_t CONSTELLATION_JITTER = 15'000'000'000'000'000;
  static_assert(CONSTELLATION_PITCH - 2 * CONSTELLATION_JITTER > CONSTELLATION_SEPARATION_METRES,
                "constellation centres must stay separated whatever the jitter draws");
  static_assert(CONSTELLATION_SEPARATION_METRES > 2 * CONSTELLATION_RADIUS_METRES,
                "a system must be nearer its own constellation's centre than any other's");

  std::vector<UniversePos> constellationCentre(constellationCount);
  _outUniverse.constellations.reserve(constellationCount);
  for (std::uint32_t index = 0; index < constellationCount; ++index)
  {
    const std::uint16_t regionIndex = static_cast<std::uint16_t>(index / _config.constellationsPerRegion);
    const std::uint32_t slot = index % _config.constellationsPerRegion;
    const std::uint32_t columns = 3;
    const std::int64_t column = slot % columns;
    const std::int64_t row = slot / columns;

    constellationCentre[index] =
        UniversePos{regionCentre[regionIndex].x + (column - 1) * CONSTELLATION_PITCH + Jitter(CONSTELLATION_JITTER, rng),
                    regionCentre[regionIndex].y + (row - 1) * CONSTELLATION_PITCH + Jitter(CONSTELLATION_JITTER, rng)};

    Constellation constellation;
    constellation.id = static_cast<ConstellationId>(index + 1);
    constellation.region = static_cast<RegionId>(regionIndex + 1);
    constellation.name =
        RootWord(rootOrder[(_config.regionCount + index) % rootCount], false) + " " + CONSTELLATION_FORM[index % CONSTELLATION_FORM.size()];
    _outUniverse.constellations.push_back(constellation);
  }

  // ---- Systems -----------------------------------------------------------
  //
  // Lattice slots inside the constellation's disc, shuffled and drawn from, for
  // the same reason the constellations tile: separation that holds by
  // construction beats separation that holds because the retry loop was lucky.
  constexpr std::int64_t SYSTEM_PITCH = 3'000'000'000'000'000;
  constexpr std::int64_t SYSTEM_JITTER = 400'000'000'000'000;
  static_assert(SYSTEM_PITCH - 2 * SYSTEM_JITTER > SYSTEM_SEPARATION_METRES, "system separation must survive the jitter");

  std::vector<UniversePos> latticeSlot;
  {
    const std::int64_t reach = CONSTELLATION_RADIUS_METRES / SYSTEM_PITCH;
    for (std::int64_t row = -reach; row <= reach; ++row)
    {
      for (std::int64_t column = -reach; column <= reach; ++column)
      {
        const UniversePos slot{column * SYSTEM_PITCH, row * SYSTEM_PITCH};
        const std::int64_t discRadius = (CONSTELLATION_RADIUS_METRES - SYSTEM_JITTER) >> SHIFT_CONSTELLATION;
        if (DistanceSquared(slot, UniversePos{}, SHIFT_CONSTELLATION) <= discRadius * discRadius)
        {
          latticeSlot.push_back(slot);
        }
      }
    }
  }

  const std::uint32_t basePerConstellation = _config.systemCount / constellationCount;
  const std::uint32_t remainder = _config.systemCount % constellationCount;
  if (basePerConstellation + 1 > latticeSlot.size())
  {
    return false; // The disc cannot hold what the config asks of it.
  }

  _outUniverse.systems.reserve(_config.systemCount);
  std::vector<std::uint32_t> systemConstellation;
  systemConstellation.reserve(_config.systemCount);

  for (std::uint32_t constellation = 0; constellation < constellationCount; ++constellation)
  {
    const std::uint32_t here = basePerConstellation + (constellation < remainder ? 1u : 0u);
    const std::vector<std::uint32_t> slotOrder = ShuffledIndices(static_cast<std::uint32_t>(latticeSlot.size()), rng);
    const std::string root = RootWord(rootOrder[(_config.regionCount + constellation) % rootCount], true);

    for (std::uint32_t member = 0; member < here; ++member)
    {
      SolarSystem system;
      system.id = static_cast<SystemId>(_outUniverse.systems.size() + 1);
      system.region = _outUniverse.constellations[constellation].region;
      system.constellation = static_cast<ConstellationId>(constellation + 1);
      system.name = root + "-" + std::to_string(member + 1);

      const UniversePos& slot = latticeSlot[slotOrder[member]];
      system.centre = UniversePos{constellationCentre[constellation].x + slot.x + Jitter(SYSTEM_JITTER, rng),
                                  constellationCentre[constellation].y + slot.y + Jitter(SYSTEM_JITTER, rng)};

      const Region& region = _outUniverse.regions[_outUniverse.constellations[constellation].region - 1];
      const std::uint32_t spread = static_cast<std::uint32_t>(region.securityCeiling - region.securityFloor) + 1;
      system.security = static_cast<std::uint8_t>(region.securityFloor + rng.NextBelow(spread));

      _outUniverse.systems.push_back(std::move(system));
      systemConstellation.push_back(constellation);
    }
  }

  // ---- Celestials and stations ------------------------------------------
  //
  // Real orbital scales (ADR-009 §3): the star at the system centre, planets
  // on a Titius-Bode-ish progression from ~0.35 AU out, stations holding the
  // Anchorage's standoff over a planet. In-system geometry is the half of the
  // universe that is not laid out for legibility, so it is laid out for truth.
  for (std::uint32_t index = 0; index < _outUniverse.systems.size(); ++index)
  {
    SolarSystem& system = _outUniverse.systems[index];
    const std::string root = RootWord(rootOrder[(_config.regionCount + systemConstellation[index]) % rootCount], false);

    Celestial star;
    star.id = 1;
    star.kind = CelestialKind::Star;
    star.name = root;
    star.position = system.centre;
    star.radiusMetres = 400'000'000 + static_cast<std::int64_t>(rng.NextBelow(800'000'000));
    system.celestials.push_back(std::move(star));

    const std::uint32_t planetCount = 2 + rng.NextBelow(7); // 2..8
    std::int64_t orbit = (AU_METRES * 35) / 100;
    for (std::uint32_t planet = 0; planet < planetCount; ++planet)
    {
      Celestial body;
      body.id = static_cast<CelestialId>(planet + 2);
      body.kind = CelestialKind::Planet;
      // Distinct inside a system by construction: 31 is coprime with the 320
      // roots, so eight consecutive planets cannot collide.
      body.name = RootWord(static_cast<std::uint32_t>((index * 7 + planet * 31) % rootCount), false);
      body.position = Offset(system.centre, PolarOffset(orbit, rng.NextBelow(ANGLE_STEPS)));
      body.radiusMetres = 2'400'000 + static_cast<std::int64_t>(rng.NextBelow(68'000'000));
      system.celestials.push_back(std::move(body));

      orbit = (orbit * static_cast<std::int64_t>(150 + rng.NextBelow(80))) / 100;
      orbit = std::min(orbit, AU_METRES * 30);
    }

    // One or two stations, each over a *different* planet, so a two-station
    // system reads as two places rather than one place with two names.
    const std::uint32_t stationCount = planetCount >= 2 && rng.NextBelow(100) < 35 ? 2u : 1u;
    for (std::uint32_t which = 0; which < stationCount; ++which)
    {
      const std::uint32_t hostPlanet = 1 + ((rng.NextBelow(planetCount) + which) % planetCount);
      Station station;
      station.id = static_cast<StationId>(which + 1);
      station.name = system.name + (which == 0 ? " Anchorage" : " Waypoint");
      station.position = Offset(system.celestials[hostPlanet].position, PolarOffset(STATION_STANDOFF_METRES, rng.NextBelow(ANGLE_STEPS)));
      system.stations.push_back(std::move(station));
    }
  }

  // ---- Vesta-3, the curated insert --------------------------------------
  //
  // Hand-authored content keeps its geometry and gets a home: the galaxy grows
  // around Vesta-3 rather than over it. Offsets are the authored file's own,
  // re-expressed relative to the system centre so the insert can sit wherever
  // the layout put system 1.
  {
    SolarSystem& start = _outUniverse.systems[0];
    start.name = "Vesta-3";
    start.security = 82;
    start.celestials.clear();
    start.stations.clear();

    start.celestials.push_back(Celestial{1, CelestialKind::Star, "Vesta", start.centre, 696'000'000});
    start.celestials.push_back(
        Celestial{2, CelestialKind::Planet, "Kessler", Offset(start.centre, UniversePos{107'500'000'000, 0}), 6'051'000});
    start.celestials.push_back(
        Celestial{3, CelestialKind::Planet, "Halgren", Offset(start.centre, UniversePos{-96'000'000'000, 114'600'000'000}), 6'371'000});
    start.stations.push_back(
        Station{1, "Vesta-3 Anchorage", Offset(start.centre, UniversePos{107'500'000'000, -STATION_STANDOFF_METRES})});
  }

  // ---- Gates -------------------------------------------------------------
  //
  // A spanning tree first, so connectivity is a property of the construction
  // and not of the tuning: every system links to its nearest *earlier* system,
  // which reaches system 1 by induction. Extra edges then lift the average
  // degree toward ADR-016's ~2.4 without letting any system past four.
  const std::uint32_t systemCount = static_cast<std::uint32_t>(_outUniverse.systems.size());
  std::vector<std::uint8_t> degree(systemCount, 0);
  std::vector<std::pair<std::uint32_t, std::uint32_t>> edges;
  const auto linked = [&edges](std::uint32_t _a, std::uint32_t _b)
  {
    return std::find(edges.begin(), edges.end(), std::pair<std::uint32_t, std::uint32_t>{std::min(_a, _b), std::max(_a, _b)}) != edges.end();
  };
  const auto link = [&](std::uint32_t _a, std::uint32_t _b)
  {
    edges.emplace_back(std::min(_a, _b), std::max(_a, _b));
    ++degree[_a];
    ++degree[_b];
  };

  for (std::uint32_t index = 1; index < systemCount; ++index)
  {
    std::uint32_t nearest = 0;
    std::int64_t nearestDistance = -1;
    for (std::uint32_t other = 0; other < index; ++other)
    {
      if (degree[other] >= MAX_GATES_PER_SYSTEM)
      {
        continue;
      }
      const std::int64_t distance = DistanceSquared(_outUniverse.systems[index].centre, _outUniverse.systems[other].centre, SHIFT_UNIVERSE);
      if (nearestDistance < 0 || distance < nearestDistance)
      {
        nearest = other;
        nearestDistance = distance;
      }
    }
    if (nearestDistance < 0)
    {
      // Every earlier system is full. Fall back to the nearest regardless, and
      // let the degree cap be the thing that bends rather than connectivity --
      // a disconnected universe is unplayable, a five-gate system is only ugly.
      for (std::uint32_t other = 0; other < index; ++other)
      {
        const std::int64_t distance = DistanceSquared(_outUniverse.systems[index].centre, _outUniverse.systems[other].centre, SHIFT_UNIVERSE);
        if (nearestDistance < 0 || distance < nearestDistance)
        {
          nearest = other;
          nearestDistance = distance;
        }
      }
    }
    link(index, nearest);
  }

  // Extra edges toward the target average, nearest pairs first within a
  // constellation so the shortcuts read as local rather than as wormholes.
  const std::uint32_t targetEdges = (systemCount * 24) / 10 / 2;
  for (std::uint32_t index = 0; index < systemCount && edges.size() < targetEdges; ++index)
  {
    if (degree[index] >= MAX_GATES_PER_SYSTEM)
    {
      continue;
    }
    std::uint32_t best = index;
    std::int64_t bestDistance = -1;
    for (std::uint32_t other = 0; other < systemCount; ++other)
    {
      if (other == index || degree[other] >= MAX_GATES_PER_SYSTEM || linked(index, other))
      {
        continue;
      }
      const std::int64_t distance = DistanceSquared(_outUniverse.systems[index].centre, _outUniverse.systems[other].centre, SHIFT_UNIVERSE);
      if (bestDistance < 0 || distance < bestDistance)
      {
        best = other;
        bestDistance = distance;
      }
    }
    if (bestDistance >= 0)
    {
      link(index, best);
    }
  }

  // Gates are symmetric pairs: one record in each system, each naming the
  // other, each sitting on the bearing of the system it leads to.
  for (const auto& edge : edges)
  {
    SolarSystem& from = _outUniverse.systems[edge.first];
    SolarSystem& to = _outUniverse.systems[edge.second];

    const auto place = [&](SolarSystem& _here, const SolarSystem& _there)
    {
      Gate gate;
      gate.id = static_cast<GateId>(_here.gates.size() + 1);
      gate.toSystem = _there.id;
      gate.name = _here.name + " Gate " + std::to_string(gate.id);
      // On the bearing of the far system, at the authored gate standoff, so a
      // system's gates fan out the way the map draws them.
      const std::int64_t dx = _there.centre.x - _here.centre.x;
      const std::int64_t dy = _there.centre.y - _here.centre.y;
      std::uint32_t step = 0;
      std::int64_t bestDot = 0;
      for (std::uint32_t candidate = 0; candidate < ANGLE_STEPS; ++candidate)
      {
        // Integer bearing: pick the table step whose direction agrees most with
        // (dx, dy). Scaled down first so the dot product cannot overflow.
        const std::int64_t dot = (dx >> 20) * (CosStep(candidate) >> 10) + (dy >> 20) * (SinStep(candidate) >> 10);
        if (candidate == 0 || dot > bestDot)
        {
          bestDot = dot;
          step = candidate;
        }
      }
      gate.position = Offset(_here.centre, PolarOffset(GATE_STANDOFF_METRES, step));
      _here.gates.push_back(std::move(gate));
    };
    place(from, to);
    place(to, from);
  }

  // ---- Anchors -----------------------------------------------------------
  //
  // The only warp destinations there are (ADR-016 §3). A station's anchor
  // doubles as its planet's, so the busy place stays one place; a planet with
  // no station gets a bare anchor; every gate gets one inside its own jump
  // radius so route hops chain without a crawl.
  AnchorId nextAnchorId = 1;
  for (SolarSystem& system : _outUniverse.systems)
  {
    std::vector<bool> planetHasStation(system.celestials.size(), false);
    for (const Station& station : system.stations)
    {
      // The planet this station orbits is the nearest celestial to it.
      std::uint32_t host = 0;
      std::int64_t hostDistance = -1;
      for (std::uint32_t index = 1; index < system.celestials.size(); ++index)
      {
        const std::int64_t distance = DistanceSquared(station.position, system.celestials[index].position, SHIFT_SYSTEM);
        if (hostDistance < 0 || distance < hostDistance)
        {
          host = index;
          hostDistance = distance;
        }
      }
      if (hostDistance >= 0)
      {
        planetHasStation[host] = true;
      }

      Anchor anchor;
      anchor.id = nextAnchorId++;
      anchor.kind = AnchorKind::Station;
      anchor.system = system.id;
      anchor.owner = station.id;
      anchor.origin = station.position;

      // Warp in at a standoff the dock rule already accepts, facing the
      // structure; undock outward from the same bearing. Both are local
      // offsets in centimetres, both far inside the grid.
      const std::uint32_t bearing = (static_cast<std::uint32_t>(system.id) * 37u + station.id * 11u) % ANGLE_STEPS;
      const UniversePos warpIn = PolarOffset(MetresToCm(STATION_WARP_IN_STANDOFF_METRES), bearing);
      anchor.warpInPoint = LocalOffsetCm{static_cast<std::int32_t>(warpIn.x), static_cast<std::int32_t>(warpIn.y)};
      anchor.warpInFacingTurns16 = static_cast<std::uint16_t>(((bearing + ANGLE_STEPS / 2) % ANGLE_STEPS) * (65536u / ANGLE_STEPS));
      anchor.arrivalSpreadRadiusCm = static_cast<std::int32_t>(MetresToCm(ARRIVAL_SPREAD_RADIUS_METRES));

      const UniversePos undock = PolarOffset(MetresToCm(STATION_UNDOCK_STANDOFF_METRES), bearing);
      anchor.undockPoint = LocalOffsetCm{static_cast<std::int32_t>(undock.x), static_cast<std::int32_t>(undock.y)};
      anchor.undockFacingTurns16 = static_cast<std::uint16_t>(bearing * (65536u / ANGLE_STEPS));

      anchor.occupantIdBase = static_cast<std::uint32_t>(anchor.id) * ANCHOR_ID_BLOCK;
      anchor.occupantCount = 1; // The structure itself.
      system.anchors.push_back(anchor);
    }

    for (std::uint32_t index = 1; index < system.celestials.size(); ++index)
    {
      if (planetHasStation[index])
      {
        continue; // The station's anchor is this planet's anchor.
      }
      Anchor anchor;
      anchor.id = nextAnchorId++;
      anchor.kind = AnchorKind::Planet;
      anchor.system = system.id;
      anchor.owner = system.celestials[index].id;
      anchor.origin = system.celestials[index].position;
      const std::uint32_t bearing = (static_cast<std::uint32_t>(system.id) * 53u + index * 17u) % ANGLE_STEPS;
      const UniversePos warpIn = PolarOffset(MetresToCm(BARE_WARP_IN_STANDOFF_METRES), bearing);
      anchor.warpInPoint = LocalOffsetCm{static_cast<std::int32_t>(warpIn.x), static_cast<std::int32_t>(warpIn.y)};
      anchor.warpInFacingTurns16 = static_cast<std::uint16_t>(((bearing + ANGLE_STEPS / 2) % ANGLE_STEPS) * (65536u / ANGLE_STEPS));
      anchor.arrivalSpreadRadiusCm = static_cast<std::int32_t>(MetresToCm(ARRIVAL_SPREAD_RADIUS_METRES));
      anchor.occupantIdBase = static_cast<std::uint32_t>(anchor.id) * ANCHOR_ID_BLOCK;
      anchor.occupantCount = 0;
      system.anchors.push_back(anchor);
    }

    for (const Gate& gate : system.gates)
    {
      Anchor anchor;
      anchor.id = nextAnchorId++;
      anchor.kind = AnchorKind::Gate;
      anchor.system = system.id;
      anchor.owner = gate.id;
      anchor.origin = gate.position;
      const std::uint32_t bearing = (static_cast<std::uint32_t>(system.id) * 71u + gate.id * 23u) % ANGLE_STEPS;
      const UniversePos warpIn = PolarOffset(MetresToCm(GATE_WARP_IN_STANDOFF_METRES), bearing);
      anchor.warpInPoint = LocalOffsetCm{static_cast<std::int32_t>(warpIn.x), static_cast<std::int32_t>(warpIn.y)};
      anchor.warpInFacingTurns16 = static_cast<std::uint16_t>(((bearing + ANGLE_STEPS / 2) % ANGLE_STEPS) * (65536u / ANGLE_STEPS));
      anchor.arrivalSpreadRadiusCm = static_cast<std::int32_t>(MetresToCm(ARRIVAL_SPREAD_RADIUS_METRES));
      anchor.occupantIdBase = static_cast<std::uint32_t>(anchor.id) * ANCHOR_ID_BLOCK;
      anchor.occupantCount = 0; // The gate entity is U4's; its id block is reserved now.
      system.anchors.push_back(anchor);
    }
  }

  // ---- Starters and the start -------------------------------------------
  //
  // Vesta-3 always, plus the safest systems the layout produced, so a new
  // commander's options are authored rather than drawn.
  _outUniverse.systems[0].starter = true;
  std::uint32_t designated = 1;
  for (std::uint32_t index = 1; index < systemCount && designated < _config.starterSystemCount; ++index)
  {
    SolarSystem& system = _outUniverse.systems[index];
    if (system.security >= 80 && !system.stations.empty())
    {
      system.starter = true;
      ++designated;
    }
  }

  _outUniverse.start.system = _outUniverse.systems[0].id;
  _outUniverse.start.station = _outUniverse.systems[0].stations[0].id;
  return true;
}

bool WriteUniverseJson(const UniverseDef& _universe, std::string& _outJson)
{
  // The banner is a comment, which the parser tolerates (ADR-012) and which
  // matters more here than in a hand-authored file: this one is generated, and
  // the first thing anyone opening six megabytes of JSON needs to know is that
  // editing it is pointless.
  _outJson =
      "// Outpost: Frontier -- the baked universe (ADR-016 §2, build order U1).\n"
      "//\n"
      "// GENERATED. Produced by GenerateUniverse() and committed as the authored\n"
      "// universe: this file is the content, and the generator is how it was made.\n"
      "// Edit the recipe (UniverseGenConfig) and re-bake; edits here are lost and,\n"
      "// worse, silent -- the content hash refuses a client whose file differs, so a\n"
      "// hand edit is a fail-closed handshake rather than a change.\n"
      "//\n"
      "// Every position is an EXACT INTEGER METRE. Ids are authored, never assigned\n"
      "// at load; id 0 means nothing.\n";

  std::string body;
  Neuron::JsonWriter writer(body);
  const auto position = [&writer](std::string_view _key, const UniversePos& _value)
  {
    writer.Key(_key);
    writer.BeginObject();
    writer.Member("x", _value.x);
    writer.Member("y", _value.y);
    writer.EndObject();
  };
  const auto offset = [&writer](std::string_view _key, const LocalOffsetCm& _value)
  {
    writer.Key(_key);
    writer.BeginObject();
    writer.Member("xCm", static_cast<std::int64_t>(_value.x));
    writer.Member("yCm", static_cast<std::int64_t>(_value.y));
    writer.EndObject();
  };

  writer.BeginObject();
  writer.Member("name", _universe.name);

  writer.Key("start");
  writer.BeginObject();
  writer.Member("system", static_cast<std::int64_t>(_universe.start.system));
  writer.Member("station", static_cast<std::int64_t>(_universe.start.station));
  writer.EndObject();

  writer.Key("regions");
  writer.BeginArray();
  for (const Region& region : _universe.regions)
  {
    writer.BeginObject();
    writer.Member("id", static_cast<std::int64_t>(region.id));
    writer.Member("name", region.name);
    writer.Member("securityFloor", static_cast<std::int64_t>(region.securityFloor));
    writer.Member("securityCeiling", static_cast<std::int64_t>(region.securityCeiling));
    writer.EndObject();
  }
  writer.EndArray();

  writer.Key("constellations");
  writer.BeginArray();
  for (const Constellation& constellation : _universe.constellations)
  {
    writer.BeginObject();
    writer.Member("id", static_cast<std::int64_t>(constellation.id));
    writer.Member("region", static_cast<std::int64_t>(constellation.region));
    writer.Member("name", constellation.name);
    writer.EndObject();
  }
  writer.EndArray();

  writer.Key("systems");
  writer.BeginArray();
  for (const SolarSystem& system : _universe.systems)
  {
    writer.BeginObject();
    writer.Member("id", static_cast<std::int64_t>(system.id));
    writer.Member("region", static_cast<std::int64_t>(system.region));
    writer.Member("constellation", static_cast<std::int64_t>(system.constellation));
    writer.Member("name", system.name);
    writer.Member("security", static_cast<std::int64_t>(system.security));
    if (system.starter)
    {
      writer.Member("starter", true);
    }
    position("centre", system.centre);

    writer.Key("celestials");
    writer.BeginArray();
    for (const Celestial& celestial : system.celestials)
    {
      writer.BeginObject();
      writer.Member("id", static_cast<std::int64_t>(celestial.id));
      writer.Member("kind", celestial.kind == CelestialKind::Star ? "star" : (celestial.kind == CelestialKind::Planet ? "planet" : "moon"));
      writer.Member("name", celestial.name);
      position("position", celestial.position);
      writer.Member("radiusMetres", celestial.radiusMetres);
      writer.EndObject();
    }
    writer.EndArray();

    if (!system.stations.empty())
    {
      writer.Key("stations");
      writer.BeginArray();
      for (const Station& station : system.stations)
      {
        writer.BeginObject();
        writer.Member("id", static_cast<std::int64_t>(station.id));
        writer.Member("name", station.name);
        position("position", station.position);
        writer.EndObject();
      }
      writer.EndArray();
    }

    if (!system.gates.empty())
    {
      writer.Key("gates");
      writer.BeginArray();
      for (const Gate& gate : system.gates)
      {
        writer.BeginObject();
        writer.Member("id", static_cast<std::int64_t>(gate.id));
        writer.Member("to", static_cast<std::int64_t>(gate.toSystem));
        writer.Member("name", gate.name);
        position("position", gate.position);
        writer.EndObject();
      }
      writer.EndArray();
    }

    writer.Key("anchors");
    writer.BeginArray();
    for (const Anchor& anchor : system.anchors)
    {
      writer.BeginObject();
      writer.Member("id", static_cast<std::int64_t>(anchor.id));
      writer.Member("kind", anchor.kind == AnchorKind::Station ? "station" : (anchor.kind == AnchorKind::Planet ? "planet" : "gate"));
      writer.Member("owner", static_cast<std::int64_t>(anchor.owner));
      position("origin", anchor.origin);
      offset("warpIn", anchor.warpInPoint);
      writer.Member("warpInFacingTurns16", static_cast<std::int64_t>(anchor.warpInFacingTurns16));
      writer.Member("arrivalSpreadRadiusCm", static_cast<std::int64_t>(anchor.arrivalSpreadRadiusCm));
      if (anchor.kind == AnchorKind::Station)
      {
        offset("undock", anchor.undockPoint);
        writer.Member("undockFacingTurns16", static_cast<std::int64_t>(anchor.undockFacingTurns16));
      }
      writer.Member("occupantIdBase", static_cast<std::int64_t>(anchor.occupantIdBase));
      writer.Member("occupantCount", static_cast<std::int64_t>(anchor.occupantCount));
      writer.EndObject();
    }
    writer.EndArray();

    writer.EndObject();
  }
  writer.EndArray();
  writer.EndObject();

  if (!writer.Complete())
  {
    return false; // Half-valid JSON must never reach disk.
  }
  _outJson += body;
  return true;
}

} // namespace Game
