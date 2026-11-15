// Author: Giperion.

// CETractor (TractorArm itself is in common_tractorarm.cpp) -- split out of common_cequip.cpp (CMN-B2).
// Declared in common_cequip.h; helpers shared with the sibling TUs are in
// common_cequip_internal.h (namespace CmnCequip).
#include "common_cequip.h"
#include "x86math.h"

#include "common_animdb.h"  // AnimDB::Add/Rem (common_animdb.cpp)

#include "common_cobject.h"
#include "common_csys.h"     // Acquire3DMathEngine (MatrixToQuaternionViaMathEngine)
#include "common_fuses.h"    // WeaponModDB_GetShieldMod (CEShield::GetWeaponModifier)
#include "common_iface.h"    // EquipStatus, EquipStatusFlag* (CEquip::GetStatus)
#include "common_ini.h"
#include "common_misc.h"     // ComputeExplosiveImpulses (CAttachedEquip::Disconnect)
#include "common_physys.h"
#include "common_spatial.h"  // ISpatialPartition (ScanList::add_in_range bucket query)
#include "common_timing.h"   // Timing::GetGlobalTime (CEShieldGenerator / CEShield)
#include "common_universe.h"
#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "../IVP/ivp_physics/ivp_real_object.h"
#include "../IVP/ivp_physics/ivp_controller.h"  // IVP_Event_Sim (do_simulation_controller)

#include <cstdarg>
#include <cstdint>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common_cequip_internal.h"

using namespace CmnCequip;

namespace
{
// Shared by CETractor::GetSourcePos/CanSeeTarget/VerifyTarget and TractorArm::Update/GetTipPos
// (the latter two via CETractor::GetSourcePos, since TractorArm has no access to CETractor's
// protected HpXform). Common's own the routine (used by TractorArm::Update/VerifyTarget)
// and the identical inline call in CanSeeTarget/GetSourcePos/TractorArm::GetTipPos are all the
// same operation: the owner ship's own EngineObject world Transform (Orientation @ +0x08
// immediately followed by WorldPosition @ +0x2c -- already asserted contiguous as a Transform by
// EngineObject's own static_asserts) composed with this equip's stored hardpoint-local HpXform
// via the 3DMathEngine singleton's MulTransform (vtable +0x1C -- confirmed against the real
// baseline CEquip vtable is NOT this slot; this is I3DMathEngine's own vtable, cross-checked
// against src/x86math/x86math.h's independently-recovered slot list and the "I3DMathEngine slot
// 0x1c, MulTransform" comment already established at
// src/OutpostFrontier/Client/space_camera_script.cpp). baseline (0629de50, 0629e730, 0629e660,
// ) tests neither the owner nor the engine: [this+4]+8 is passed as is and the lazily
// created engine is called directly; the 0x30-byte result is written whole by the engine.
Transform ResolveTractorSourceXform(CEqObj const* Owner, Transform const& LocalXform)
{
    Transform const* OwnerXform =
        (Transform const*)((unsigned char const*)Owner + offsetof(EngineObject, Orientation));

    Transform Result;
    Acquire3DMathEngine()->MulTransform((FTransform&)Result, (FTransform const&)*OwnerXform,
                                        (FTransform const&)LocalXform);   // +0x1C
    return Result;
}
}  // namespace

