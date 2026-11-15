// Author: Giperion.

#pragma once

#include "common_pub.h"
#include "common_text.h"
#include "common_csys.h"
#include "common_server_types.h"
#include "common_server_controller.h"
// CPlayerAccount, by value: baseline embeds one in PlayerDB at this+0x10 (the ctor
// at 0x06d50b10 does LEA ECX,[ESI+0x10] / CALL COMMON.DLL::CPlayerAccount::
// CPlayerAccount, and the GetServerSig thunk at 0x06ce1ac0 is ADD ECX,0x10 /
// JMP [COMMON.DLL::CPlayerAccount::GetAccountNameSigChar]). A by-value member
// needs the complete type, hence this include. Verified acyclic: common_net.h
// reaches only common_exports/common_universe/common_reputation and their
// leaves, none of which include common_server.h.
#include "common_metalist.h"
#include "common_net.h"
#include "common_exports.h"
#include "server_exports.h"
// CDeadReckonedVector, complete: CRemotePhysicsSimulation's copy constructor and
// operator= below are defined in this header (baseline compiled them inline into
// Common.dll, Server.dll and OutpostFrontier.exe) and copy-construct / assign its three
// embedded sub-objects. Verified acyclic: common_cobject.h's include closure does
// not reach common_server.h.
#include "common_cobject.h"
#include "neuroncomponents.h"
#include "common_ivp_controller.h"  // IVP_Controller_Golem, CRemotePhysicsController's first base
#include <cmath>     // sqrt (CRemotePhysicsSimulation::CalcPositionError)
#include <new>
#include <string>
#include <cstring>   // _strnicmp (ci_char_traits)
#include <cwchar>    // _wcsnicmp (ci_wchar_traits)
#include <vector>
#include <map>
#include <list>
#include <set>

// ===========================================================================
// Forward declarations and type definitions for Server.dll export signatures
// These types must match baseline mangled names exactly.
// ===========================================================================

// Global types used in Server.dll exports
struct FmtStr;
struct BaseHint;
struct AmbientScriptDescription;
struct CharacterBehaviorDescription; // common_iface.h (pub::GF::CharacterBehaviorCreate)
struct NewsBroadcastDescription; // common_goods.h (pub::GF::NewsBroadcastCreate)
struct MissionVendorOfferDescription; // common_goods.h (pub::GF::MissionVendorOfferCreate)
struct EquipDescVector; // common_goods.h (pub::GetLoadout / pub::Player::SetShipAndLoadout)
class Transform;
// Quaternion is fully defined in common_physys.h, which common_server_types.h now
// pulls in for SSPObjUpdateInfo::Orientation (a by-value member).

// AmbientScriptDescription / AmbientScriptProperties — see common_goods.h. They live there
// (not here) because they need the full FmtStr definition and pair with the other
// pub::GF property-bag classes (MissionVendorProperties, NewsBroadcastProperties, ...).

// MissionMessageType — used in Player::DisplayMissionMessage
// baseline: ?DisplayMissionMessage@Player@pub@@YAHABIABUFmtStr@@W4MissionMessageType@@_N@Z
enum MissionMessageType {
    MISSION_MSG_TYPE_0 = 0,
    MISSION_MSG_TYPE_1 = 1,
    MISSION_MSG_TYPE_2 = 2,
    MISSION_MSG_TYPE_3 = 3,
};

// DOCK_HOST_RESPONSE — used in SpaceObj::Dock
// baseline: ?Dock@SpaceObj@pub@@YAHABI0HW4DOCK_HOST_RESPONSE@@@Z
enum DOCK_HOST_RESPONSE {
    DOCK_HOST_RESPONSE_0 = 0,
    DOCK_HOST_RESPONSE_1 = 1,
    DOCK_HOST_RESPONSE_2 = 2,
    DOCK_HOST_RESPONSE_3 = 3,
    // 4 and 5: Content.dll TU53 passes both (the routine 06f6ebcf pushes 4 after a
    // granted dock; the routine 06f6e8dc pushes 5 when a ship's docking is cancelled).
    DOCK_HOST_RESPONSE_4 = 4,
    DOCK_HOST_RESPONSE_5 = 5,
};

// EQInfo and TerminalInfo are declared in server_cobject.h under pub::SpaceObj namespace

// ZoneShape — used in Zone::GetShape
// baseline: ?GetShape@Zone@pub@@YAHIAAW4ZoneShape@@@Z
enum ZoneShape {
    ZONE_SHAPE_SPHERE = 0,
    ZONE_SHAPE_BOX = 1,
    ZONE_SHAPE_CYLINDER = 2,
    ZONE_SHAPE_RING = 3,
};

// MVEmptyReason — used in GF::ReportWhyMissionVendorEmpty
// baseline: ?ReportWhyMissionVendorEmpty@GF@pub@@YAHIW4MVEmptyReason@@@Z
enum MVEmptyReason {
    MV_EMPTY_REASON_0 = 0,
    MV_EMPTY_REASON_1 = 1,
    MV_EMPTY_REASON_2 = 2,
};

// weighted_vector<T> — used in Zone::GetPopulation
// baseline: ?GetPopulation@Zone@pub@@YAHIAAPBV?$weighted_vector@I@@@Z -- a CLASS
// template. Declared once, in common_weighted_vector.h (Common.dll owns it); this is only a
// forward declaration. The {Data, Count, Capacity} struct that used to sit here
// was a second, invented body under the same name.
template <typename T>
class weighted_vector;

// ---------------------------------------------------------------------------
// pub::CargoEnumerator — the sink both pub::SpaceObj::EnumerateCargo and
// pub::Player::EnumerateCargo drive.

// CORRECTED 2026-09-17: this was a `{void* CallbackFn; void* Context;}` pair,
// which baseline does not have. It is an ABSTRACT class with exactly one virtual
// in slot 0, and both baseline callers reach it the same way:

//   pub::Player::EnumerateCargo (module storage)
// MOV  EDX,dword ptr [EDI]        ; the enumerator's vtable
// PUSH EAX                        ; EquipDesc::Id, zero-extended
// PUSH ECX                        ; EquipDesc::Count
// PUSH EAX                        ; &goodId (a stack local)
// MOV  ECX,EDI                    ; this = the enumerator
// CALL dword ptr [EDX]            ; slot 0, not a raw fn ptr
//   pub::SpaceObj::EnumerateCargo (module storage)
// MOV  EDX,dword ptr [ECX]
// PUSH EAX                        ; CEquip subobject id
// PUSH EAX                        ; count
// PUSH EAX                        ; &goodId
// CALL dword ptr [EDX]

// Pushes are right-to-left, so the parameter order is (GoodId, Count, Id) --
// NOT the (Id, ArchId, Count) of the unrelated IObjInspect::CargoEnumerator in
// common_iface.h. The two are different types: the mangled export
// `?EnumerateCargo@Player@pub@@YAHABIAAUCargoEnumerator@2@@Z` back-references
// `pub` for this one's scope.

// The good id is passed BY REFERENCE (a `LEA` of a stack local, not the value),
// which is why the signature takes `unsigned int const&`.
// ---------------------------------------------------------------------------
namespace pub {
struct CargoEnumerator {
    virtual void notify_cargo(unsigned int const& GoodId, int Count, unsigned short Id) = 0;
};
} // namespace pub

struct Costume; // common_reputation.h
struct CharacterDescription; // common_goods.h

