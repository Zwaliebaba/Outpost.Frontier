// Author: Giperion.

#include "player_spawn.h"
#include "client_objdb.h"

#include "character_select_equip.h"
#include "client_cliobj.h"   // Archetype::LoadMountedEquipmentCompanions / HookUpInstanceTreeMaterials
#include "client_fuse.h"
#include "client_gameobj.h"  // FClientShipGameobj (the routine)
#include "hud_waypoint.h"    // HudWaypointOnPlayerWaypointsCleared (the routine)
#include "loading_screen.h"  // LoadingScreenStart (the routine)
#include "engine_light.h"
#include "common_archetype.h"
#include "common_cobject.h"
#include "common_goods.h"    // BaseWatcher, EquipDescVector
#include "common_id.h"
#include "common_ini.h"
#include "common_reputation.h"
#include "common_server.h"   // IServer (the field)
#include "common_misc.h"     // SinglePlayer
#include "common_universe.h"
#include "gate_tunnel.h"     // GateTunnelIniLoad (the routine)
#include "login_mgr.h"       // G_Server (the field)
#include "globals.h"         // g_display.gate_tunnels (the field)
#include "menu_keymap_config.h"
#include "menu_sound_arch.h"  // PreloadClientSoundArchesByCategory (the routine)
#include "nn_modal.h"         // NnSystemInit (the routine)
#include "server_connect.h"
#include "space_camera.h"
#include "space_scene_load.h"
#include "star_sys.h"

#include <cstddef>
#include <list>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace
{
    // baseline globals touched by the routine / the routine.
    // (ship archetype) lives in character_select_equip.cpp.
    static unsigned G_PlayerCharacterId = 0;
    static unsigned G_PlayerConnectionId = 0;
    static unsigned G_PlayerShipCharMirror = 0;
    // the BaseWatcher on the player's Ship gameobj (its Watchable at IObjRW+8).
    // points it at the gameobj it builds (0x0054b850); the routine and its
    // inlined copies read the watched pointer and subtract eight.
    static BaseWatcher G_PlayerShipWatcher;
    // cleared by the routine before IServer::PlayerLaunch (0x0054b867), set
    // by the IClient launch handler (0x0053d87e / 0x0053d8da), waited on by the routine
    // (the routine at 0x0054ae30, space_state.cpp InitSpaceState). A byte, because the wait
    // takes it by address.
    static unsigned char G_bPlayerLaunchAcknowledged = 0;

    // (99 records of 0x18, FPlayerWaypointRecord in player_spawn.h) and
    // (the count). The six accessors the routine..the routine are public below.
    static const int kMaxPlayerWaypoints = 0x63;
    static FPlayerWaypointRecord G_PlayerWaypoints[kMaxPlayerWaypoints] = {};
    static int G_PlayerWaypointCount = 0;
    // read by the routine, written only by the routine (0x004c4eca, called
    // from the nav map's best-path button 0x00493c0b).
    static unsigned G_PlayerWaypointTailTag = 0;

    // / the field / the field -- the multiplayer group mission-offer answer
    // (player_spawn.h). the routine zeroes all three; IClient slot 114 stores the first two;
    // the offer sequence at 0x004559b0 stores the third (0x00455a07) and the routine clears it.
    static unsigned G_MissionOfferAnswered = 0;
    static unsigned G_MissionOfferAcceptorId = 0;
    static FGfScriptSequence* G_MissionOfferSequence = nullptr;
}

unsigned GetPlayerShipArchetypeId()
{
    return GetPlayerShipArchetypeIdbaseline();
}

int GetPlayerWaypointCountbaseline()
{
    // .
    return G_PlayerWaypointCount;
}

FPlayerWaypointRecord* GetPlayerWaypointbaseline(int Index)
{
    // (cdecl): null for Index < 0 or >= the count (signed compares).
    if (Index < 0 || Index >= G_PlayerWaypointCount)
        return nullptr;
    return &G_PlayerWaypoints[Index];
}

