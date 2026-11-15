// Author: Giperion.

#pragma once

// Content.dll TU56 (06f6fd90-06f70dd0): reputation reactions to combat, driven by the
// SpaceSupport controller (TU53):
//   - message 0xd (sub-object damage): a player hitting an object builds up hostility in the
//     attacker slots of the player's ship; past 25 / 50 the victim side is told (message 0x1005
//     to controller 0) and at 50 the victim (and its formation) turn hostile (the routine);
//   - message 0x39 (object destroyed): the killer player's reputation with the victim's faction
//     changes by that faction's event rate 0 (the routine);
//   - the heartbeat (message 0x20; SpaceSupport 06f5d6c6): NPCs near each player that like a
//     second NPC turn hostile to whoever that NPC is hostile to (the routine, message 0x1003
//     to SpaceSupport).
// The TU's data: the 16 static ids of ContentStaticIds.h (06f6fd90-06f6ff70,
// ..the field), the scan table the field (initializer the routine) and
// (initializer the branch); all private to CombatReputation.cpp.

class FContentSpaceObj;          // NpcBase.h (TU20)
struct FSubObjDamageEvent;       // server_gameobject.h (message 0xd)

// (cdecl): the damage event is copied (its ObjId is replaced by the solar parent
// when there is one).
void __cdecl HandleDamageReputation(FSubObjDamageEvent const* Event);

// baseline is a thiscall member of the SpaceSupport controller (ECX = the
// controller at 06f5d764, not read; RET 4) forwarding Event to the routine. FSpaceSupport is
// private to SpaceSupport.cpp, so the port has it as a free function.
void __cdecl OnDamageReputation(FSubObjDamageEvent const* Event);

// baseline is a thiscall member of the SpaceSupport controller (ECX = the
// controller at 06f5da0b, not read; RET 8). Ship is the destroyed object, KillerId the killer
// (or the top damaging player) chosen by the caller.
void __cdecl ApplyKillReputation(FContentSpaceObj const* Ship, unsigned int const* KillerId);

// (cdecl, plain RET): the argument the routine forwards is not read.
void __cdecl UpdateNpcReputations(float Unused);

// (cdecl; caller SpaceSupport heartbeat 06f5d6c6 with the heartbeat period
// ): a tail jump to the routine with the same argument.
void __cdecl OnHeartbeatReputation(float HeartbeatPeriod);
