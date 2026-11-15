// Author: Giperion.

#pragma once

// ===========================================================================
// server_createinfo.h -- the CREATE* record types Server.dll hands to IClient,
// the builders that fill them, and the ONE declaration rpclocal.dll and
// OutpostFrontier.exe read the same records through.

// NAMES. These are baseline's own, read out of Server.dll's RTTI rather than
// invented. The record the CREATESHIP branch builds carries a vftable at +0x00
// (.rdata:the branch), and the dword at [vftable - 4] is a
// RTTICompleteObjectLocator:
// 4c87d806  addr  .data:CShipCreateInfo::RTTI_Type_Descriptor
// while the base vftable the base constructor stamps
// (.rdata:the branch, module storage) has its own locator at
// , whose type descriptor is
// 2c87d806  addr  .data:CEqObjCreateInfo::RTTI_Type_Descriptor
// The solar sibling's table is .rdata:the branch (stamped at
// and module storage), locator module storage, descriptor
// pointer module storage -> .data:CSolarCreateInfo::RTTI_Type_Descriptor.
// The raw descriptor strings are
// char[24]  ".?AUCEqObjCreateInfo@@"
// char[24]  ".?AUCShipCreateInfo@@"
// char[24]  ".?AUCSolarCreateInfo@@"
// `.?AU` is `struct`.

// ONE TYPE, THREE MODULES. The record crosses two module boundaries:
// Observer hands it by reference to IClient::SendServerCreateShip (CREATESHIP) /
// SendServerCreateSolar (CREATESOLAR), rpclocal's marshaller reads it back
// field by field, and rpclocal's receiver delivers its own static copy to the
// client's IClient. baseline compiled the same struct into both DLLs from a
// shared header with INLINE members -- rpclocal.dll carries its own copies of
// every one of them, with its own vftables:

//   member                        Server.dll        rpclocal.dll
//   CEqObjCreateInfo::ctor        module storage    module storage
//   CEqObjCreateInfo::~dtor       module storage    module storage
//   CEqObjCreateInfo::clear       module storage    module storage
//   CShipCreateInfo::clear        module storage    module storage
//   CEqObjCreateInfo vftable      module storage   module storage
//   CShipCreateInfo vftable       module storage   module storage
//   CSolarCreateInfo vftable      module storage   module storage

// The two copies are byte-for-byte the same code (same stores, same order, the
// same two operator_new(0x10)/(0x14) list sentinels). So the port does what
// baseline did: the members are defined inline below and every module that
// constructs one gets its own copy. Nothing is exported, and nothing here is
// baseline ABI -- the two std::list members and the DamageList are THIS
// toolchain's containers on both sides of the call, which is exactly why both
// sides must see this one declaration and no private mirror of it.

// The members that sit inside Common.dll's own classes (DamageList,
// EquipDescVector, CollisionGroupDesc, ID_String) are Common.dll's published
// types, included, never redeclared.
// ===========================================================================

#include <list>

#include "common_cobject.h"    // CObject / CShip / CLoot / CGuided, CollisionGroupDesc
#include "common_goods.h"      // EquipDescVector
#include "common_id.h"         // ID_String
#include "common_misc.h"       // DamageList, DamageEntry
#include "common_physys.h"     // Quaternion

// ---------------------------------------------------------------------------
// Shared helper: the matrix -> quaternion conversion every builder does through
// the lazily-acquired I3DMathEngine singleton at .data:the field.
// MOV EAX,[.data:the field]
// JNZ the branch
// CALL the routine            ; Acquire3DMathEngine
// CALL dword ptr [ECX + 0x50]  ; MatrixToQuaternion
// The same three-instruction preamble is repeated verbatim at module storage
// (loot), module storage (countermeasure/mine) and module storage (guided).
// Server.dll-only; defined in src/Server/server_createinfo.cpp.
// ---------------------------------------------------------------------------
void Server_MatrixToQuat(Quaternion& Out, Matrix const& In);