void ResetPlayerWaypointsbaseline()
{
    // only a positive count is cleared, and then baseline tail-jumps
    // (0x004c46d3) to the routine = HudWaypointOnPlayerWaypointsCleared (hud_waypoint.h).
    if (G_PlayerWaypointCount > 0)
    {
        G_PlayerWaypointCount = 0;
        HudWaypointOnPlayerWaypointsCleared();
    }
}

void RemovePlayerWaypointbaseline(int Index)
{
    // (cdecl): Index in [0, count) drops the count by one and, unless it was the
    // last record, moves every later record down one slot.
    if (Index < 0 || Index >= G_PlayerWaypointCount)
        return;
    G_PlayerWaypointCount = G_PlayerWaypointCount - 1;
    for (int Slot = Index; Slot < G_PlayerWaypointCount; ++Slot)
        G_PlayerWaypoints[Slot] = G_PlayerWaypoints[Slot + 1];
}

int GetPlayerWaypointMaxIdbaseline()
{
    // the largest AutoId (signed compares), 0 for an empty list.
    int MaxId = 0;
    for (int Slot = 0; Slot < G_PlayerWaypointCount; ++Slot)
    {
        if (G_PlayerWaypoints[Slot].AutoId > MaxId)
            MaxId = G_PlayerWaypoints[Slot].AutoId;
    }
    return MaxId;
}

unsigned GetPlayerWaypointTailTagbaseline()
{
    // MOV EAX,[the field]; RET.
    return G_PlayerWaypointTailTag;
}

void RequestBestPathbaseline(Vector const& From, Vector const& To, unsigned Tag, unsigned Field30)
{
    // (cdecl): the field = Tag, then the 0x34-byte request on the stack --
    // +0x00 the field, +0x04 2, +0x08 byte 0, +0x0c From, +0x18 0, +0x1c the field,
    // +0x20 To, +0x2c 0, +0x30 the fourth argument -- through IServer slot 72 (+0x120,
    // RequestBestPath) with the session the field and the size 0x34. The untouched bytes
    // (+0x09..+0x0b) are stack garbage in baseline; the port zeroes them.
    G_PlayerWaypointTailTag = Tag;
    unsigned char Request[0x34];
    memset(Request, 0, sizeof(Request));
    unsigned const Field00 = static_cast<unsigned>(GetPlayerSpawnSystemIdbaseline());
    unsigned const Field04 = 2;
    unsigned const Field1c = GetPlayerStarSystemIdbaseline();
    memcpy(Request + 0x00, &Field00, 4);
    memcpy(Request + 0x04, &Field04, 4);
    memcpy(Request + 0x0c, &From, 0xc);
    memcpy(Request + 0x1c, &Field1c, 4);
    memcpy(Request + 0x20, &To, 0xc);
    memcpy(Request + 0x30, &Field30, 4);
    G_Server->RequestBestPath(GetPlayerConnectionId(), Request, 0x34);
}

void ResetMissionOfferAnswerbaseline()
{
    // XOR EAX,EAX; MOV [the field],EAX; MOV [the field],EAX;
    // MOV [the field],EAX; RET.
    G_MissionOfferAnswered = 0;
    G_MissionOfferAcceptorId = 0;
    G_MissionOfferSequence = nullptr;
}

void SetMissionOfferAnswerbaseline(unsigned Answered, unsigned AcceptorId)
{
    // IClient slot 114, 0x0053e378 / 0x0053e386.
    G_MissionOfferAcceptorId = AcceptorId;
    G_MissionOfferAnswered = Answered;
}

unsigned GetMissionOfferAnsweredbaseline()
{
    return G_MissionOfferAnswered;
}

unsigned GetMissionOfferAcceptorIdbaseline()
{
    return G_MissionOfferAcceptorId;
}

FGfScriptSequence* GetMissionOfferSequencebaseline()
{
    return G_MissionOfferSequence;
}

void SetMissionOfferSequencebaseline(FGfScriptSequence* Sequence)
{
    G_MissionOfferSequence = Sequence;
}

