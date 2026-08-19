#pragma once

#include "Ids.h"
#include "Universe.h"

#include <cstdint>
#include <string>

/*
 * The bake (ADR-016 §2, Universe build order U1).
 *
 * Procedural generation authored *once* into content, not run at boot: the
 * committed `GameData/Universe/Frontier.json` is the universe, and this is what
 * produced it. That inversion is the whole design. A universe generated at
 * runtime would have to agree across two machines and a save file; a universe
 * generated into a file, hashed, and refused at the door when it differs
 * (ADR-004's content hash) has to agree with nothing.
 *
 * **Pure and integer-only.** No clock, no OS, no floating point anywhere in the
 * generation path, and one seeded PCG32 as the sole randomness (ADR-005 §5).
 * Integer-only is not fastidiousness: a bake run on one compiler and committed,
 * then re-run on another in CI and compared, must produce the same bytes, and
 * float would make that a coincidence. Angles come from the fixed-point table
 * in the .cpp for the same reason.
 *
 * Scale is ADR-009 §3's: **between** systems, distances are laid out for map
 * legibility; **within** a system, they are real. So a system's planets sit at
 * true orbital radii while the systems themselves are spaced for a readable
 * strategic map.
 */

namespace Game
{

/*
 * What the bake is asked for. Every field is part of the content hash's input
 * by construction -- change one and the file changes -- so this struct is
 * effectively the universe's recipe and belongs beside the file it produced.
 */
struct UniverseGenConfig
{
  std::uint64_t seed = 0x0FF5E7C0DEull;
  std::string name = "Frontier";

  std::uint16_t regionCount = 50;
  std::uint16_t constellationsPerRegion = 5;
  std::uint16_t systemCount = 2500;
  std::uint16_t starterSystemCount = 3;
};

/*
 * Layout constants, in metres on the universe plane.
 *
 * They are public because the invariants suite checks against them: a test that
 * re-derived "far enough apart" from its own constant would be checking that
 * two numbers were typed the same way twice.
 *
 * The separations are what make constellations *readable* on the strategic map
 * rather than merely labelled, and they are chosen so the clustering property
 * holds **by construction**: a system sits within `CONSTELLATION_RADIUS_METRES`
 * of its own constellation's centre, and centres are at least
 * `CONSTELLATION_SEPARATION_METRES` apart with that separation more than twice
 * the radius -- so every system is nearer its own centre than any other, which
 * is a Voronoi condition a test can assert without knowing how placement works.
 */
inline constexpr std::int64_t SYSTEM_SEPARATION_METRES = 2'000'000'000'000'000;         // ~0.21 ly
inline constexpr std::int64_t CONSTELLATION_RADIUS_METRES = 12'000'000'000'000'000;     // ~1.27 ly
inline constexpr std::int64_t CONSTELLATION_SEPARATION_METRES = 32'000'000'000'000'000; // ~3.4 ly
inline constexpr std::int64_t REGION_PITCH_METRES = 220'000'000'000'000'000;            // ~23 ly

/// One astronomical unit, exact to the metre. In-system distances are real
/// (ADR-009 §3), and this is the unit they are real in.
inline constexpr std::int64_t AU_METRES = 149'597'870'700;

/// How far a station sits off the planet it orbits -- the Anchorage standoff
/// the authored Vesta-3 content established, kept so every station in the
/// universe reads at the same scale.
inline constexpr std::int64_t STATION_STANDOFF_METRES = 8'051'000;

/// How far a gate sits from its system's star. Far enough to read as the edge
/// of the system on the map, near enough that the grid is still the system's.
inline constexpr std::int64_t GATE_STANDOFF_METRES = 900'000'000'000;

/// ADR-016 §3's "1-4 gates per system, ~2.4 average". The cap is structural
/// (the map draws them fanned) and the average is what the extra-edge pass
/// aims at.
inline constexpr std::uint8_t MAX_GATES_PER_SYSTEM = 4;

/*
 * Where a warp arrives, per anchor kind, in metres from the anchor's origin.
 *
 * The station number is the load-bearing one: it must sit inside
 * `DOCK_RADIUS_METRES` so a fleet that warps to a station is immediately able
 * to dock (the invariant U1's accept names), and far enough out that a 200 m
 * structure is not inside the formation. 3 km satisfies both with room, and
 * ADR-018 D7's footprint-derived radius only ever makes the dock rule *more*
 * permissive, so the base radius is the one the bake must clear.
 */
inline constexpr std::int64_t STATION_WARP_IN_STANDOFF_METRES = 3'000;
inline constexpr std::int64_t BARE_WARP_IN_STANDOFF_METRES = 5'000;
inline constexpr std::int64_t GATE_WARP_IN_STANDOFF_METRES = 1'200;

/// ADR-017 §3: ~800 m off the structure, facing outward, clear of the
/// `Structure` contact radius so an undocking fleet never spawns in contact.
inline constexpr std::int64_t STATION_UNDOCK_STANDOFF_METRES = 800;

/// The room ADR-018 D18's deterministic per-order arrival offsets get to spread
/// in. Baked so that answering hub contention never needs an anchor-schema
/// migration; the offset *rule* is the simulation's.
inline constexpr std::int64_t ARRIVAL_SPREAD_RADIUS_METRES = 1'200;

/*
 * Ship ids for authored occupants (ADR-018 D6a/A5).
 *
 * An anchor that authors occupants owns a block of the id space, so an
 * occupant's id is a function of where it stands rather than of when it
 * spawned -- which is what makes spin-up, teardown and recreate reproduce it
 * bit-exactly, and what stops two worlds ever colliding.
 *
 * **Blocks go only to anchors that author something, and the arithmetic is why.**
 * `ShipId` is u16 until the delta cluster widens it (D6), and the committed
 * universe has ~18,600 anchors: a block for every one of them would put
 * authored ids past 65,535 before a single ship had been built. Only station
 * anchors author anything today (~3,350 of them), and gates will when U4 gives
 * them an entity, so blocks are handed out in bake order to the anchors that
 * need them. That fits the u16 era with room and needs no re-bake when the
 * widening lands -- the field is already u32, because a baked id is not a wire
 * value.
 */
inline constexpr std::uint32_t ANCHOR_ID_BLOCK = 8;

/// Authored ids start here; dynamic ids start at `DYNAMIC_SHIP_ID_BASE`. The
/// two spaces are partitioned rather than interleaved so that "is this ship
/// content or is it something a commander built?" is answerable from the id
/// alone, which the registry's assert relies on.
inline constexpr std::uint32_t AUTHORED_SHIP_ID_BASE = 1;
inline constexpr std::uint32_t DYNAMIC_SHIP_ID_BASE = 32768;

/*
 * Generates the universe. Returns false only for a config that cannot be
 * satisfied (no regions, fewer systems than constellations), which is a
 * programming error rather than content: there is no user input here to be
 * tolerant of.
 *
 * Deterministic in the strong sense the accept asks for: the same config
 * produces the same `UniverseDef`, field for field, on any build.
 */
[[nodiscard]] bool GenerateUniverse(const UniverseGenConfig& _config, UniverseDef& _outUniverse);

/// Serialises a generated universe to the canonical JSON the game reads back.
/// Round-trips through `ParseUniverse` exactly; that is a bake invariant, not
/// an aspiration.
[[nodiscard]] bool WriteUniverseJson(const UniverseDef& _universe, std::string& _outJson);

} // namespace Game
