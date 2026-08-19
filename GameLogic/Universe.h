#pragma once

#include "Ids.h"

#include <DirectXMath.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/*
 * The universe (ADR-009).
 *
 * One plane, addressed in **exact integer metres**, with the tactical
 * simulation running in float32 on a small grid anchored somewhere on it.
 * Everything persistent is an integer: no float lives in this file, and that is
 * the decision rather than an accident. Integers are exact, they compare and
 * hash without epsilon, and a universe that drifts because a position was
 * accumulated in float is a universe whose save files stop matching.
 *
 * Pure data and pure functions. No file IO -- the hosts read bytes and hand
 * them to UniverseParse (ADR-009 §7), which keeps GameLogic OS-free.
 */

namespace Game
{

/// A position on the universe plane, in metres. Range is about +/-975 light
/// years, so growth is authoring rather than a coordinate migration.
struct UniversePos
{
  std::int64_t x = 0;
  std::int64_t y = 0;

  [[nodiscard]] friend bool operator==(const UniversePos& _a, const UniversePos& _b) noexcept
  {
    return _a.x == _b.x && _a.y == _b.y;
  }
};

/*
 * The tactical grid's origin (ADR-009 §2).
 *
 * A grid is ~40 km across so float32 keeps millimetre precision inside it
 * (ADR-001 §3). Absolute position is `anchor + local`, reconstructible without
 * loss because the local part is carried as integer centimetres -- the wire's
 * own unit (ADR-004).
 */
struct GridAnchor
{
  SystemId system = INVALID_ID;
  UniversePos origin;
};

/// Half-extent of a tactical grid, in metres (ADR-001 §3: 40 km x 40 km).
inline constexpr std::int64_t GRID_HALF_EXTENT_METRES = 20000;

/// Local offset from an anchor, in centimetres. int32 holds +/-21,474 km, so
/// the grid bound is the binding constraint rather than the type.
struct LocalOffsetCm
{
  std::int32_t x = 0;
  std::int32_t y = 0;

  [[nodiscard]] friend bool operator==(const LocalOffsetCm& _a, const LocalOffsetCm& _b) noexcept
  {
    return _a.x == _b.x && _a.y == _b.y;
  }
};

/*
 * Where a ship can warp to (ADR-016 §3).
 *
 * `Site` is reserved and unused: mining fields and PVE encounters are new
 * anchor rows rather than new architecture, and the enumerator exists now so
 * that adding one is content rather than a wire renumber.
 */
enum class AnchorKind : std::uint8_t
{
  Station = 0,
  Planet = 1,
  Gate = 2,
  Site = 3 // Reserved: no content, never baked.
};

enum class CelestialKind : std::uint8_t
{
  Star,
  Planet,
  Moon // Authorable, unused by the MVP content.
};

/// Landmarks now, economy anchors later (ADR-009 §4). Rendered as distant
/// backdrop, never flown to: inter-system space is not flown, and in-system
/// celestials are far outside the tactical grid.
struct Celestial
{
  CelestialId id = INVALID_ID;
  CelestialKind kind = CelestialKind::Planet;
  std::string name;
  UniversePos position;
  std::int64_t radiusMetres = 0;
};

/// 1-2 per system (ADR-009 §4). Drawn with the `Structure` hull -- static and
/// radially symmetric, which is also why it carries no heading.
struct Station
{
  StationId id = INVALID_ID;
  std::string name;
  UniversePos position;
};

/// An edge to another system. Traversal is post-MVP; the edge is authored now
/// so the strategic map and the save format never need a second description.
struct Gate
{
  GateId id = INVALID_ID;
  SystemId toSystem = INVALID_ID;
  std::string name;
  UniversePos position;
};

/*
 * A warp destination (ADR-016 §3), and the only kind there is.
 *
 * Ships warp **to anchors, never to coordinates**. That one rule bounds how
 * many grids a system can host, makes "where can I go?" a finite pickable list,
 * and gives future content an extension point that costs a row rather than a
 * design.
 *
 * A station's anchor doubles as its planet's: "warp to Kessler" and "warp to
 * the Anchorage" land on the same grid, because the busy place should be one
 * place.
 */
struct Anchor
{
  AnchorId id = INVALID_ID;
  AnchorKind kind = AnchorKind::Planet;
  SystemId system = INVALID_ID;