bool AddPlayerWaypointbaseline(Vector const& Pos, unsigned SystemId, unsigned ObjectId)
{
    // (cdecl): refused at 0x63 records or once the largest AutoId reaches 0x63;
    // otherwise the record goes to the end with AutoId = largest + 1.
    if (G_PlayerWaypointCount >= kMaxPlayerWaypoints)
        return false;
    int const MaxId = GetPlayerWaypointMaxIdbaseline();
    if (MaxId >= kMaxPlayerWaypoints)
        return false;
    FPlayerWaypointRecord& Record = G_PlayerWaypoints[G_PlayerWaypointCount];
    Record.Pos = Pos;
    Record.SystemId = SystemId;
    Record.ObjectId = ObjectId;
    Record.AutoId = MaxId + 1;
    G_PlayerWaypointCount = G_PlayerWaypointCount + 1;
    return true;
}

// — baseline: `if (the field != 0) fVar2 = *(float*)(Archetype::GetShip(the field)+0x100);`
// Ship+0x100 is HoldSize (see common_archetype.h Ship::HoldSize). Used alongside the
// not-yet-ported EquipDescList::get_cargo_space_occupied (the routine) to compute remaining
// cargo space; this half of that pair has no EquipDescList dependency, so it is fully portable now.
float GetPlayerShipHoldSizebaseline()
{
    // 0.0 (the field) without a ship archetype id (the field); otherwise
    // GetShip(id)+0x100 (HoldSize), no null test (0x004c4622..0x004c4628).
    if (GetPlayerShipArchetypeIdbaseline() == 0)
        return 0.0f;
    Archetype::Ship* const ShipArch = Archetype::GetShip(GetPlayerShipArchetypeIdbaseline());
    return ShipArch->HoldSize;
}

unsigned GetPlayerCharacterId()
{
    return G_PlayerCharacterId;
}

namespace
{
    // / the field -- the server's answer to IServer::RequestCreateShip and "an
    // answer arrived". Written by IClient slot 61 (module storage / 0x0053d92f); the routine
    // clears both before the request (0x0054abbe / 0x0054abc4) and waits on +0x95 through
    // (0x0054abb9, 0x0054abcf).
    unsigned char G_PlayerCreateShipResult = 0;
    unsigned char G_bPlayerCreateShipAnswered = 0;
}

void SetPlayerCreateShipResponse(unsigned char Result, unsigned CharacterId)
{
    // +0x95 = 1, +0x94 = Result, the field = CharacterId -- the only baseline
    // writer of the field (XREF list at module storage).
    G_bPlayerCreateShipAnswered = 1;
    G_PlayerCreateShipResult = Result;
    G_PlayerCharacterId = CharacterId;
}

bool IsPlayerCreateShipAnswered()
{
    return G_bPlayerCreateShipAnswered != 0;
}

unsigned char GetPlayerCreateShipResult()
{
    return G_PlayerCreateShipResult;
}

void ClearPlayerCreateShipResponse()
{
    // / 0x0054abc4: MOV [the field],BL / MOV [the field],BL (BL = 0).
    G_bPlayerCreateShipAnswered = 0;
    G_PlayerCreateShipResult = 0;
}

unsigned char* GetPlayerCreateShipAnsweredByte()
{
    // PUSH 0x00678b95 (0x0054abb9) -- the byte the routine waits on.
    return &G_bPlayerCreateShipAnswered;
}

unsigned char* GetPlayerLaunchAcknowledgedByte()
{
    // PUSH 0x00678b96 (0x0054ae2b).
    return &G_bPlayerLaunchAcknowledged;
}

bool IsPlayerShipId(unsigned Id)
{
    // XOR EAX,EAX; TEST ECX,0x3fffffff / JZ; CMP the field,ECX / SETZ AL.
    if ((Id & 0x3fffffffu) == 0)
    {
        return false;
    }
    return G_PlayerShipCharMirror == Id;
}

unsigned GetPlayerShipId()
{
    // .
    return G_PlayerShipCharMirror;
}

unsigned GetPlayerConnectionId()
{
    return G_PlayerConnectionId;
}

void SetPlayerConnectionId(unsigned Id)
{
    G_PlayerConnectionId = Id;
}