// pub::Reputation::Enumerator
// baseline: ?EnumerateGroups@Reputation@pub@@YAHAAUEnumerator@12@@Z
namespace pub {
namespace Reputation {
// Enumerator is an ABSTRACT struct with one virtual, not a function-pointer /
// context pair. Proof is the adapter thunk baseline installs at 0x06d5f940:
//     MOV EDX,[ESP+4]     ; unsigned const* GroupId
//     MOV ECX,[ECX+4]     ; adapter->Sink, i.e. the Enumerator&
//     MOV EDX,[EDX]       ; *GroupId
//     MOV EAX,[ECX]       ; the Enumerator's OWN vtable pointer
//     MOV [ESP+4],EDX     ; pass the group id by value
//     JMP dword ptr [EAX] ; tail-call its slot 0 with this = the Enumerator
// The only way [ECX] can be a vtable is if Enumerator has a virtual at slot 0,
// and the argument is passed by value after one dereference.
struct Enumerator {
    virtual char Execute(unsigned int GroupId) = 0;
};

// ?EnumerateGroups@Reputation@pub@@YAHAAUEnumerator@12@@Z
// Body: src/Server/server_pub.cpp. Content.dll offer filter calls it.
SERVER_API int __cdecl EnumerateGroups(Enumerator& GroupEnumerator);

// pub::Reputation::SetRank — Server.dll pubGlobalReputation::SetRank.
// Content.dll the routine passes the vibe handle by reference and the table
// rank as float. Body lives in src/Server/server_pub.cpp.
SERVER_API int __cdecl SetRank(int const& VibeHandle, float Rank);

// ?Alloc@Reputation@pub@@YAHAAHABUFmtStr@@1@Z -- Server.dll 0x7f0f0. Body: server_pub.cpp:6004.
// Content.dll TU42 the routine / the routine (the NPC name reputation, NpcNames.cpp).
SERVER_API int __cdecl Alloc(int& VibeHandle, FmtStr const& Name, FmtStr const& Nickname);

// ?GetName@Reputation@pub@@YAHABHAAUFmtStr@@1@Z -- Server.dll 0x7f730. Body: server_pub.cpp:6219.
// Content.dll TU42 the routine / the routine.
SERVER_API int __cdecl GetName(int const& VibeHandle, FmtStr& OutName, FmtStr& OutNickname);

// ?GetAffiliation@Reputation@pub@@YAHABHAAI@Z — Server.dll 0x7f400. Body: server_pub.cpp.
// Content.dll the routine (refcounted reputation handle, TU19).
SERVER_API int __cdecl GetAffiliation(int const& VibeHandle, unsigned int& OutAffiliation);

// ?SetAffiliation@Reputation@pub@@YAHABHABI@Z — Server.dll 0x7f430. Body: server_pub.cpp.
// Content.dll the routine (solar NPC ctor, TU24).
SERVER_API int __cdecl SetAffiliation(int const& VibeHandle, unsigned int const& Affiliation);

// ?SetAttitude@Reputation@pub@@YAHABH0M@Z — Server.dll. Body: server_pub.cpp.
// Content.dll Act_SetVibeOfferBaseHack (the routine 06ecac56).
SERVER_API int __cdecl SetAttitude(int const& VibeA, int const& VibeB, float Attitude);

// ?GetReputationGroup@Reputation@pub@@YAHAAIPBD@Z — Server.dll 0x7f6a0.
// Content.dll Story msg 0x35 (06ea625c) resolves "gd_bh_grp". Body: server_pub.cpp.
SERVER_API int __cdecl GetReputationGroup(unsigned int& OutId, char const* Name);

// ?GetGroupName@Reputation@pub@@YAHABIAAI@Z -- Server.dll 0x7f810. Content.dll TU27
// (06ef0c48, the random mission's offer description). Body: server_pub.cpp:6261.
SERVER_API int __cdecl GetGroupName(unsigned int const& GroupId, unsigned int& OutNameId);

// ?GetShortGroupName@Reputation@pub@@YAHABIAAI@Z -- Server.dll 0x7f840. Content.dll TU48
// (06f5226a, faction name tables). Body: server_pub.cpp.
SERVER_API int __cdecl GetShortGroupName(unsigned int const& GroupId, unsigned int& OutNameId);

// ?GetReputation@Reputation@pub@@YAHAAHPBD@Z — Server.dll 0x7f8a0.
// Content.dll TL/SP Pzr parse (attacker_rep_name). Body: server_pub.cpp.
SERVER_API int __cdecl GetReputation(int& OutRep, char const* Name);

// ?SetReputation@Reputation@pub@@YAHABHABIM@Z — Server.dll 0x7f470 (vibe overload).
// Content.dll Story msg 0x35 (06ea6272). Body: server_pub.cpp.
SERVER_API int __cdecl SetReputation(int const& VibeHandle, unsigned int const& GroupId,
                                     float Value);

// ?GetRank@Reputation@pub@@YAHABHAAM@Z — Server.dll 0x7f590. Content.dll GameSupport
// msg 0x33 (06effa3f) reads the vibe rank outside single player. Body: server_pub.cpp.
SERVER_API int __cdecl GetRank(int const& VibeHandle, float& OutRank);

// ?GetGroupFeelingsTowards@Reputation@pub@@YAHABHABIAAM@Z — Server.dll 0x7f870.
// Content.dll GameSupport group enumerator (the branch). Body: server_pub.cpp.
SERVER_API int __cdecl GetGroupFeelingsTowards(int const& VibeHandle, unsigned int const& GroupId,
                                               float& OutFeeling);

// ?GetReputation@Reputation@pub@@YAHABHABIAAM@Z — Server.dll 0x7f4b0 (vibe/group overload:
// OutValue = 0, then Vibe::GetGroupFeelingsTowards). Content.dll IAT 06fb35e8: TU22
// , TU17 the routine, TU48 the routine ... Body: server_pub.cpp:6077.
SERVER_API int __cdecl GetReputation(int const& VibeHandle, unsigned int const& GroupId,
                                     float& OutValue);

// ?Free@Reputation@pub@@YAHABH@Z — Server.dll: frees a vibe handle. Content.dll TU58
// / the routine (after pub::SpaceObj::GetRep). Body: server_pub.cpp:6023.
SERVER_API int __cdecl Free(int const& VibeHandle);

// ?GetReputation@Reputation@pub@@YAHABI0AAM@Z — Server.dll 0x7f760 (group/group overload:
// -4 for an invalid id without writing OutValue, else get_feelings_towards). Content.dll
// TU44 the routine. Body: server_pub.cpp:6231.
SERVER_API int __cdecl GetReputation(unsigned int const& GroupA, unsigned int const& GroupB,
                                     float& OutValue);

// ?GetAttitude@Reputation@pub@@YAHABH0AAM@Z — Server.dll 0x7f4e0. Content.dll
// Cnd_WatchVibe (the branch) and the mission FAIL/succeed rep checks. Body: server_pub.cpp.
SERVER_API int __cdecl GetAttitude(int const& VibeA, int const& VibeB, float& OutAttitude);
} // namespace Reputation

namespace GF {
// pub::GF::FindLocation — Server.dll MakeLocationID(base, room).
// Content.dll the routine (Cnd_CharSelect) calls it. Body: src/Server/server_pub.cpp.
SERVER_API unsigned int __cdecl FindLocation(unsigned int BaseId, char const* Name);

// pub::GF::GetSpaceflightLocation — Server.dll 0x37ce0: Common's SpaceflightLocationID.
// Content.dll the routine (relocate_player = Spaceflight). Body: src/Server/server_pub.cpp.
SERVER_API unsigned int __cdecl GetSpaceflightLocation();

// ?GetBasePosition@GF@pub@@YAHABI0AAVVector@@@Z -- Server.dll 0x37d20: position of the
// solar that docks with base_id in system_id, 0 on success, -2 when none. Content.dll
// (player position while docked). Body: src/Server/server_pub.cpp.
SERVER_API int __cdecl GetBasePosition(const unsigned int& system_id, const unsigned int& base_id,
                                       Vector& out_pos);

// ?GetCharacterPlacementByName@GF@pub@@YAHIPBDAAH@Z -- Server.dll 0x38100: the index of
// the named character placement of a room, 0 on success. Content.dll the routine
// (mbases.ini "fixture"). Body: src/Server/server_pub.cpp.
SERVER_API int __cdecl GetCharacterPlacementByName(unsigned int LocationId, char const* Name,
                                                   int& OutIndex);

// pub::GF::GetBodyPart / GetAccessory — Server.dll costume tables.
// Content.dll Act_EtherComm (the branch). Bodies: src/Server/server_pub.cpp.
SERVER_API int __cdecl GetBodyPart(char const* Name, int TypeId);
SERVER_API int __cdecl GetAccessory(char const* Name);

// ?GetCostumeByID@GF@pub@@YAXHAAUCostume@@@Z -- Server.dll 0x37e70.
// Content.dll the routine (base_appr). Body: src/Server/server_pub.cpp.
SERVER_API void __cdecl GetCostumeByID(int Id, Costume& OutCostume);

// pub::GF::AmbientScriptDestroy — Server.dll BaseDB destroy.
// Content.dll the routine walk at 06eb9fd2. Body: src/Server/server_pub.cpp.
SERVER_API void __cdecl AmbientScriptDestroy(unsigned long* Handle);

// ?AmbientScriptCreate@GF@pub@@YAKABUAmbientScriptDescription@@@Z -- Server.dll 0x384e0.
// Content.dll the routine (06eb9e5e). Body: src/Server/server_pub.cpp:5798.
SERVER_API unsigned long __cdecl AmbientScriptCreate(AmbientScriptDescription const& Description);

// Bar characters -- Content.dll TU17 the routine / the routine / the routine.
// Bodies: src/Server/server_pub.cpp (5504-5557, 5631, 5601).
// ?GetCostumeSkeletonGender@GF@pub@@YAHABUCostume@@AAH@Z -- Server.dll module storage.
SERVER_API int __cdecl GetCostumeSkeletonGender(Costume const& Desc, int& OutGender);
// ?CharacterCreate@GF@pub@@YAIABUCharacterDescription@@@Z -- Server.dll module storage.
SERVER_API unsigned int __cdecl CharacterCreate(CharacterDescription const& Description);
// ?CharacterDestroy@GF@pub@@YAXPAI@Z -- Server.dll module storage.
SERVER_API void __cdecl CharacterDestroy(unsigned int* Handle);
// ?CharacterSetBehavior@GF@pub@@YAXIK@Z -- Server.dll module storage.
SERVER_API void __cdecl CharacterSetBehavior(unsigned int CharacterId, unsigned long BehaviorHandle);
// ?CharacterBehaviorCreate@GF@pub@@YAKABUCharacterBehaviorDescription@@@Z -- Server.dll
// (common_iface.h has the struct). Content.dll callers: the bar scene
// creator the routine (06ea9c2b), the routine. Body: src/Server/server_pub.cpp:5544.
SERVER_API unsigned long __cdecl CharacterBehaviorCreate(CharacterBehaviorDescription const& Description);
// ?CharacterBehaviorDestroy@GF@pub@@YAXPAK@Z -- Server.dll module storage.
SERVER_API void __cdecl CharacterBehaviorDestroy(unsigned long* Handle);
// ?GetNumCharacterPlacements@GF@pub@@YAHI@Z -- Server.dll 0x380e0. Body: server_pub.cpp.
SERVER_API int __cdecl GetNumCharacterPlacements(unsigned int LocationId);
// ?GetCharacterPlacementName@GF@pub@@YAPBDIH@Z -- Server.dll 0x38140.
SERVER_API char const* __cdecl GetCharacterPlacementName(unsigned int LocationId, int Index);
// ?GetCharacterPlacementPosture@GF@pub@@YAHIHAAK@Z -- Server.dll 0x381a0.
SERVER_API int __cdecl GetCharacterPlacementPosture(unsigned int LocationId, int Index,
                                                    unsigned long& OutPosture);
// News and mission vendor -- Content.dll TU19 BaseSupport (the branch) and TU35
// . Bodies: src/Server/server_pub.cpp (5739, 5754, 5778, 5786).
// ?NewsBroadcastCreate@GF@pub@@YAKABUNewsBroadcastDescription@@@Z (IAT 06fb3674).
SERVER_API unsigned long __cdecl NewsBroadcastCreate(NewsBroadcastDescription const& Description);
// ?NewsBroadcastDestroy@GF@pub@@YAXPAK@Z (IAT 06fb367c).
SERVER_API void __cdecl NewsBroadcastDestroy(unsigned long* Handle);
// ?MissionVendorOfferCreate@GF@pub@@YAKABUMissionVendorOfferDescription@@@Z (IAT 06fb36b0;
// Content.dll caller TU35 the routine 06f049a6). Body: server_pub.cpp:5731.
SERVER_API unsigned long __cdecl MissionVendorOfferCreate(MissionVendorOfferDescription const& Description);
// ?MissionVendorOfferDestroy@GF@pub@@YAXPAK@Z (IAT 06fb3668).
SERVER_API void __cdecl MissionVendorOfferDestroy(unsigned long* Handle);
// ?ReportWhyMissionVendorEmpty@GF@pub@@YAHIW4MVEmptyReason@@@Z (IAT 06fb36ac). Body:
// server_pub.cpp:5760. Content.dll TU35 the routine passes the PLAYER id (State +0xc,
// / 06f048ca) and the reason of the routine (0 offers made, 1 no faction to offer
// for, 2 no difficulty, 3 no objective zone, 4 other CreateMission failure); Server only
// forwards both to IClient slot 42.
SERVER_API int __cdecl ReportWhyMissionVendorEmpty(unsigned int ClientId, MVEmptyReason Reason);
// ?MissionVendorAcceptance@GF@pub@@YAXK_NABUFmtStr@@I@Z (IAT 06fb3670).
SERVER_API void __cdecl MissionVendorAcceptance(unsigned long Handle, bool bAccepted,
                                                FmtStr const& Reason, unsigned int PlayerId);
} // namespace GF
} // namespace pub

// pub::System enumerators and ConnectionType enum
// baseline: ?EnumerateConnections@System@pub@@YAHABIAAUConnectionEnumerator@12@W4ConnectionType@@@Z
enum ConnectionType {
    CONN_TYPE_ALL = 0,
    CONN_TYPE_JUMP_GATE = 1,
    CONN_TYPE_JUMP_HOLE = 2,
    CONN_TYPE_TRADELANE = 3,
};