CETractor::CETractor(CShip* OwnerObj, unsigned short EquipId, Archetype::Tractor const* ArchObj,
                     bool bMissionFlagIn)
    : CInternalEquip(EquipTypeTractor, OwnerObj, EquipId, ArchObj, bMissionFlagIn)
    , Arms()
{
    // Layout check for the protected Arms member (see the class comment in common_cequip.h);
    // compile-time only, emits no code.
    static_assert(offsetof(CETractor, Arms) == 0x50, "CETractor+0x50 arm vector");

    // Common (0x3e030). Identity is the default/failure-path Transform; if
    // Owner is a real ship (ObjectClass & 0x503) whose archetype names a HpTractorSource
    // hardpoint, baseline overwrites it with that hardpoint's parent(ship)-space Transform.
    HpXform.Rotation = Matrix(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
    HpXform.Translation = Vector{0.0f, 0.0f, 0.0f};

    // no archetype or hardpoint-name test; on failure baseline rewrites the
    // same identity (0629e122), which the port already holds.
    if (OwnerObj != nullptr && ((unsigned)OwnerObj->ObjectClass & 0x503) == 0x503)
    {
        Archetype::Ship const* ShipArch = (Archetype::Ship const*)OwnerObj->Archetype;
        long DiscardHandle = 0;
        HardpointInfo Info;
        if (FindHardpoint_PS(OwnerObj->EngineInstanceId, ShipArch->HpTractorSource.Value,
                             DiscardHandle, Info))
        {
            HpXform.Rotation = Info.Orient;
            HpXform.Translation = Info.Position;
        }
    }
}

CETractor::CETractor(CETractor const& Other)
    : CInternalEquip(Other)
    , HpXform(Other.HpXform)
    , Arms(Other.Arms)
{
    // Common (0x3d9a0). baseline's inlined vector<TractorArm> copy: the
    // per-element copy is a verbatim field copy (Owner/State/PullOffset/Timer/ErrorCode) plus a
    // BaseWatcher relink guarded exactly like TractorArm's own copy ctor -- i.e.
    // TractorArm(TractorArm const&) applied to each source arm (including baseline's own quirk of
    // NOT rebinding each copied arm's Owner to `this`, since Owner is one of the copied fields).
}

CETractor::~CETractor()
{
    // Common (0x3e170). CEquip::Destroy, then baseline's inlined
    // ~vector<TractorArm> (module storage-0629e212: each arm's inlined ~TractorArm unwatches and
    // zeroes it, then the buffer is freed and begin/end/cap cleared) -- the Arms member's own
    // destructor here, which runs before ~CInternalEquip exactly as in baseline.
    CEquip::Destroy();
}

// Common (0x3e440). vtable+0xc == CEquip::IsFunctioning, vtable+0x30 ==
// CEquip::Activate -- confirmed against the real baseline vtable layout (Common's
// `??_7CEquip@@6B@` listing), not slot-count guesswork.
bool CETractor::Update(float Dt, unsigned Flags)
{
    if (IsFunctioning())
    {
        int ActiveCount = 0;
        for (std::vector<TractorArm>::iterator It = Arms.begin(); It != Arms.end(); ++It)
        {
            It->Update(Dt);
            if (It->GetMode() != TractorArm::MODE_0)
                ++ActiveCount;
        }
        if (ActiveCount == 0)
            Activate(false);
    }
    return CEquip::Update(Dt, Flags);
}

// Common 0x3e790
float CETractor::GetRange() const
{
    return ((Archetype::Tractor const*)EquipArch())->MaxLength;
}

int CETractor::CountActiveArms() const
{
    // Common (0x3e240).
    int Count = 0;
    for (std::vector<TractorArm>::const_iterator It = Arms.begin(); It != Arms.end(); ++It)
        if (It->GetMode() != TractorArm::MODE_0)
            ++Count;
    return Count;
}

TractorFailureCode CETractor::AddTarget(CLoot* Target)
{
    // Common (0x3e270). Notify(Event_0x14, ...) fires on any verify failure,
    // passing the address of the failure code itself (baseline reuses the by-then-dead `param_1`
    // stack slot for VerifyTarget's return value before taking its address -- confirmed against
    // the raw listing, not a pointer to the original CLoot). On success: fills the first idle
    // arm slot if one exists, else appends one arm to the vector (std::vector::insert stands in
    // for baseline's VC6 instantiation the routine), then activates the equipment if it wasn't
    // already.
    TractorFailureCode Code = VerifyTarget(Target);
    if (Code != TractorOk)
    {
        Notify(INotify::Event_0x14, &Code);
        return Code;
    }

    TractorArm* Slot = nullptr;
    for (std::vector<TractorArm>::iterator It = Arms.begin(); It != Arms.end(); ++It)
    {
        if (It->GetMode() == TractorArm::MODE_0)
        {
            Slot = &*It;
            break;
        }
    }

    if (Slot == nullptr)
    {
        // a temporary TractorArm(this) is inserted at end() through
        // the vector insert helper the routine (`insert(end, 1, arm)` on this+0x50), the
        // temporary is destroyed, and the new last element is the slot.
        Arms.insert(Arms.end(), 1, TractorArm(this));
        Slot = &Arms.back();
    }

    Slot->SetTarget(Target);

    if (!IsActive())
        Activate(true);

    return TractorOk;
}

void CETractor::RemoveTarget(unsigned TargetId)
{
    // Common (0x3e3d0).
    int RemainingActive = 0;
    for (std::vector<TractorArm>::iterator It = Arms.begin(); It != Arms.end(); ++It)
    {
        if (It->GetMode() == TractorArm::MODE_0)
            continue;
        CLoot* Target = It->GetTarget();
        if (Target == nullptr || Target->get_id() != TargetId)
        {
            ++RemainingActive;
        }
        else
        {
            It->TurnOff();
        }
    }
    if (RemainingActive == 0)
        Activate(false);
}

TractorFailureCode CETractor::VerifyTarget(CLoot const* Target) const
{
    // Common (0x3e4a0).
    if (Target == nullptr || Target->Units == 0)
        return TractorNoContents;

    Vector const SourcePos = GetSourcePos();
    Vector const Delta{Target->WorldPosition.x - SourcePos.x, Target->WorldPosition.y - SourcePos.y,
                        Target->WorldPosition.z - SourcePos.z};
    // Range == 0 (FCOMP 0, TEST AH,0x44 / JNP) skips the test; otherwise
    // the float deltas are summed (y^2 + z^2) + x^2 and compared with Range^2 on the FPU,
    // TEST AH,0x5 / JP: out of range only when Range^2 is below (not equal, not unordered).
    float const Range = GetRange();
    if (Range != 0.0f)
    {
        double const DistSq = (double)Delta.y * Delta.y + (double)Delta.z * Delta.z +
                              (double)Delta.x * Delta.x;
        if ((double)Range * Range < DistSq)
            return TractorOutOfRange;
    }

    // the owner as a ship, or null; the accessors below are called on it
    // and the contents archetype (+0xec) is read with no test, as in baseline.
    CShip* ShipOwner = nullptr;
    if (Owner != nullptr && ((unsigned)Owner->ObjectClass & 0x503) == 0x503)
        ShipOwner = (CShip*)Owner;

    Archetype::Equipment const* ContentsArch = Target->contents_arch();   // [Target+0xec]
    // only a strictly positive volume is tested (TEST AH,0x41 / JNZ); the
    // unrounded hold remainder FCOMP volume, TEST AH,0x5 / JP: full only when below.
    float const Volume = ContentsArch->Volume;
    if (Volume > 0.0f && ShipOwner->get_cargo_hold_remaining() < Volume)
        return TractorCargoFull;

    switch (ContentsArch->get_class_type())
    {
    case Archetype::AClassRepairKit:
        if (ShipOwner->get_repair_kit_capacity_remaining() == 0)
            return TractorRepairKitFull;
        break;
    case Archetype::AClassShieldBattery:
        if (ShipOwner->get_shield_battery_capacity_remaining() == 0)
            return TractorShieldBattFull;
        break;
    case Archetype::AClassMunition:
    case Archetype::AClassMine:
    case Archetype::AClassCounterMeasure:
        if (ShipOwner->get_ammo_capacity_remaining(ContentsArch) == 0)
            return TractorAmmoFull;
        break;
    default:
        break;
    }

    return TractorOk;
}

Vector CETractor::GetSourcePos() const
{
    // Common (0x3e730).
    return ResolveTractorSourceXform(Owner, HpXform).Translation;
}

bool CETractor::CanSeeTarget(CLoot const* Target) const
{
    // Common (0x3e660). Same range check as VerifyTarget's first one, just
    // against an arbitrary target position rather than a real line-of-sight raycast.
    // the three differences stay unrounded on the FPU; Range == 0 passes;
    // else (dz^2 + dy^2) + dx^2 against Range^2, TEST AH,0x5 / JP: false only when below.
    Vector const SourcePos = GetSourcePos();
    double const Dx = (double)Target->WorldPosition.x - SourcePos.x;
    double const Dy = (double)Target->WorldPosition.y - SourcePos.y;
    double const Dz = (double)Target->WorldPosition.z - SourcePos.z;
    float const Range = GetRange();
    if (Range != 0.0f && (double)Range * Range < Dz * Dz + Dy * Dy + Dx * Dx)
        return false;
    return true;
}

TractorArm const* CETractor::GetTractorArm(unsigned TargetId) const
{
    // Common (0x3e7a0).
    for (std::vector<TractorArm>::const_iterator It = Arms.begin(); It != Arms.end(); ++It)
    {
        CLoot* Target = It->GetTarget();
        if (Target != nullptr && Target->get_id() == TargetId)
            return &*It;
    }
    return nullptr;
}

Archetype::Tractor const* CETractor::TractorArch() const
{
    return (Archetype::Tractor const*)(Arch);
}

CETractor* __cdecl CETractor::cast(CEquip* Equip)
{
    // Listing 0x3e7e0: null-check, then Type == 0x100000.
    if (Equip == nullptr || Equip->GetType() != EquipTypeTractor)
        return nullptr;
    return (CETractor*)(Equip);
}

CETractor const* __cdecl CETractor::cast(CEquip const* Equip)
{
    return cast(const_cast<CEquip*>(Equip));
}