// The inverse, through the same file-static engine (.data:the field):
// I3DMathEngine slot +0x54, QuaternionToMatrix. The two object builders call
// it on their request's +0x58 quaternion to rebuild CreateParms' 3x3:
// (solar)  module storage lazy fetch, module storage CALL [ECX+0x54]
// (ship)   module storage lazy fetch, module storage CALL [ECX+0x54]
// No null test on the engine in either. Defined in server_createinfo.cpp.
void Server_QuatToMatrix(Matrix& Out, Quaternion const& In);

// ---------------------------------------------------------------------------
// (module storage, 12 instructions, no arguments). Server.dll's own
// "3DMathEngine" acquire: a 0xC-byte descriptor on the stack
// {0xc, "3DMathEngine", 0} (module storage / module storage / module storage),
// NEURONCOMPONENTS_Acquire() (module storage) and the manager's vtable +0x0c,
// ICOManager::CreateInstance(&desc, &out) (module storage), returning `out`.
// It does NOT cache and checks nothing. The caching is at the call sites, one
// file-static per baseline translation unit, each in the MSVC lazy idiom
// `MOV EAX,[DAT] / CMP EAX,EBX / JNZ / CALL the routine / MOV [DAT],EAX`:
//   .data:the field  Observer.cpp  (module storage, 06cf5eea, 06cf7204)
//   .data:the field  StarSys.cpp   (module storage, 06d07997, 06d07c0a, ...)
//   .data:the field  pub::Player::SetCamera (module storage)
// (and the field / the field / the field in three more files).
// Server.dll-only; defined in src/Server/server_createinfo.cpp.
// ---------------------------------------------------------------------------
struct I3DMathEngine;
I3DMathEngine* Server_Acquire3DMathEngine();

// ===========================================================================
// CEqObjCreateInfo -- baseline size 0xac: both constructors' last store is the
// byte at +0xa8 (module storage / module storage) and both base clear()s clear
// it (module storage / module storage). +0xa8 is therefore a BASE member, which
// is also why CREATESHIP (payload+0xc8) and CREATESOLAR (payload+0x7d) both
// carry it (rpclocal module storage / module storage).
// ===========================================================================
struct CEqObjCreateInfo
{
    // / rpclocal module storage. baseline's store trail, EBX == 0
    // and EAX == -1 throughout:
    // +0x10 = -1   06cf6414..6426 +0x04/+0x08/+0x0c/+0x14..+0x20 = 0
    // +0x44 = 0    06cf642c +0x68 = -1   06cf6433 +0x48 = 0
    // +0x6c = 0    06cf643d..6446 the vector at +0x70 (allocator
    //                          byte, then _First/_Last/_End = 0)
    // ..6469 +0x80 list: operator_new(0x10) sentinel, _Size 0
    // ..6494 +0x8c list: operator_new(0x14) sentinel, _Size 0
    // ..64ac +0x98 byte / +0x9c / +0xa0 / +0xa4 = 0  (the DamageList
    //                          tail -- Common's DamageList() does the same)
    // +0xa8 byte = 0     06cf64bb the base vftable
    // baseline leaves +0x24..+0x43 (accessory array) and +0x4c..+0x64 (position,
    // quaternion) as they lie; every fill overwrites them. The port zeroes them
    // so that no reader ever sees indeterminate bytes -- no branch depends on
    // it and every writer overwrites them, so it changes nothing baseline does.
    CEqObjCreateInfo()
        : ObjId(0u)
        , ArchetypeNickname(0u)
        , DockingBaseId(0u)
        , VibeGroupId(-1)
        , CostumeHead(0u)
        , CostumeBody(0u)
        , CostumeLeftHand(0u)
        , CostumeRightHand(0u)
        , CostumeAccessoryCount(0)
        , VoiceId(0u)
        , HitPts(-1)
        , Field_0x6c(0u)
        , bMarkedBy(0u)
    {
        for (int Index = 0; Index < 8; ++Index)
            CostumeAccessory[Index] = 0u;
        Position.x = 0.0f;
        Position.y = 0.0f;
        Position.z = 0.0f;
        Orientation.w = 0.0f;
        Orientation.x = 0.0f;
        Orientation.y = 0.0f;
        Orientation.z = 0.0f;
    }

