// Author: Giperion.

// IClientImpl slots outside IClientS.cpp (see OutpostFrontierClient.h for the split):
//   the GF mirror forwards   module storage..00437b30 (slots 21..45, 74),
//   the player-state block   module storage..005a64f0 (slots 17, 66, 67, 70..80, 101..108,
//                            110, 111) and IChatClient slot 0 (module storage),
//   the login forwards       module storage..005a8240 (slots 11..14),
//   three shared stubs       0052fba0 (slot 10), 00401e90 (slot 15), 0051ca40 (slot 16).
// Every slot's baseline body is quoted by address.

#include "OutpostFrontierClient.h"

#include "best_path.h"                // objective map slots 71 / 72 / 73 (the field)
#include "character_select_equip.h"   // player-state setters (the routine ..)
#include "chat_init.h"                // ChatDispatchIncoming (the routine), the routine/0046b520
#include "client_gameobj.h"           // FClientGameobj / FClientShipGameobj
#include "client_objdb.h"             // ClientObjDb_FindObject (the routine)
#include "common_cobject.h"           // CEqObj
#include "common_goods.h"             // EquipDescList, the mirror property classes
#include "common_iface.h"             // ScriptBehavior
#include "common_id.h"                // MakeId
#include "common_net.h"               // MarshalBuffer
#include "common_reputation.h"        // Reputation::Vibe
#include "common_server.h"            // IServer (the field)
#include "common_misc.h"              // SinglePlayer
#include "neuroncomponents.h"
#include "hud_target.h"               // G_bTradeRequested (the field)
#include "login_mgr.h"                // CLoginMgr G_LoginMgr (the field)
#include "menu_universe_base.h"       // LocationController setters (the routine ..)
#include "misc_util.h"                // FmtStrSprintf (the routine)
#include "nn_mission_vendor.h"        // NnMissionVendor, G_MissionVendorNoMissionsReason
#include "nn_modal.h"                 // NnMainButtonOnModalOpen (the routine)
#include "nn_story_star.h"            // StoryStarOnMissionMessage (the routine, slot 70)
#include "nn_trade.h"                 // G_NnTrade (the field)
#include "player_behavior.h"          // SetUnknownFloat673568 / 67356c (the routine/b0)
#include "player_spawn.h"             // player ids (the field / the field)
#include "rpc_iclient_args.h"         // FServerSetReputationInfo (slot 66)
#include "server_connect.h"           // GetLocalServerInterface (the field)
#include "win_factory.h"              // WinFactoryCreate (the routine)

#include <list>
#include <map>
#include <stdint.h>
#include <string>
#include <string.h>
#include <utility>

namespace
{
    // XREF[3], read and written only by slots 107 / 108 (0x005a61f2,
    // , 0x005a623f): one more for every trade request slot 107 announces (the +0x18c
    // bit 2 / the routine AddTradeRequest path), one less for every one slot 108 withdraws.
    // Both slots change it even when the ship is not found. Nothing reads it otherwise.
    int G_PendingTradeRequestCount = 0;

    // a std::map<unsigned, std::wstring> (player id -> name), constructed by the
    // static initialiser the branch (the routine, atexit the branch -> the tree teardown
    // ). XREF[4]: the ctor, the dtor and slots 110 / 111 (the routine insert).
    // Nothing else reads it.
    std::map<unsigned, std::wstring> G_PlayerListNames;

    IServer* LocalServer()
    {
        return static_cast<IServer*>(GetLocalServerInterface());
    }

    // The four-byte float every GF / player-state slot receives as a dword.
    float DwordAsFloat(unsigned int Bits)
    {
        float Value = 0.0f;
        memcpy(&Value, &Bits, sizeof(Value));
        return Value;
    }
}

// (the static initializer of .data:Client): the two vptrs and +0x08 = 0.
IClientImpl::IClientImpl()
    : Field_0x08(0)
{
}

// ---------------------------------------------------------------------------------------------
// Shared stubs
// ---------------------------------------------------------------------------------------------

// `MOV AL,1; RET 8` -- neither argument is read.
bool IClientImpl::Startup(unsigned int /*MaxPlayers*/, unsigned int /*Port*/)
{
    return true;
}

// a bare `RET` (thiscall, no arguments).
void IClientImpl::Shutdown()
{
}

