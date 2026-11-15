// Author: Giperion.

#pragma once

// Content.dll TU20 (06eb6c00-06eb7360): the base class of every space object Content
// tracks per system — solar NPCs (TU24, the routine), the TU50 ship base (the routine:
// NPC ships TU43 the routine and the player ship TU46 the routine). SystemInfo (TU58)
// finds these objects by space object id (the routine) and by nickname (the routine,
// name at +0xd8).

// baseline: vtable 06fb464c has one slot (scalar deleting dtor the routine), size 0x118.
// No RTTI in Content; the class name is the port's.

#include "common_ini.h"       // Vector
#include "common_csys.h"      // Matrix
#include "common_tstring.h"   // TString

#include <vector>

struct FContentPlayerStateBase;   // ContentPlayerState.h: refcounted reputation handle (the routine)
struct FContentBase;              // BaseDB.h (TU17 record, the routine)
struct FSystemInfo;               // SystemInfo.h (TU58)

// Construction parameters. the routine (callers GameSupport 06efff90, TU43 06f33521,
// TU58 06f748f3) is the default ctor. Derived parameter blocks embed it: TU50's at +0
// (the routine passes the block itself), TU24's at +0x48 (the routine converts with a
// null check, i.e. a base-pointer conversion).
struct FContentSpaceObjParams
{
    FContentPlayerStateBase* ReputationHandle = nullptr;   // +0x00
    FContentBase* Base = nullptr;                          // +0x04
    // +0x08: the routine zeroes Length and Name[0] only.
    TString<0x30> Nickname;                                // +0x08
    FSystemInfo* System = nullptr;                         // +0x3c
    // +0x40: -1 from the routine (default hit points). Written by the mission ship spawn
    // (06f16650: the slot's hit_points), read by the TU43 ship create 06f3386e as
    // pub::SpaceObj::ShipInfo::HitPts.
    int HitPoints = -1;

    // .
    FContentSpaceObjParams();
    // Every parameter block's dtor ends with the routine on its +0 (TU58 06f74a3d after the
    // TU24 record is built; the TU50 block dtor the routine 06ef4b92): ReputationHandle ?
    // Release() : -, then 0. The +0 member is a releasing handle; the space object copies it
    // raw (no AddRef).
    ~FContentSpaceObjParams();
    // Copy (inlined at the head of the TU50 block copy ctor the routine, 06f736a0-06f736e2):
    // the handle with an AddRef (vtable slot 0, no AddRef when null), the rest member-wise.
    FContentSpaceObjParams(FContentSpaceObjParams const& Other);
    FContentSpaceObjParams& operator=(FContentSpaceObjParams const&) = delete;
};

static_assert(sizeof(FContentSpaceObjParams) == 0x44, "FContentSpaceObjParams size");

// One of the eight attacker slots at +0x14 (0x18 bytes). the routine finds or recycles a
// slot; TU56 the routine (06f70203 onwards, CombatReputation.cpp) fills it on the ATTACKING
// player's ship, keyed by the damaged object.
struct FSpaceObjAttacker
{
    unsigned int AttackerId;   // +0x00 the damaged object's id; 0 = free slot
    // +0x04 (float: TU56 06f702a5 / 06f702b9 FADD): hostility, +RelativeDamage * 50 per hit and
    // +10 when more than a second passed since HostilityTime.
    float Hostility;
    float FirstTime;           // +0x08 TU56: time the attacker was first recorded
    float LastTime;            // +0x0c TU56: time of the latest hit; the routine recycles the lowest
    float HostilityTime;       // +0x10 TU56: time of the last +10 step
    unsigned char bWarned;     // +0x14 TU56: Hostility > 25 reported (message 0x1005, word 0 = 0)
    unsigned char bProvoked;   // +0x15 TU56: Hostility >= 50 reported (word 0 = 1)
};

static_assert(sizeof(FSpaceObjAttacker) == 0x18, "FSpaceObjAttacker size");