// CORRECTED 2026-09-24 (claude-code:w6-sys): the three sinks below were each a
// `{void* CallbackFn; void* Context;}` pair, which baseline does not have. Their real shapes,
// read off the three exports that drive them:
namespace pub {
namespace System {
// Not an interface: pub::System::EnumerateConnections (module storage) writes straight into
// the caller's struct -- [+0xf0] is the count and the record goes to +0x00/+0x28/+0x50 [n]
// and +0x78 + n*12 (module storage-06cfb231), with no bound check on the count.
struct ConnectionEnumerator {
    unsigned int GotoSystemId[10]; // +0x00  FSystemConnection +0x00
    ID_String Object[10];          // +0x28  FSystemConnection +0x04
    unsigned int ObjectId[10];     // +0x50  FSystemConnection +0x08
    Vector Pos[10];                // +0x78  FSystemConnection +0x0c
    unsigned int Count;            // +0xf0
};
static_assert(sizeof(ConnectionEnumerator) == 0xf4, "ConnectionEnumerator size is 0xf4");

// ?EnumerateConnections@System@pub@@YAHABIAAUConnectionEnumerator@12@W4ConnectionType@@@Z --
// Server.dll module storage, body server_pub.cpp. Appends to the caller's struct from
// enumerator.Count on (the caller zeroes Count and clears Object[] first). Content.dll TU41
// / the routine / the routine / the routine.
SERVER_API int __cdecl EnumerateConnections(const unsigned int& system_id,
                                            ConnectionEnumerator& enumerator,
                                            ConnectionType connection_type);

// The 0x64-byte record pub::System::EnumerateObjects (module storage-06cfb43f) builds from
// each Universe::FSystemObject before handing it to the sink. baseline's name for it is not
// exported.
struct FSysObj {
    char Nickname[0x20];        // +0x00 strncpy(FSystemObject::Nickname.Data, 0x20)
    Vector Pos;                 // +0x20
    unsigned int ArchetypeId;   // +0x2c
    int IdsName;                // +0x30
    int IdsInfo;                // +0x34
    char Reputation[0x20];      // +0x38 strncpy(FSystemObject::Reputation.Data, 0x20)
    unsigned int DockWith;      // +0x58 FSystemObject +0x80
    unsigned int Goto;          // +0x5c FSystemObject +0x84
    unsigned int SystemId;      // +0x60 the enumerated system
};
static_assert(sizeof(FSysObj) == 0x64, "FSysObj size is 0x64");

// One virtual in slot 0 (module storage-06cfb446: MOV EDX,[EDI]; PUSH &record; CALL [EDX]);
// the enumeration stops when it returns false (module storage TEST AL,AL).
struct SysObjEnumerator {
    virtual bool Slot00(FSysObj const* Object) = 0;
};

// One virtual in slot 0 (module storage-06cfae35: PUSH &reader; PUSH &zoneId; CALL [EAX]),
// called with the reader already set_state'd to the zone's [Zone] section. The return
// value is not read, and baseline's own name/return type are not exported.
struct ZoneEnumerator {
    virtual void Slot00(unsigned int const* ZoneId, INI_Reader* Reader) = 0;
};

// ?EnumerateObjects@System@pub@@YAHABIAAUSysObjEnumerator@12@@Z -- Server.dll module storage,
// body server_pub.cpp:6799. Content.dll the routine (TU17 dock object of a base).
SERVER_API int __cdecl EnumerateObjects(const unsigned int& SystemId, SysObjEnumerator& Enumerator);

// pub::System::EnumerateZones — Server.dll walks Universe zone INI sections.
// Content.dll the routine / the routine. Body: src/Server/server_pub.cpp.
SERVER_API int __cdecl EnumerateZones(unsigned int const& SystemId, ZoneEnumerator& Enumerator);

// pub::System::GetNestedProperties — Server.dll ISystem zone-property query.
// Content.dll the routine at 06f9adab. Body: src/Server/server_pub.cpp.
SERVER_API int __cdecl GetNestedProperties(unsigned int const& SystemId, Vector const& Position,
                                           unsigned long& OutProps);

// ?GetName@System@pub@@YAHIAAI@Z -- Server.dll module storage: the system's name string id.
// Content.dll the routine (06f89597, TARGET_SYSTEM). Body: src/Server/server_pub.cpp.
SERVER_API int __cdecl GetName(unsigned int SystemId, unsigned int& OutNameId);

// ?InZones@System@pub@@YAHIABVVector@@MQAIIAAI@Z -- Server.dll module storage (Vector
// overload). Content.dll the routine (06f72d3c), the routine. Body: server_pub.cpp.
SERVER_API int __cdecl InZones(unsigned int system_id, const Vector& position, float radius,
                               unsigned int* const out_zones, unsigned int max_zones,
                               unsigned int& out_count);
// ?InZones@System@pub@@YAHIABVTransform@@MMMQAIIAAI@Z -- Server.dll module storage (view
// frustum overload: angle x, angle y, distance). Content.dll the routine (06f5b282). Body:
// server_pub.cpp.
SERVER_API int __cdecl InZones(unsigned int system_id, const Transform& transform,
                               float angle_x, float angle_y, float distance,
                               unsigned int* const out_zones, unsigned int max_zones,
                               unsigned int& out_count);

// ?ScanObjects@System@pub@@YAHABIQAIIABVVector@@MIAAI@Z -- Server.dll module storage.
// Content.dll the routine (06f765ce). Body: server_pub.cpp.
SERVER_API int __cdecl ScanObjects(const unsigned int& system_id, unsigned int* const buffer,
                                   unsigned int max_results, const Vector& position, float radius,
                                   unsigned int type_mask, unsigned int& out_count);
} // namespace System

namespace Zone {
// ?GetRadius@Zone@pub@@YAMI@Z -- Server.dll module storage. Content.dll the routine.
SERVER_API float __cdecl GetRadius(unsigned int zone_id);
// ?GetDistance@Zone@pub@@YAMIABVVector@@@Z -- Server.dll module storage (signed, negative
// inside). Content.dll the routine (06f5b30b), TU53 06f61bb2 / 06f61f8d. Body: server_pub.cpp.
SERVER_API float __cdecl GetDistance(unsigned int zone_id, const Vector& point);
// ?GetSize@Zone@pub@@YAHIAAVVector@@@Z -- Server.dll module storage. Content the routine.
SERVER_API int __cdecl GetSize(unsigned int zone_id, Vector& out_size);
// ?GetShape@Zone@pub@@YAHIAAW4ZoneShape@@@Z -- Server.dll module storage. Content the routine.
SERVER_API int __cdecl GetShape(unsigned int zone_id, ZoneShape& out_shape);
// ?GetPos@Zone@pub@@YA?AVVector@@I@Z -- Server.dll module storage. Content the routine.
SERVER_API Vector __cdecl GetPos(unsigned int zone_id);
// ?GetOrientation@Zone@pub@@YAHABIAAVMatrix@@@Z -- Server.dll module storage. Content
// .
SERVER_API int __cdecl GetOrientation(const unsigned int& zone_id, Matrix& out_matrix);
// ?GetProperties@Zone@pub@@YAHIAAK@Z -- Server.dll module storage. Content Cnd_InZone
// (the branch 06ec2a3a).
SERVER_API int __cdecl GetProperties(unsigned int zone_id, unsigned long& out_props);
// ?GetSystem@Zone@pub@@YAII@Z -- Server.dll module storage (0 for an unknown zone).
// Content.dll TU27 the routine / the routine. Body: server_pub.cpp:6426.
SERVER_API unsigned int __cdecl GetSystem(unsigned int zone_id);
// ?GetPopulation@Zone@pub@@YAHIAAPBV?$weighted_vector@I@@@Z -- Server.dll module storage
// (the zone's faction population; null and -2 for an unknown zone). Content.dll TU27
// . Body: server_pub.cpp:6441.
SERVER_API int __cdecl GetPopulation(unsigned int zone_id,
                                     weighted_vector<unsigned int> const*& out_population);
// ?ClassifyPoint@Zone@pub@@YAMIABVVector@@@Z -- Server.dll (body server_pub.cpp:6527).
// Content.dll TU37 wander guide 06f0b72c (a point is taken when the result is < 1.0).
SERVER_API float __cdecl ClassifyPoint(unsigned int zone_id, const Vector& point);
// ?GetName@Zone@pub@@YAHIAAI@Z -- Server.dll module storage: the zone's name string id, -2
// for an unknown zone. Content.dll TU63 the routine (06f98144). Body: server_pub.cpp.
SERVER_API int __cdecl GetName(unsigned int zone_id, unsigned int& out_name_id);
} // namespace Zone
} // namespace pub

// ID_String — global struct (already defined in common_id.h, forward-declare if needed)
// Used in Zone::GetLootableInfo

// IServer (92 slots), IChatServer, GetServerInterface / GetChatServerInterface and the three
// chat hooks live in server_iserver.h, whose closure avoids common_cobject.h so OutpostServer.exe's
// MFC TUs can include it.
#include "server_iserver.h"

// (Server.dll internal, cdecl): format, wrap as
// StyleNode(Style) + TextNode(text) rich text, encode into the chat command
// buffer and send it to Target from the console id. Defined in
// src/Server/server_chat.cpp; called by CPlayerGroup::SendChat.
void __cdecl Server_SendStyledChat(unsigned short Style, unsigned int Target,
                                   const wchar_t* Fmt, ...);

// ci traits, CiString / CiWString, CAccount, PlayerDB and `Players` live in
// server_playerdb.h (its closure avoids common_cobject.h, so MFC TUs of OutpostServer.exe
// can include it).
#include "server_playerdb.h"


// ============================================================================
// SrvAsteroid
// ============================================================================
namespace CmnAsteroid {
    class CAsteroidSystem;
    class CAsteroidField;
}

namespace SrvAsteroid {
    // One server-side record per CmnAsteroid::CAsteroidField: baseline's 0x20-byte
    // "field entry" (operator_new(0x20) at module storage / 06d03e01 / 06d0365a).
    // Defined in src/Server/server_classes.cpp; only pointers cross this header.
    struct FSrvAsteroidField;

    // baseline layout (module storage): +0x00 CmnSystem, +0x04..+0x0c the VC6
    // std::list<FSrvAsteroidField*> {allocator, _Head, _Size}, +0x10 SystemId,
    // +0x14 RefCount. The port keeps the members in that order but does not
    // reproduce the VC6 container layout (no baseline ABI, AGENTS.md).
    class SERVER_API SrvAsteroidSystem {
    public:
        SrvAsteroidSystem();
        SrvAsteroidSystem(const SrvAsteroidSystem& Other);
        ~SrvAsteroidSystem();
        SrvAsteroidSystem& operator=(const SrvAsteroidSystem& Other);

        void load(const char* Path);
        void update();
        void set_sys_id(unsigned int SystemId);
        void set_cmn_system(CmnAsteroid::CAsteroidSystem* Sys);
        void map_asteroid_fields();
        int AddRef();
        int Release();

    protected:
        CmnAsteroid::CAsteroidSystem* m_CmnSystem;      // +0x00
        std::list<FSrvAsteroidField*> m_Fields;         // +0x04..+0x0c
        unsigned int m_SystemId;                        // +0x10
        int m_RefCount;                                 // +0x14

        // The module-internal free functions below operate on the members
        // directly, as baseline's inline copies of AddRef/set_cmn_system do.
        friend int SrvAsteroid_LoadSystem(unsigned int SystemId, const char* Path);
        friend int SrvAsteroid_UnloadSystem(unsigned int SystemId);
        friend void SrvAsteroid_UpdateSystem(IObjDB* Db);
        friend void SrvAsteroid_MapSystem(unsigned int SystemId);
    };

    // -- Server.dll-internal (not exported) free functions over the per-system
    //    map the field (std::map<unsigned int, SrvAsteroidSystem>, built by
    //    the static initializer the branch, destroyed by atexit the routine).

    // (cdecl). Multiplayer: StarSystem::load module storage, right
    // after CmnAsteroid::Load. Existing entry -> ++RefCount, return 2; otherwise
    // insert a default entry, ++RefCount, SystemId = id, load(Path), return 1.
    int SrvAsteroid_LoadSystem(unsigned int SystemId, const char* Path);

    // (cdecl). StarSystem::unload's helper the routine
    // (module storage). Missing -> 0; --RefCount != 0 -> 2; else erase -> 1.
    int SrvAsteroid_UnloadSystem(unsigned int SystemId);

    // (cdecl). StarSystem's step body module storage: the id from
    // Db->GetZoneDb()'s vtable[0], then the routine on every field entry.
    void SrvAsteroid_UpdateSystem(IObjDB* Db);

    // (cdecl). Single player: StarSystem::load module storage.
    // Mirrors the fields the client already built (CmnAsteroid::Find).
    void SrvAsteroid_MapSystem(unsigned int SystemId);
}

// ============================================================================
// CPlayerGroup
// ============================================================================
// BaseGroupMessage::Type is the second dword of the 8-byte payload
// (0x06d190d0) hands to IChatClient::SendCommonChatMsg as
//     SendCommonChatMsg(CHAT_ID{target}, the field, 8, &{ MsgType, Param })
// -- the literal 8 there is the SIZE of that payload, not a message id.
// The three values baseline actually fires are visible at the call sites:
//   6 -- CPlayerGroup::AddMember, both loops (0x06d5b667 and 0x06d5b6af)
//   7 -- CPlayerGroup::DelMember, sent to the player being removed (0x06d5ba0e)
//   8 -- CPlayerGroup::DelMember, sent to every remaining member (0x06d5b9d7)
namespace BaseGroupMessage {
    enum Type {
        MemberJoined = 6,
        RemovedFromGroup = 7,
        MemberLeft = 8,
    };
}

// (cdecl): the same 8-byte control payload { MsgType, Param & 0xffff }
// sent to EVERY active player (PlayerDB::traverse_active from null), each
// addressed by CHAT_ID{slot client id}. Only caller: EqObj's destroy override
// (module storage, MsgType 0x14 with the dead player's id).
// Defined in server_classes.cpp.
void Server_BroadcastGroupControl(unsigned int MsgType, unsigned int Param);

struct CMissionObjectives {
    void* Data;
};

// The real layout ({FmtStr Text; uint Type;}, 0x1c bytes) is declared once in
// rpc_iclient_args.h, next to the IClient slot 70 record rpclocal marshals from it.
struct CSetMissionMessage;

// baseline layout (0x30 bytes), read off the default ctor at 0x06d5b320:
//   +0x00  vftable (??_7CPlayerGroup@@6B@, exactly ONE slot -- the scalar
//          deleting dtor at 0x06ce13c0; the next vftable, MD5Hash's, starts
//          4 bytes later at module storage)
//   +0x04  m_GroupID          (0x06d5b400 MOV [EDI+4],EAX)
//   +0x08  m_Members  std::set<unsigned int>, 0x14 bytes, size at +0x18
//   +0x1c  m_Invites  std::set<unsigned int>, 0x14 bytes, size at +0x2c
// Both containers are red-black trees (operator new(0x14) head node at
// / 0x06d5b3af, node value at node+0xc), NOT fixed arrays: the
// port's old unsigned int[8] + count pair was invented. IsFull (0x06ce10e0)
// is literally CMP [this+0x18],8 / SBB / INC, i.e. m_Members.size() >= 8.

// Mangled names pin the cv-qualifiers: GetID/GetMemberCount/GetInviteCount/
// IsFull are all QAE (non-const); only GetMember is QBE (const).
class SERVER_API CPlayerGroup {
public:
    CPlayerGroup();
    CPlayerGroup(const CPlayerGroup& Other);
    virtual ~CPlayerGroup();
    CPlayerGroup& operator=(const CPlayerGroup& Other);
    static CPlayerGroup* __cdecl FromGroupID(unsigned int GroupId);
    bool AddMember(unsigned int PlayerId);
    bool DelMember(unsigned int PlayerId);
    bool IsMember(unsigned int PlayerId);
    bool AddInvite(unsigned int PlayerId);
    bool DelInvite(unsigned int PlayerId);
    bool IsInvited(unsigned int PlayerId);
    unsigned int GetMemberCount();
    unsigned int GetMember(int Index) const;
    unsigned int GetInviteCount();
    unsigned int GetID();
    bool IsFull();
    void HandleClientLogout(unsigned int PlayerId);
    void SendGroup(BaseGroupMessage::Type MsgType, unsigned int Param);
    void SendChat(int Style, const wchar_t* Msg, ...);
    void DeliverChat(unsigned long MsgType, const void* MsgData);
    void SetMissionObjectives(CMissionObjectives& Objs);
    void SetMissionMessage(CSetMissionMessage& Msg);
    void StoreMemberList(std::vector<unsigned int>& OutMembers);
    void RewardMembers(int Amount);
    unsigned int GetMissionID();
    unsigned int GetMissionSetBy();
    void SetMissionID(unsigned int MissionId, unsigned int SetBy);
protected:
    unsigned int m_GroupID;
    std::set<unsigned int> m_Members;
    std::set<unsigned int> m_Invites;

