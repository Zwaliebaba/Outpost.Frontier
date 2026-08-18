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

/// Reserved for "no such thing". Ids are authored from 1 so that a
/// zero-initialised field is detectably empty rather than accidentally valid.
inline constexpr std::uint16_t INVALID_ID = 0;

} // namespace Game