    // / rpclocal module storage. NOT in the vftable -- the
    // one-entry table at module storage holds only clear() -- so this is
    // non-virtual, reached by a direct CALL (module storage) and, in rpclocal,
    // by the two atexit thunks module storage / module storage that tear the
    // two receiver statics down. baseline re-stamps the base vftable
    // (module storage), erases the +0x8c list, then the +0x80 list, then frees
    // the vector's buffer (module storage `MOV EAX,[EBX + 0x74]` / CALL
    // ) and zeroes +0x74/+0x78/+0x7c -- that is ~EquipDescVector
    // (Common 0x06269110) inlined. The lists, the DamageList and the
    // EquipDescVector members destroy themselves here.
    ~CEqObjCreateInfo()
    {
    }

    // Copy constructor -- Server.dll the routine (module storage .. 06d0b106,
    // RET 4, SEH frame). Not exported, and out of line in baseline: its only caller
    // is the CShipCreateInfo copy the routine makes of its request
    // (module storage), which then copies the derived tail inline. The store
    // trail, in order:
    // +0x04 / +0x08 / +0x0c / +0x10
    // REP MOVSD 0xd dwords: +0x14 .. +0x47 (the Costume)
    // +0x48, +0x4c Vector, +0x58 four dwords, +0x68,
    //                         +0x6c, the vector's allocator byte +0x70
    // the vector: n = _First ? (_Last - _First) >> 5 : 0,
    //                         a negative n clamped to 0 (JGE), operator new(n *
    //                         0x20) -- even for n == 0 --, EquipDesc's copy
    //                         constructor on each element (the destination
    //                         null-tested), then _Last = _End = the end
    // +0x80 allocator byte, a fresh 0x10 sentinel, _Size
    //                         0, then list<CollisionGroupDesc>::insert(end, x)
    //                         per source element
    // +0x8c allocator byte, a fresh 0x14 sentinel, _Size
    //                         0, list<DamageEntry>::insert(end, first, last)
    // +0x98 byte, +0x9c / +0xa0 / +0xa4, +0xa8 byte
    // the base vftable
    // The two lists and the DamageList tail are those members' own copies.
    CEqObjCreateInfo(CEqObjCreateInfo const& Other)
        : ObjId(Other.ObjId)
        , ArchetypeNickname(Other.ArchetypeNickname)
        , DockingBaseId(Other.DockingBaseId)
        , VibeGroupId(Other.VibeGroupId)
        , CostumeHead(Other.CostumeHead)
        , CostumeBody(Other.CostumeBody)
        , CostumeLeftHand(Other.CostumeLeftHand)
        , CostumeRightHand(Other.CostumeRightHand)
        , CostumeAccessoryCount(Other.CostumeAccessoryCount)
        , VoiceId(Other.VoiceId)
        , Position(Other.Position)
        , Orientation(Other.Orientation)
        , HitPts(Other.HitPts)
        , Field_0x6c(Other.Field_0x6c)
        , CollisionGroups(Other.CollisionGroups)
        , Damage(Other.Damage)
        , bMarkedBy(Other.bMarkedBy)
    {
        for (int Index = 0; Index < 8; ++Index)
            CostumeAccessory[Index] = Other.CostumeAccessory[Index];

        Equipment.AllocPad = Other.Equipment.AllocPad;
        int Count = (Other.Equipment.Begin != nullptr)
            ? static_cast<int>(Other.Equipment.End - Other.Equipment.Begin)
            : 0;
        if (Count < 0)
            Count = 0;
        EquipDesc* const Block =
            static_cast<EquipDesc*>(::operator new(static_cast<size_t>(Count) * sizeof(EquipDesc)));
        EquipDesc* Dest = Block;
        for (EquipDesc const* Source = Other.Equipment.Begin; Source != Other.Equipment.End; ++Source)
        {
            if (Dest != nullptr)
                new (Dest) EquipDesc(*Source);
            ++Dest;
        }
        Equipment.Begin = Block;
        Equipment.End = Dest;
        Equipment.Cap = Dest;
    }
    CEqObjCreateInfo& operator=(CEqObjCreateInfo const&) = delete;

