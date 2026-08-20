#include "pch.h"

#include "UniverseGen.h"

#include "FixedAngle.h"
#include "ShipClass.h"
#include "SiteEpoch.h"

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

/// Which band a system's baked security sits in (ADR-024 §3c). The thresholds
/// are content, so the generator asks the file rather than carrying a copy.
[[nodiscard]] std::uint8_t BandIndex(const SiteDistributionInfo& _distribution, std::uint8_t _security) noexcept
{
  if (_security >= _distribution.highSecurityFloor)
  {
    return static_cast<std::uint8_t>(SecurityBand::High);
  }
  if (_security >= _distribution.lowSecurityFloor)
  {
    return static_cast<std::uint8_t>(SecurityBand::Low);
  }
  return static_cast<std::uint8_t>(SecurityBand::Null);
}

/// One draw against a weight table. All-zero weights pick the first entry
/// rather than dividing by nothing -- a weightless table is a content mistake
/// the invariants catch, and it must not be an out-of-range read here first.
[[nodiscard]] std::uint32_t PickWeighted(const std::uint8_t* _weights, std::uint32_t _count, Neuron::Pcg32& _rng) noexcept
{
  std::uint32_t total = 0;
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    total += _weights[index];
  }
  if (total == 0)
  {
    return 0;
  }
  std::uint32_t roll = _rng.NextBelow(total);
  for (std::uint32_t index = 0; index < _count; ++index)
  {
    if (roll < _weights[index])
    {
      return index;
    }
    roll -= _weights[index];
  }
  return _count - 1;
}

/*
 * The grade a site of this archetype may actually reach in this band.
 *
 * **The archetype's cap beats the band's range**, and that precedence is the
 * resolution of a conflict the content states twice: ADR-024 ruling 1b makes
 * High-Sec nebula pockets *faded* -- grade I, thin yields -- while §3c asks
 * every region to cover all three ores at grade >= `minRegionOreGrade` (II).
 * In High-Sec only a pocket carries Nebulite at all, so both cannot hold. The
 * specific rule wins over the general one: the pocket stays faded, and the
 * coverage floor yields to the cap where a cap exists.
 */
[[nodiscard]] std::uint8_t EffectiveGradeCap(const SiteArchetypeInfo& _archetype, std::uint8_t _band, std::uint8_t _gradeMax) noexcept
{
  const std::uint8_t cap = _archetype.gradeCapByBand[_band];
  return cap == 0 ? _gradeMax : std::min(cap, _gradeMax);
}