// `MOV EAX,1; RET`.
int IClientImpl::Update()
{
    return 1;
}

// ---------------------------------------------------------------------------------------------
// Login forwards (module storage..005a824f) on CLoginMgr the field
// ---------------------------------------------------------------------------------------------

// the field = 1, then JMP CLoginMgr::OnConnectResponse (the routine) with the
// argument in place (`RET 4` there).
void IClientImpl::OnConnect(unsigned int ClientId)
{
    G_bServerConnected = true;
    G_LoginMgr.OnConnectResponse(ClientId);
}

// JMP CLoginMgr::OnLoginResponse (the routine, `RET 8`); the second argument
// points at the response, whose first byte is the login status.
void IClientImpl::SendServerLoginResponse(unsigned int ClientId, const unsigned char& Status)
{
    G_LoginMgr.OnLoginResponse(ClientId, &Status);
}

// (`RET 8`): the routine on the connect context with the second argument -- the
// CCharacterSet the server side built (ch2 0x03 FLPACKET_SERVER_CHARACTERINFO). The first
// argument is not read.
void IClientImpl::SendServerCharacterInfo(unsigned int /*ClientId*/, const CCharacterSet& Characters)
{
    CharacterSelect_ReceiveCharacterInfo(Characters);
}

// (`RET 8`): CLoginMgr::OnCharacterSelectResponse (the routine) on the connect
// context with the second argument (the response); the first is not read. The CLoginMgr chain
// (NotifyCharacterSelectComplete) still reads the record as dwords, hence the cast.
void IClientImpl::SendServerCharSelectVerified(unsigned int /*ClientId*/,
                                               const FServerCharSelectVerifiedInfo& Info)
{
    G_LoginMgr.OnCharacterSelectResponse(reinterpret_cast<unsigned int const*>(&Info));
}

// ---------------------------------------------------------------------------------------------
// Player state (module storage..)
// ---------------------------------------------------------------------------------------------

// (`RET 4`): nothing in single player; otherwise the field = 0 and, when
// is set, the field = 1. ClientId is not read.
void IClientImpl::DisconnectClient(unsigned int /*ClientId*/)
{
    if (SinglePlayer())
    {
        return;
    }
    SetPlayerConnectionId(0);
    if (G_bServerConnected)
    {
        G_bServerDisconnected = true;
    }
}

// ---------------------------------------------------------------------------------------------
// GF mirror forwards (module storage..00437b30). ECX = the field is the LocationController.
// ---------------------------------------------------------------------------------------------

// (RET 8): the routine(ShipArchId) -- ch2 0x23 SETSHIPARCH.
void IClientImpl::SendServerSetShipArch(unsigned int /*ClientId*/, unsigned int ShipArchId)
{
    SetPlayerShipArchetypeIdbaseline(ShipArchId);
}

// (RET 8): the routine(HullStatus) -- ch2 0x49 SETHULLSTATUS.
void IClientImpl::SendServerSetHullStatus(unsigned int /*ClientId*/, float HullStatus)
{
    SetPlayerHealthScalebaseline(HullStatus);
}

// (RET 8): `PUSH [ESP+8] ; CALL the routine ; POP ECX ; RET 8` -- ch2 0x48
// SETCOLLISIONGROUPS, the second argument the address of a std::list<CollisionGroupDesc>
// (Server.dll &PlayerData::collision_groups; rpclocal DeliverSetCollisionGroups hands over a
// real std::list since W4-C).
void IClientImpl::SendServerSetCollisionGroups(unsigned int /*ClientId*/,
                                               const std::list<CollisionGroupDesc>& Groups)
{
    SyncPlayerCollisionGroupListbaseline(Groups);
}

// (RET 8) -- ch2 0x24 SETEQUIPMENT: EquipDescList built from the EquipDescVector
// (Common ??0EquipDescList@@QAE@ABUEquipDescVector@@@Z, 0x00437563), the routine(&it), then its
// std::list torn down (0x00437584..0x004375bb, the EH state at +0x1c).
void IClientImpl::SendServerSetEquipment(unsigned int /*ClientId*/, const EquipDescVector& Equipment)
{
    EquipDescList const Received(Equipment);
    SyncPlayerEquipDescListbaseline(Received);
}