    // Vftable slot 0, the only slot. module storage / rpclocal module storage:
    //   +0x04/+0x08/+0x0c/+0x48/+0x6c = 0, +0xa8 byte = 0, +0x10 = -1
    //   the vector: _Last := _First (VC6 erase(begin, end) on a POD element --
    //     `ADD EAX,0x20 / MOV ECX,0x8 / REP MOVSD`, the 0x20 stride that is
    //     sizeof(EquipDesc)), buffer KEPT
    //   the +0x80 list and the +0x8c list: every node unlinked and freed, _Size
    //     decremented per node, sentinel KEPT
    // It does NOT touch +0x68, the Costume block, position, orientation, or the
    // DamageList's four tail members (+0x98..+0xa4).
    virtual void clear()
    {
        ObjId = 0u;
        ArchetypeNickname = 0u;
        DockingBaseId = 0u;
        VoiceId = 0u;
        Field_0x6c = 0u;
        bMarkedBy = 0u;
        VibeGroupId = -1;
        Equipment.End = Equipment.Begin;
        CollisionGroups.clear();
        // baseline's list erase decrements the list's own _Size (+0x94). The
        // port's DamageList keeps that count a second time in Count, so both
        // go to zero together; the tail members are left alone as baseline does.
        Damage.Entries.clear();
        Damage.Count = 0;
    }

    // +0x00 is the vptr.
    unsigned int ObjId;                 // +0x04  CObject::Id (+0xb0)
    unsigned int ArchetypeNickname;     // +0x08  get_archetype()->Nickname (Root+0x08)
    unsigned int DockingBaseId;         // +0x0c  CEqObj+0x160
    int VibeGroupId;                    // +0x10  CEqObj::VibeId (+0x104); ctor seeds -1
    // +0x14 .. +0x44 is pub::Costume, copied out of CEqObj+0x108 .. +0x138.
    // The copy is field-per-field and then a counted loop, which is what proves
    // the shape: module storage/.../06d077ca move the four head dwords,
    // moves the count, and module storage copies exactly `count`
    // dwords starting at source+0x10 into record+0x24.
    unsigned int CostumeHead;           // +0x14
    unsigned int CostumeBody;           // +0x18
    unsigned int CostumeLeftHand;       // +0x1c
    unsigned int CostumeRightHand;      // +0x20
    unsigned int CostumeAccessory[8];   // +0x24 .. +0x43
    int CostumeAccessoryCount;          // +0x44
    unsigned int VoiceId;               // +0x48  CEqObj+0x13c
    Vector Position;                    // +0x4c  CObject::WorldPosition (+0x2c)
    // Wire order is x,y,z,w; rpclocal's header codec FLDs all four as floats
    // and quantizes them against 127.0 (a unit quaternion).
    Quaternion Orientation;             // +0x58  from CObject::Orientation (+0x08)
    // An INTEGER: rpclocal module storage `FILD dword ptr [EBP+0x68]` on the way
    // out and module storage stores ftol's result back in.
    int HitPts;                         // +0x68  (int)CSimple::get_hit_pts(); ctor seeds -1
    unsigned int Field_0x6c;            // +0x6c  always stored 0 (module storage)
    EquipDescVector Equipment;          // +0x70  CEqObj::get_equip_desc_list, vtable +0xac
    // Node operator_new(0x10) -> an 8-byte value; the destructor erases it
    // through module storage, COMMON.DLL::std::list<CollisionGroupDesc>::erase.
    std::list<CollisionGroupDesc> CollisionGroups;  // +0x80 .. +0x8b
    // ONE Common.dll DamageList, baseline 0x1c bytes (+0x8c .. +0xa7): the list
    // header at +0x8c (node operator_new(0x14) -> a 12-byte DamageEntry, erased
    // through module storage, std::list<DamageEntry>::erase) and then the
    // class's own four tail members --
    //   +0x98 bDestroyed (a BYTE store in both ctors, module storage /
    // , and a byte copy on the wire, rpclocal
    // `MOV DL,byte ptr [EBX + 0xc]` with EBX == src+0x8c)
    //   +0x9c Cause, +0xa0 InflictorId, +0xa4 InflictorOwner
    //         (rpclocal module storage / module storage / module storage, the
    //         dwords at +0x10/+0x14/+0x18 of the same object)
    // which is exactly common_misc.h's DamageList. It was previously sliced here
    // into a bare list plus four loose fields named as if they were the
    // record's own.
    DamageList Damage;                  // +0x8c .. +0xa7
    unsigned char bMarkedBy;            // +0xa8  GameObject::is_marked_by(PlayerId),
                                        // / module storage (ship),
                                        // / module storage (solar)
};