    // baseline exports BOTH protected statics:
    //   ordinal 508  ?s_GroupIDToGroupPtrMap@CPlayerGroup@@1V?$map@$$CBIPAVCPlayerGroup@@
    //                U?$less@$$CBI@std@@V?$allocator@PAVCPlayerGroup@@@3@@std@@A  (module storage)
    //   ?s_uiGroupID@CPlayerGroup@@1IA                                           (module storage)
    // (An earlier note here said the map was not exported; the export table says
    // otherwise.) baseline's VC6 map<const unsigned int, CPlayerGroup*> spelled its
    // default allocator with the MAPPED type (allocator<CPlayerGroup*>); ours is the
    // same map as an ordinary modern std::map with its default allocator, so the
    // decorated name differs from baseline's on purpose (project rule, 2026-09-24).
    typedef std::map<unsigned int, CPlayerGroup*> GroupMapType;
    static GroupMapType s_GroupIDToGroupPtrMap;
    static unsigned int s_uiGroupID;
};

// ============================================================================
// pub namespaces
// ============================================================================
namespace pub {

SERVER_API int __cdecl IsCommodity(const unsigned int& GoodId, bool& OutCommodity);
SERVER_API int __cdecl GetCargoHoldSize(const unsigned int& ShipArchId, unsigned int& OutSize);
// ?GetGoodProperties@pub@@YAHABIAAM1@Z -- Server.dll 0x7e3f0, body server_pub.cpp:658
// (Equipment volume, Root mass). Content.dll TU44 the routine (06f35c6f / 06f35d0a).
SERVER_API int __cdecl GetGoodProperties(const unsigned int& GoodId, float& OutVolume, float& OutMass);
SERVER_API int __cdecl GetLoadoutName(const unsigned int& LoadoutId, char* OutBuffer, int MaxLen);
SERVER_API int __cdecl GetLoadoutID(unsigned int& OutId, const char* Nickname);
// ?GetLoadout@pub@@YAHAAVEquipDescVector@@ABI@Z -- Server.dll (ordinal 201), body
// server_pub.cpp. Content.dll Act_SetShipAndLoadout Parse (the routine, 06ec9a51).
SERVER_API int __cdecl GetLoadout(EquipDescVector& OutLoadout, const unsigned int& LoadoutId);
SERVER_API int __cdecl GetEquipmentID(unsigned int& OutId, const char* Nickname);
SERVER_API int __cdecl GetGoodID(unsigned int& OutId, const char* Nickname);
// ?GetShipID@pub@@YAHAAIPBD@Z -- baseline Server.dll RVA 0x7E2E0 (server_pub.cpp).
SERVER_API int __cdecl GetShipID(unsigned int& OutShipId, const char* ShipName);
SERVER_API int __cdecl TranslateArchToGood(const unsigned int& ArchId, unsigned int& OutGoodId);
SERVER_API int __cdecl GetVoiceID(unsigned int& OutId, const char* Nickname);
SERVER_API int __cdecl GetCostumeID(int& OutId, const char* Nickname);
SERVER_API struct IFileSystem* __cdecl GetDataPath();
SERVER_API int __cdecl GetTime(double& OutTime);
SERVER_API int __cdecl GetRwTime(double& OutTime);
// ?SinglePlayer@pub@@YA_NXZ — Server.dll 0x7e1e0 (forwards to Common ::SinglePlayer).
SERVER_API bool __cdecl SinglePlayer();
SERVER_API bool __cdecl NextSystemID(unsigned int& InOutId);
SERVER_API bool __cdecl NextBaseID(unsigned int& InOutId);
SERVER_API int __cdecl GetSystemID(unsigned int& OutId, const char* Nickname);
SERVER_API int __cdecl GetSystemGateConnection(const unsigned int& GateId, unsigned int& OutConnectedSystemId);
SERVER_API int __cdecl GetBaseID(unsigned int& OutId, const char* Nickname);
// ?Save@pub@@YAHII@Z -- Server.dll 0x7efa0 (server_pub.cpp). Content Act_Save (06ec48e0).
SERVER_API int __cdecl Save(unsigned int PlayerId, unsigned int SaveType);
// ?SinglePlayer@pub@@YA_NXZ -- baseline Server.dll 0x7e1e0 (server_pub.cpp).
SERVER_API bool __cdecl SinglePlayer();
SERVER_API int __cdecl BuildSystemReader(INI_Reader& Reader, const unsigned int& SystemId);
SERVER_API int __cdecl BuildBaseReader(INI_Reader& Reader, const unsigned int& BaseId);
SERVER_API unsigned int __cdecl GetSystemNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& SystemId);
SERVER_API unsigned int __cdecl GetBaseNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& BaseId);
SERVER_API int __cdecl GetBaseStridName(unsigned int& OutStrid, const unsigned int& BaseId);
SERVER_API int __cdecl GetLocations(const unsigned int& BaseId, unsigned int* const OutLocations, unsigned int MaxCount, unsigned int& OutCount);
SERVER_API unsigned int __cdecl GetLocationNickname(char* OutBuffer, unsigned int MaxLen, const unsigned int& RoomId);
SERVER_API int __cdecl GetBases(const unsigned int& SystemId, unsigned int* const OutBases, unsigned int MaxCount, unsigned int& OutCount);
SERVER_API int __cdecl GetSystem(unsigned int& OutSystemId, const unsigned int& BaseId);
SERVER_API unsigned int __cdecl GetNicknameId(const char* Nickname);
// ?TranslateSystemToMsgIdPrefix@pub@@YAHIAAU?$TString@$0EA@@@@Z -- Server.dll 0x7e160, body
// server_pub.cpp. Content.dll TU25 06ed966a / the routine (the jump target's system).
SERVER_API int __cdecl TranslateSystemToMsgIdPrefix(unsigned int SystemId, TString<64>& OutPrefix);
// ?TranslateShipToMsgIdPrefix@pub@@YAHIAAU?$TString@$0EA@@@@Z -- Server.dll 0x7e0d0, body
// server_pub.cpp. Content.dll TU25 line ctors (the ship archetype's msg_id_prefix).
SERVER_API int __cdecl TranslateShipToMsgIdPrefix(unsigned int ShipArchId, TString<64>& OutPrefix);
// ?TranslateGoodToMsgIdPrefix@pub@@YAHIAAU?$TString@$0EA@@@@Z -- Server.dll 0x7e050, body
// server_pub.cpp. Content.dll TU25 the routine (the traded good's msg_id_prefix).
SERVER_API int __cdecl TranslateGoodToMsgIdPrefix(unsigned int GoodId, TString<64>& OutPrefix);
// ?GetType@pub@@YAHABIAAI@Z -- Server.dll 0x7e330, body server_pub.cpp:607 (the ship
// archetype's Root::ObjectType). Content.dll TU42 the routine (06f31a10).
SERVER_API int __cdecl GetType(const unsigned int& ShipArchId, unsigned int& OutType);
// ?GetShipArchSTRID@pub@@YAHABIAAI@Z -- Server.dll 0x7e230, body server_pub.cpp:590.
// Content.dll TU42 the routine (06f31bfb).
SERVER_API int __cdecl GetShipArchSTRID(const unsigned int& ShipArchId, unsigned int& OutStrid);
// pub::GetMaxHitPoints — Server.dll ordinal 211. Content.dll the routine at 06f9523d.
// Body: src/Server/server_pub.cpp. 0 success, -2 solar missing, -4 bad id.
SERVER_API int __cdecl GetMaxHitPoints(unsigned int const& ObjId, int& OutHP);
SERVER_API unsigned short __cdecl MakeId(const char* Nickname);
SERVER_API void __cdecl DebugPrint(const char* Msg, int Level);
SERVER_API int __cdecl GetNavMapScale(unsigned int SystemId, float& OutScale);
SERVER_API int __cdecl ReportFreeTerminal(unsigned int P1, int P2);
SERVER_API HINSTANCE __cdecl DLL_LoadLibrary(const char* DllPath);

namespace Market {
    SERVER_API int __cdecl GetMinInventory(unsigned int BaseId, unsigned int GoodId, int& OutInv);
    SERVER_API int __cdecl GetPrice(unsigned int BaseId, unsigned int GoodId, float& OutPrice);
    SERVER_API int __cdecl GetNominalPrice(unsigned int GoodId, float& OutPrice);
    SERVER_API int __cdecl GetGoodJumpDist(unsigned int GoodId, unsigned int& OutDist);
    SERVER_API int __cdecl IsGoodInDemand(unsigned int BaseId, unsigned int GoodId, bool& OutDemand);
    SERVER_API int __cdecl GetNumCommoditiesForSale(unsigned int BaseId, int* OutCount);
    SERVER_API int __cdecl GetCommoditiesForSale(unsigned int BaseId, unsigned int* const OutGoods, int* InOutCount);
    SERVER_API int __cdecl GetNumCommoditiesInDemand(unsigned int BaseId, int* OutCount);
    SERVER_API int __cdecl GetCommoditiesInDemand(unsigned int BaseId, unsigned int* const OutGoods, int* InOutCount);
}

namespace Audio {
    struct Tryptich {
        unsigned int Field0;
        unsigned int Field4;
        unsigned int Field8;
        unsigned int FieldC;
        unsigned char Field10;
        unsigned int Field14;
    };

    SERVER_API int __cdecl SetMusic(unsigned int PlayerId, const Tryptich& Track);
    SERVER_API int __cdecl CancelMusic(unsigned int PlayerId);
    SERVER_API int __cdecl PlaySoundEffect(unsigned int PlayerId, unsigned int SoundId);
}

namespace Phantom {
    SERVER_API int __cdecl Create(unsigned int SystemId, const Vector& MinPos, const Vector& MaxPos, const Matrix& Orient, unsigned int Flags, void*& OutHandle);
    SERVER_API int __cdecl Create(unsigned int SystemId, float Radius, const Vector& Center, unsigned int Flags, void*& OutHandle);
    SERVER_API int __cdecl Destroy(void* Handle);
    SERVER_API int __cdecl SetActive(void* Handle, bool bActive);
    SERVER_API int __cdecl Attach(const unsigned int& ObjId, void* Handle);
    SERVER_API int __cdecl Detach(void* Handle);
}

namespace AI {
    SERVER_API OP_RTYPE __cdecl SubmitState(unsigned int Flid, BaseOp* Op);
    SERVER_API OP_RTYPE __cdecl SubmitDirective(unsigned int Flid, BaseOp* Op);
    SERVER_API OP_RTYPE __cdecl set_directive_priority(unsigned int Flid, DirectivePriority Priority);
    SERVER_API int __cdecl get_behavior_id(unsigned int Flid);
    SERVER_API int __cdecl get_state_graph_id(unsigned int Flid);
    SERVER_API bool __cdecl get_personality(unsigned int Flid, Personality& OutPersonality);
    SERVER_API bool __cdecl enable_maneuver(unsigned int Flid, int ManeuverId, bool bEnable);
    SERVER_API bool __cdecl enable_all_maneuvers(unsigned int Flid);
    SERVER_API bool __cdecl lock_maneuvers(unsigned int Flid, bool bLock);
    SERVER_API ScanResponse __cdecl get_scan_response(unsigned int ScannerFlid, unsigned int TargetFlid, unsigned int GoodId);
    SERVER_API int __cdecl submit_forced_target(unsigned int Flid, unsigned int TargetFlid);
    SERVER_API int __cdecl remove_forced_target(unsigned int Flid, unsigned int TargetFlid);
    SERVER_API bool __cdecl set_player_enemy_clamp(unsigned int Flid, int MinClamp, int MaxClamp);
    // Argument 1 is the ship that changes formation, argument 2 its new leader (baseline
    // arg 1 is the object whose IObjAffect +0x10/+0x14 get the leader/offset).
    SERVER_API FORMATION_RTYPE __cdecl update_formation_state(unsigned int ShipFlid, unsigned int LeaderFlid, const Vector& Offset);
    SERVER_API void __cdecl refresh_state_graph(unsigned int Flid);
}

