#pragma once

#include <cstdint>

/*
 * Universe identity (ADR-009 §5).
 *
 * These are **data-stable**: assigned in the universe definition, never at
 * runtime. They are what the wire and any future save file say, so a renumber
 * is a content migration rather than an edit -- which is exactly why the
 * authored file carries them explicitly instead of the loader inventing them
 * from array order.
 *
 * u16 throughout, and the bake is what turns that from a guess into a
 * measurement (ADR-016, U1). At 2,500 systems across ~50 regions the counts
 * are: 2,500 systems, ~250 constellations, ~50 regions, and **per system**
 * one star plus 2-8 planets, 1-2 stations and 1-4 gates -- all of which are
 * scoped *inside* a system, so their id spaces never approach 65k however far
 * the universe grows. The one space that is universe-wide is `AnchorId`,
 * because a warp order carries one number and nothing else: ~2,500 systems x
 * up to 14 anchors is ~20,000, a third of the range, and growing the universe
 * an order of magnitude is the thing that would reopen this.
 *
 * Plain type aliases rather than strong types, deliberately. A strong id type
 * earns its keep when ids of different kinds get passed to the same function
 * and can be swapped by mistake; here they are table indices read from a file
 * and looked up by name, and the wrapper would be ceremony. Revisit if a
 * signature ever takes two of these at once.
 */

namespace Game
{

using RegionId = std::uint16_t;
using ConstellationId = std::uint16_t;
using SystemId = std::uint16_t;
using CelestialId = std::uint16_t;
using StationId = std::uint16_t;
using GateId = std::uint16_t;

/// Universe-wide, unlike every id above it: a warp order names an anchor and
/// nothing else, so an anchor cannot be scoped by the system it sits in
/// (ADR-016 §3).
using AnchorId = std::uint16_t;

/*
 * Runtime identity (ADR-005 §1), and the opposite of the above in one respect:
 * these are assigned by the simulation as ships spawn, not authored. What they
 * share is stability -- a `ShipId` is the same number for a ship's whole life,
 * on the wire and in every order that names it, which is why the world keeps an
 * id-to-slot indirection rather than letting slots be the identity.
 *
 * `ShipId` is `u16` to match `Neuron::EntityRecord::id`, so a replicated record
 * needs no translation on the way out. `OrderId` is `u32` because order ids are
 * never reused within a session and 65k orders is a long evening.
 *
 * **`ShipId` widens to u32 in the T1/T2 clusters (ADR-018 D6)**, staged by
 * arithmetic rather than taste: a 23-byte `EntityRecord` fits 39 records per
 * datagram, under the 41-ship floor, so the *wire* record cannot widen until
 * [ADR-021](../Design/ADR/ADR-021-interest-and-delta.md) removes the full-fit
 * constraint. Until then the registry allocator keeps issued ids inside the u16
 * window and asserts it. The bake already speaks u32 (`Anchor::occupantIdBase`)
 * because a baked id is not a wire value.
 */
using ShipId = std::uint16_t;
using WingId = std::uint8_t;
using OrderId = std::uint32_t;

/// No such ship. Matches `Neuron::INVALID_ENTITY_ID` so the two agree on the
/// wire without either side owning the other's constant.
inline constexpr ShipId INVALID_SHIP_ID = 0xffffu;

/// No wing. Unlike ships, wings are authored-ish groupings and count from 1.
inline constexpr WingId INVALID_WING_ID = 0;

/// Reserved for "no such thing". Ids are authored from 1 so that a
/// zero-initialised field is detectably empty rather than accidentally valid.
inline constexpr std::uint16_t INVALID_ID = 0;

} // namespace Game
