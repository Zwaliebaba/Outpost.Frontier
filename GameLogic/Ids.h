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
 * `ShipId` is `u32` to match `Neuron::EntityRecord::id`, so a replicated record
 * needs no translation on the way out. `OrderId` is `u32` because order ids are
 * never reused within a session and 65k orders is a long evening.
 *
 * **It widened with U3d-b** (ADR-018 D6, ADR-022 §8a), and the staging was
 * arithmetic rather than taste: a 23-byte `EntityRecord` fits 39 records per
 * datagram, under ADR-018's 41-ship floor, so the *wire* record could not widen
 * while one datagram had to hold everything. ADR-022 §5b removed that -- the
 * per-tick budget is a bandwidth figure packed into as many datagrams as it
 * takes -- and the two widened together, because the whole point of this being
 * a `u16` was that it matched the record.
 *
 * The bake already spoke u32 (`Anchor::occupantIdBase`), because a baked id was
 * never a wire value; what changed is that the runtime allocator's ceiling is
 * no longer a wire constraint wearing an allocator's clothes.
 *
 * **One consequence worth naming**: `World::m_slotById` and the registry's
 * ship-keyed indices are flat tables indexed by id, so they are as long as the
 * highest id ever issued rather than as long as the fleet. Authored ids stop
 * below `DYNAMIC_SHIP_ID_BASE` (32,768) and dynamic ones count up from there,
 * so the cost is four bytes per id ever issued in a session -- a megabyte per
 * quarter-million spawns, which is far outside anything ADR-018 D4 sizes for.
 * If a shard ever runs long enough for that to matter, the answer is a sparse
 * index, and this note is where the next person should start.
 */
using ShipId = std::uint32_t;
using WingId = std::uint8_t;
using OrderId = std::uint32_t;

/// No such ship. Matches `Neuron::INVALID_ENTITY_ID` so the two agree on the
/// wire without either side owning the other's constant.
inline constexpr ShipId INVALID_SHIP_ID = 0xffffffffu;

/// No wing. Unlike ships, wings are authored-ish groupings and count from 1.
inline constexpr WingId INVALID_WING_ID = 0;

/// Reserved for "no such thing". Ids are authored from 1 so that a
/// zero-initialised field is detectably empty rather than accidentally valid.
inline constexpr std::uint16_t INVALID_ID = 0;

} // namespace Game