/// The content's own spelling of an anchor kind. A function rather than the
/// nested ternary this replaced, because a fourth kind made that unreadable and
/// a fifth would make it wrong.
[[nodiscard]] const char* AnchorKindName(AnchorKind _kind) noexcept
{
  switch (_kind)
  {
  case AnchorKind::Station:
    return "station";
  case AnchorKind::Planet:
    return "planet";
  case AnchorKind::Gate:
    return "gate";
  case AnchorKind::Site:
    return "site";
  }
  return "planet";
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

bool GenerateUniverse(const UniverseGenConfig& _config, const SitesInfo& _sites, UniverseDef& _outUniverse)
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
    constellation.centre = constellationCentre[index];
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
  //
  // Orbit radii are kept as they are drawn, because the site pass below rides
  // its ring *between* two planet orbits and would otherwise have to recover a
  // radius from a position -- which at 30 AU means an integer square root and a
  // shift chosen so 4.5e12 squared does not wrap. Remembering the number costs
  // a vector and cannot be got wrong.
  std::vector<std::vector<std::int64_t>> orbitsBySystem(_outUniverse.systems.size());
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
      orbitsBySystem[index].push_back(orbit);

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
  std::uint32_t nextOccupantIdBase = AUTHORED_SHIP_ID_BASE;
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

      anchor.occupantCount = 1; // The structure itself.
      anchor.occupantIdBase = nextOccupantIdBase;
      nextOccupantIdBase += ANCHOR_ID_BLOCK;
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
      anchor.occupantIdBase = 0; // Authors nothing, so it holds no block.
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
      // The gate entity, which is U4's (ADR-016 §10). Exactly one and never
      // more: a gate is one structure, and the block's spare id exists so the
      // next anchor's ids cannot be run into rather than so this one can grow.
      anchor.occupantCount = 1;
      anchor.occupantIdBase = nextOccupantIdBase;
      nextOccupantIdBase += ANCHOR_ID_BLOCK;
      system.anchors.push_back(anchor);
    }
  }

  /*
   * The id space, checked rather than assumed (ADR-018 D6a).
   *
   * `ANCHOR_ID_BLOCK`'s comment states the worst case a recipe at this scale
   * can ask for and shows it fitting; this is that claim made mechanical, so a
   * recipe that asked for more would fail the bake instead of wrapping two
   * anchors onto the same occupant ids. A collision there would not surface as
   * a bad file -- it would surface, much later, as two grids disagreeing about
   * which ship a number means.
   */
  if (nextOccupantIdBase > DYNAMIC_SHIP_ID_BASE)
  {
    return false;
  }

  // ---- Sites ---------------------------------------------------------------
  //
  // Mining fields (ADR-024 §3a, build order E1b). `AnchorKind::Site` was
  // reserved for exactly this, and cashing in a reserved id renumbers nothing.
  //
  // **Two decisions keep this slice additive**, which is what makes a 15 MB
  // regenerated file reviewable at all. Sites are appended *after* every other
  // anchor already has its id, so no station, planet or gate anchor moves and
  // no occupant block shifts -- the same argument ADR-016 §10 used for
  // appending `HullClass::Gate`. And every roll below comes from a **per-system
  // stream of its own**, so the main sequence is untouched and not one existing
  // byte of name, position, security or gate topology changes.
  //
  // A site authors no occupants, so it takes no id block: rocks are not
  // entities (ADR-024 §4c), which is the property that lets ~6,250 anchors join
  // without going near the window U4 measured into refusal.
  //
  // **A recipe given no site content bakes no sites**, rather than baking
  // degenerate ones. That is what makes `SitesInfo{}` a usable argument: a
  // suite testing gate topology or naming has no business carrying an economy,
  // and a caller that forgets one gets a universe without mining fields instead
  // of a grade-zero read off the end of the grade table.
  const bool haveSiteContent =
      std::any_of(std::begin(_sites.grades), std::end(_sites.grades), [](const SiteGradeInfo& _g) { return _g.poolUnits > 0; });
  if (haveSiteContent)
  {
    const SiteDistributionInfo& distribution = _sites.distribution;
    const std::uint32_t fieldRadiusSpread = _sites.fieldRadiusMaxMetres >= _sites.fieldRadiusMinMetres
                                                ? _sites.fieldRadiusMaxMetres - _sites.fieldRadiusMinMetres + 1
                                                : 1;

    for (std::uint32_t index = 0; index < _outUniverse.systems.size(); ++index)
    {
      SolarSystem& system = _outUniverse.systems[index];
      const std::vector<std::int64_t>& orbits = orbitsBySystem[index];
      if (orbits.empty())
      {
        continue; // A system with no planets has no disc to put a field on.
      }

      // The site stream. Seeded from the recipe and the system id so it is a
      // property of *which* system this is rather than of when it was reached,
      // and so re-running the bake with sites off would leave the rest bit-
      // identical.
      Neuron::Pcg32 siteRng{_config.seed ^ (0x9E3779B97F4A7C15ull * (static_cast<std::uint64_t>(system.id) + 1))};

      const std::uint8_t band = BandIndex(distribution, system.security);
      const std::uint32_t siteCount = 2 + PickWeighted(distribution.siteCountWeights[band], 2, siteRng);

      std::uint8_t archetypeUsed[SITE_ARCHETYPE_COUNT] = {};
      const std::uint8_t sameCap = distribution.maxSameArchetypePerSystem[band] == 0
                                       ? static_cast<std::uint8_t>(siteCount)
                                       : distribution.maxSameArchetypePerSystem[band];

      for (std::uint32_t slot = 0; slot < siteCount; ++slot)
      {
        // The new-player floor: a High-Sec system's first field is always the
        // one with no hazard on it (ADR-024 §3c).
        std::uint8_t archetype = static_cast<std::uint8_t>(distribution.highSecFirstSiteArchetype);
        if (band != static_cast<std::uint8_t>(SecurityBand::High) || slot != 0)
        {
          archetype = static_cast<std::uint8_t>(PickWeighted(distribution.archetypeWeights[band], SITE_ARCHETYPE_COUNT, siteRng));
          if (archetypeUsed[archetype] >= sameCap)
          {
            // Walk to the next archetype with room rather than re-rolling: a
            // re-roll loop has no bound, and the walk is deterministic.
            for (std::uint8_t step = 1; step < SITE_ARCHETYPE_COUNT; ++step)
            {
              const std::uint8_t candidate = static_cast<std::uint8_t>((archetype + step) % SITE_ARCHETYPE_COUNT);
              if (archetypeUsed[candidate] < sameCap)
              {
                archetype = candidate;
                break;
              }
            }
          }
        }
        ++archetypeUsed[archetype];

        const SiteArchetypeInfo& archetypeInfo = _sites.archetypes[archetype];
        const std::uint8_t gradeMin = distribution.gradeMin[band];
        const std::uint8_t gradeCap = EffectiveGradeCap(archetypeInfo, band, distribution.gradeMax[band]);
        const std::uint8_t gradeSpread = gradeCap > gradeMin ? static_cast<std::uint8_t>(gradeCap - gradeMin + 1) : 1;
        const std::uint8_t grade = static_cast<std::uint8_t>(std::min<std::uint32_t>(gradeCap, gradeMin + siteRng.NextBelow(gradeSpread)));

        Anchor anchor;
        anchor.id = nextAnchorId++;
        anchor.kind = AnchorKind::Site;
        anchor.system = system.id;
        // A site has no entity to belong to, so `owner` is its ordinal within
        // the system -- a per-system site id, which is what every other kind's
        // owner is too. Never zero, because the parser reads owners as ids.
        anchor.owner = static_cast<std::uint16_t>(slot + 1);

        anchor.site.archetype = static_cast<SiteArchetype>(archetype);
        anchor.site.grade = grade;

        // The ring rides between two planet orbits; beyond the last one there
        // is no gap to halve, so it steps out by a fixed fraction instead.
        const std::uint32_t inner = siteRng.NextBelow(static_cast<std::uint32_t>(orbits.size()));
        anchor.site.orbitRingRadiusMetres = inner + 1 < orbits.size()
                                                ? orbits[inner] + (orbits[inner + 1] - orbits[inner]) / 2
                                                : (orbits[inner] * SITE_RING_BEYOND_LAST_PLANET_PCT) / 100;

        anchor.site.fieldRadiusCm =
            static_cast<std::int32_t>((_sites.fieldRadiusMinMetres + siteRng.NextBelow(fieldRadiusSpread)) * 100);
        anchor.site.layoutSeed = siteRng.Next();

        // The pool, split by the archetype's composition for this band. The
        // shares are floored, so a pool can come out a unit or two under its
        // grade's total -- which is a rounding remainder and not a shortfall
        // worth a redistribution pass nobody could perceive.
        const std::uint32_t poolUnits = _sites.grades[grade - 1].poolUnits;
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          anchor.site.poolUnits[ore] = poolUnits * archetypeInfo.compositionPct[band][ore] / 100u;
        }

        // Epoch 0 is what the file carries; the runtime asks
        // `SiteEpochPlacement` for today's (ADR-024 §3d).
        const SitePlacement placement =
            SiteEpochPlacement(system.centre, anchor.id, anchor.site, 0, static_cast<std::int64_t>(_sites.warpInStandoffMetres));
        anchor.origin = placement.origin;
        anchor.warpInPoint = placement.warpInPoint;
        anchor.warpInFacingTurns16 = placement.warpInFacingTurns16;

        // The wide arrival arc -- the half of the anti-camp design that bites,
        // since under anchor-only warp an attacker reaches the field the same
        // way a miner does (ADR-024 §3a).
        anchor.arrivalSpreadRadiusCm =
            static_cast<std::int32_t>(static_cast<std::int64_t>(anchor.site.fieldRadiusCm) * _sites.arrivalSpreadPctOfField / 100);

        anchor.occupantIdBase = 0;
        anchor.occupantCount = 0;
        system.anchors.push_back(anchor);
      }
    }
  }

  // ---- The two guarantees the content states, honoured then checked --------
  //
  // `SiteDistributionInfo` carries them as data so the invariants suite can
  // check the file's own claims instead of a second copy of them. Rolling alone
  // does not deliver either: a High-Sec region can easily draw no nebula pocket
  // at a weight of ten, and ruling 1b's promise -- Tier 1 craftable in every
  // band while no market exists -- would quietly fail for the players furthest
  // from the frontier.
  if (haveSiteContent)
  {
    const SiteDistributionInfo& distribution = _sites.distribution;
    const std::uint8_t highBand = static_cast<std::uint8_t>(SecurityBand::High);

    // (a) At least one faded pocket per High-Sec region. Repaired rather than
    // re-rolled: converting one site is bounded work with a deterministic
    // choice, where re-rolling a region until it complies is a loop whose
    // length depends on luck.
    if (distribution.fadedPocketSystemsPerHighRegion > 0)
    {
      for (const Region& region : _outUniverse.regions)
      {
        std::uint32_t pockets = 0;
        Anchor* candidate = nullptr;
        for (SolarSystem& system : _outUniverse.systems)
        {
          if (system.region != region.id || BandIndex(distribution, system.security) != highBand)
          {
            continue;
          }
          for (Anchor& anchor : system.anchors)
          {
            if (anchor.kind != AnchorKind::Site)
            {
              continue;
            }
            if (anchor.site.archetype == SiteArchetype::NebulaPocket)
            {
              ++pockets;
            }
            else if (candidate == nullptr && anchor.owner != 1)
            {
              /*
               * The first convertible site in the region, in bake order -- and
               * **never a system's first site**, because in High-Sec that slot
               * is the new-player floor (§3c) and converting it would trade one
               * guarantee for the other. The bake caught exactly that: six
               * regions had their floor overwritten before this clause existed.
               */
              candidate = &anchor;
            }
          }
        }

        if (pockets >= distribution.fadedPocketSystemsPerHighRegion || candidate == nullptr)
        {
          continue;
        }

        const SiteArchetypeInfo& pocket = _sites.archetypes[static_cast<std::uint8_t>(SiteArchetype::NebulaPocket)];
        const std::uint8_t grade = EffectiveGradeCap(pocket, highBand, distribution.gradeMax[highBand]);
        candidate->site.archetype = SiteArchetype::NebulaPocket;
        candidate->site.grade = grade;
        const std::uint32_t poolUnits = _sites.grades[grade - 1].poolUnits;
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          candidate->site.poolUnits[ore] = poolUnits * pocket.compositionPct[highBand][ore] / 100u;
        }
      }
    }

    // (b) Every region covers all three ores, at the coverage floor or at the
    // archetype's cap where one applies -- `EffectiveGradeCap`'s precedence,
    // which is what makes this satisfiable in High-Sec at all.
    //
    // **Checked, not repaired.** (a) is what makes it true; if it ever stops
    // being true the bake must say so rather than ship content that contradicts
    // its own guarantee, because the player who discovers it is one who cannot
    // craft Astra-Glass anywhere near home.
    if (distribution.minRegionOreGrade > 0)
    {
      for (const Region& region : _outUniverse.regions)
      {
        bool covered[ORE_COUNT] = {};
        bool anySystem = false;
        for (const SolarSystem& system : _outUniverse.systems)
        {
          if (system.region != region.id)
          {
            continue;
          }
          anySystem = true;
          const std::uint8_t band = BandIndex(distribution, system.security);
          for (const Anchor& anchor : system.anchors)
          {
            if (anchor.kind != AnchorKind::Site)
            {
              continue;
            }
            const SiteArchetypeInfo& archetypeInfo = _sites.archetypes[static_cast<std::uint8_t>(anchor.site.archetype)];
            const std::uint8_t floor =
                std::min(distribution.minRegionOreGrade, EffectiveGradeCap(archetypeInfo, band, distribution.gradeMax[band]));
            if (anchor.site.grade < floor)
            {
              continue;
            }
            for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
            {
              covered[ore] = covered[ore] || anchor.site.poolUnits[ore] > 0;
            }
          }
        }

        if (!anySystem)
        {
          continue;
        }
        for (const bool ore : covered)
        {
          if (!ore)
          {
            return false;
          }
        }
      }
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
    position("centre", constellation.centre);
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
      writer.Member("kind", AnchorKindName(anchor.kind));
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
      if (anchor.kind == AnchorKind::Site)
      {
        // Only where it means something, the way `undock` is written only on
        // stations: a block of zeroes on 18,000 anchors is noise in a file
        // people are expected to read.
        writer.Key("site");
        writer.BeginObject();
        writer.Member("archetype", SiteArchetypeContentKey(anchor.site.archetype));
        writer.Member("grade", static_cast<std::int64_t>(anchor.site.grade));
        writer.Member("orbitRingRadiusMetres", anchor.site.orbitRingRadiusMetres);
        writer.Member("fieldRadiusCm", static_cast<std::int64_t>(anchor.site.fieldRadiusCm));
        writer.Member("layoutSeed", static_cast<std::int64_t>(anchor.site.layoutSeed));
        writer.Key("poolUnits");
        writer.BeginObject();
        for (std::uint8_t ore = 0; ore < ORE_COUNT; ++ore)
        {
          writer.Member(OreContentKey(static_cast<OreId>(ore)), static_cast<std::int64_t>(anchor.site.poolUnits[ore]));
        }
        writer.EndObject();
        writer.EndObject();
      }
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
