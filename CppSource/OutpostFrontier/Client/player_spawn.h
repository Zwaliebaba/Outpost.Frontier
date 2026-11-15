// Author: Giperion.

#pragma once

// baseline player ship spawn (the routine) and space-connect prelude (the routine).
// The spawn builds the player's Ship gameobj (FClientShipGameobj, the routine) exactly as
// baseline does: slot 90 create, CShip::init from the character-select globals, the ObjDB
// registration the routine(Go, 1) and the BaseWatcher the field on it.

#include "common_ini.h" // Vector

class FClientShipGameobj;

// one of the player's 99 waypoint records (stride 0x18), counted by
// . Layout straight from the routine's stores; the two middle dwords are its
// second and third arguments. +0x0c is the waypoint's star system (the HUD compares it with
// the player's system, the nav map passes the current ISystem's id); +0x10 is the object the
// waypoint sits on, 0 for none (the routine lookup and the target command in the HUD).
struct FPlayerWaypointRecord
{
    Vector Pos;          // +0x00
    unsigned SystemId;   // +0x0c
    unsigned ObjectId;   // +0x10
    int AutoId;          // +0x14  largest id + 1 at insert time
};
static_assert(sizeof(FPlayerWaypointRecord) == 0x18, "waypoint record 0x18");

int GetPlayerWaypointCountbaseline();
FPlayerWaypointRecord* GetPlayerWaypointbaseline(int Index); // (null out of range)
void ResetPlayerWaypointsbaseline();
void RemovePlayerWaypointbaseline(int Index);
int GetPlayerWaypointMaxIdbaseline();
bool AddPlayerWaypointbaseline(Vector const& Pos, unsigned SystemId, unsigned ObjectId);
// the field, the third argument IClient slot 93 gives its last waypoint
// (0x0053c395). Its only writer is the routine below.
unsigned GetPlayerWaypointTailTagbaseline();
// (cdecl) -- the field = Tag, and ask the server for a best path From -> To
// (IServer slot 72 RequestBestPath, 0x34 bytes: player vibe the field, 2, From, the player's
// system the field, To, Field30). Caller: the nav map's best-path button (0x00493c0b).
void RequestBestPathbaseline(Vector const& From, Vector const& To, unsigned Tag, unsigned Field30);

// The multiplayer group mission-offer answer. the field is the Gf ScriptSequence showing
// the offer (the sequence at 0x004559b0 stores itself there, 0x00455a07, when it asks a type-3
// offer in multiplayer; its the routine closes the NN_OfferDialog and clears it, 0x004557d3).
// is non-zero once the server answered (read at 0x004559ef / 0x00455a49 /
// ); the field is the connection id of the player whose answer it is, compared
// with the field at 0x00456039.
struct FGfScriptSequence;
void ResetMissionOfferAnswerbaseline();
void SetMissionOfferAnswerbaseline(unsigned Answered, unsigned AcceptorId);   // IClient slot 114
unsigned GetMissionOfferAnsweredbaseline();
unsigned GetMissionOfferAcceptorIdbaseline();
FGfScriptSequence* GetMissionOfferSequencebaseline();
void SetMissionOfferSequencebaseline(FGfScriptSequence* Sequence);

unsigned GetPlayerShipArchetypeId();
float GetPlayerShipHoldSizebaseline();    // (0 if no ship archetype yet)
unsigned GetPlayerCharacterId();
// IClient slot 61 (the branch, REQUESTCREATESHIPRESP): the field = 1, the field = Result,
// = CharacterId. The waiter on +0x95 is the routine (0x0054abcf, InitSpaceState).
void SetPlayerCreateShipResponse(unsigned char Result, unsigned CharacterId);
bool IsPlayerCreateShipAnswered();
unsigned char GetPlayerCreateShipResult();
// (cdecl) -- Id & 0x3fffffff non-zero and equal to the field, the ship id the
// spawn (0x0054b835) stores. 13 call sites: HUD_Target_Base 0x004e6f15 (result unused there),
// , 0x0053566f, 0x0053bca0..0x0053e01c (nine), 0x00545b23, 0x00545bc5.
bool IsPlayerShipId(unsigned Id);
// itself -- read directly by the system-switch step the routine (0x0054aacc), which
// hands it to IServer::SystemSwitchOutComplete.
unsigned GetPlayerShipId();
unsigned GetPlayerConnectionId();
// Writers of the field outside the spawn code: CLoginMgr::OnConnectResponse (module storage),
// the disconnect paths of the routine / 005a8260 / 005a8770 / 005a8840 (store 0) and IClient
// slot 17 (module storage, store 0).
void SetPlayerConnectionId(unsigned Id);
// The watched pointer of the BaseWatcher the field: the player gameobj's Watchable
// (IObjRW+8), or null once that gameobj is gone. the routine subtracts eight from it.
void* GetPlayerShipWatcher();
// The SpaceApp ctor's step on the field (0x005493f8..0x0054940b): unhook it when it holds
// something.
void ClearPlayerShipWatcher();
// NOT BASELINE (port accessor): the CShip of the watched player gameobj, or null.
void* GetPlayerShipObject();
// NOT BASELINE (port accessor): that CShip's world position, zero without one.
void GetPlayerShipWorldPosition3(float OutPos3[3]);
// cleared by the spawn before IServer::PlayerLaunch (0x0054b867), set by the
// IClient launch handler (0x0053d87e / 0x0053d8da), waited on by the routine (0x0054ae30).
void SetPlayerLaunchAcknowledged(bool bAcknowledged);
bool IsPlayerLaunchAcknowledged();
// The space init's side of the field / 95 / 96 (the routine): both create-ship bytes cleared
// before IServer::RequestCreateShip (0x0054abbe / 0x0054abc4), and the addresses of the two bytes
// waits on (0x0054abb9 and 0x0054ae2b).
void ClearPlayerCreateShipResponse();
unsigned char* GetPlayerCreateShipAnsweredByte();
unsigned char* GetPlayerLaunchAcknowledgedByte();

FClientShipGameobj* SpawnPlayerShip();