// (RET 0xc): the routine(AddResult, &Item) -- ch2 0x2e SETADDITEM; the callee
// reads only its second argument, the EquipDesc (0x004c4177).
void IClientImpl::SendServerSetAddItem(unsigned int /*ClientId*/, int AddResult, const EquipDesc& Item)
{
    AddPlayerEquipbaseline(static_cast<unsigned char>(AddResult), &Item);
}

// (RET 0x10): the routine(RemoveResult, ItemId, Count) -- ch2 0x2f SETREMOVEITEM.
// The first argument is never read by the callee.
void IClientImpl::SendServerSetRemoveItem(unsigned int /*ClientId*/, int RemoveResult, unsigned short ItemId,
                                          int Count)
{
    RemovePlayerEquipbaseline(static_cast<unsigned char>(RemoveResult), ItemId, Count);
}

// (RET 0xc): LocationController the routine(BaseId, RoomId) -- ch2 0x0d
// SETSTARTROOM.
void IClientImpl::SendServerSetStartRoom(unsigned int /*ClientId*/, unsigned int BaseId, unsigned int RoomId)
{
    SetClientLocationBaseStartLocation(BaseId, RoomId);
}

// (RET 0xc): LocationController `CALL 0x0053aaa0` (RoomId, Key) -- analysis labels the
// target IDPMsgHandler::OnReceive; it is the shared `RET 8` stub (0x004376ef E8 AC331000 ->
// ). ch2 0x15 GFDESTROYCHARACTER therefore does nothing.
void IClientImpl::SendServerGFDestroyCharacter(unsigned int /*ClientId*/, unsigned int /*RoomId*/,
                                               unsigned int /*Key*/)
{
}

// (RET 0xc) -- ch2 0x26 GFUPDATECHAR: MarshalBuffer over (Data, Size),
// CharacterProperties unmarshalled from it, LocationController the routine(&props), then
// ~CharacterProperties.
void IClientImpl::SendServerGFUpdateChar(unsigned int /*ClientId*/, const unsigned char* Data,
                                         unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    CharacterProperties Properties;
    Properties.unmarshal(&Buffer);
    UpsertClientLocationRoomCharacter(Properties);
}

// (RET 8): LocationController the routine(RoomId) -- ch2 0x0f GFCOMPLETECHARLIST.
void IClientImpl::SendServerGFCompleteCharList(unsigned int /*ClientId*/, unsigned int RoomId)
{
    SetClientLocationRoomCharactersComplete(RoomId);
}

// (RET 0xc) -- ch2 0x1a GFSCRIPTBEHAVIOR: as GFUPDATECHAR with ScriptBehavior and
// .
void IClientImpl::SendServerGFScriptBehavior(unsigned int /*ClientId*/, const unsigned char* Data,
                                             unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    ScriptBehavior Behavior;
    Behavior.unmarshal(&Buffer);
    UpsertClientLocationRoomScriptBehavior(Behavior);
}

// (RET 0xc): LocationController the routine(RoomId, BehaviorId) -- ch2 0x17
// GFDESTROYSCRIPTBEHAVIOR.
void IClientImpl::SendServerGFDestroyScriptBehavior(unsigned int /*ClientId*/, unsigned int RoomId,
                                                    unsigned int BehaviorId)
{
    EraseClientLocationRoomScriptBehavior(RoomId, BehaviorId);
}

// (RET 8): LocationController the routine(RoomId) -- ch2 0x11
// GFCOMPLETESCRIPTBEHAVIORLIST.
void IClientImpl::SendServerGFCompleteScriptBehaviorList(unsigned int /*ClientId*/, unsigned int RoomId)
{
    SetClientLocationRoomScriptBehaviorsComplete(RoomId);
}

// (RET 0xc) -- ch2 0x1f GFUPDATEAMBIENTSCRIPT: as GFUPDATECHAR with
// AmbientScriptProperties and the routine.
void IClientImpl::SendServerGFUpdateAmbientScript(unsigned int /*ClientId*/, const unsigned char* Data,
                                                  unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    AmbientScriptProperties Properties;
    Properties.unmarshal(&Buffer);
    UpsertClientLocationRoomAmbientScript(Properties);
}

