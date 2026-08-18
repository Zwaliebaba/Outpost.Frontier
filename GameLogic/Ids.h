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
 * u16 throughout: launch is ~300 systems across ~6 regions, and 65k of
 * headroom on each axis is the cheapest possible answer to "will we run out".
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
using SystemId = std::uint16_t;
using CelestialId = std::uint16_t;
using StationId = std::uint16_t;
using GateId = std::uint16_t;

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