  /// The station, planet or gate this anchor belongs to. Scoped by `kind` --
  /// a `StationId` for `Station`, a `CelestialId` for `Planet`, a `GateId` for
  /// `Gate` -- because those id spaces are per-system and independent.
  std::uint16_t owner = INVALID_ID;

  /// Where the 40 km grid sits on the universe plane.
  UniversePos origin;

  /// Where a warp arrives, as a local offset from `origin`, with the facing the
  /// formation solve centres on. Never random: ADR-016 §3 makes arrival an
  /// authored property so two fleets arriving the same tick differ only by
  /// ADR-015's separation.
  LocalOffsetCm warpInPoint;
  std::uint16_t warpInFacingTurns16 = 0;

  /*
   * Contention (ADR-018 D18). One authored point per anchor would serialise
   * hub traffic, so simultaneous arrivals spread deterministically around the
   * authored bearing rather than stacking. The *rule* is the sim's (a function
   * of the transfer record, never randomness); what the bake owes is the room
   * to do it in, which is this radius -- carried from the first committed file
   * so contention never forces an anchor-schema migration.
   */
  std::int32_t arrivalSpreadRadiusCm = 0;

  /*
   * Undock (ADR-017 §3), station anchors only and zero elsewhere: where an
   * undocking fleet appears, and which way it faces. ~800 m off the structure,
   * facing outward, clear of its contact radius.
   */
  LocalOffsetCm undockPoint;
  std::uint16_t undockFacingTurns16 = 0;

  /*
   * The first ship id this anchor's authored occupants take (ADR-018 D6a/A5).
   *
   * Derived from the anchor at bake time rather than allocated at spin-up, so
   * a world torn down and recreated reproduces its occupants' ids bit-exactly
   * and no two anchors can ever collide. u32 because D6 widened ship ids; the
   * bake is free to use the whole range because it is not the wire.
   */
  std::uint32_t occupantIdBase = 0;
  std::uint16_t occupantCount = 0;
};

/*
 * A UI grouping with clustered members and clear gaps between them.
 *
 * *ADR-009 §4 deliberately left constellations out, on the grounds that a field
 * nothing reads is a field that rots.* ADR-016 read them: the strategic map's
 * middle pinch level is constellations, and U1 makes their separation a
 * measurable bake invariant rather than an aesthetic hope. They still carry no
 * mechanics -- regions scope security and, later, ownership.
 */
struct Constellation
{
  ConstellationId id = INVALID_ID;
  RegionId region = INVALID_ID;
  std::string name;

  /*
   * Where the cluster is, as the bake placed it -- not the centroid of whatever
   * systems ended up in it.
   *
   * It is carried rather than derived because the two are different numbers and
   * the difference matters twice: the map draws the constellation pinch level
   * from this, and the clustering invariant ("every system is nearer its own
   * constellation's centre than any other's") is only true against the placed
   * centre. Derived centroids drift with however many systems were drawn, which
   * is how that invariant first appeared to fail.
   */
  UniversePos centre;
};

struct SolarSystem
{
  SystemId id = INVALID_ID;
  RegionId region = INVALID_ID;
  ConstellationId constellation = INVALID_ID;
  std::string name;
  UniversePos centre;

  /// 0..100, low is lawless. Per-system, varying inside its region's band, so
  /// the map's security overlay has a gradient rather than fifty flat plates.
  std::uint8_t security = 0;

  /// True for the handful of systems a new commander may start in.
  bool starter = false;

  std::vector<Celestial> celestials;
  std::vector<Station> stations;
  std::vector<Gate> gates;
  std::vector<Anchor> anchors;
};

/// Regions scope the map's coarsest pinch level, the security band, and later
/// ownership. `securityFloor`/`securityCeiling` are the band a region's systems
/// vary inside (0..100); the map draws the band badge from them.
struct Region
{
  RegionId id = INVALID_ID;
  std::string name;
  std::uint8_t securityFloor = 0;
  std::uint8_t securityCeiling = 0;
};

/// Where a session begins. Authored rather than inferred, so adding a second
/// system does not silently move the player.
struct UniverseStart
{
  SystemId system = INVALID_ID;
  StationId station = INVALID_ID;
};

struct UniverseDef
{
  std::string name;
  std::vector<Region> regions;
  std::vector<Constellation> constellations;
  std::vector<SolarSystem> systems;
  UniverseStart start;