// (RET 0xc): LocationController the routine(RoomId, Key) -- ch2 0x19
// GFDESTROYAMBIENTSCRIPT.
void IClientImpl::SendServerGFDestroyAmbientScript(unsigned int /*ClientId*/, unsigned int RoomId,
                                                   unsigned int Key)
{
    EraseClientLocationRoomAmbientScript(RoomId, Key);
}

// (RET 8): LocationController the routine(RoomId) -- ch2 0x13
// GFCOMPLETEAMBIENTSCRIPTLIST.
void IClientImpl::SendServerGFCompleteAmbientScriptList(unsigned int /*ClientId*/, unsigned int RoomId)
{
    SetClientLocationRoomAmbientScriptsComplete(RoomId);
}

// (RET 0xc): LocationController the routine(BaseId, Key) -- ch2 0x16
// GFDESTROYMISSIONCOMPUTER: the base runtime's MissionVendorProperties erase.
void IClientImpl::SendServerGFDestroyMissionComputer(unsigned int /*ClientId*/, unsigned int BaseId,
                                                     unsigned int Key)
{
    EraseClientLocationBaseMissionOffer(BaseId, Key);
}

// (RET 0xc) -- ch2 0x1d GFUPDATEMISSIONCOMPUTER: as GFUPDATECHAR with
// MissionVendorProperties and the routine (the base runtime's +0x00 bucket upsert).
void IClientImpl::SendServerGFUpdateMissionComputer(unsigned int /*ClientId*/, const unsigned char* Data,
                                                    unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    MissionVendorProperties Properties;
    Properties.unmarshal(&Buffer);
    UpsertClientLocationBaseMissionOffer(Properties);
}

// (RET 8): LocationController the routine(BaseId) -- ch2 0x10
// GFCOMPLETEMISSIONCOMPUTERLIST, the base runtime's +0x14 byte.
void IClientImpl::SendServerGFCompleteMissionComputerList(unsigned int /*ClientId*/, unsigned int BaseId)
{
    SetClientLocationBaseMissionListComplete(BaseId);
}

// (RET 0xc) -- ch2 0x20 GFMISSIONVENDORACCEPTANCE: MissionVendorAcceptance
// unmarshalled from (Data, Size), the routine(&it) (cdecl), then its destructor.
void IClientImpl::SendServerGFMissionVendorAcceptance(unsigned int /*ClientId*/, const unsigned char* Data,
                                                      unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    MissionVendorAcceptance Acceptance;
    Acceptance.unmarshal(&Buffer);
    NnMissionVendor::OnAcceptResult(Acceptance);
}

// (RET 8): the field = Reason -- ch2 0x5a GFMISSIONVENDORWHYEMPTY.
void IClientImpl::SendServerGFMissionVendorWhyEmpty(unsigned int /*ClientId*/, unsigned int Reason)
{
    G_MissionVendorNoMissionsReason = static_cast<int>(Reason);
}

// (RET 0xc): LocationController the routine(BaseId, Key) -- ch2 0x14
// GFDESTROYNEWSBROADCAST: the base runtime's NewsBroadcastProperties erase.
void IClientImpl::SendServerGFDestroyNewsBroadcast(unsigned int /*ClientId*/, unsigned int BaseId,
                                                   unsigned int Key)
{
    EraseClientLocationBaseNews(BaseId, Key);
}

// (RET 0xc) -- ch2 0x1e GFUPDATENEWSBROADCAST: as GFUPDATECHAR with
// NewsBroadcastProperties and the routine (the base runtime's +0x18 bucket upsert).
void IClientImpl::SendServerGFUpdateNewsBroadcast(unsigned int /*ClientId*/, const unsigned char* Data,
                                                  unsigned int Size)
{
    MarshalBuffer Buffer;
    Buffer.use_read_buffer(Data, static_cast<int>(Size));
    NewsBroadcastProperties Properties;
    Properties.unmarshal(&Buffer);
    UpsertClientLocationBaseNews(Properties);
}

// (RET 8): LocationController the routine(BaseId) -- ch2 0x0e
// GFCOMPLETENEWSBROADCASTLIST, the base runtime's +0x2c byte.
void IClientImpl::SendServerGFCompleteNewsBroadcastList(unsigned int /*ClientId*/, unsigned int BaseId)
{
    SetClientLocationBaseNewsListComplete(BaseId);
}