// ===========================================================================
// CShipCreateInfo -- IClient +0x0bc (SendServerCreateShip) FLPACKET_SERVER_CREATESHIP.

// baseline size 0xe0: CShipCreateInfo::clear() (module storage) ends at
// `MOV byte ptr [EBP + 0xdc],0x0`, and the inlined constructor
// writes the same seven fields and no others -- at the CREATESHIP call site
// (module storage .. module storage) and in rpclocal's one-time init of the
// receiver static module storage (module storage .. module storage).

// Note what neither the constructor nor clear() touches: +0xb8 .. +0xcc, the
// two Vectors. They are unconditionally overwritten by the fill.
// ===========================================================================
struct CShipCreateInfo : CEqObjCreateInfo
{
    // +0xac = 0     module storage  +0xb0 = 0
    // +0xb4 = 0     module storage  +0xd0 = 0
    // +0xd4 = -1    module storage  +0xd8 = 0
    // +0xdc byte = 0
    // the derived vftable (.rdata:the branch)
    // The port zeroes the two Vectors as well (see the base constructor).
    CShipCreateInfo()
        : OwnerPlayer(0u)
        , Field_0xb0(0u)
        , ReputationRank(0u)
        , Throttle(0.0f)
        , VibeOtherGroupId(-1)
        , Field_0xd8(0u)
        , bUsingTradelane(0u)
    {
        Velocity.x = 0.0f;
        Velocity.y = 0.0f;
        Velocity.z = 0.0f;
        AngularVelocity.x = 0.0f;
        AngularVelocity.y = 0.0f;
        AngularVelocity.z = 0.0f;
    }

    // / rpclocal module storage. baseline inlines the base body
    // (module storage .. module storage, byte for byte the stores of
    // ) and then the derived tail module storage .. module storage:
    // +0xac/+0xb0/+0xb4/+0xd0/+0xd8 = 0, +0xd4 = -1, +0xdc byte = 0. rpclocal's
    // CREATESHIP receiver runs the same body inlined after the slot call
    // (module storage .. module storage).
    virtual void clear() override
    {
        CEqObjCreateInfo::clear();
        OwnerPlayer = 0u;
        Field_0xb0 = 0u;
        ReputationRank = 0u;
        Throttle = 0.0f;
        Field_0xd8 = 0u;
        VibeOtherGroupId = -1;
        bUsingTradelane = 0u;
    }