void* GetPlayerShipWatcher()
{
    // The watched pointer of the field (the Watchable at the player gameobj's IObjRW+8), or
    // null -- what the routine reads before subtracting eight.
    return const_cast<Watchable*>(G_PlayerShipWatcher.target());
}

void ClearPlayerShipWatcher()
{
    // (the SpaceApp ctor): CMP [the field],0 / JZ, then
    // BaseWatcher::set_pointer(0) on it.
    if (G_PlayerShipWatcher.target() != nullptr)
        G_PlayerShipWatcher.set(nullptr);
}

void* GetPlayerShipObject()
{
    // NOT BASELINE (port accessor): the CShip behind the watched gameobj, narrowed the way
    // narrows it (Object +0x10, class & 0x503 == 0x503, 0x0054b59f..0x0054b5c0).
    Watchable const* const Target = G_PlayerShipWatcher.target();
    if (Target == nullptr)
        return nullptr;
    FClientGameobj* const Go =
        static_cast<FClientGameobj*>(static_cast<IObjRW*>(const_cast<Watchable*>(Target)));
    CObject* const Obj = Go->Object;
    if (Obj == nullptr ||
        (static_cast<unsigned>(Obj->ObjectClass) & static_cast<unsigned>(CObject::ClassShip)) !=
            static_cast<unsigned>(CObject::ClassShip))
        return nullptr;
    return static_cast<CShip*>(Obj);
}

void GetPlayerShipWorldPosition3(float OutPos3[3])
{
    // NOT BASELINE (port accessor for the HUD scanner): the player CShip's world position.
    OutPos3[0] = 0.f;
    OutPos3[1] = 0.f;
    OutPos3[2] = 0.f;

    CShip* const Ship = static_cast<CShip*>(GetPlayerShipObject());
    if (Ship == nullptr)
        return;

    OutPos3[0] = Ship->WorldPosition.x;
    OutPos3[1] = Ship->WorldPosition.y;
    OutPos3[2] = Ship->WorldPosition.z;
}

void SetPlayerLaunchAcknowledged(bool bAcknowledged)
{
    G_bPlayerLaunchAcknowledged = bAcknowledged ? 1 : 0;
}

bool IsPlayerLaunchAcknowledged()
{
    return G_bPlayerLaunchAcknowledged != 0;
}

