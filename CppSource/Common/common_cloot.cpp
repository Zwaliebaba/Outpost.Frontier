// Author: Giperion.

// CLoot (baseline CLoot.cpp).
#include "common_cobject.h"
#include "common_cobject_internal.h" // CmnCobject::RotateWorldToObjectSpace, GetObjectDbCollisionGroup

#include "common_physys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using CmnCobject::GetObjectDbCollisionGroup;
using CmnCobject::RotateWorldToObjectSpace;

// ============================================================================
// CLoot implementation
// ============================================================================

CLoot::CLoot(Class ObjectClass)
    : CSimple(ObjectClass)
{
    // . The alignment bytes +0xfe/+0xff are not written by baseline.
    OwnerId = 0;
    NameId = 0;
    ContentsArch = nullptr;
    Units = 0;
    ContentsHitPts = 0.0f;
    OwnerSafeTime = 0.0f;
    bTemporary = false;
    Reserved100 = 0;
    bCanAiTractor = true;
    TypeBits = 0x10000000;
}

// . 0629d54e-0629d752 is CSimple(CSimple const&) inlined (EngineObject,
// CObject and CSimple layers with their vftable stamps); then the CLoot dwords +0xe4..+0xf8,
// the bytes +0xfc / +0xfd and the dword +0x100, and the CLoot vftable. The alignment bytes
// +0xfe/+0xff are not written by baseline and are not written here either.
CLoot::CLoot(CLoot const& Other)
    : CSimple(Other)
    , OwnerId(Other.OwnerId)
    , NameId(Other.NameId)
    , ContentsArch(Other.ContentsArch)
    , Units(Other.Units)
    , ContentsHitPts(Other.ContentsHitPts)
    , OwnerSafeTime(Other.OwnerSafeTime)
    , bCanAiTractor(Other.bCanAiTractor)
    , bTemporary(Other.bTemporary)
    , Reserved100(Other.Reserved100)
{
}

CLoot::~CLoot()
{
}

void CLoot::init(CreateParms const& Parms)
{
    // Common 0x4c460. Initialize loot container.
    OwnerId = Parms.OwnerId;
    if (OwnerId != 0)
        OwnerSafeTime = LOOT_OWNER_SAFE_TIME;
    NameId = Parms.NameId;
    ContentsArch = Parms.ContentsArch;
    Units = Parms.Units;
    ContentsHitPts = Parms.ContentsHitPts;
    bCanAiTractor = Parms.bCanAiTractor;
    bTemporary = Parms.bTemporary;
    CSimple::init(Parms);
}

void CLoot::init_physics(Vector const& Velocity, Vector const& AngularVelocity)
{
    // . Parms: PhySys default linear and angular damping, mass =
    // container archetype Mass (+0x20, no null test), system id from IObjDB, Type 2, +0x1c =
    // 0, bEnableCollisions 1, +0x26 / +0x28..+0x30 = 0, Name = _itoa(OwnerId (+0xe4) when
    // non-zero, else Id (+0xb0), radix 24). The bytes baseline leaves unwritten are zero here.
    PhySys::CreateParms Phy;
    memset(&Phy, 0, sizeof(Phy));
    Phy.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
    Phy.AngularDamping[0] = PhySys::DEFAULT_ANGULAR_DAMPING.x;
    Phy.AngularDamping[1] = PhySys::DEFAULT_ANGULAR_DAMPING.y;
    Phy.AngularDamping[2] = PhySys::DEFAULT_ANGULAR_DAMPING.z;
    Phy.Type = 2;
    Phy.bEnableCollisions = 1;
    Phy.Mass = Archetype->Mass;
    Phy.CollisionGroupId = GetObjectDbCollisionGroup(ObjectDb);
    _itoa((int)((OwnerId != 0) ? OwnerId : Id), Phy.Name, 0x18);
    PhySys::BuildIVP_Real(this, Phy);

    // / 062ac633: no body (+0x54) -> FDUMP the CLoot.cpp:0x53 error with the
    // container (+0x88) and contents (+0xec) nicknames (contents string fetched first) and
    // return without touching velocity.
    if (BodyPtr == nullptr)
    {
        char const* const ContentsName = ContentsArch->Nickname.get_string();
        char const* const ContainerName = container_arch()->Nickname.get_string();
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: CLoot::init_physics() failed for %s (containing %s). Verify 'DA_archeytpe' data! NOT ignorable.",
              "Common\\CLoot.cpp", 0x53, ContainerName, ContentsName);
        return;
    }

    PhySys::AddToVelocity(this, Velocity);
    // Common 0x4c4c0: rotate world-space AngularVelocity into object space
    // via Orientation^T before adding it — see RotateWorldToObjectSpace above.
    PhySys::AddToAngularVelocityOS(this, RotateWorldToObjectSpace(Orientation, AngularVelocity));
}

int CLoot::update(float Dt, unsigned Flags)
{
    // Common 0x4c680. Countdown owner-safe-time; once it lapses, restore
    // the normal (owner-inclusive) collision group.
    update_zones(Dt, Flags);
    if (OwnerSafeTime > 0.0f)
    {
        OwnerSafeTime -= Dt;
        if (OwnerSafeTime <= 0.0f)
        {
            char Name[8];
            _itoa((int)Id, Name, 0x18);
            PhySys::SetCollisionGroup(this, Name);
        }
    }
    return CObject::update(Dt, Flags);
}

void CLoot::set_hit_pts(float NewHitPts)
{
    // Common 0x4c700. When the contents archetype equals the container
    // archetype (the loot IS its cargo, e.g. a raw canister), mirror into
    // ContentsHitPts too; the base HitPts is always updated. module storage-062ac72f: both
    // archetype ids are read with no null test.
    if (contents_arch()->get_id() == container_arch()->get_id())
        ContentsHitPts = NewHitPts;
    CSimple::set_hit_pts(NewHitPts);
}

unsigned CLoot::get_name() const
{
    // NameId (+0xe8) when non-zero, else ContentsArch(+0xec)->
    // IdsName (+0x14) with no null test.
    if (NameId != 0)
        return (unsigned)NameId;
    return (unsigned)ContentsArch->IdsName;
}

unsigned CLoot::get_owner() const
{
    // Common 0x3d470
    return OwnerId;
}

unsigned CLoot::get_units() const
{
    return Units;
}

float CLoot::get_volume() const
{
    // FILD qword {Units (+0xf0), 0} FMUL contents arch (+0xec, no
    // null test)->Volume (+0x5c), returned in ST0.
    return (float)((double)Units * contents_arch()->Volume);
}

float CLoot::get_contents_hit_pts() const
{
    return ContentsHitPts;
}

void CLoot::set_units(unsigned Count)
{
    Units = Count;
}

void CLoot::set_contents_hit_pts(float Pts)
{
    ContentsHitPts = Pts;
}

bool CLoot::is_loot_temporary() const
{
    return bTemporary;
}

bool CLoot::can_ai_tractor() const
{
    // Common 0x3d510
    return bCanAiTractor;
}

Archetype::Equipment* CLoot::contents_arch() const
{
    // Common 0x3d490. baseline caches a resolved pointer directly.
    return const_cast<Archetype::Equipment*>(ContentsArch);
}

Archetype::Equipment* CLoot::container_arch() const
{
    // JMP CAsteroid::asteroidarch (ICF fold of `return [this+0x88]`).
    return (Archetype::Equipment*)Archetype;
}