    // Wire: payload+0x9c (a full dword, and the encoder's flag bit 0x04 is
    // `!= 0` on it -- rpclocal module storage).
    unsigned int OwnerPlayer;      // +0xac  CSimple::OwnerPlayer (+0xb4)
    // Wire: only the LOW WORD travels (rpclocal module storage / module storage)
    // and the receiver widens it back with MOVZX (module storage).
    unsigned int Field_0xb0;       // +0xb0  CShip+0x21c
    // Wire: only the LOW BYTE travels (module storage / module storage,
    // MOVZX back at module storage).
    unsigned int ReputationRank;   // +0xb4  Reputation::Vibe::GetRank out-parameter
    Vector Velocity;               // +0xb8  vtable +0x54 == CObject::get_velocity
    Vector AngularVelocity;        // +0xc4  vtable +0x58 == CObject::get_angular_velocity
    // Quantized against 127.0 on the wire (rpclocal module storage).
    float Throttle;                // +0xd0  CShip::Throttle (+0x29c)
    // Sender-only: the second group of Reputation::Vibe::AttitudeTowards
    // (rpclocal module storage). Server.dll stores PlayerData::rep_vibe here
    // (module storage / module storage).
    int VibeOtherGroupId;          // +0xd4  PlayerData::rep_vibe; ctor seeds -1
    // Wire: only the LOW WORD travels (module storage / module storage,
    // MOVZX back at module storage).
    unsigned int Field_0xd8;       // +0xd8  CShip+0x1b4
    unsigned char bUsingTradelane; // +0xdc  CShip::is_using_tradelane()
};

// ===========================================================================
// FShipCreateRequest -- the argument of StarSystem::create_ship (the routine).
// Its callers (pub::SpaceObj::Create module storage, IServerImpl::PlayerLaunch
// ) build a CShipCreateInfo and three more members right after
// its 0xe0 bytes; the routine copies only the CShipCreateInfo part
// (the routine + the derived tail, module storage .. 06d0a98f) and reads the
// rest straight from the request:
// MOV DX,word ptr [EBP + 0xe0]   -> the EquipIdMaker state
// MOV EAX,dword ptr [EBP + 0xe4] -> Ship::FEquipState::Data
// MOV EBP,dword ptr [EBP + 0xe8] -> & 0x3fffffff, then
//                   mark_for_player(it, 1)
// baseline struct name not recovered.
// ===========================================================================
struct FShipCreateRequest : CShipCreateInfo
{
    SubObjectID::EquipIdMaker EquipIds;   // +0xe0 (a word)
    void const* EquipStateData;           // +0xe4
    unsigned int MarkPlayerId;            // +0xe8
};

// ===========================================================================
// CSolarCreateInfo -- IClient +0x0b8 (SendServerCreateSolar) FLPACKET_SERVER_CREATESOLAR.

// Its vftable module storage holds module storage in slot 0 -- the BASE
// clear() -- so it has no override of its own; rpclocal's receiver static
// likewise gets module storage (module storage), whose slot 0
// is rpclocal's copy of the base clear, module storage.

// The derived constructor is inlined at every construction site and writes the
// same three fields and nothing else:
//   Server  module storage / module storage / module storage   (pub::SpaceObj::CreateSolar)
//   Server  module storage / module storage / module storage   (StarSystem::add_player)
//   rpclocal module storage / module storage / module storage  (receiver static)
// i.e. +0xac byte 0, +0xad byte 0, +0xb0 dword -1.

// The last three members are proven by pub::SpaceObj::CreateSolar
// (module storage, record at entry-0xd4) and their only reader,
// , which receives the record's address at module storage:
//   +0xb4  written module storage (byte from the SolarInfo's +0x80),
//          read    module storage `MOV AL,byte ptr [EBP + 0xb4]`
//   +0xb8  ID_String::clear on it at module storage (`LEA ECX,[ESP + 0xd8]`,
//          ESP == entry-0xf4), written module storage, read module storage
//   +0xbc  written module storage (dword from the SolarInfo's +0xcc),
//          read    module storage and tested `& 0x3fffffff` at module storage
// baseline size is therefore at least 0xc0; nothing in either module touches a
// byte past +0xbf. The constructor leaves these three as they lie, and so does
// the port.
// ===========================================================================
struct CSolarCreateInfo : CEqObjCreateInfo
{
    CSolarCreateInfo()
        : Field_0xac(0u)
        , Field_0xad(0u)
        , VibeOtherGroupId(-1)
    {
    }

