#pragma once

#include "Hash.h"

#include <cstdint>
#include <string_view>

/*
 * The game's wire schema, as one number (ADR-004 §2).
 *
 * NeuronCore hashes its own message layout and refuses a mismatched build at
 * the handshake; this is the other half, and it covers what the engine cannot
 * see: the messages GameLogic defines, their field names and types, and **the
 * quantisation constants**. That last part is the one worth spelling out. Two
 * builds that agreed on every field but disagreed about whether position is
 * centimetres or millimetres would pass every layout check and then place ships
 * ten metres apart, which is the class of bug a schema hash exists to make
 * impossible rather than merely unlikely.
 *
 * The rule is the same one `CORE_SCHEMA_TEXT` carries: **any field added,
 * removed or retyped, and any quantisation constant changed, must change the
 * string below** -- or two builds will disagree silently instead of refusing
 * each other at the door.
 */

namespace Game
{

/*
 * `Snapshot` is `Header + ShipRecord[]`, where `ShipRecord` is deliberately
 * `Neuron::EntityRecord` rather than a type of ours (ADR-004 §6, ADR-014 §4).
 * The string names it as the engine spells it, because that is what is on the
 * wire; what it *means* is GameLogic's and is written beside it.
 *
 * `OrderStateRecord` and `OrderSubmit` are not here yet. They arrive with S9,
 * and the hash changing then is the mechanism working, not a break.
 */
inline constexpr std::string_view GAME_SCHEMA_TEXT =
    "SnapshotHeader{u32 tick,u32 baselineTick,u16 shipCount,u16 orderCount,u32 lastOrderSeqProcessed}"
    "ShipRecord=EntityRecord{u16 id,u8 typeId,u8 flags,i32 posXCm,i32 posYCm,"
    "i16 velXCmPerSec,i16 velYCmPerSec,u16 headingTurns16,u8 gaugeA,u8 gaugeB}"
    "meaning{typeId=HullClass,gaugeA=hullPct,gaugeB=shieldPct}"
    "quantisation{position=cm,velocity=cm/s,heading=turns/65536}"
    "hull{11 classes,Fighter+Cruiser reserved}";

/// Stable across runs and builds by construction: FNV-1a over the text above,
/// computed at compile time so it costs nothing to ask.
[[nodiscard]] constexpr std::uint64_t GameSchemaHash() noexcept
{
  return Neuron::HashText(GAME_SCHEMA_TEXT);
}

static_assert(GameSchemaHash() != 0, "a zero schema hash would read as 'no game'");

} // namespace Game