// `MOV ECX,[the field]; MOV EAX,[ECX]; JMP [EAX+0xec]` -- a tail call into the
// server interface's slot 59 (MissionSaveB) with this slot's own two arguments in place.
void IClientImpl::SendServerMissionSaveA(unsigned int ClientId, unsigned long MissionId)
{
    LocalServer()->MissionSaveB(ClientId, MissionId);
}

// ---------------------------------------------------------------------------------------------
// Reputation / mission / stats (module storage..005a5fb0)
// ---------------------------------------------------------------------------------------------

// (RET 8) -- ch2 0x29 SETREPUTATION.
//   multiplayer (SinglePlayer() false): Vibe::EnsureExists(info), Vibe::SetAttitudeTowardsPlayer(
//     info->VibeHandle, info->Attitude), Vibe::SetAffiliation(info, info+0x08, false);
//   single player: the display name is +0x48, or -- when NameB carries a mad-lib id (+0x34) --
//     NameB printed into a 0x18-char stack buffer (the routine); the rank is read into the Info
//     argument's own stack slot (Vibe::GetRank), then Vibe::Set(info, +0x08, rank, (byte)+0x14,
//     +0x10, &NameA, &NameB, name).
void IClientImpl::SendServerSetReputation(unsigned int /*ClientId*/, const FServerSetReputationInfo& Info)
{
    if (!SinglePlayer())
    {
        Reputation::Vibe::EnsureExists(Info.VibeHandle);
        Reputation::Vibe::SetAttitudeTowardsPlayer(Info.VibeHandle, Info.Attitude);
        Reputation::Vibe::SetAffiliation(Info.VibeHandle, Info.Field_0x08, false);
        return;
    }
    wchar_t const* DisplayName = Info.DisplayName;
    wchar_t Printed[0x18];
    if (Info.NameB.MadLibId != 0)
    {
        FmtStrSprintf(&Info.NameB, Printed, 0x18);
        DisplayName = Printed;
    }
    // baseline quirk: Vibe::GetRank writes into the stack slot that held the Info pointer, so the
    // rank's initial value (kept when GetRank leaves it alone) is the pointer's own bits.
    unsigned Rank = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&Info));
    Reputation::Vibe::GetRank(Info.VibeHandle, Rank);
    Reputation::Vibe::Set(Info.VibeHandle, Info.Field_0x08, Rank, Info.RelationCount,
                          Info.Relations, Info.NameA, Info.NameB, DisplayName);
}

// (RET 8) -- ch2 0x2a ADJUSTATTITUDE, Info = {group, delta}: Vibe::AdjustAttitude(
// info->group, the routine() (the player's vibe, the field, in a stack local), info->delta).
// The group goes by reference to the record's own +0x00 (baseline pushes its address), hence the
// int view of that unsigned field.
void IClientImpl::SendServerAdjustAttitude(unsigned int /*ClientId*/, const FPayloadServerAdjustAttitude& Info)
{
    int const PlayerVibe = GetPlayerSpawnSystemIdbaseline(); // -> the field
    Reputation::Vibe::AdjustAttitude(*reinterpret_cast<int const*>(&Info.Field_0x00), PlayerVibe,
                                     Info.Field_0x04);
}

// (RET 8) -- ch2 0x45 SET_MISSION_MESSAGE: the routine(&Message.Text,
// Message.Type) -- the FmtStr at +0 and the message kind at +0x18 (0x005a5ba0..0x005a5ba9).
void IClientImpl::SendServerSetMissionMessage(const unsigned int& /*ClientId*/, const CSetMissionMessage& Message)
{
    StoryStarOnMissionMessage(&Message.Text, static_cast<int>(Message.Type));
}

// (RET 8) -- ch2 0x34 REPLACEMISSIONOBJECTIVE, body in best_path.cpp.
void IClientImpl::SendServerReplaceMissionObjective(const unsigned int& /*ClientId*/,
                                                    const FServerReplaceMissionObjectiveInfo& Info)
{
    BestPathReplaceObjective(&Info);
}

// (RET 8) -- ch2 0x35 SETMISSIONOBJECTIVES, body in best_path.cpp.
void IClientImpl::SendServerSetMissionObjectives(const unsigned int& /*ClientId*/,
                                                 const FServerSetMissionObjectivesInfo& Info)
{
    BestPathSetObjectives(&Info);
}