  [[nodiscard]] const SolarSystem* FindSystem(SystemId _id) const noexcept;
  [[nodiscard]] const Station* FindStation(SystemId _system, StationId _station) const noexcept;

  /// The anchor a warp order names, or null. Anchor ids are unique across the
  /// whole universe -- unlike celestial/station/gate ids, which are per-system
  /// -- because a warp order carries one number and nothing else.
  [[nodiscard]] const Anchor* FindAnchor(AnchorId _id) const noexcept;

  /// The grid anchor a session starts at: the start station's position
  /// (ADR-009 §9). Invalid system id when the start cannot be resolved.
  [[nodiscard]] GridAnchor StartAnchor() const noexcept;
};

/*
 * FNV-1a over the canonicalised parsed content (ADR-012 §D).
 *
 * Canonical means two things, and both are load-bearing for the handshake:
 *  - it hashes the *parsed* content, so comments, whitespace, indentation and
 *    JSON key order cannot change it;
 *  - it walks entities in **id order rather than file order**, so moving a
 *    system up the array is not a content change, while editing one is.
 *
 * Everything hashed is an integer or a string. There is no float in the
 * universe model, so there is no question about how one hashes.
 *
 * Not `noexcept`, and it used to be: walking in id order means sorting indices,
 * which allocates once per entity list, and a `noexcept` function that
 * allocates promises something it cannot keep -- `bad_alloc` there is
 * `std::terminate`, on the boot path that reads the largest file this game
 * owns (R17). The annotation was incidental rather than a design promise: the
 * one production caller (`Outpost::LoadUniverse`) already allocates and already
 * reports failures as diagnostics. Found by the clang-tidy CI step's first
 * Windows run (ADR-018 A24).
 */
[[nodiscard]] std::uint64_t ComputeUniverseHash(const UniverseDef& _universe);

/*
 * Anchor and local conversion (ADR-009 §2).
 *
 * False when the position is not on the grid the anchor describes. That refusal
 * is the "never subtract distant UniversePos values" rule made mechanical: two
 * systems are light-years apart, their difference is meaningless as a local
 * offset, and a function that quietly returned a wrapped int32 would be worse
 * than one that says no.
 */
[[nodiscard]] bool LocalFromUniverse(UniversePos _anchor, UniversePos _position, LocalOffsetCm& _outLocal) noexcept;

/*
 * Exact inverse of LocalFromUniverse over whole-metre offsets, which every
 * authored placement is. Integer throughout, so that round trip loses nothing
 * -- the property ADR-009 §2 promises.
 *
 * Sub-metre residue has no universe-level representation, and that is the
 * precision layering rather than a gap: universe placements are metres, and
 * sub-metre lives in the grid (ADR-009 §2). Ask IsWholeMetres when it matters.
 */
[[nodiscard]] UniversePos UniverseFromLocal(UniversePos _anchor, LocalOffsetCm _local) noexcept;

/// The domain over which UniverseFromLocal is an exact inverse.
[[nodiscard]] constexpr bool IsWholeMetres(LocalOffsetCm _local) noexcept
{
  return _local.x % 100 == 0 && _local.y % 100 == 0;
}

/// The same offset as the simulation holds it: local float32 metres on the
/// plane (ADR-001 §3). Lossy by construction, and only ever used for maths that
/// is already local -- never to round-trip a persistent position.
[[nodiscard]] DirectX::XMFLOAT2 LocalMetresFromUniverse(UniversePos _anchor, UniversePos _position) noexcept;

[[nodiscard]] const char* CelestialKindName(CelestialKind _kind) noexcept;
[[nodiscard]] bool ParseCelestialKindName(std::string_view _name, CelestialKind& _outKind) noexcept;

} // namespace Game