namespace Player {
    struct MissionObjective {
        unsigned int Type;
        FmtStr Description;
    };

    SERVER_API int __cdecl GetRep(const unsigned int& PlayerId, int& OutRep);
    // ?SendNNMessage@Player@pub@@YAHII@Z / ?ForceLand@Player@pub@@YAHII@Z /
    // ?RevertCamera@Player@pub@@YAHI@Z -- Server.dll, bodies in server_pub.cpp. Content.dll
    // Act_PlayNN (the branch), Act_ForceLand (the branch), Act_RevertCam (the routine).
    SERVER_API int __cdecl SendNNMessage(unsigned int PlayerId, unsigned int MessageId);
    SERVER_API int __cdecl ForceLand(unsigned int PlayerId, unsigned int BaseId);
    SERVER_API int __cdecl RevertCamera(unsigned int PlayerId);
    // ?SetCamera@Player@pub@@YAHIABVTransform@@MM@Z -- Server.dll module storage, body
    // server_pub.cpp. Content.dll Act_StaticCam (the routine).
    SERVER_API int __cdecl SetCamera(unsigned int PlayerId, const Transform& Camera, float Fov,
                                     float Aspect);
    // ?EnumerateCargo@Player@pub@@YAHABIAAUCargoEnumerator@2@@Z / ?RemoveCargo@Player@pub@@ --
    // Server.dll module storage / module storage, bodies server_pub.cpp. Content.dll
    // Act_RemoveCargo (the branch).
    SERVER_API int __cdecl EnumerateCargo(const unsigned int& PlayerId,
                                          CargoEnumerator& Enumerator);
    SERVER_API int __cdecl RemoveCargo(const unsigned int& PlayerId, unsigned short CargoSlot,
                                       unsigned int Quantity);
    // ?SetShipAndLoadout@Player@pub@@ -- Server.dll module storage, body server_pub.cpp.
    // Content.dll Act_SetShipAndLoadout (the branch).
    SERVER_API int __cdecl SetShipAndLoadout(const unsigned int& PlayerId, unsigned int ShipArchId,
                                             const EquipDescVector& Loadout);
    SERVER_API int __cdecl InspectCash(const unsigned int& PlayerId, int& OutCash);
    // ?SetInitialPos@Player@pub@@YAHABIABVVector@@@Z / ?SetInitialOrnt@Player@pub@@YAHABIABVMatrix@@@Z
    // — bodies server_pub.cpp. Content.dll Act_SetInitialPlayerPos (the routine).
    SERVER_API int __cdecl SetInitialPos(const unsigned int& PlayerId, const Vector& Position);
    SERVER_API int __cdecl SetInitialOrnt(const unsigned int& PlayerId, const Matrix& Orientation);
    // ?AdjustCash@Player@pub@@YAHABIH@Z — body server_pub.cpp. Content.dll the routine.
    SERVER_API int __cdecl AdjustCash(const unsigned int& PlayerId, int Amount);
    // ?GetShipID@Player@pub@@YAHABIAAI@Z -- body server_pub.cpp:4282. Content.dll
    // (06ea86cd, bar mission offer: no offer without a ship).
    SERVER_API int __cdecl GetShipID(const unsigned int& PlayerId, unsigned int& OutShipId);
    // ?GetShip@Player@pub@@YAHABIAAI@Z -- body server_pub.cpp:4272 (the ship's space object
    // id). Content.dll GameSupport msg 0x36 (06efeef0).
    SERVER_API int __cdecl GetShip(const unsigned int& PlayerId, unsigned int& OutShipObjId);
    SERVER_API int __cdecl GetAssetValue(const unsigned int& PlayerId, float& OutValue);
    SERVER_API int __cdecl GetRank(const unsigned int& PlayerId, int& OutRank);
    SERVER_API int __cdecl SetRank(unsigned int PlayerId, int Rank);
    // baseline Server.dll 0x06d56460; Content.dll loader calls with
    // (player, controller, player, false, 0) at 0x06f486fc–0x06f4870c.
    SERVER_API int __cdecl SetMsnID(unsigned int PlayerId, unsigned int MissionId,
                                   unsigned int SubId, bool bNotify, unsigned int NotifyArg);
    // pub::Player::DisplayMissionMessage — Server.dll module storage. Body: server_pub.cpp.
    SERVER_API int __cdecl DisplayMissionMessage(unsigned int const& PlayerId, FmtStr const& Text,
                                                MissionMessageType MsgType, bool bBroadcast);
    // pub::Player::PopUpDialog — Server.dll module storage. Body: server_pub.cpp:4838.
    // Content.dll the routine (06eba303) pushes (player id by value, &title, &text, 5).
    SERVER_API int __cdecl PopUpDialog(unsigned int PlayerId, FmtStr const& Title,
                                      FmtStr const& Text, unsigned int Buttons);
    // pub::Player::LoadHint — Server.dll module storage. Body: server_pub.cpp:4968.
    // Content.dll the routine (06eba7ca) pushes (player id by value, &CostumeHint).
    SERVER_API int __cdecl LoadHint(unsigned int PlayerId, BaseHint* Hint);
    // pub::Player::MarkObj — Server.dll module storage. Body: server_pub.cpp.
    // Content.dll 06f0f9f9 pushes (player, object+4, 1).
    SERVER_API int __cdecl MarkObj(unsigned int PlayerId, unsigned int ObjectId, int MarkType);
    SERVER_API int __cdecl GetGender(const unsigned int& PlayerId, int& OutGender);
    // Content.dll Story constructor calls this with zero (06ea5f28-06ea5f2d).
    SERVER_API int __cdecl SetMoneyNeededToNextRank(unsigned int PlayerId, int Money);
    // ?GetMoneyNeededToNextRank@Player@pub@@YAHABIAAH@Z — Server.dll 0x75280.
    SERVER_API int __cdecl GetMoneyNeededToNextRank(const unsigned int& PlayerId, int& OutMoney);
    // ?ReturnPlayerStats@Player@pub@@YAHIPAEH@Z — Server.dll 0x75e40. Content.dll
    // GameSupport msg 0x33 (06effad3) sends the stats block built there.
    SERVER_API int __cdecl ReturnPlayerStats(unsigned int PlayerId, unsigned char* Data, int DataLen);
    // ?ReturnBestPath@Player@pub@@YAHIPAEH@Z — Server.dll 0x75e20 (IAT 06fb34a4), body
    // server_pub.cpp:4851. Content.dll GameSupport msg 0x31 (06eff2fb, 06eff860) sends the
    // path block built there.
    SERVER_API int __cdecl ReturnBestPath(unsigned int PlayerId, unsigned char* Data, int DataLen);
    SERVER_API int __cdecl GetGroupMembers(unsigned int PlayerId, std::vector<unsigned int>& OutMembers);
    // ?GetGroupSize@Player@pub@@YAHIAAI@Z — Server.dll 0x75fa0 (server_pub.cpp). Content.dll
    // .
    SERVER_API int __cdecl GetGroupSize(unsigned int PlayerId, unsigned int& OutSize);
    // Player::RewardGroup — Server.dll 0x763a0 (server_pub.cpp). Content.dll the routine.
    SERVER_API int __cdecl RewardGroup(unsigned int PlayerId, int Amount);
    // baseline ?GetName@Player@pub@@YAHIAAV?$basic_string@GU?$char_traits@G@std@@...@Z (0x75fe0):
    // VC6's wstring; ours takes std::wstring.
    SERVER_API int __cdecl GetName(unsigned int PlayerId, std::wstring& OutName);
    // ?IsGroupMember@Player@pub@@YAHIIAA_N@Z — Server.dll 0x76170 (server_pub.cpp). Content.dll
    // (06f481ae).
    SERVER_API int __cdecl IsGroupMember(unsigned int PlayerId, unsigned int OtherId, bool& OutResult);
    // ?RemoveFromGroup@Player@pub@@YAHI@Z — Server.dll 0x761c0 (server_pub.cpp). Content.dll
    // TU47 06f4c6ae.
    SERVER_API int __cdecl RemoveFromGroup(unsigned int PlayerId);
    SERVER_API int __cdecl SetMissionObjectives(const unsigned int& PlayerId, const unsigned int& MissionId, const MissionObjective* Objs, unsigned int Count, const FmtStr& Title, unsigned char Type, const FmtStr& Desc);
    SERVER_API int __cdecl ReplaceMissionObjective(const unsigned int& PlayerId, const unsigned int& MissionId, unsigned int Index, const MissionObjective& Obj);
    SERVER_API int __cdecl SetMissionObjectiveState(const unsigned int& PlayerId, const unsigned int& MissionId, int Index, unsigned int State);
}

} // namespace pub

// ============================================================================
// CRemotePhysicsSimulation — network physics interpolation for remote objects.

// baseline lives in COMMON.DLL, not Server.dll: its vftable is exported as
// ordinal 1023, ??_7CRemotePhysicsSimulation@@6B@ at module storage, and
// Server.dll only imports the class (Observer's ctor sets `this` with
// `LEA ECX,[ESI + 0x8]` and then calls
// COMMON.DLL::CRemotePhysicsSimulation::CRemotePhysicsSimulation at
// ).

// EXPORTS -- FOUR different answers in one class, which is why no single class-level
// macro could ever express it, and why the class-level SERVER_API this used to carry
// was wrong in both directions. SERVER_API is dllimport while Common.dll is being
// built, so our Common.dll exported none of these; baseline exports sixteen symbols.
// Read off both export tables:

//   member                        Common.dll        Server.dll
//   GetSimulationTime             0x23500 ord 1662  0x2380  ord 273
//   CalcPositionError             0x23510 ord 1253  0x2390  ord 70
//   GetSampleCount                0x23540 ord 1652  0x23c0  ord 259
//   GetLatestUpdate               0x23550 ord 1602  0x23d0  ord 200
//   GetSample                     0x23570 ord 1651  0x23f0  ord 258
//   SetLogging                    0x235a0 ord 1983  0x2420  ord 420
//   copy ctor   ??0...@ABV0@@Z    0x235b0 ord 179   0x2430  ord 8
//   operator=   ??4...            0x23b30 ord 742   0x29b0  ord 27
//   vftable     ??_7...@6B@       0x13b994 ord 1023 0x84f18 ord 38
//   default ctor ??0...@XZ        0x31c70 ord 180   --
//   dtor         ??1...@XZ        0x31d50 ord 571   --
//   Reset                         0x31d70 ord 1936  --
//   RemoteUpdate                  0x31e10 ord 1931  --
//   Doit                          0x31fe0 ord 1381  --
//   CheckForSync                  0x325c0 ord 1263  --
//   tracef                        0x31bb0 ord 3754  --

// (1) The eight members present in both columns are baseline's design: a real body in
//     each module. (2) Server.dll having its OWN ??_7 is baseline's design too.
// (3) That vftable's slots nevertheless resolving into Common.dll is also baseline --
//     slot 1 at module storage is a bare JMP to the imported RemoteUpdate, and slot 0
//     at module storage is the compiler's vector deleting destructor, which pushes the
//     local thunk at 06d5fcfc that JMPs to Common's imported ~CRemotePhysicsSimulation.
// (4) The seven Common-only members live in Common.dll alone, as in baseline: Server
//     imports every one -- module storage (ctor), 06cf531a (dtor), 06cf5527 (Doit),
// (Reset), 06cf5d34 (CheckForSync).

// OutpostFrontier.exe is the third module with the same eight: baseline OutpostFrontier.exe
// exports GetSimulationTime module storage ord 46, CalcPositionError 0053fce0 ord 36,
// GetSampleCount 0053fd10 ord 45, GetLatestUpdate 0053fd20 ord 42, GetSample 0053fd40
// ord 44, SetLogging 0053fd70 ord 54, copy ctor 0053fd80 ord 8, operator= 00540300
// ord 21 and ??_7 module storage ord 31. All three modules' copies were compared
// instruction by instruction (a script over Common, Server and
// OutpostFrontier, absolute addresses and CALL displacements masked): 2, 20, 2,
// 12, 14, 3, 355 and 443 instructions, identical in all three.