// (RET 8) -- ch2 0x33 SETMISSIONOBJECTIVESTATE, body in best_path.cpp.
void IClientImpl::SendServerSetMissionObjectiveState(const unsigned int& /*ClientId*/,
                                                     const FServerMissionObjectiveStateInfo& Info)
{
    BestPathSetObjectiveState(&Info);
}

// (RET 0xc) -- ch2 0x46 MARKOBJ: the object in bucket 1, else 2, else 7
// (the routine); when found, the routine(the field, MarkType) on it -- the watch-list flag.
void IClientImpl::SendServerMarkObj(const unsigned int& /*ClientId*/, unsigned int ObjectId, int MarkType)
{
    FClientGameobj* Found = ClientObjDb_FindObject(ObjectId, 1);
    if (Found == nullptr)
    {
        Found = ClientObjDb_FindObject(ObjectId, 2);
        if (Found == nullptr)
        {
            Found = ClientObjDb_FindObject(ObjectId, 7);
            if (Found == nullptr)
            {
                return;
            }
        }
    }
    static_cast<FClientSimpleGameobj*>(Found)->SetWatchListed(
        static_cast<int>(GetPlayerConnectionId()), MarkType);
}

// (RET 8) -- ch2 0x47 CFGINTERFACENOTIFICATION, Info = {id, float}: the id against
// MakeId("target_window") and MakeId("status_window") (each zero-extended from its word); the
// first sets module storage (the routine), the second module storage (the routine). The
// exe reads Field_0x04 as float bits (TODO(type) in rpc_iclient_args.h); Field_0x08 is unread.
void IClientImpl::SendServerCfgInterfaceNotification(const unsigned int& /*ClientId*/,
                                                     const FPayloadServerCfgInterfaceNotification& Info)
{
    unsigned int const TargetWindow = MakeId("target_window");
    unsigned int const StatusWindow = MakeId("status_window");
    if (Info.Field_0x00 == TargetWindow)
    {
        SetUnknownFloat673568(DwordAsFloat(Info.Field_0x04));
        return;
    }
    if (Info.Field_0x00 == StatusWindow)
    {
        SetUnknownFloat67356c(DwordAsFloat(Info.Field_0x04));
    }
}

// (RET 8): the routine(Cash, 0) -- ch2 0x30 SETCASH.
void IClientImpl::SendServerSetCash(const unsigned int& /*ClientId*/, int Cash)
{
    SetPlayerCreditsbaseline(static_cast<unsigned>(Cash), 0);
}

// (RET 8): the routine(NumKills) -- ch2 0x3e SETNUMKILLS.
void IClientImpl::SendServerSetNumKills(const unsigned int& /*ClientId*/, int NumKills)
{
    SetPlayerNumKillsbaseline(static_cast<unsigned>(NumKills));
}

// (RET 8): the routine(NumSuccesses) -- ch2 0x3f SETMISSIONSUCCESSES.
void IClientImpl::SendServerSetMissionSuccesses(const unsigned int& /*ClientId*/, int NumSuccesses)
{
    SetPlayerMissionSuccessesbaseline(static_cast<unsigned>(NumSuccesses));
}

// (RET 8): the routine(NumFailures) -- ch2 0x40 SETMISSIONFAILURES.
void IClientImpl::SendServerSetMissionFailures(const unsigned int& /*ClientId*/, int NumFailures)
{
    SetPlayerMissionFailuresbaseline(static_cast<unsigned>(NumFailures));
}

// ---------------------------------------------------------------------------------------------
// Player trade (module storage..005a61b3), FLPACKET_COMMON_PLAYER_TRADE on NN_Trade
// ---------------------------------------------------------------------------------------------

// (RET 8): the NeuroNet modal close (the routine), the trade window through its
// factory (the routine("NN_Trade", "NN_Trade", 0, 0); the result is not kept -- the ctor stored
// ), placed at {0, 0.35, -1.4} (0x3eb33333 / 0xbfb33333, slot 5), shown
// (+0x6c |= 3), aimed at the other ship (the routine), and the other ship's pending trade request
// dropped from NN_GroupControl (the routine). The window pointer is not tested.
void IClientImpl::SendCommonPlayerTradeInitiateTrade(unsigned int /*ClientId*/, unsigned int ShipId)
{
    NnMainButtonOnModalOpen();
    WinFactoryCreate("NN_Trade", "NN_Trade", nullptr, nullptr);
    float const Where[3] = {0.0f, 0.35f, -1.4f};
    G_NnTrade->SetPosition(Where);
    G_NnTrade->Flags = static_cast<unsigned char>(G_NnTrade->Flags | 0x03);
    G_NnTrade->SetTarget(ShipId);
    ChatGroupTradeRequestRemoved(ShipId);
}

