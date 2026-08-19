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
 * `Snapshot` is `Header + ShipRecord[] + OrderStateRecord[]`, where `ShipRecord`
 * is deliberately `Neuron::EntityRecord` rather than a type of ours (ADR-004 §6,
 * ADR-014 §4). The string names it as the engine spells it, because that is what
 * is on the wire; what it *means* is GameLogic's and is written beside it.
 *
 * **The order messages are described as they are written, not as they are
 * declared.** `OrderSubmit` is a struct with a fixed `shipIds[64]`, and what
 * goes on the wire is `shipCount` ids and no more; a schema that copied the
 * declaration would promise 128 bytes that are never sent. The `meaning{}`
 * clause carries the same discipline for the neutral fields: `groupId` and the
 * two gauges are numbers the engine moves, and this is the only place that says
 * what they stand for -- so two builds that disagreed about whether a gauge is
 * a percentage would refuse each other here instead of drawing different bars.
 */
inline constexpr std::string_view GAME_SCHEMA_TEXT =
    "SnapshotHeader{u32 tick,u32 baselineTick,u16 shipCount,u16 orderCount,u32 lastOrderSeqProcessed}"
    "ShipRecord=EntityRecord{u16 id,u8 typeId,u8 groupId,i32 posXCm,i32 posYCm,"
    "i16 velXCmPerSec,i16 velYCmPerSec,u16 headingTurns16,u8 gaugeA,u8 gaugeB}"
    "OrderStateRecord{u32 serverOrderId,u32 clientOrderSeq,u16 etaSeconds,u8 state,u8 legIndex,u8 legCount,"
    "u8 memberCount}"
    "OrderSubmit{u32 orderSeq,u8 kind,u8 formation,u8 queueMode,u16 shipCount,u16 shipIds[shipCount],"
    "i32 targetXCm,i32 targetYCm,u16 targetFacingTurns16,u16 anchor}"
    "meaning{typeId=HullClass,groupId=WingId,gaugeA=hull255,gaugeB=shield255,state=OrderState,etaSeconds=s|65535=none,"
    "anchor=AnchorId|0=none}"
    "quantisation{position=cm,velocity=cm/s,heading=turns/65536}"
    "hull{11 classes,Fighter+Cruiser reserved}"
    "caps{shipsPerOrder=64,ordersPerSnapshot=16}"
    "enums{OrderKind:Move=0,Attack=1,Stance=2,Abilities=3,Warp=4,Dock=5;FormationId:Line=0,Wedge=1,Claw=2;"
    "QueueMode:Replace=0,Append=1;"
    "OrderState:Underway=0,Arriving=1,Done=2}";

/// Stable across runs and builds by construction: FNV-1a over the text above,
/// computed at compile time so it costs nothing to ask.
[[nodiscard]] constexpr std::uint64_t GameSchemaHash() noexcept
{
  return Neuron::HashText(GAME_SCHEMA_TEXT);
}

static_assert(GameSchemaHash() != 0, "a zero schema hash would read as 'no game'");

} // namespace Game