    // pub::SpaceObj::CreateSolar stores 1 (module storage); the routine
    // hands it to CSolar::init's parameter block (module storage / module storage).
    unsigned char Field_0xac;       // +0xac
    // pub::SpaceObj::CreateSolar stores 1 (module storage). Wire payload+0x7c
    // (rpclocal module storage out, module storage back in).
    unsigned char Field_0xad;       // +0xad
    // The second group of Reputation::Vibe::AttitudeTowards (rpclocal
    // ). the routine stores PlayerData::rep_vibe here per
    // player (module storage / module storage), the same `id*0x418 - 0x100`
    // expression the ship path uses for its +0xd4.
    int VibeOtherGroupId;           // +0xb0  ctor seeds -1
    unsigned char Field_0xb4;       // +0xb4
    ID_String Field_0xb8;           // +0xb8
    unsigned int Field_0xbc;        // +0xbc
};

// ===========================================================================
// The four flat records. These are plain baseline POD and they are written here
// exactly as the lane brief asked -- one named member per store, with the
// storing instruction quoted. rpclocal's codecs (rpc_payload_ch2.cpp) read and
// rebuild these very types for IClient::SendServerCreateLoot / CreateCounter /
// CreateMine / CreateGuided: their former private mirrors (0x14 / 0x48 / 0x48 /
// 0x38) were folded onto them 2026-10-16.
// ===========================================================================

// The 0x48-byte LootDesc that the routine fills. It is NOT the SendServerCreateLoot
// argument -- it hangs off FServerCreateLootRequest::Items below.
struct FServerLootDesc
{
    unsigned int LootId;              // +0x00  module storage  MOV [EDI],EAX
    unsigned int ContentsNickname;    // +0x04  module storage, CLoot::ContentsArch->Nickname
    float ContentsHitPts;             // +0x08  module storage  FSTP [EDI + 0x8]
    unsigned int Units;               // +0x0c  module storage, CLoot::Units (+0xf0)
    unsigned int ContainerNickname;   // +0x10  module storage, get_archetype()->Nickname
    float HitPts;                     // +0x14  module storage  FSTP [EDI + 0x14]
    unsigned int NameOverride;        // +0x18  module storage, only when get_name() differs
                                      //        from ContentsArch->IdsName (Root+0x14)
    Vector Position;                  // +0x1c  module storage/06d07c01/06d07c07
    Quaternion Orientation;           // +0x28  module storage .. module storage
    Vector Velocity;                  // +0x38  module storage/06d07c60/06d07c66, vtable +0x54
    // The three tail bytes are written by three different places, which is why
    // they are three separate members and not one dword:
    unsigned char Field_0x44;         // +0x44  caller, module storage  MOV byte [ESP+0x114],1
    unsigned char bTemporary;         // +0x45  builder, module storage, CLoot+0xfd
    unsigned char bMarkedBy;          // +0x46  caller, module storage, is_marked_by(PlayerId)
    unsigned char Pad47;              // +0x47
};

// The SendServerCreateLoot argument block, built at S+0x6c by the CREATELOOT branch.
struct FServerCreateLootRequest
{
    unsigned int OwnerId;             // +0x00  module storage, CLoot::OwnerId (+0xe4)
    unsigned int Kind;                // +0x04  module storage  MOV [ESP + 0x74],0x5
    unsigned char bMarkedBy;          // +0x08  module storage
    unsigned char bTemporary;         // +0x09  module storage, CLoot::bTemporary (+0xfd)
    unsigned char Pad0a[2];
    FServerLootDesc* Items;           // +0x0c  module storage
    unsigned char Count;              // +0x10  module storage  MOV byte [ESP + 0x80],0x1
    unsigned char Pad11[3];
};