// (RET 8). With the window open: a completed trade (Reason == 1) re-syncs the player's
// ship cargo with the player's equipment list -- the character's gameobj (bucket 1, narrowed to
// ManagerKind 1 with no null test after), its object (vt+0x150) narrowed to class 0x503
// (CEqObj, again not tested), CEqObj::sync_cargo(the routine()); then the window closes itself
// (its 0xef02). Whether or not it was open, the field = 0.
void IClientImpl::SendCommonPlayerTradeTerminateTrade(unsigned int /*ClientId*/, int Reason)
{
    if (G_NnTrade != nullptr)
    {
        if (Reason == 1)
        {
            FClientGameobj* const Found = ClientObjDb_FindObject(GetPlayerCharacterId(), 1);
            if (Found != nullptr)
            {
                FClientGameobj* const Ship = (Found->ManagerKind == 1) ? Found : nullptr;
                CObject* const Obj = Ship->GetObject();
                CEqObj* const Eq =
                    (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x503u) == 0x503u)
                        ? static_cast<CEqObj*>(Obj)
                        : nullptr;
                Eq->sync_cargo(GetPlayerEquipDescListbaseline());
            }
        }
        G_NnTrade->SetProperty(0xef02, 0, 0);
    }
    G_bTradeRequested = 0;
}

// (RET 0xc): the "accepted" text of whoever answered -- the other player's
// (the routine) unless it is the local character (the field), then the local player's
// (the routine).
void IClientImpl::SendCommonPlayerTradeAcceptTrade(unsigned int /*ClientId*/, unsigned int ShipId, bool bAccepted)
{
    if (G_NnTrade == nullptr)
    {
        return;
    }
    if (ShipId != GetPlayerCharacterId())
    {
        G_NnTrade->SetTargetAccepted(static_cast<unsigned char>(bAccepted));
    }
    else
    {
        G_NnTrade->SetPlayerAccepted(static_cast<unsigned char>(bAccepted));
    }
}

// (RET 0xc): the other player's money (the routine) unless the change is the local
// character's own; either way both "accepted" texts drop back to red (the routine(0),
// (0)).
void IClientImpl::SendCommonPlayerTradeSetTradeMoney(unsigned int /*ClientId*/, unsigned int ShipId,
                                                     unsigned long Money)
{
    if (G_NnTrade == nullptr)
    {
        return;
    }
    if (ShipId != GetPlayerCharacterId())
    {
        G_NnTrade->SetTargetMoney(static_cast<int>(Money));
    }
    G_NnTrade->SetTargetAccepted(0);
    G_NnTrade->SetPlayerAccepted(0);
}

// (RET 0xc): as SetTradeMoney with the routine (the other player offered the item).
void IClientImpl::SendCommonPlayerTradeAddTradeEquip(unsigned int /*ClientId*/, unsigned int ShipId,
                                                     const EquipDesc& Item)
{
    if (G_NnTrade == nullptr)
    {
        return;
    }
    if (ShipId != GetPlayerCharacterId())
    {
        G_NnTrade->AddTargetItem(&Item);
    }
    G_NnTrade->SetTargetAccepted(0);
    G_NnTrade->SetPlayerAccepted(0);
}

// (RET 0xc): as SetTradeMoney with the routine (the other player took the item
// back).
void IClientImpl::SendCommonPlayerTradeDelTradeEquip(unsigned int /*ClientId*/, unsigned int ShipId,
                                                     const EquipDesc& Item)
{
    if (G_NnTrade == nullptr)
    {
        return;
    }
    if (ShipId != GetPlayerCharacterId())
    {
        G_NnTrade->RemoveTargetItem(&Item);
    }
    G_NnTrade->SetTargetAccepted(0);
    G_NnTrade->SetPlayerAccepted(0);
}

