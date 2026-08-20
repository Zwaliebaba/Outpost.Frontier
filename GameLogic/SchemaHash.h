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
    "SnapshotHeader{u32 tick,u32 baselineTick,u16 gridAnchor,u16 shipCount,u16 orderCount,u32 lastOrderSeqProcessed}"
    "ShipRecord=EntityRecord{u16 id,u8 typeId,u8 groupId,i32 posXCm,i32 posYCm,"
    "i16 velXCmPerSec,i16 velYCmPerSec,u16 headingTurns16,u8 gaugeA,u8 gaugeB,u8 statusBits}"
    "OrderStateRecord{u32 serverOrderId,u32 clientOrderSeq,u16 etaSeconds,u8 state,u8 legIndex,u8 legCount,"
    "u8 memberCount}"
    "OrderSubmit{u32 orderSeq,u8 kind,u8 formation,u8 queueMode,u16 shipCount,u16 shipIds[shipCount],"
    "i32 targetXCm,i32 targetYCm,u16 targetFacingTurns16,u16 anchor}"
    "StationCommand{u32 orderSeq,u8 verb,u16 station,u8 formation,u8 wing,u16 shipCount,u32 shipIds[shipCount]}"
    // ADR-017 §8 put station commands on the *acked order stream* rather than a
    // stream of their own -- one sequence, one ack, one reason enum -- so both
    // messages share `WireType::OrderSubmit` and this byte is what tells them
    // apart. It has to be in the hash: both open with `u32 orderSeq` and then a
    // byte whose value spaces overlap, so a build that disagreed about the
    // discriminator would read an Undock as a Move and validate it.
    "CommandFrame{u8 kind,body}"
    "StationRoster{u16 station,u16 count,(u32 shipId,u8 classId,u8 wingId)[count]}"
    "FleetSummaries{u16 count,(u16 anchor,u8 state,u16 shipCount,u16 etaSeconds)[count]}"
    // ADR-016 §6's family shares one engine wire type, so the byte that says
    // which member a record carries is ours and belongs in this hash. No length
    // prefix: every body above is self-delimiting, and a reader that does not
    // know a kind must refuse rather than skip -- which is only safe *because*
    // this line makes a build that renumbered `SummaryKind` fail the handshake.
    "SummaryFrame{u8 recordCount,(u8 kind,body)[recordCount]}"
    "meaning{typeId=HullClass,groupId=WingId,gaugeA=hull255,gaugeB=shield255,state=OrderState,etaSeconds=s|65535=none,"
    "anchor=AnchorId|0=none,statusBits.bit0=undockProtected,"
    "summary.anchor=whereItIsOrWhereItIsGoing,summary.etaSeconds=s|65535=none}"
    "quantisation{position=cm,velocity=cm/s,heading=turns/65536}"
    "hull{12 classes,Fighter+Cruiser reserved,Gate=11}"
    "caps{shipsPerOrder=64,ordersPerSnapshot=16,dockRadiusMetres=5000,undockProtectionSeconds=15,"
    "parkingRingMetres=2500|4000,parkingBearings=12,warpBaseSeconds=5,jumpRadiusMetres=2500,gateJumpTicks=400}"
    "enums{OrderKind:Move=0,Attack=1,Stance=2,Abilities=3,Warp=4,Dock=5;FormationId:Line=0,Wedge=1,Claw=2;"
    "QueueMode:Replace=0,Append=1;"
    "OrderState:Underway=0,Arriving=1,Done=2;"
    "StationVerb:Undock=0,AssignWing=1;"
    "FleetState:OnGrid=0,Docked=1,InTransit=2;"
    "SummaryKind:StationRoster=0,FleetSummaries=1;"
    "CommandKind:Order=0,Station=1;"
    "OrderReason:Accepted=0,EmptySelection=1,NotOwned=2,UnknownShip=3,QueueFull=4,OutOfBounds=5,"
    "InvalidFormation=6,TooManyShips=7,UnknownKind=8,UnknownStation=9,NotAtStation=10,NotDocked=11,"
    "InvalidQueueMode=12,CombatEngaged=13,UnknownAnchor=14,NoPresence=15,NotAtGate=16}"

    /*
     * The **order** the checks run in, not just their names (ADR-018 D9/A21).
     *
     * Two builds that check the same rules in a different sequence return
     * different reasons for an order that breaks two of them, and the reason is
     * what the player reads -- so a bounce that says one thing on the client
     * and another on the server is a compatibility failure even though both
     * builds "have" the same enum. It is in the hash because that is where
     * behaviour that must match belongs.
     */
    "checkOrder{order:EmptySelection,TooManyShips,UnknownKind,InvalidQueueMode,InvalidFormation,QueueFull,"
    "UnknownStation,UnknownAnchor,OutOfBounds,UnknownShip,NotAtStation,NotAtGate;"
    "station:EmptySelection,TooManyShips,InvalidFormation,UnknownStation,NotDocked}";

/// Stable across runs and builds by construction: FNV-1a over the text above,
/// computed at compile time so it costs nothing to ask.
[[nodiscard]] constexpr std::uint64_t GameSchemaHash() noexcept
{
  return Neuron::HashText(GAME_SCHEMA_TEXT);
}

static_assert(GameSchemaHash() != 0, "a zero schema hash would read as 'no game'");

} // namespace Game