// The 0x48-byte record the routine fills, sent through IClient +0x0cc (SendServerCreateCounter,
// CREATECOUNTER) and +0x0c4 (SendServerCreateMine, CREATEMINE). One builder, two packets.
struct FServerCreateProjectileInfo
{
    unsigned int ObjId;               // +0x00  module storage, CObject::Id (+0xb0)
    unsigned int ArchetypeNickname;   // +0x04  module storage
    unsigned int OwnerId;             // +0x08  module storage, CProjectile::OwnerId (+0xe4)
    unsigned short SubObjId;          // +0x0c  module storage, SUBOBJ_ID_NONE
    unsigned short Pad0e;
    Vector Position;                  // +0x10  module storage/06d07d06/06d07d0c
    Quaternion Orientation;           // +0x1c  module storage .. module storage
    Vector Velocity;                  // +0x2c  module storage/06d07d2c/06d07d33, vtable +0x54
    // +0x38 is a THIRD Vector, not padding. Neither builder ever writes it, but
    // both call sites construct it: the countermeasure branch at
    // (`LEA ECX,[ESP + 0x154]`, ESP == S-0xc, record at S+0x110)
    // and the mine branch at module storage (`LEA ECX,[ESP + 0x19c]`, record at
    // S+0x158) both hand record+0x38 to the routine, the three-float Vector
    // constructor, with three zeroes.
    Vector Field_0x38;                // +0x38
    unsigned int Field_0x44;          // +0x44  module storage  MOV [ESI + 0x44],0x0
};

// The 0x38-byte record the routine fills, sent through IClient +0x0c8 (SendServerCreateGuided).
struct FServerCreateGuidedInfo
{
    unsigned int ObjId;               // +0x00  module storage
    Vector Position;                  // +0x04  module storage/06d07d77/06d07d7d
    Quaternion Orientation;           // +0x10  module storage .. module storage
    unsigned int TargetId;            // +0x20  module storage, target->get_id() or 0
    unsigned short TargetSubObjId;    // +0x24  module storage, CGuided+0xf8
    unsigned short Pad26;
    unsigned int ArchetypeNickname;   // +0x28  module storage
    unsigned int Field_0x2c;          // +0x2c  module storage  MOV [ESI + 0x2c],0x0
    unsigned int OwnerId;             // +0x30  module storage, CProjectile::OwnerId (+0xe4)
    unsigned short SubObjId;          // +0x34  module storage, SUBOBJ_ID_NONE
    unsigned short Pad36;
};
// rpclocal's sender 0x06d8a730 reads up to +0x34 (a word) and caseD_37 rebuilds 0x38 bytes.
static_assert(sizeof(FServerCreateGuidedInfo) == 0x38, "CREATEGUIDED argument struct 0x38");

// ---------------------------------------------------------------------------
// The builders, named for what baseline's RTTI says they build.
// ---------------------------------------------------------------------------

// (218 instructions), __cdecl -- the caller balances with
// `ADD ESP,0x8` at module storage.
void __cdecl CShipCreateInfo_Fill(CShipCreateInfo* Info, CShip* Ship);

// (181 instructions), __cdecl -- the caller balances with
// `ADD ESP,0x8` at module storage. Its only caller is StarSystem::add_player's
// multiplayer solar walk.
void __cdecl CSolarCreateInfo_Fill(CSolarCreateInfo* Info, CSolar* Solar);

// (75 instructions), __cdecl.
void __cdecl ServerLootDesc_Fill(FServerLootDesc* Desc, CLoot* Loot);

// (62 instructions), __cdecl. CProjectile covers both CMine and
// CCounterMeasure; baseline's own analysis signature says CObject and only ever
// reads CObject/CProjectile fields.
void __cdecl ServerCreateProjectileInfo_Fill(FServerCreateProjectileInfo* Info,
                                             CProjectile* Projectile);

// (71 instructions), __cdecl.
void __cdecl ServerCreateGuidedInfo_Fill(FServerCreateGuidedInfo* Info, CGuided* Guided);