// HOW THE DECLARATIONS EXPRESS IT. One class-level SHARED_INLINE_API -- dllexport in
// Common.dll, Server.dll AND OutpostFrontier.exe, dllimport everywhere else -- the way
// baseline's header made the class dllexport in every module that compiled it:
//   * the eight shared members are defined INLINE below, which is what baseline did:
//     every module that compiles this header with SHARED_INLINE_API = dllexport
//     emits and exports its own copy, with its own ??_7 (emitted with the copy
//     constructor). There is exactly one body per member, here;
//   * the seven Common-only members have their only bodies in
//     src/Common/common_remotephysics.cpp. Server.dll and OutpostFrontier.exe declare them
//     through the same dllexport class but define none, so their references (the
//     ctor, Doit, Reset, CheckForSync calls, slot 1 of their ??_7, and the destructor
//     their deleting destructors call) bind to Common.lib's import thunks -- the
//     shape of baseline's module storage (JMP to the imported RemoteUpdate) and
// .
// Do NOT give the seven bodies to Server.dll or the exe to make a link error go away:
// two bodies for one function is how the Server copies drifted (singular basis in
// Doit, zeroed tolerances in CheckForSync, both fixed in 52da807) while Common's did
// not.

// SIZE. 0x2E8, not 0x2E4: the vector deleting destructor at module storage
// does `PUSH 0x2e8` as the per-element stride it hands the array-destroy
// helper. This declaration reaches 0x2E8 naturally — the trailing bool at
// +0x2E0 is padded out to the class's 8-byte alignment (m_SimulationTime).

// VTABLE. Exactly two slots, and the byte extent proves it: module storage
// is slot 0 (the vector deleting destructor, module storage), module storage
// is slot 1 (RemoteUpdate, module storage), and module storage is already the
// next symbol — a string literal ("Common\Phy...").
// This class's own RTTI complete-object-locator pointer sits one slot before
// the table at module storage, in the usual MSVC arrangement; the previous
// table, the 1-slot ??_7CDeadReckonedVector@@6B@ at module storage, has its
// own locator at module storage.

//   Three embedded CDeadReckonedVector sub-objects at +0x08, +0xD8, +0x1A8.
//   GetSample reads from the first DRV's sample ring at +0x10.
//   SampleCount at +0x2B8, SimulationTime (double) at +0x2C0, logging at +0x2E0.
// ============================================================================
class SHARED_INLINE_API CRemotePhysicsSimulation {
public:
    CRemotePhysicsSimulation();                                                  // Common only

    // Copy constructor -- Common module storage ord 179, Server module storage ord 8,
    // OutpostFrontier.exe module storage ord 8 (355 instructions each, RET 0x4).

    // baseline is the compiler-generated member-wise copy constructor, traced store by
    // store in Common (a script walked every MOV/LEA/REP MOVSD and recorded which
    // destination byte each store fills from which source byte). For each embedded
    // CDeadReckonedVector it is that class's own copy constructor, inlined:
    //   +0x08 DRV  -- REP MOVSD 9 dwords at +0x10 (Samples, 062835c3), REP MOVSD 6
    //                 dwords at +0x38 (Timestamps, 062835d0), then three-dword groups
    //                 +0x50 .. +0xb0, dwords +0xbc .. +0xd4, and finally the vptr,
    // `MOV [EAX+0x8],0x0639b98c` (??_7CDeadReckonedVector;
    //                 PE bytes c7 40 08 8c b9 39 06).
    //   +0xD8 DRV  -- same shape, vptr at module storage (c7 80 d8 00 00 00 8c b9 39 06).
    //   +0x1A8 DRV -- same shape, vptr at module storage (c7 80 a8 01 00 00 8c b9 39 06).
    // Then the trailing members: Vectors +0x278/+0x284/+0x290, the Quaternion +0x29c,
    // the Vector +0x2ac, the dword +0x2b8, dwords +0x2c0 .. +0x2dc, the byte +0x2e0
    // (module storage/module storage) -- and this's own vptr last (module storage,
    // ). The ONLY bytes it does not write are padding: +0x00..+0x07 before
    // the vptr store, DRV+0x04..+0x07 and DRV+0x2c..+0x2f in each sub-object, +0x2bc,
    // and +0x2e1..+0x2e7. CDeadReckonedVector's own copy constructor (implicit,
    // common_cobject.h, Common 0x231a0) is exactly that per-sub-object shape -- vptr
    // plus every field from +0x08 on. Each module stamps its OWN ??_7CDeadReckonedVector,
    // as baseline's do (Server's stores at module storage/06ce2707/06ce2887 write 0x06d64f10).
    // The doubles at +0x2c0/+0x2c8/+0x2d0/+0x2d8 are copied as dword pairs, which is how
    // VC6 copies any 8-byte member; the padding dword +0x2bc is skipped.
    CRemotePhysicsSimulation(const CRemotePhysicsSimulation& Other)
        : m_PositionDRV(Other.m_PositionDRV)
        , m_OrientAxisXDRV(Other.m_OrientAxisXDRV)
        , m_OrientAxisYDRV(Other.m_OrientAxisYDRV)
        , m_Velocity(Other.m_Velocity)                          // +0x278
        , m_BlendFromVelocity(Other.m_BlendFromVelocity)        // +0x284
        , m_BlendToVelocity(Other.m_BlendToVelocity)            // +0x290
        , m_Orientation(Other.m_Orientation)                    // +0x29c
        , m_ProjectedPosition(Other.m_ProjectedPosition)        // +0x2ac
        , m_SampleCount(Other.m_SampleCount)                    // +0x2b8
        , m_SimulationTime(Other.m_SimulationTime)              // +0x2c0
        , m_ConvergeWindow(Other.m_ConvergeWindow)              // +0x2c8
        , m_Unread2D0{Other.m_Unread2D0[0], Other.m_Unread2D0[1]}  // +0x2d0
        , m_SyncBackoff(Other.m_SyncBackoff)                    // +0x2d8
        , m_bLogging(Other.m_bLogging)                          // +0x2e0
    {
    }

    virtual ~CRemotePhysicsSimulation();                                         // vtable slot 0, Common only
    // vtable slot 1 (module storage -> module storage),
    // ?RemoteUpdate@CRemotePhysicsSimulation@@UAEXABVVector@@ABVQuaternion@@N@Z.
    // Feeds one network sample to the three dead-reckoning sub-objects: the
    // position verbatim, then the orientation quaternion's first two basis
    // COLUMNS, obtained from I3DMathEngine::QuaternionToMatrix (vtable +0x54).
    // `RET 0x10` at module storage fixes the arity at 16 bytes of arguments.
    // Common only.
    virtual void RemoteUpdate(Vector const& Position, Quaternion const& Orientation,
                              double Timestamp);

    // operator= -- Common module storage ord 742, Server module storage ord 27,
    // OutpostFrontier.exe module storage ord 21 (443 instructions each, RET 0x4). The
    // compiler-generated member-wise assignment: three inlined
    // CDeadReckonedVector::operator= (06283b39..06283cdd, 06283ce3..06283e8d,
    // each a 3-trip Vector loop for Samples, a 3-trip double
    // loop for Timestamps, then +0x48 .. +0xcc member by member), then the trailing
    // members +0x278 .. +0x2e0 (06284043 .. 0628414c). No vptr is written anywhere and
    // there is NO self-assignment test -- the first compare in the body is the Samples
    // loop counter at module storage -- so none is added here; copying an object onto
    // itself member by member is a no-op either way.
    CRemotePhysicsSimulation& operator=(const CRemotePhysicsSimulation& Other)
    {
        m_PositionDRV = Other.m_PositionDRV;
        m_OrientAxisXDRV = Other.m_OrientAxisXDRV;
        m_OrientAxisYDRV = Other.m_OrientAxisYDRV;
        m_Velocity = Other.m_Velocity;                                // +0x278
        m_BlendFromVelocity = Other.m_BlendFromVelocity;              // +0x284
        m_BlendToVelocity = Other.m_BlendToVelocity;                  // +0x290
        m_Orientation = Other.m_Orientation;                          // +0x29c
        m_ProjectedPosition = Other.m_ProjectedPosition;              // +0x2ac
        m_SampleCount = Other.m_SampleCount;                          // +0x2b8
        m_SimulationTime = Other.m_SimulationTime;                    // +0x2c0
        m_ConvergeWindow = Other.m_ConvergeWindow;                    // +0x2c8
        m_Unread2D0[0] = Other.m_Unread2D0[0];                        // +0x2d0
        m_Unread2D0[1] = Other.m_Unread2D0[1];                        // +0x2d4
        m_SyncBackoff = Other.m_SyncBackoff;                          // +0x2d8
        m_bLogging = Other.m_bLogging;                                // +0x2e0
        return *this;                                                 // EAX == this
    }

    // The accessors below read the position sub-object's fields directly instead of
    // calling CDeadReckonedVector's own accessors -- baseline does the same (its
    // CalcPositionError/GetLatestUpdate/GetSample bodies index `this` with the
    // position DRV's field offsets pre-added, e.g. 0x74 = m_PositionDRV(+0x08) +
    // TrackedPosition(+0x6c)).

    // CalcPositionError -- Common module storage .. module storage (RET 0x4), Server
    // , OutpostFrontier.exe module storage. Every term stays on the x87
    // stack: FLD float / FSUB float at 06283514..06283522 leave the three differences
    // unrounded, and the FSQRT result (06283537) is returned in ST0 without a store.
    // Summation order from the register shuffle: FMULP ST3 squares Dx, FMUL ST1 +
    // FADDP ST3 adds Dz^2 to it, FMUL ST2 + FADDP ST3 adds Dy^2 -- (Dx^2 + Dz^2) +
    // Dy^2. Nothing narrows to float, so the port holds every intermediate in double,
    // the SSE2 counterpart of an unstored x87 register.
    double CalcPositionError(Vector const& Point)
    {
        Vector const& Tracked = m_PositionDRV.TrackedPosition;      // this+0x74
        double const Dx = (double)Point.x - Tracked.x;
        double const Dy = (double)Point.y - Tracked.y;
        double const Dz = (double)Point.z - Tracked.z;
        return sqrt((Dx * Dx + Dz * Dz) + Dy * Dy);
    }

    // GetLatestUpdate -- Common module storage, Server module storage, OutpostFrontier.exe
    // . Copies the three dwords at this+0x10/+0x14/+0x18 (the position
    // DRV's Samples[0]) into the hidden return Vector.
    Vector GetLatestUpdate()
    {
        return m_PositionDRV.Samples[0];                             // this+0x10
    }

    // GetSample -- Common module storage, Server module storage, OutpostFrontier.exe
    // . The Vector at this + 0x10 + Index*0xC, no bounds check.
    Vector GetSample(int Index)
    {
        return m_PositionDRV.Samples[Index];                         // this+0x10+Index*0xc
    }

    // GetSampleCount -- Common module storage, Server module storage, OutpostFrontier.exe
    // MOV EAX,[this+0x2b8]; RET.
    unsigned long GetSampleCount()
    {
        return m_SampleCount;
    }

    // GetSimulationTime -- Common module storage, Server module storage, OutpostFrontier.exe
    // FLD double [this+0x2c0]; RET.
    double GetSimulationTime()
    {
        return m_SimulationTime;
    }

    // SetLogging -- Common module storage, Server module storage, OutpostFrontier.exe
    // byte store to +0x2e0, RET 0x4.
    void SetLogging(bool bEnable)
    {
        m_bLogging = bEnable;
    }

    // Common only from here to the end of the public section.
    // Doit — advance simulation by Dt seconds (Server.dll's Observer update).
    void Doit(double Dt);

    // Common 0x325c0: decides whether the caller should force a resync of this
    // object's dead-reckoned state. Param1/Param2 are two independently-sampled positions
    // (their separation scales the tolerance below); Param3 is the orientation to check
    // against the tracked orientation/velocity-derived basis vectors. See the .cpp for the
    // full breakdown (raw-analysis reconstruction — analysis's pseudo-C for this function is
    // unreliable, see the comment on the implementation).
    bool CheckForSync(Vector const& Param1, Vector const& Param2, Quaternion const& Param3);

    // Common 0x31d70: re-primes the trailing scratch/sync fields (sample count,
    // simulation time, sync backoff) without touching the DRV sub-objects or m_bLogging.
    void Reset();

    // Common 0x31bb0: gated on m_bLogging. Writes to c:\OutpostServerTrace.txt (MP server,
    // handle kept open across calls) or appends to c:\client.updates (single-player/client).
    void tracef(char const* Format, ...);

public:
    // Data members are public: PhySys::GetVelocity (module storage) reads m_Velocity
    // (+0x278) of a CRemotePhysicsController's simulation base directly.