FClientShipGameobj* SpawnPlayerShip()
{
    // (no arguments; returns the gameobj in EAX, which the one caller 0x0054acb3
    // drops).

    // operator new(0x190) + the routine.
    FClientShipGameobj* const Go = new FClientShipGameobj();

    // the ship archetype of the routine (the field), Root vt+0x0c
    // load(), the gameobj's slot 90 (+0x168) create(Arch), then Archetype::Root::Release.
    // baseline tests neither the archetype nor the gameobj.
    Archetype::Ship* const ShipArch = Archetype::GetShip(GetPlayerShipArchetypeIdbaseline());
    ShipArch->load();
    Go->create(ShipArch);
    ShipArch->Release();

    // the Object at +0x10 narrowed to a CShip (class & 0x503).
    CObject* const Obj = Go->Object;
    CShip* const Ship =
        (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) &
                            static_cast<unsigned>(CObject::ClassShip)) ==
                               static_cast<unsigned>(CObject::ClassShip))
            ? static_cast<CShip*>(Obj)
            : nullptr;

    // the stack CShip::CreateParms -- the routine on the CSimple head,
    // +0x5c = 0, +0x60 = -1, the routine on +0x64 (zeroes +0x64..+0x70 and the count +0x94),
    // +0x98 = 0, EquipDescVector at +0x9c, the collision group list at +0xac, +0xe8..+0xf0 = 0,
    // byte +0xf4 = 0. Value-initialisation zeroes every one of those.
    CShip::CreateParms Parms{};
    InitCreateParmsScratchbaseline(&Parms);
    Parms.DockingCount = 0;
    Parms.VibeId = 0xffffffffu;

    // +0x00 the field, +0x04 the field, +0x08 the field.
    Parms.Id = G_PlayerCharacterId;
    Parms.ObjDb = GetClientObjDb();
    Parms.OwnerPlayer = G_PlayerConnectionId;

    // +0x60 = the routine (the player's reputation vibe, the field).
    Parms.VibeId = static_cast<unsigned>(GetPlayerSpawnSystemIdbaseline());

    // the routine (the field): its four header dwords into
    // +0x64..+0x70, its count (+0x30) into +0x94, and that many dwords from its +0x10 into
    // +0x74.
    unsigned const* const VibeHeader = GetPlayerRepVibeScratchbaseline();
    Parms.SpaceCostume.PartIds[0] = VibeHeader[0];
    Parms.SpaceCostume.PartIds[1] = VibeHeader[1];
    Parms.SpaceCostume.PartIds[2] = VibeHeader[2];
    Parms.SpaceCostume.PartIds[3] = VibeHeader[3];
    Parms.SpaceCostume.AccessoryCount = GetPlayerRepVibeScratchCountbaseline();
    unsigned const* const VibeTail = GetPlayerRepVibeScratchTailbaseline();
    for (int Index = 0; Index < (int)Parms.SpaceCostume.AccessoryCount; ++Index)
        Parms.SpaceCostume.AccessoryIds[Index] = VibeTail[Index];

    // +0x98 = the routine (the field).
    Parms.VoiceId = GetPlayerSpawnScratchScalarbaseline();
    // +0x0c = the routine (the field) * Arch->HitPts (+0x1c).
    Parms.HitPts = GetPlayerHealthScalebaseline() * ShipArch->HitPts;

    // EquipDescVector(the routine()) assigned into +0x9c; the
    // temporary's buffer is freed at 0x0054b719.
    Parms.Equipment = EquipDescVector(GetPlayerEquipDescListbaseline());
    // the list at +0xac assigned from the routine (DAT_...). The
    // inlined VC6 operator= (copy over the shared prefix, erase the surplus / insert the rest).
    Parms.ArchGroupDescs = GetPlayerCollisionGroupListbaseline();

    // +0xf0 = Reputation::Vibe::GetRank(the routine()), return value
    // ignored (GetRank writes the out value on every path).
    int const VibeId = GetPlayerSpawnSystemIdbaseline();
    unsigned Rank = 0;
    Reputation::Vibe::GetRank(VibeId, Rank);
    Parms.Rank = Rank;
    // wcscpy(Parms + 0xb8, the field), the character's name. Not
    // reproduced: nothing reads baseline +0xb8..+0xe7 on this path -- CEqObj::init and
    // CShip::init (Common module storage..) never touch it (only CSolar::init reads +0xb8), and
    // the port's CreateParms has named CSolar fields there instead.

    // the CShip's vt+0x10c init(Parms).
    Ship->init(Parms);

    // CObject::get_archetype(), its companion (+0x04) vt+0x20 Load(1).
    Ship->get_archetype()->Companion->Load(1);

    // the routine(&Parms.Equipment, 1).
    Archetype::LoadMountedEquipmentCompanions(&Parms.Equipment, 1);
    // the routine(Go->get_index()).
    Archetype::HookUpInstanceTreeMaterials(Go->get_index());

    // the gameobj's slot 92 (+0x170).
    Go->Slot170();

    // the routine(Go, 1) -- ObjDB bucket + remote-physics block.
    ClientObjDb_RegisterGameobj(Go, true);

    // multiplayer only, CObject::enable_collisions_r(true).
    if (!SinglePlayer())
        Ship->enable_collisions_r(true);

    // the field = the field.
    G_PlayerShipCharMirror = G_PlayerCharacterId;

    // point the field at Go+8 unless it already watches it.
    G_PlayerShipWatcher.set(static_cast<Watchable const*>(static_cast<IObjRW*>(Go)));

    // the field = 0, then IServer (the field) slot 48 (+0xc0)
    // PlayerLaunch(the field, the field). No null test on the server.
    G_bPlayerLaunchAcknowledged = false;
    static_cast<IServer*>(GetLocalServerInterface())
        ->PlayerLaunch(G_PlayerCharacterId, G_PlayerConnectionId);

    // the locals' destructors (list, vector) run on scope exit.
    return Go;
}