// +0x108 vector element (0x10 bytes): one line of a mission dialog (TU28, MissionDialogs.h:
// the +0x6c part of a dialog line). the routine matches Key and hands TargetId / TextId to
// FSystemInfo::SendComm (the routine -> pub::SpaceObj::SendComm). The whole vector is
// assigned by TU28 06ef28fc (StartMissionDialog) and TU58 06f7e7cd.
struct FSpaceObjPendingComm
{
    unsigned int Key;          // +0x00 the dialog line after which this one is spoken
    unsigned int TextId;       // +0x04 the line's id (pub::GetNicknameId), SendComm's TextId
    unsigned int TargetId;     // +0x08 the addressee's SpaceObj id, SendComm's TargetId
    unsigned int Field_0x0c;   // +0x0c not read by TU20 (TU28 loader writes 0)
};

static_assert(sizeof(FSpaceObjPendingComm) == 0x10, "FSpaceObjPendingComm size");

// Message 0x1001 sent to controller 0 by the ContentCallback combat_state_notification
// overrides of the solar NPC (TU24 06ed5a00) and the NPC ship (TU43 06f34090): the three
// callback arguments, then the space object's system id (the routine()->+0x48).
struct FSpaceObjCombatStateMsg
{
    unsigned int Type;         // +0x00 CSNType
    unsigned int Param1;       // +0x04
    unsigned int Param2;       // +0x08
    unsigned int SystemId;     // +0x0c
};

static_assert(sizeof(FSpaceObjCombatStateMsg) == 0x10, "FSpaceObjCombatStateMsg size");

// output (0x30 bytes): orientation first, then position.
struct FSpaceObjPlacement
{
    Matrix Orientation;        // +0x00
    Vector Position;           // +0x24
};

static_assert(sizeof(FSpaceObjPlacement) == 0x30, "FSpaceObjPlacement size");

class FContentSpaceObj
{
public:
    // (RET 4).
    explicit FContentSpaceObj(FContentSpaceObjParams const* Params);
    // (vptr reset + free the +0x108 buffer); slot 0 the routine.
    virtual ~FContentSpaceObj();
    // +0x04 (slot 1). baseline's base vtable 06fb464c has only the dtor: every derived class
    // declares slot 1 itself (FSolarNpc 06ed59f0 = byte +0x164, FContentShipBase 06ea1480 =
    // false, FContentNpcShip 06ea1460 = true), and TU25 06eebd10 / TU53 call it on objects
    // that FindShipOrSolar / the SpaceObjs list give as the base type. The base is never
    // created on its own, so the port declares it here, pure. Name unknown.
    virtual bool Slot04() = 0;

    // (RET 0xc; only the second argument is read).
    void SendPendingComm(unsigned int Unused1, unsigned int Key, unsigned int Unused3);
    // affiliation of the reputation handle, -1 without one.
    unsigned int GetAffiliation() const;
    // reputation of the handle, -1 without one.
    int GetReputation() const;
    // (RET 4): +0x10 = System.
    void SetSystem(FSystemInfo* InSystem);
    // (in TU25's range, ICF-shared getter `MOV EAX,[ECX+0x10]; RET`): the
    // callers TU24 06ed5a25, TU43 06f3401b/06f34063/06f340b5 and TU37 pass a space object.
    FSystemInfo* GetSystem() const
    {
        return System;
    }
    // (RET 4).
    void GetPlacement(FSpaceObjPlacement* Out) const;
    // (RET 4).
    void GetPosition(Vector* Out) const;
    // (RET 4).
    void GetOrientation(Matrix* Out) const;
    // (RET 8).
    void GetMotion(Vector& OutLinear, Vector& OutAngular) const;
    // (in TU43's range; callers TU58 06f72b34, 06f742a3 on SystemInfo's
    // SpaceObjs): pub::SpaceObj::Destroy(SpaceObjId, DESTROY_NORMAL).
    void DestroySpaceObj();
    // (RET 8): always returns true.
    bool FindOrAllocAttacker(unsigned int AttackerId, FSpaceObjAttacker** Out);

    unsigned int SpaceObjId;                              // +0x04 0 from the ctor
    FContentPlayerStateBase* ReputationHandle;            // +0x08 not released by the dtor
    FContentBase* Base;                                   // +0x0c
    FSystemInfo* System;                                  // +0x10
    FSpaceObjAttacker Attackers[8];                       // +0x14
    TString<0x30> Nickname;                               // +0xd4 (chars at +0xd8)
    std::vector<FSpaceObjPendingComm> PendingComms;       // +0x108
};