    // There is NO explicit padding member between the vtable pointer and the
    // first sub-object, even though baseline's first DRV sits at +0x08 and the
    // vptr occupies only +0x00..+0x03. MSVC pads a polymorphic class whose
    // alignment exceeds the pointer size -- this one is 8-aligned because of
    // its double members -- so that the data portion starts on the class's own
    // alignment boundary. Declaring `unsigned char m_Pad04[4]` here duplicated
    // that padding: it pushed the first DRV to +0x0C, cascaded 4 bytes through
    // every later offset, and took sizeof to 0x2F0 instead of baseline's 0x2E8.
    // Three CDeadReckonedVector sub-objects (common_cobject.h, 0xD0 each). baseline
    // stamps their vptrs in the default ctor (module storage/c8b/c9c, each followed by
    // a call to CDeadReckonedVector::Reset -- the inlined default constructor) and the
    // copy ctor (module storage/06283887/06283a07), and its destructor (module storage)
    // re-stamps them -- the inlined empty CDeadReckonedVector destructors, which the
    // compiler's member destruction expresses. Every method the simulation calls on
    // them is non-virtual.

    // NONE OF THE THREE IS A VELOCITY. RemoteUpdate (module storage) feeds
    // them: the position sample to the first, and to the other two the first
    // and second COLUMNS of the rotation matrix I3DMathEngine::
    // QuaternionToMatrix builds from the sample's orientation quaternion --
    // /module storage/module storage read the out-matrix at
    // +0x00/+0x0C/+0x18 (element [i][0]) and module storage/module storage/
    // read it at +0x04/+0x10/+0x1C (element [i][1]). Those are
    // the object's world-space X and Y axes; the Z axis is never tracked,
    // because two orthonormal axes already determine the third.
    // CheckForSync agrees independently: it compares the SAME two columns
    // against this+0x144 (= +0xD8 + TrackedPosition) and this+0x214
    // (= +0x1A8 + TrackedPosition).
    CDeadReckonedVector m_PositionDRV;        // +0x08   position
    CDeadReckonedVector m_OrientAxisXDRV;     // +0xD8   orientation basis column 0
    CDeadReckonedVector m_OrientAxisYDRV;     // +0x1A8  orientation basis column 1
    // The velocity Doit() publishes this tick (module storage..06292333): a lerp from
    // m_BlendFromVelocity to m_BlendToVelocity inside the blend window, the position DRV's
    // ProjectVelocity after it. PhySys::GetVelocity returns it (module storage), the
    // controller's do_simulation_controller copies it into the golem's TargetSpeed
    // (module storage) and the controller's ctor seeds it with the initial velocity.
    Vector m_Velocity;                        // +0x278
    // m_Velocity as of the last RemoteUpdate (module storage): the lerp's "from".
    Vector m_BlendFromVelocity;               // +0x284
    // Velocity + 0.5*Acceleration*Window^2 of the new fit, computed by RemoteUpdate
    // (module storage..06291fa4): the lerp's "to".
    Vector m_BlendToVelocity;                 // +0x290
    // The orthonormal basis Doit() rebuilds each tick, written by
    // I3DMathEngine::MatrixToQuaternion (module storage) in (W,X,Y,Z) order.
    Quaternion m_Orientation;                 // +0x29C
    // Mirror of m_PositionDRV.ProjectedPosition (module storage..062922cd).
    Vector m_ProjectedPosition;               // +0x2AC
    unsigned long m_SampleCount;              // +0x2B8
    // (+0x2BC: alignment padding, not copied by the copy ctor / operator=)
    double m_SimulationTime;                  // +0x2C0
    // The blend window: RemoteUpdate copies m_PositionDRV.ConvergeWindow here
    // (module storage/module storage), Reset zeroes it, Doit compares the clock with it.
    double m_ConvergeWindow;                  // +0x2C8
    // Eight bytes the copy ctor and operator= copy as two dwords; no other Common.dll code
    // reads or writes them (the ctor and Reset leave them alone). Their type is unknown.
    unsigned long m_Unread2D0[2];             // +0x2D0
    // "Force a resync" backoff timer used by CheckForSync (baseline 0x325c0). Seeded to 5.0 by
    // the ctor/Reset; CheckForSync doubles it each time m_SimulationTime outgrows it
    // (geometric backoff) and resets it back to 5.0 once a resync is actually forced.
    double m_SyncBackoff;                     // +0x2D8
    bool m_bLogging;                          // +0x2E0
};

static_assert(offsetof(CRemotePhysicsSimulation, m_PositionDRV) == 0x08, "CRPS +0x08");
static_assert(offsetof(CRemotePhysicsSimulation, m_OrientAxisXDRV) == 0xD8, "CRPS +0xd8");
static_assert(offsetof(CRemotePhysicsSimulation, m_OrientAxisYDRV) == 0x1A8, "CRPS +0x1a8");
static_assert(offsetof(CRemotePhysicsSimulation, m_Velocity) == 0x278, "CRPS +0x278");
static_assert(offsetof(CRemotePhysicsSimulation, m_Orientation) == 0x29C, "CRPS +0x29c");
static_assert(offsetof(CRemotePhysicsSimulation, m_ProjectedPosition) == 0x2AC, "CRPS +0x2ac");
static_assert(offsetof(CRemotePhysicsSimulation, m_SampleCount) == 0x2B8, "CRPS +0x2b8");
static_assert(offsetof(CRemotePhysicsSimulation, m_SimulationTime) == 0x2C0, "CRPS +0x2c0");
static_assert(offsetof(CRemotePhysicsSimulation, m_ConvergeWindow) == 0x2C8, "CRPS +0x2c8");
static_assert(offsetof(CRemotePhysicsSimulation, m_SyncBackoff) == 0x2D8, "CRPS +0x2d8");
static_assert(offsetof(CRemotePhysicsSimulation, m_bLogging) == 0x2E0, "CRPS +0x2e0");

// baseline's own element stride, read off the vector deleting destructor
// (module storage `PUSH 0x2e8`). No STL member anywhere in the class, so the
// assertion is safe to make here.
static_assert(sizeof(CRemotePhysicsSimulation) == 0x2E8, "CRemotePhysicsSimulation sizeof 0x2e8");

// ============================================================================
// CRemotePhysicsController -- the object PhySys::RCBegin hangs off CObject+0x60 for a
// remotely simulated object. NOT to be confused with CRemotePhysicsSimulation above: this
// class HAS one, as its second base.

// RTTI (Common.dll): TypeDescriptor module storage ".?AVCRemotePhysicsController@@",
// Class Hierarchy Descriptor module storage (attributes 1 = multiple inheritance, six
// entries at module storage): CRemotePhysicsController, IVP_Controller_Golem,
// IVP_Controller_Motion, IVP_Controller_Independent, IVP_Controller, and
// CRemotePhysicsSimulation with mdisp 0xd0. Two Complete Object Locators: module storage
// (offset 0, referenced by module storage) and module storage (offset 0xd0, referenced
// by module storage).

// VFTABLES.
// (for IVP_Controller_Golem, at +0x00), eight slots:
//     [0] 06342fc0 [1] 0627cd20 [2] 06285a60 [3] 063435f0  -- inherited: Motion / IVP_Controller / Independent / Golem
//     [4] 062923e0 do_simulation_controller   (override)
//     [5] 06285a70 get_controller_priority      (IVP_Controller_Motion's, 500)
//     [6] 06285a80 scalar deleting destructor   (dtor body the routine)
//     [7] 06292570 beam_to_target               (override of the pure slot)
// (for CRemotePhysicsSimulation, at +0xd0), two slots:
//     [0] 06285de0 `SUB ECX,0xd0; JMP the routine` -- the this-adjusting destructor thunk
//     [1] 062923a0 RemoteUpdate                 (override)

// LAYOUT (operator new(0x3c0), module storage):
//   +0x000  IVP_Controller_Golem     (0xd0)
//   +0x0d0  CRemotePhysicsSimulation (0x2e8) -- constructed at module storage
//   +0x3b8  IVP_Real_Object* RealObject -- module storage `MOV [ESI+0x3b8],EBX`, EBX = the
//           CObject's +0x54 body (module storage). Not read by any Common.dll code that
//           names this class; kept because baseline stores it.
// sizeof reaches 0x3c0 through CRemotePhysicsSimulation's 8-byte alignment.

// Not exported by baseline Common.dll (no CRemotePhysicsController symbol in its export
// table), so no export macro. Server.dll only ever NAMES it: the __RTDynamicCast at
// (the routine), which compares type descriptors by name across modules,
// then reads the CRemotePhysicsSimulation base's sample count (+0x388 = +0xd0 + 0x2b8)
// and latest sample (+0xe0 = +0xd0 + 0x10) through that base's inline accessors.
// Defined in src/Common/common_remotephysics.cpp.
// ============================================================================
class CRemotePhysicsController : public IVP_Controller_Golem, public CRemotePhysicsSimulation
{
public:
    // Inlined by baseline into PhySys::RCBegin (module storage .. module storage): the
    // golem base, the simulation base, the two vptrs, RemoteUpdate(Pos, Orient, 0.0)
    // through THIS class's override (the routine called directly at module storage),
    // then the velocity into +0x348, the body into +0x3b8 and the sample count at +0x388
    // back to zero.
    CRemotePhysicsController(IVP_Real_Object* InBody, FLControllerGolemParms const& Parms,
                             Vector const& Position, Quaternion const& Orientation,
                             Vector const& Velocity);

    // (body) / the routine (scalar deleting) / the branch (thunk).
    virtual ~CRemotePhysicsController();

    // vftable 0639ba38 slot 4.
    virtual void do_simulation_controller(IVP_Event_Sim* EventSim,
                                          IVP_U_Vector<IVP_Core>* AffectedCores) override;

    // vftable 0639ba38 slot 7.
    virtual int beam_to_target(IVP_Event_Sim const* EventSim, int Reason) override;

    // vftable 0639ba2c slot 1 (RET 0x10).
    virtual void RemoteUpdate(Vector const& Position, Quaternion const& Orientation,
                              double Timestamp) override;

public:
    IVP_Real_Object* RealObject;  // +0x3b8
};

static_assert(sizeof(CRemotePhysicsController) == 0x3C0,
              "CRemotePhysicsController is operator new(0x3c0) at module storage");

// ============================================================================
// OwnerList<pub::SpaceObj::CargoDesc>. The MetaNode/MetaList/OwnerList templates
// live in common_metalist.h. baseline exports the six OwnerList<CargoDesc> members
// (??0 default and copy, ??1, ??4, ??_7, free) from Common.dll (ords 3, 4, 495,
// 697, 962, 2562), Server.dll (1, 2, 18, 23, 34, 483) AND OutpostFrontier.exe (1, 2,
// 14, 15, 25, 58), with identical bodies, so each of the three modules expands
// OWNERLIST_CARGODESC_EXPORTS once (src/Common/common_goodinfo.cpp,
// src/Server/server_classes.cpp, src/OutpostFrontier/Client/client_objdb.cpp).
// ============================================================================
namespace pub {
namespace SpaceObj {
    // Forward declaration -- full definition is in server_cobject.h when needed.
    // Nothing in OwnerList needs it complete: Data is freed with operator delete.
    struct CargoDesc;

    // pub::SpaceObj::SetInvincible — Server.dll module storage. Body: server_pub.cpp.
    // Content.dll the routine at 06f14822. Duration is the dword at the list node +0x3c.
    SERVER_API int __cdecl SetInvincible(unsigned int ObjId, bool bInvincible, bool Param3,
                                        float Duration);
    // ?SetInvincible2@SpaceObj@pub@@YAHI_N0M@Z -- body server_pub.cpp. Content.dll TU61
    // at 06f911b3 (IAT 06fb35ac): (id, false, true, 1.0f) for a critical loot.
    SERVER_API int __cdecl SetInvincible2(unsigned int ObjId, unsigned char bInvincible,
                                         unsigned char Param3, float Duration);
    // baseline06cf88a0 writes the two raw getter bytes into its output references.
    SERVER_API int __cdecl GetInvincible(unsigned int ObjId, unsigned char& OutFromPlayers,
                                         unsigned char& OutProtect, float& OutMaxHpLoss);

    // pub::SpaceObj::GetType (?GetType@SpaceObj@pub@@YAHIAAI@Z) — Server.dll RVA 0x18da0.
    // Body: server_pub.cpp. Content.dll the routine at 06f683ba.
    SERVER_API int __cdecl GetType(unsigned int ObjId, unsigned int& OutType);
    // ?GetJumpTarget@SpaceObj@pub@@YAHABIAAI1@Z -- Server.dll 0x19a40, body server_pub.cpp:3903.
    // Content.dll the routine (06f47526) keeps the third value (the target object).
    SERVER_API int __cdecl GetJumpTarget(const unsigned int& ObjId, unsigned int& OutSystemId,
                                         unsigned int& OutObjectId);

    // ?GetDockingTarget@SpaceObj@pub@@YAHABIAAI@Z — Server.dll 0x19880. Body:
    // server_pub.cpp. Content.dll the routine (06f749a5), the routine.
    SERVER_API int __cdecl GetDockingTarget(const unsigned int& ObjId, unsigned int& OutTarget);