// (RET 8): the ship ShipId (bucket 1) -- when there is one and its ManagerKind
// (+0x1c) is 1 -- gets +0x18c bit 2 and the routine(ShipId), the trade request from that ship
// queued on
// NN_GroupControl (NnGroupControl::AddTradeRequest the routine); then the field is
// incremented either way. Since the routine is the trade-request path, bit 2 marks a ship with a
// pending trade request (the earlier "in the local player's group" reading is not supported by
// the callee).
void IClientImpl::SendCommonPlayerTradeRequestTrade(unsigned int /*ClientId*/, unsigned int ShipId)
{
    FClientGameobj* const Found = ClientObjDb_FindObject(ShipId, 1);
    if (Found != nullptr && Found->ManagerKind == 1)
    {
        FClientShipGameobj* const Ship = static_cast<FClientShipGameobj*>(Found);
        Ship->Field_0x18c = static_cast<unsigned char>(Ship->Field_0x18c | 0x04);
        ChatGroupTradeRequestReceived(ShipId);
    }
    ++G_PendingTradeRequestCount;
}

// (RET 8): the mirror of RequestTrade -- bit 2 cleared and the routine(ShipId) (the
// ship's pending trade request dropped); then the field = 0 and the field decremented.
void IClientImpl::SendCommonPlayerTradeStopTradeRequest(unsigned int /*ClientId*/, unsigned int ShipId)
{
    FClientGameobj* const Found = ClientObjDb_FindObject(ShipId, 1);
    if (Found != nullptr && Found->ManagerKind == 1)
    {
        FClientShipGameobj* const Ship = static_cast<FClientShipGameobj*>(Found);
        Ship->Field_0x18c = static_cast<unsigned char>(Ship->Field_0x18c & 0xfb);
        ChatGroupTradeRequestRemoved(ShipId);
    }
    G_bTradeRequested = 0;
    --G_PendingTradeRequestCount;
}

// (RET 0x10) -- ch2 0x52 PLAYERLIST (add): the name, the player id, a byte flag.
void IClientImpl::SendServerPlayerListAdd(unsigned int /*ClientId*/, const wchar_t* Name, unsigned int PlayerId,
                                          bool bAlreadyPresent)
{
    // wcslen(Name), _Grow + the routine (wmemcpy) + _Eos -- a copy.
    std::wstring const NameCopy(Name, wcslen(Name));
    // {PlayerId, L""} through the routine (map insert, an existing
    // entry kept) and the routine (the pair's iterator) -> the node.
    std::map<unsigned, std::wstring>::iterator const It =
        G_PlayerListNames.insert(std::make_pair(static_cast<unsigned>(PlayerId), std::wstring())).first;
    // the node's +0x10 string assigned from the copy.
    It->second = NameCopy;
    // the routine(Name, PlayerId, (byte)bAlreadyPresent == 0) with the
    // caller's pointer.
    ChatPlayerJoined(Name, PlayerId, static_cast<unsigned char>(bAlreadyPresent) == 0);
}

// (RET 8) -- ch2 0x52 PLAYERLIST (remove).
void IClientImpl::SendServerPlayerListRemove(unsigned int /*ClientId*/, unsigned int PlayerId)
{
    // {PlayerId, L""} through the routine / the routine -- an insert,
    // so an unknown id gets an empty entry; nothing ever erases from the field.
    std::map<unsigned, std::wstring>::iterator const It =
        G_PlayerListNames.insert(std::make_pair(static_cast<unsigned>(PlayerId), std::wstring())).first;
    // the node's name copied into a local that nothing reads before
    // its destruction at 0x005a65ca.
    std::wstring const Departing(It->second);
    (void)Departing;
    // the routine(PlayerId, 1).
    ChatPlayerLeft(PlayerId, true);
}

// ---------------------------------------------------------------------------------------------
// IChatClient
// ---------------------------------------------------------------------------------------------

// IChatClient slot 0, the branch (RET 0x10) -- ch1 CHATMSG: the routine(SenderId, Data,
// Size), i.e. the channel key, the buffer and its size. TargetId is not read.
void IClientImpl::SendCommonChatMsg(CHAT_ID /*TargetId*/, CHAT_ID SenderId, unsigned long Size,
                                    const void* Data)
{
    ChatDispatchIncoming(SenderId, Data, static_cast<unsigned>(Size));
}