    // ?GetRep@SpaceObj@pub@@YAHIAAH@Z — Server.dll 0x18ba0. Body: server_pub.cpp.
    // Content.dll Cnd_WatchVibe (the branch).
    SERVER_API int __cdecl GetRep(unsigned int ObjId, int& OutRep);

    // ?GetRelativeHealth@SpaceObj@pub@@YAHABIAAM@Z — Server.dll 0x20551. Body: server_pub.cpp.
    // Content.dll Cnd_HealthDec (the branch), Act_AdjHealth.
    SERVER_API int __cdecl GetRelativeHealth(unsigned int const& ObjId, float& OutRatio);

    // ?SetRelativeHealth@SpaceObj@pub@@YAHABIM@Z — body server_pub.cpp:3793.
    // Content.dll Act_AdjHealth (the routine 06ec503f).
    SERVER_API int __cdecl SetRelativeHealth(const unsigned int& ObjId, float Ratio);

    // ?LightFuse@SpaceObj@pub@@YAHABIPBDM@Z — body server_pub.cpp:3841.
    // Content.dll Act_LightFuse (the branch 06ec5a6e).
    SERVER_API int __cdecl LightFuse(const unsigned int& ObjId, const char* FuseName, float Delay);

    // ?RequestSpaceScript@SpaceObj@pub@@YAHABIABVVector@@ABHIPBD@Z — body server_pub.cpp:3948.
    // Content.dll Act_CallSceneScript (the branch 06ec5503): (object, position, mode, player,
    // script path). The fourth argument is the client the request goes to: Server.dll hands
    // it to IClient::SendServerRequestSpaceScript as the ClientId (module storage).
    SERVER_API int __cdecl RequestSpaceScript(const unsigned int& ObjId, const Vector& Pos,
                                              const int& Priority, unsigned int ClientId,
                                              const char* ScriptName);

    // ?GetLocation@SpaceObj@pub@@YAHIAAVVector@@AAVMatrix@@@Z — Server.dll 0x18732.
    // Body: server_pub.cpp. Content.dll Cnd_CmpToPlane (the branch), Cnd_DistVec, ...
    SERVER_API int __cdecl GetLocation(unsigned int ObjId, Vector& OutPos, Matrix& OutOrient);

    // ?GetSystem@SpaceObj@pub@@YAHIAAI@Z — Server.dll module storage. Body: server_pub.cpp.
    // Content.dll Cnd_DistShip (the branch).
    SERVER_API int __cdecl GetSystem(unsigned int ObjId, unsigned int& OutSystemId);

    // ?GetRadius@SpaceObj@pub@@YAHABIAAMAAVVector@@@Z — Server.dll. Body: server_pub.cpp.
    // Content.dll Cnd_DistCircle (the routine 06ec04fb).
    SERVER_API int __cdecl GetRadius(unsigned int const& ObjId, float& OutRadius,
                                     Vector& OutCenter);

    // ?GetTarget@SpaceObj@pub@@YAHABIAAI@Z — Server.dll. Body: server_pub.cpp.
    // Content.dll Cnd_PlayerManeuver (the branch 06ec2057).
    SERVER_API int __cdecl GetTarget(unsigned int const& ObjId, unsigned int& OutTargetId);

    // ?GetSolarRep@SpaceObj@pub@@YAHIAAH@Z — Server.dll. Body: server_pub.cpp.
    // Content.dll Act_SetVibeOfferBaseHack (the routine 06ecac21).
    SERVER_API int __cdecl GetSolarRep(unsigned int SolarId, int& OutRep);

    // ?GetSolarParent@SpaceObj@pub@@YAHABIAAI@Z — Server.dll 0x18d00. Body: server_pub.cpp.
    // Content.dll TU56 the routine (06f7000f).
    SERVER_API int __cdecl GetSolarParent(unsigned int const& ObjId, unsigned int& OutParentId);

    // ?GetTradelaneNextAndPrev@SpaceObj@pub@@YAHABIAAI1@Z — Server.dll 0x1ac00. Body:
    // server_pub.cpp:3818. Content.dll TU58 the routine (06f771f6 ...).
    SERVER_API int __cdecl GetTradelaneNextAndPrev(const unsigned int& obj_id_ref,
                                                   unsigned int& out_next, unsigned int& out_prev);

    // ?GetMotion@SpaceObj@pub@@YAHIAAVVector@@0@Z — Server.dll 0x18b20. Body: server_pub.cpp.
    // Content.dll the routine (TU20 space object).
    SERVER_API int __cdecl GetMotion(unsigned int ObjId, Vector& OutLinear, Vector& OutAngular);

    // ?GetArchetypeID@SpaceObj@pub@@YAHABIAAI@Z — Server.dll: OutArchId = Good2Arch(GoodId).
    // Body: server_pub.cpp. Content.dll TU48 the routine (06f4fa69, scan_for_cargo).
    SERVER_API int __cdecl GetArchetypeID(unsigned int const& GoodId, unsigned int& OutArchId);

    // ?GetCargoSpaceOccupied@SpaceObj@pub@@YAHABIAAI@Z — Server.dll 0x196d0. Body:
    // server_pub.cpp. Content.dll the routine (TU50 ship base).
    SERVER_API int __cdecl GetCargoSpaceOccupied(unsigned int const& ObjId, unsigned int& OutOccupied);

    // ?SendComm@SpaceObj@pub@@YAHIIIPBUCostume@@IPAIHIM_N@Z — Server.dll module storage.
    // Body: server_pub.cpp. Content.dll the routine (06f60e04), Act_SendComm (06ec8a16).
    SERVER_API int __cdecl SendComm(unsigned int SenderId, unsigned int TargetId,
                                     unsigned int VoiceId, const Costume* CommCostume,
                                     unsigned int CommType, unsigned int* TextIds,
                                     int TextCount, unsigned int Mood, float Duration,
                                     bool bInterrupt);

    // ?DrainShields@SpaceObj@pub@@YAHI@Z — body server_pub.cpp. Content.dll
    // Act_DisableTradelane (the branch).
    SERVER_API int __cdecl DrainShields(unsigned int ObjId);

    // ?ExistsAndAlive@SpaceObj@pub@@YAHI@Z -- Server.dll 0x18870, body server_pub.cpp. 0 when
    // the object exists and is alive, -2 otherwise. Content.dll GCS player line CanPlay
    // (the routine, 06ed674b).
    SERVER_API int __cdecl ExistsAndAlive(unsigned int ObjId);

    // ?GetVoiceID@SpaceObj@pub@@YAHABIAAI@Z — body server_pub.cpp. Content.dll NPC ship
    // tether/lead callbacks (06f342b3, 06f34479).
    SERVER_API int __cdecl GetVoiceID(unsigned int const& ObjId, unsigned int& OutVoiceId);

    // ?DockRequest@SpaceObj@pub@@YAHABI0@Z — body server_pub.cpp. Content.dll
    // Act_DockRequest (the branch): (ship, dock target).
    SERVER_API int __cdecl DockRequest(const unsigned int& ObjId, const unsigned int& TargetId);

    // ?Dock@SpaceObj@pub@@YAHABI0HW4DOCK_HOST_RESPONSE@@@Z -- Server.dll 0x1a6e0, body
    // server_pub.cpp. Content.dll TU53 dock queues (the routine, the routine,
    // ): (ship, dock target, dock port or -1, response).
    SERVER_API int __cdecl Dock(const unsigned int& ShipId, const unsigned int& TargetId,
                                int DockPort, DOCK_HOST_RESPONSE Response);

    // ?Relocate@SpaceObj@pub@@YAHABI0ABVVector@@ABVMatrix@@@Z -- Server.dll 0x1a370, body
    // server_pub.cpp. Content.dll TU53 the routine (06f67756).
    SERVER_API int __cdecl Relocate(const unsigned int& ObjId, const unsigned int& SystemId,
                                    const Vector& Position, const Matrix& Orientation);

    // ?JumpIn@SpaceObj@pub@@YAHABI0@Z -- Server.dll module storage, body server_pub.cpp.
    // Content.dll TU58 the routine (06f7391e), TU53 06f60b48: (ship, gate).
    SERVER_API int __cdecl JumpIn(const unsigned int& ShipId, const unsigned int& GateId);

    // ?GetHardpoint@SpaceObj@pub@@YAHABIPBDPAVVector@@PAVMatrix@@@Z -- Server.dll 0x18e70,
    // body server_pub.cpp. Content.dll TU53 the routine (06f6be16), the routine (06f6c715).
    SERVER_API int __cdecl GetHardpoint(const unsigned int& ObjId, const char* HardpointName,
                                        Vector* OutPosition, Matrix* OutOrientation);

    // ?Launch@SpaceObj@pub@@YAHABI0H@Z -- Server.dll module storage, body server_pub.cpp.
    // Content.dll TU53 the routine (06f60c5d): (ship, dock target it leaves, 0).
    SERVER_API int __cdecl Launch(const unsigned int& LaunchObjId, const unsigned int& DockObjId,
                                  int LaunchType);

    // ?Activate@SpaceObj@pub@@YAHABI_NH@Z -- Server.dll module storage, body server_pub.cpp.
    // Content.dll TU53 the routine (06f6bca4 / 06f6bda2): (dock target, occupied, terminal).
    SERVER_API int __cdecl Activate(const unsigned int& ObjId, bool bActivate, int Param);

    // ?FormationResponse@SpaceObj@pub@@YAHABIW4FORMATION_RTYPE@@@Z -- Server.dll
    // , body server_pub.cpp. Content.dll SpaceSupport msg 0x19 (06f5dc88).
    SERVER_API int __cdecl FormationResponse(const unsigned int& ObjId, FORMATION_RTYPE Response);

    // ?LaneResponse@SpaceObj@pub@@YAHABIH@Z -- Server.dll module storage, body server_pub.cpp.
    // Content.dll TU55 the routine (06f6fd67): (ship, response 1/4/5).
    SERVER_API int __cdecl LaneResponse(const unsigned int& ObjId, int Response);

    // ?GetScannerRange@SpaceObj@pub@@YAHIAAH0@Z -- body server_pub.cpp (GetScannerRange).
    // Content.dll TU37 the routine (06f0c457): the patrol scanner keeps the second value.
    SERVER_API int __cdecl GetScannerRange(unsigned int ObjId, int& OutRadarRange,
                                           int& OutCargoScanRange);

    // ?JettisonEquipment@SpaceObj@pub@@YAHABIABGABH@Z -- body server_pub.cpp
    // (JettisonEquipment). Content.dll TU37 the branch (06f0d260): (ship, equip id, count).
    SERVER_API int __cdecl JettisonEquipment(const unsigned int& ObjId,
                                             const unsigned short& EquipId, const int& Quantity);
} // namespace SpaceObj

// ?ReportFreeTerminal@pub@@YAHIH@Z -- Server.dll module storage, body server_pub.cpp.
// Content.dll SpaceSupport msg 0x32 (06f5e2cf): (dock target, terminal index).
SERVER_API int __cdecl ReportFreeTerminal(unsigned int DockTargetId, int TerminalIndex);

// ?FindHardpoint@pub@@YAHPBDIAAVVector@@AAVMatrix@@@Z -- Server.dll RVA 0x7e270, body
// server_pub.cpp. Content.dll TU53 the routine (06f60583): ("HpMount", ship arch, ...).
// 0 found, -2 the archetype failed to load, -3 no such hardpoint.
SERVER_API int __cdecl FindHardpoint(char const* Name, unsigned int ShipArchId, Vector& OutPos,
                                     Matrix& OutRot);
} // namespace pub

static_assert(sizeof(OwnerList<pub::SpaceObj::CargoDesc>) == 0x14, "OwnerList sizeof 0x14");

// Member-level export on purpose: class-level dllexport (or an extern-template base) also exports
// all of MetaList<CargoDesc>, which baseline never exports; MSVC member-level export has no ??_7, the one known gap (toy test 2026-09-24).
#define OWNERLIST_CARGODESC_EXPORTS                                                               \
    template SHARED_INLINE_API OwnerList<pub::SpaceObj::CargoDesc>::OwnerList();                  \
    template SHARED_INLINE_API OwnerList<pub::SpaceObj::CargoDesc>::OwnerList(                    \
        OwnerList<pub::SpaceObj::CargoDesc> const&);                                              \
    template SHARED_INLINE_API OwnerList<pub::SpaceObj::CargoDesc>::~OwnerList();                 \
    template SHARED_INLINE_API OwnerList<pub::SpaceObj::CargoDesc>&                               \
        OwnerList<pub::SpaceObj::CargoDesc>::operator=(OwnerList<pub::SpaceObj::CargoDesc> const&); \
    template SHARED_INLINE_API void OwnerList<pub::SpaceObj::CargoDesc>::free();
