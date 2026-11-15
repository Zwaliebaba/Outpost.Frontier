// Author: Giperion.

// Common.dll AI weapon fire control (baseline 0x062e6a20, 0x062e8400, 0x062ea050-0x062ebb10):
// SustainedThresholdTimer, the gun / missile / mine-dropper passes of
// SystemManagerGunGroupManager, and the countermeasure-dropper block at SystemManager+0x1344
// (FCmDropperWatcher). The classes are declared in common_systemmanager.h.

// CMN-SPLIT stage 1 (2026-10-14): split out of common_aisteer.cpp, code unchanged.
// CMN-S2-AIR (2026-10-14): the free functions over `void* Owner` became members of the typed
// SystemManagerGunGroupManager / FCmDropperWatcher / SystemManagerTimerTable; no behaviour change.

#include "common_aisteer.h"
#include "common_aisteer_internal.h"
#include "common_systemmanager_internal.h" // CmnSysMgr::IsWatcherLive / WatchedObject
#include "x86math.h"
#include "common_csys.h"          // Acquire3DMathEngine, Matrix
#include "common_physys.h"        // PhySys::FindRayCollisions, RayHit
#include "common_misc.h"          // RandomVector, SUBOBJ_ID_NONE
#include "common_cequip.h"        // CELauncher::cast/GetBarrelPosWS, CEquipManager::FindByID
#include "common_cobject.h"       // CShip, CEqObj::get_behavior_interface
#include "common_systemmanager.h" // SystemManager, SystemManagerGunGroupManager
#include "common_cobjdb.h"        // IObjDB::GetZoneDb
#include "common_universe.h"      // Universe::ISystem
#include "common_iustategraph.h"  // BuildAiTargetList (HasTargetInForwardArc)
#include "x87_trig.h"             // X87Tan (062eabf0 FPTAN)

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace CmnAiSteer;
using CmnSysMgr::IsWatcherLive;
using CmnSysMgr::WatchedObject;

// Common 0x062e6a20 (the routine), item 1790. Raw listing
// Common (module storage-062e6a6e, 27 instructions), thiscall on the
// timer, RET 8. Both compares are `TEST AH,0x41 / JNZ` -- strictly greater falls through.
bool SustainedThresholdTimer::Update(float DeltaTime, float Level)
{
    if (Level > 0.0f && Level > TriggerThreshold)
    {
        Active = 1;
    }
    else if (!Active)
    {
        return false;
    }

    // `FLD dt; FADD [+0xc]; FST [+0xc]; FCOMP [+0x8]` -- the sum is stored
    // as float but the compare reads the unrounded x87 value (TEST AH,0x41 / JNZ: not greater
    // -> return true; fall-through is strictly greater).
    double const Sum = (double)DeltaTime + Accumulator;
    Accumulator = (float)Sum;
    if (Sum > Duration)
    {
        Accumulator = 0.0f;
        Active = 0;
        return false;
    }
    return true;
}

// `[[this+4]+8] ? -8 : 0`, the pattern every method below repeats (e.g. 062ea0f4-062ea0fe).
IObjRW* SystemManagerGunGroupManager::OwnShip() const
{
    return WatchedObject(Owner->Watcher);
}

// `[this+8] ? -8 : 0` (e.g. 062eae19-062eae22).
IObjRW* SystemManagerGunGroupManager::CurrentTarget() const
{
    return WatchedObject(TargetWatcher());
}

// Common 0x062ea050 (the routine), item 1836, Commonea050-062ea219.
// thiscall on &SystemManager::GunGroups, plain RET, the result in AL.
bool SystemManagerGunGroupManager::FireAutoTurrets()
{
    // the enable byte, then Targeting.AttackState (Sys+0x1434).
    if (bEnabled == 0)
        return false;
    SystemManager* const Sys = Owner;
    if (Sys->Targeting.AttackState != 0)
        return false;

    // SpreadAngle ([ESP+0xc]), zeroed when
    // Sys->ExternalIface->is_player() (+0xbc).
    float SpreadAngle = GunFireAccuracyConeAngle;
    if (Sys->SelfTarget.Iface->is_player())
        SpreadAngle = 0.0f;

    // ForceAll ([ESP+0x7]) = Sys+0xbc, the fire-guns damage reaction timer's
    // Active byte; an empty array returns true. EBX walks the slots at their +0x02 byte.
    unsigned char const ForceAll = Sys->Timers.Timers[6].Active;
    for (unsigned Index = 0; Index < (unsigned)GunCount; ++Index)
    {
        FGunGroupSlot& Slot = Guns[Index];

        // FCOMP 0.0f; TEST AH,0x1; JNZ -- less or unordered falls to the
        // ForceAll test. Then the slot must be an auto-turret.
        bool bEligible = Slot.Phase >= 0.0f && Slot.bFireTick != 0;
        if (!bEligible && ForceAll == 0)
            continue;
        if (Slot.bAutoTurret == 0)
            continue;

        // own ship get_equipment_val(+0x11c)(&Val, HardpointId,
        // EquipValTargetId, 0.0f): the gun's own target id. Nonzero status or
        // (id & 0x3fffffff) == 0 skips the slot.
        EquipmentVal Val{};
        int const Status = OwnShip()->get_equipment_val(Val, Slot.HardpointId, EquipValTargetId, 0.0f);
        if (Status != 0)
            continue;
        unsigned TargetId;
        std::memcpy(&TargetId, &Val, sizeof(TargetId));
        if ((TargetId & 0x3fffffff) == 0)
            continue;

        // Sys->ObjectDb->FindById(&Id) (IObjDB +0x04); none skips the slot.
        IObjRW* const Target = Sys->ObjectDb->FindById(&TargetId);              // EBP
        if (Target == nullptr)
            continue;

        // the routine(HardpointId, Target, GunRange) must answer 1.
        if (CanFireLauncherAtTarget(Slot.HardpointId, Target, GunRange) != 1)
            continue;

        // own ship is_pointing_at(+0x10c)(&bPointing, HardpointId,
        // PUSH 0x3d567750 == 0.05235988f, pi/60).
        bool bPointing = false;
        OwnShip()->is_pointing_at(bPointing, Slot.HardpointId, 0.05235988f);
        if (!bPointing)
            continue;

        // the routine(&FirePos, Target, SUBOBJ_ID_NONE, 1, SpreadAngle,
        // HardpointId) into [ESP+0x14].
        Vector FirePos;
        ComputeLeadFirePosition(&FirePos, Target, SUBOBJ_ID_NONE, 1, SpreadAngle, Slot.HardpointId);

        // IObjAffect slot_34(&HardpointId, 1, &FirePos, -1) on the own ship.
        OwnShip()->slot_34(&Slot.HardpointId, 1u, &FirePos, -1);

        // a classified munition restarts its phase at -FireInterval (FCHS).
        if (Slot.MunitionKind != FGunGroupSlot::KindPlain)
            Slot.Phase = -Slot.FireInterval;
    }
    return true;
}

// Common 0x062ea770 (the routine), item 1839, Commonea770-062ea951,
// RET 0xc: (HardpointId, Target, RangeThreshold). Target (EBX for the whole body) is the
// target's IObjRW.
int SystemManagerGunGroupManager::CanFireLauncherAtTarget(unsigned short HardpointId, IObjRW* Target,
                                                          float RangeThreshold)
{
    // no target, or no own ship -> 2.
    if (Target == nullptr)
        return 2;
    if (!IsWatcherLive(Owner->Watcher))
        return 2;

    // an EquipStatus local whose bits baseline clears (`AND byte,0xf8`,
    // `AND dword,0xe0000000`) before the own ship's get_equipment_status (+0x118) fills it
    // for HardpointId. Every bit read below is one of the cleared ones.
    EquipStatus Status{};
    int const CallResult = OwnShip()->get_equipment_status(Status, HardpointId);
    unsigned char const Flags = Status.Flags;                                   // AL, byte +0
    unsigned char const EquipFlags = (unsigned char)Status.EquipFlags;          // CL, byte +4

    int Result = 2;                                                              // ESI
    if (CallResult == 0 && (Flags & 1) != 0 && (Flags & 6) == 0 && (EquipFlags & 0xa) != 0)
    {
        // the routine(&TargetPos, Target).
        Vector TargetPos;
        GetIfacePositionOrZero(&TargetPos, Target);

        // the own ship's cobject, kept only when (ObjectClass & 0x103) ==
        // 0x103, else 0; its EquipManager (+0xe4) FindByID(HardpointId), CELauncher::cast.
        // baseline does not test the CEqObj before `LEA ECX,[EAX+0xe4]`: the own ship is always a
        // CEqObj here.
        CObject* Obj = (CObject*)OwnShip()->cobject();
        if (Obj == nullptr || (Obj->ObjectClass & 0x103) != 0x103)
            Obj = nullptr;
        CEqObj* const EqObj = (CEqObj*)Obj;
        CELauncher* const Launcher = CELauncher::cast(EqObj->EquipManager.FindByID(HardpointId));
        if (Launcher == nullptr)
            return 0;                                                            // , EAX == 0

        // |TargetPos - GetBarrelPosWS(0)| by the inlined fast length
        // ((dz*dz + dy*dy) + dx*dx on the x87 stack); TEST AH,0x5 + JP: not less than
        // RangeThreshold (or unordered) -> 5, else 1 and the line-of-fire check.
        Vector const BarrelPos = Launcher->GetBarrelPosWS(0);
        double const Dx = (double)TargetPos.x - BarrelPos.x;
        double const Dy = (double)TargetPos.y - BarrelPos.y;
        double const Dz = (double)TargetPos.z - BarrelPos.z;
        double const Dist = InlineFastLength((Dz * Dz + Dy * Dy) + Dx * Dx);
        if (!(Dist < (double)RangeThreshold))
            return 5;
        Result = 1;
    }
    else
    {
        // / the branch / the branch: 2 when Flags bit 0 is set, else 0;
        // EquipFlags bit 2 -> 4; bit 3 -> return 3.
        if ((Flags & 1) == 0)
            Result = 0;
        if ((EquipFlags & 4) != 0)
            Result = 4;
        if ((EquipFlags & 8) != 0)
            return 3;
        if (Result != 1)
            return Result;
    }

    // the routine(Target, HardpointId) == 1 -> 7.
    if (IsLineOfFireBlocked(Target, HardpointId))
        Result = 7;
    return Result;
}

// Common 0x062ea450 (the routine), item 1838, Commonea450-062ea760,
// RET 8: (Target, HardpointId). Re-traced 2026-10-08 (cmn-ai8).
bool SystemManagerGunGroupManager::IsLineOfFireBlocked(IObjRW* Target, unsigned short HardpointId)
{
    SystemManager* const Sys = Owner;

    // TargetId [ESP+0x2c] = Target->get_id() (+0x20).
    unsigned const TargetId = Target->get_id();

    // the own ship's cobject() (+0x150), result discarded.
    (void)OwnShip()->cobject();

    // TargetObj [ESP+0x28] = Target->cobject() when non-null and bit 0
    // of its class is set (AND ECX,1; CMP CL,1), else null. A hit on it is not a blocker.
    CObject const* TargetObj = Target->cobject();
    if (TargetObj == nullptr || ((unsigned)TargetObj->ObjectClass & 1) != 1)
        TargetObj = nullptr;

    FLauncherFireCacheSlot* const Slots = LineOfFireCache;
    int FreeIndex = -1;
    int MatchIndex = -1;
    for (int i = 0; i < 16; ++i)
    {
        if (!Slots[i].InUse)
        {
            if (FreeIndex == -1)
                FreeIndex = i;
            continue;
        }
        if (Slots[i].TargetId == TargetId && Slots[i].HardpointId == HardpointId)
        {
            MatchIndex = i;
            break;
        }
    }

    // no match -- claim the first free slot, if any (NeedsRefresh = 1,
    // Age = RefreshTimer = 0, bBlocked = 0, InUse = 1, ids), and answer false.
    if (MatchIndex == -1)
    {
        if (FreeIndex != -1)
        {
            FLauncherFireCacheSlot& NewSlot = Slots[FreeIndex];
            NewSlot.NeedsRefresh = 1;
            NewSlot.Age = 0.0f;
            NewSlot.RefreshTimer = 0.0f;
            NewSlot.bBlocked = 0;
            NewSlot.InUse = 1;
            NewSlot.TargetId = TargetId;
            NewSlot.HardpointId = HardpointId;
        }
        return false;
    }

    // a fresh match answers its cached byte; a stale one is cleared
    // (+0x14, +0x15, +0xc) and recomputed.
    FLauncherFireCacheSlot& Slot = Slots[MatchIndex];
    if (Slot.NeedsRefresh == 0)
        return Slot.bBlocked != 0;

    Slot.NeedsRefresh = 0;
    Slot.bBlocked = 0;
    Slot.Age = 0.0f;

    // the own ship's cobject(), kept when (class & 0x103) == 0x103 else 0;
    // baseline then takes `LEA ECX,[EAX+0xe4]` with no null test -- the own ship is a CEqObj.
    CObject* Own = (CObject*)OwnShip()->cobject();
    if (Own == nullptr || ((unsigned)Own->ObjectClass & 0x103) != 0x103)
        Own = nullptr;
    CELauncher* const Launcher = CELauncher::cast(((CEqObj*)Own)->EquipManager.FindByID(HardpointId));
    if (Launcher == nullptr)
        return Slot.bBlocked != 0;

    // vtable slot 2 (+0x08, CExternalEquip::IsDestroyed) or slot 4
    // (+0x10, CELauncher::IsDisabled) -> the cleared answer.
    if (Launcher->IsDestroyed())
        return Slot.bBlocked != 0;
    if (Launcher->IsDisabled())
        return Slot.bBlocked != 0;

    // barrel 0's world position [ESP+0x10] and direction [ESP+0x30].
    Vector const BarrelPos = Launcher->GetBarrelPosWS(0);
    Vector const BarrelDir = Launcher->GetBarrelDirWS(0);

    // the system id -- Sys->ObjectDb->GetZoneDb() (+0x08), its GetId
    // (+0x00); then Target->get_position() (+0x00).
    Universe::ISystem const* const System = Sys->ObjectDb->GetZoneDb();
    unsigned const SystemId = System->GetId();
    Vector const TargetPos = Target->get_position();

    // Far [ESP+0x48] = Barrel + Dir * 2000.0f (the field), Near
    // [ESP+0x1c] = Barrel + Dir * 20.0f (the field). x and y add the unrounded product;
    // z's product is FSTP'd to float first.
    Vector Far;
    Far.x = (float)((double)BarrelDir.x * 2000.0f + BarrelPos.x);
    Far.y = (float)((double)BarrelPos.y + (double)BarrelDir.y * 2000.0f);
    float const FarDz = (float)((double)BarrelDir.z * 2000.0f);
    Far.z = (float)((double)BarrelPos.z + FarDz);
    Vector Near;
    Near.x = (float)((double)BarrelDir.x * 20.0f + BarrelPos.x);
    Near.y = (float)((double)BarrelDir.y * 20.0f + BarrelPos.y);
    float const NearDz = (float)((double)BarrelDir.z * 20.0f);
    Near.z = (float)((double)NearDz + BarrelPos.z);

    // PhySys::FindRayCollisions(SystemId, Near, Far, Hits [ESP+0x60], 8).
    PhySys::RayHit Hits[8];
    int const HitCount = PhySys::FindRayCollisions(SystemId, Near, Far, Hits, 8);
    for (int i = 0; i < HitCount; ++i)
    {
        // a non-null hit object with (class & 0x103) == 0x103, class 0x303
        // or 0x503, that is not the target's own object.
        CObject* const HitObj = Hits[i].Object;
        if (HitObj == nullptr)
            continue;
        unsigned const HitClass = (unsigned)HitObj->ObjectClass;
        if ((HitClass & 0x103) != 0x103)
            continue;
        if (HitClass != 0x303 && HitClass != 0x503)
            continue;
        if (HitObj == TargetObj)
            continue;

        // the first such hit decides: bBlocked = 1 when the target's squared
        // distance from the barrel exceeds the hit's (FCOMPP; TEST AH,0x41 + JNZ leaves on
        // less, equal or unordered). Sums run (z*z + y*y) + x*x on the x87 stack; only the
        // target's dz is FSTP'd to float.
        double const Hx = (double)Hits[i].Point.x - BarrelPos.x;
        double const Hy = (double)Hits[i].Point.y - BarrelPos.y;
        double const Hz = (double)Hits[i].Point.z - BarrelPos.z;
        double const Tx = (double)TargetPos.x - BarrelPos.x;
        double const Ty = (double)TargetPos.y - BarrelPos.y;
        float const Tz = (float)((double)TargetPos.z - BarrelPos.z);
        double const HitSq = (Hz * Hz + Hy * Hy) + Hx * Hx;
        double const TargetSq = ((double)Tz * Tz + Ty * Ty) + Tx * Tx;
        if (TargetSq > HitSq)
            Slot.bBlocked = 1;
        return Slot.bBlocked != 0;
    }

    return Slot.bBlocked != 0;
}

// Common 0x062ea960 (the routine), item 1840, Commonea960-062eac6d,
// RET 0x18. Re-traced 2026-10-08 (cmn-ai8).
void SystemManagerGunGroupManager::ComputeLeadFirePosition(Vector* OutPos, IObjRW* Target,
                                                           unsigned short UnusedSubobjSlot,
                                                           unsigned char UseDirectAim, float SpreadAngle,
                                                           unsigned short SubtargetId)
{
    (void)UnusedSubobjSlot; // baseline pushes this (stack slot +0xC) but never reads it back
    SystemManager* const Sys = Owner;

    // Target's get_orientation (+0x10, folded with get_transform) is
    // copied whole (12 dwords) to [ESP+0x40]. The lead position local [ESP+0x64] IS that
    // copy's +0x24 translation, so without UseDirectAim the result starts at the target's
    // own position.
    Transform TargetXform = Target->get_transform();
    Vector& LeadPos = TargetXform.Translation;

    if (UseDirectAim != 0)
    {
        // CMP SUBOBJ_ID_NONE,SubtargetId; JZ -> the CShip path.
        if (SubtargetId != SUBOBJ_ID_NONE)
        {
            // own ship get_tgt_lead_fire_pos(+0x108)(SubtargetId, LeadPos).
            (void)OwnShip()->get_tgt_lead_fire_pos(SubtargetId, LeadPos);
        }
        else
        {
            // own ship cobject() (+0x150); when non-null and
            // (ObjectClass & 0x503) == 0x503, CShip::get_tgt_lead_fire_pos(LeadPos); on a null
            // object, another class or a false answer, LeadPos = Target->get_position().
            CObject const* const Obj = OwnShip()->cobject();
            bool bGot = false;
            if (Obj != nullptr && ((unsigned)Obj->ObjectClass & 0x503) == 0x503)
                bGot = ((CShip const*)Obj)->get_tgt_lead_fire_pos(LeadPos);
            if (!bGot)
                LeadPos = Target->get_position();
        }
    }

    // SelfTarget's position cache (FAiTarget::RefreshPos inlined).
    Sys->SelfTarget.RefreshPos();

    // DirVec ([ESP+0x1c], the routine) = LeadPos - SelfTarget.CachedPos,
    // each FSUB FSTP'd to float.
    Vector DirVec;
    Vec3Assign(&DirVec, LeadPos.x - Sys->SelfTarget.CachedPos.x, LeadPos.y - Sys->SelfTarget.CachedPos.y,
               LeadPos.z - Sys->SelfTarget.CachedPos.z);

    // ONE rand(), FILD * 1/32768 (the field), FSTP float into the
    // UseDirectAim argument slot.
    int const RandBits = std::rand();
    float const RandFrac = (float)((double)RandBits * (double)3.0517578125e-05f);

    // Target->is_player() (+0xbc). A player target slower than
    // sqrt(10) -- (x*x + y*y) + z*z of get_velocity (+0x04) against 10.0f (the field),
    // TEST AH,0x5 + JP: only "less" returns -- gets LeadPos as is. The exponent is
    // gun_fire_accuracy_power (Sys+0x171c) for a player target, _npc (Sys+0x1720) otherwise.
    float Exponent;
    if (Target->is_player())
    {
        Vector const Velocity = Target->get_velocity();
        double const SpeedSq = ((double)Velocity.x * Velocity.x + (double)Velocity.y * Velocity.y) +
                               (double)Velocity.z * Velocity.z;
        if (SpeedSq < 10.0)
        {
            *OutPos = LeadPos;
            return;
        }
        Exponent = Sys->PersonalityData.GunParams.GunFireAccuracyPower;
    }
    else
    {
        Exponent = Sys->PersonalityData.GunParams.GunFireAccuracyPowerNpc;
    }

    // _CIpow(RandFrac, Exponent) * SpreadAngle, FSTP float.
    float const Spread = (float)(std::pow((double)RandFrac, (double)Exponent) * (double)SpreadAngle);

    // RandomVector(1.0f) into [ESP+0x28].
    Vector const RandVec = RandomVector(1.0f);

    // FCOMP 0.01f (the field); TEST AH,0x41 + JNZ -- not greater
    // (or unordered) returns LeadPos.
    if (!(Spread > 0.01f))
    {
        *OutPos = LeadPos;
        return;
    }

    // Cross = RandVec x DirVec, each component a pair of x87 products
    // FSUBP'd and FSTP'd to float.
    Vector Cross;
    Cross.x = (float)((double)DirVec.z * RandVec.y - (double)RandVec.z * DirVec.y);
    Cross.y = (float)((double)RandVec.z * DirVec.x - (double)DirVec.z * RandVec.x);
    Cross.z = (float)((double)DirVec.y * RandVec.x - (double)RandVec.y * DirVec.x);

    // the routine normalises it.
    Vector Normal;
    VecNormalize3(&Cross, &Normal);

    // Dist = sqrt((Dz*Dz + Dy*Dy) + Dx*Dx) * tan(Spread), all on the x87
    // stack (FSQRT, FPTAN, FMULP).
    double const DirLength = std::sqrt(((double)DirVec.z * DirVec.z + (double)DirVec.y * DirVec.y) +
                                       (double)DirVec.x * DirVec.x);
    double const Dist = DirLength * X87Tan((double)Spread);

    // x keeps Normal.x * Dist unrounded before adding LeadPos.x; y and z
    // FSTP their product to float first.
    float const OffsetY = (float)((double)Normal.y * Dist);
    float const OffsetZ = (float)((double)Normal.z * Dist);
    OutPos->x = (float)((double)Normal.x * Dist + (double)LeadPos.x);
    OutPos->y = (float)((double)OffsetY + (double)LeadPos.y);
    OutPos->z = (float)((double)OffsetZ + (double)LeadPos.z);
}

// Common 0x062eb020 (the routine). Item 1842. Commoneb020-062eb068:
// thiscall on a Transform (ECX), RET 8 (Out, In). The math engine (the field, lazily
// ) InverseTransform (+0x3c) writes a 12-byte local, which is copied to *Out;
// EAX = Out. Vector / Transform and x86math's FVector / FTransform are the same layouts.
Vector* TransformInversePoint(Transform const* Self, Vector* Out, Vector const* In)
{
    Vector Local;
    Acquire3DMathEngine()->InverseTransform((FVector&)Local, *(FTransform const*)Self, *(FVector const*)In);
    *Out = Local;
    return Out;
}

// Common 0x062eac70 (the routine), item 1841, Commoneac70-062eb018, RET 4
// (the stack argument is never read). Re-traced 2026-10-08 (cmn-ai8).
int SystemManagerGunGroupManager::UpdateGunFireState()
{
    SystemManager* const Sys = Owner;

    // bit 0 of Sys+0x147c (Targeting.SelectionPriority) clear -> 8.
    if ((Sys->Targeting.SelectionPriority & 1) == 0)
        return 8;

    // the own ship (Sys+8) and the target (this+8) must both be live,
    // else 0.
    if (!IsWatcherLive(Sys->Watcher))
        return 0;
    if (!IsWatcherLive(TargetWatcher()))
        return 0;

    // Count [ESP+0x10] = 0, bOverride [ESP+0xb] = 0, SpreadAngle
    // [ESP+0xc]; the own ship's get_target (+0x74) into a zeroed local nobody reads.
    int Count = 0;
    unsigned char bOverride = 0;
    float SpreadAngle = GunFireAccuracyConeAngle;
    IObjRW* UnusedTarget = nullptr;
    (void)OwnShip()->get_target(UnusedTarget);

    // +0x10 & 0x30000000 -> bOverride = 1, SpreadAngle = 0.
    if ((Field_0x10 & 0x30000000) != 0)
    {
        bOverride = 1;
        SpreadAngle = 0.0f;
    }

    // ForceAll [ESP+0x9] = Sys+0xbc (fire-guns damage reaction), Result
    // [ESP+0x14] = 2.
    unsigned char const ForceAll = Sys->Timers.Timers[6].Active;
    int Result = 2;

    // Target (EBX) = this+8's object; its get_orientation (+0x10, folded
    // with get_transform) copied whole to [ESP+0x44]. SUBOBJ_ID_NONE goes to [ESP+0x34].
    IObjRW* const Target = CurrentTarget();
    Transform TargetXform = Target->get_transform();

    // FCOMP 0.0f (the field); TEST AH,0x41 + JP skips on greater or
    // unordered. Otherwise the aim point is refreshed: the routine(&LeadPos, Target,
    // SUBOBJ_ID_NONE, 0, SpreadAngle, SUBOBJ_ID_NONE), its target-local image through
    // on the copied transform into AimOffset, and AimRefreshTimer =
    // gun_target_point_switch_time (Sys+0x172c).
    if (AimRefreshTimer <= 0.0f)
    {
        Vector LeadPos;                                                          // [ESP+0x28]
        ComputeLeadFirePosition(&LeadPos, Target, SUBOBJ_ID_NONE, 0, SpreadAngle, SUBOBJ_ID_NONE);
        Vector LocalAim;                                                         // [ESP+0x38]
        Vector const* const Aim = TransformInversePoint(&TargetXform, &LocalAim, &LeadPos);
        AimOffset = *Aim;
        AimRefreshTimer = Sys->PersonalityData.GunParams.GunTargetPointSwitchTime;
    }

    // [ESP+0x74]: the ids to fire. The frame ends 0x80 bytes later, 64 ids -- Guns holds at
    // most 0x40 slots (ResolveHardpoints).
    unsigned short FireIds[64];
    for (unsigned Index = 0; Index < (unsigned)GunCount; ++Index)
    {
        FGunGroupSlot& Slot = Guns[Index];                                      // ESI

        // eligible when (Phase >= 0.0f -- TEST AH,0x1 + JNZ sends less
        // and unordered on -- and bFireTick set), or bOverride, or ForceAll. BL = ForceAll.
        bool const bReady = Slot.Phase >= 0.0f && Slot.bFireTick != 0;
        if (!bReady && bOverride == 0 && ForceAll == 0)
            continue;

        // a classified munition or an auto-turret skips the slot.
        if (Slot.MunitionKind != FGunGroupSlot::KindPlain || Slot.bAutoTurret != 0)
            continue;

        // Result = the routine(Id, Target, GunRange); unless it is 1,
        // only ForceAll carries on.
        Result = CanFireLauncherAtTarget(Slot.HardpointId, CurrentTarget(), GunRange);
        if (Result != 1 && ForceAll == 0)
            continue;

        // own ship IObjAffect slot_44(&Id, Target, SUBOBJ_ID_NONE, -1) --
        // points the gun at the target.
        OwnShip()->slot_44(&Slot.HardpointId, CurrentTarget(), SUBOBJ_ID_NONE, -1);

        // own ship is_pointing_at(+0x10c)(&bPointing [ESP+0x12], Id,
        // == 0.05235988f). A pointing gun, or ForceAll, adds the id to FireIds
        // (no bound check in baseline; at most 64 slots exist).
        bool bPointing = false;
        OwnShip()->is_pointing_at(bPointing, Slot.HardpointId, 0.05235988f);
        if (bPointing || ForceAll != 0)
        {
            FireIds[Count] = Slot.HardpointId;
            ++Count;
        }

        // fire_style SINGLE stops the walk after the first slot that got here.
        if (bFireAllGuns == 0)
            break;
    }

    // JLE -- nothing to fire returns Result.
    if (Count <= 0)
        return Result;

    // the target's transform is copied again to [ESP+0x44].
    TargetXform = CurrentTarget()->get_transform();

    // TargetPos [ESP+0x18]: CShip::get_tgt_lead_fire_pos of the own ship
    // (cobject (+0x150) non-null, (ObjectClass & 0x503) == 0x503); otherwise, or when it
    // answers false, the target's get_position (+0x00) (ESI, re-read at 062eaf07).
    IObjRW* const PositionSource = CurrentTarget();
    Vector TargetPos;
    CObject const* const Obj = OwnShip()->cobject();
    bool bGot = false;
    if (Obj != nullptr && ((unsigned)Obj->ObjectClass & 0x503) == 0x503)
        bGot = ((CShip const*)Obj)->get_tgt_lead_fire_pos(TargetPos);
    if (!bGot)
        TargetPos = PositionSource->get_position();

    // TargetPos replaces the copied transform's translation ([ESP+0x68]).
    TargetXform.Translation = TargetPos;

    // math engine TransformXform (+0x28)(&World [ESP+0x38], TargetXform,
    // AimOffset), World copied to [ESP+0x28].
    Vector World;
    Acquire3DMathEngine()->TransformXform((FVector&)World, (FTransform const&)TargetXform,
                                          (FVector const&)AimOffset);
    Vector const FirePos = World;

    // own ship IObjAffect slot_34(FireIds, Count, &FirePos, -1).
    OwnShip()->slot_34(FireIds, (unsigned)Count, &FirePos, -1);
    return Result;
}

// Common 0x062eb070 (the routine), item 1843, Commoneb070-062eb380, RET 4
// (the stack argument is never read). Re-traced 2026-10-08 (cmn-ai8). Walks Guns like
// UpdateGunFireState but fires only the seeker, HpType-0xa and cruise-disruptor launchers.
int SystemManagerGunGroupManager::UpdateMissileFireState()
{
    int Result = 0;                                                             // [E-4]

    // the target (this+8) and the own ship (Sys+8) must both be live, else 0.
    if (!IsWatcherLive(TargetWatcher()))
        return 0;
    SystemManager* const Sys = Owner;
    if (!IsWatcherLive(Sys->Watcher))
        return 0;

    // ShipFlags [E-0xc] = Sys+0x147c (Targeting.SelectionPriority);
    // +0x10 & 0x30000000 -> 2.
    unsigned const ShipFlags = (unsigned)Sys->Targeting.SelectionPriority;
    if ((Field_0x10 & 0x30000000) != 0)
        return 2;

    // bForceFire [E-0x15] = (Sys+0xdc != 0), the fire-missiles damage
    // reaction timer's Active byte.
    unsigned char const bForceFire = Sys->Timers.Timers[8].Active != 0 ? 1 : 0;

    for (unsigned Index = 0; Index < (unsigned)GunCount; ++Index)
    {
        FGunGroupSlot& Slot = Guns[Index];                                      // EBP - 8

        // Phase >= 0.0f (TEST AH,0x1 + JNZ skips less / unordered) and
        // bFireTick set.
        if (!(Slot.Phase >= 0.0f))
            continue;
        if (Slot.bFireTick == 0)
            continue;

        // the munition kind. BL (bWide) picks the aim angle below.
        int const Kind = Slot.MunitionKind;
        unsigned char bWide = 0;
        if (Kind == FGunGroupSlot::KindSeeker)
        {
            // bSeekerMunition set: ShipFlags bit 1; clear: bit 2, wide.
            if (Slot.bSeekerMunition != 0)
            {
                if ((ShipFlags & 2) == 0)
                    continue;
            }
            else
            {
                bWide = 1;
                if ((ShipFlags & 4) == 0)
                    continue;
            }
        }
        else if (Kind == FGunGroupSlot::KindHpType10)
        {
            bWide = 1;
            if ((ShipFlags & 8) == 0)
                continue;
        }
        else if (Kind == FGunGroupSlot::KindCruiseDisruptor)
        {
            // FCOMP Sys+0x1768 (anti_cruise_missile_pre_fire_delay);
            // TEST AH,0x41 + JNZ -- CruiseHeldTime must be greater.
            if (!(CruiseHeldTime > Sys->PersonalityData.MissileParams.AntiCruiseMissilePreFireDelay))
                continue;

            // SelfTarget's position cache (FAiTarget::RefreshPos inlined).
            Sys->SelfTarget.RefreshPos();

            // the target's get_position (+0x00) less the cached centre, its
            // squared length (z*z + y*y) + x*x on the x87 stack, the 0x5f3759df fast length.
            Vector const& TargetPos = CurrentTarget()->get_position();
            double const Dx = (double)TargetPos.x - Sys->SelfTarget.CachedPos.x;
            double const Dy = (double)TargetPos.y - Sys->SelfTarget.CachedPos.y;
            double const Dz = (double)TargetPos.z - Sys->SelfTarget.CachedPos.z;
            double const Dist = InlineFastLength((Dz * Dz + Dy * Dy) + Dx * Dx);

            // FCOM Sys+0x1760 (anti_cruise_missile_min_distance), TEST
            // AH,0x41 + JNZ -- must be greater; FCOMP Sys+0x1764 (_max_distance), TEST AH,0x5 +
            // JP -- must be less.
            if (!(Dist > Sys->PersonalityData.MissileParams.AntiCruiseMissileMinDistance))
                continue;
            if (!(Dist < Sys->PersonalityData.MissileParams.AntiCruiseMissileMaxDistance))
                continue;
        }
        else
        {
            continue;
        }

        // Result = the routine(Id, Target, MissileLaunchRange); only 1
        // carries on.
        Result = CanFireLauncherAtTarget(Slot.HardpointId, CurrentTarget(), MissileLaunchRange);
        if (Result != 1)
            continue;

        // own ship IObjAffect slot_44(&Id, Target, SUBOBJ_ID_NONE, -1).
        OwnShip()->slot_44(&Slot.HardpointId, CurrentTarget(), SUBOBJ_ID_NONE, -1);

        // own ship is_pointing_at(+0x10c)(&bPointing [E-0x16], Id, bWide ?
        // WideConeAngle : MissileLaunchConeAngle); pointing or bForceFire fires.
        bool bPointing = false;
        float const Angle = bWide != 0 ? WideConeAngle : MissileLaunchConeAngle;
        OwnShip()->is_pointing_at(bPointing, Slot.HardpointId, Angle);
        if (!bPointing && bForceFire == 0)
            continue;

        // IObjAffect slot_34(&Id, 1, null, -1); a classified munition then
        // restarts its phase at -FireInterval.
        OwnShip()->slot_34(&Slot.HardpointId, 1u, nullptr, -1);
        if (Slot.MunitionKind != FGunGroupSlot::KindPlain)
            Slot.Phase = -Slot.FireInterval;
    }

    return Result;
}

namespace
{
    // a .data byte (uninitialised, so 0 at load) whose only reference in
    // the module is the read at module storage. Kept as the global it is.
    unsigned char GNearFieldAimResetSuppressed = 0;
}

// Common 0x062e8400 (the routine), item 1806, Commone8400-062e86cd.
bool SystemManagerTimerTable::HasTargetInForwardArc(float MaxAngle, float MaxRange)
{
    // BuildAiTargetList(this); the loop re-reads End on every pass (062e86a7).
    FAiTargetVector* const List = BuildAiTargetList(this);
    for (FAiTarget* Elem = List->Begin; Elem != List->End; ++Elem)
    {
        // the lazily cached type (flag raised after the call).
        if (Elem->bFlagsValid == 0)
        {
            Elem->Iface->get_type(Elem->CachedFlags);
            Elem->bFlagsValid = 1;
        }
        // TEST [ESI+8],0x7f0000 / JZ next.
        if ((Elem->CachedFlags & 0x7f0000) == 0)
            continue;

        // refresh the target's and the own ship's cached positions
        // (Owner+0x14fc is the own FAiTarget; its +0x7e is SystemManager+0x157a).
        Elem->RefreshPos();
        FAiTarget* const Own = &Owner->SelfTarget;
        Own->RefreshPos();

        // D = Own.CachedPos - Elem.CachedPos (FSTP float each).
        Vector D;
        D.x = Own->CachedPos.x - Elem->CachedPos.x;
        D.y = Own->CachedPos.y - Elem->CachedPos.y;
        D.z = Own->CachedPos.z - Elem->CachedPos.z;

        // OwnFwd = -(own z column) (the routine + the routine);
        // Dot = (F.z*D.z + F.y*D.y) + F.x*D.x, FST float kept for the cosine below;
        // the unrounded value must be strictly greater than 0.0f.
        Matrix const& OwnRotA = Own->Iface->get_transform().Rotation;
        Vector const OwnFwdA{ -OwnRotA.E[0][2], -OwnRotA.E[1][2], -OwnRotA.E[2][2] };
        double const Dot = ((double)OwnFwdA.z * D.z + (double)OwnFwdA.y * D.y) + (double)OwnFwdA.x * D.x;
        float const DotF = (float)Dot;
        if (!(Dot > 0.0))
            continue;

        // `FLD MaxRange; FMUL MaxRange; FCOMPP` against |D|^2
        // ((z*z + y*y) + x*x); TEST AH,0x41 + JNZ: continue unless MaxRange^2 is greater.
        double const DistSq = ((double)D.z * D.z + (double)D.y * D.y) + (double)D.x * D.x;
        if (!((double)MaxRange * MaxRange > DistSq))
            continue;

        // the target's get_velocity() (+0x4); speed^2 must exceed 100.0f
        // (the field).
        Vector const Velocity = Elem->Iface->get_velocity();
        double const SpeedSq = ((double)Velocity.z * Velocity.z + (double)Velocity.y * Velocity.y) +
                               (double)Velocity.x * Velocity.x;
        if (!(SpeedSq > 100.0))
            continue;

        // both forwards (own re-read, then the target's), dot
        // (O.z*T.z + O.y*T.y) + T.x*O.x strictly above 0.95f (the field).
        Matrix const& OwnRotB = Own->Iface->get_transform().Rotation;
        Vector const OwnFwdB{ -OwnRotB.E[0][2], -OwnRotB.E[1][2], -OwnRotB.E[2][2] };
        Matrix const& TgtRot = Elem->Iface->get_transform().Rotation;
        Vector const TgtFwd{ -TgtRot.E[0][2], -TgtRot.E[1][2], -TgtRot.E[2][2] };
        double const Heading = ((double)OwnFwdB.z * TgtFwd.z + (double)OwnFwdB.y * TgtFwd.y) +
                               (double)TgtFwd.x * OwnFwdB.x;
        if (!(Heading > (double)0.95f))
            continue;

        // the routine (GetCachedRange) -> attitude; TEST AH,0x41 + JP
        // skips "greater or unordered", so only attitude <= -0.6f (the field) goes on.
        float Attitude;
        float ReverseAttitude;
        GetCachedRange(Elem, &Attitude, &ReverseAttitude);
        if (!(Attitude <= -0.6f))
            continue;

        // cos = DotF / |D| (FDIVR), capped at 1.0f (TEST AH,0x5 + JP keeps
        // the ratio when 1.0 >= it), FSTP float; _CIacos; TEST AH,0x41 + JNP: an angle not
        // above MaxAngle returns true.
        double const Ratio = (double)DotF / std::sqrt(DistSq);
        float const Cosine = (1.0 < Ratio) ? 1.0f : (float)Ratio;
        if (std::acos((double)Cosine) <= (double)MaxAngle)
            return true;
    }
    return false;
}

// Common 0x062eb390 (the routine), item 1844, Commoneb390-062eb547, RET 4
// (the stack argument is never read).
unsigned SystemManagerGunGroupManager::UpdateMineDropState()
{
    unsigned Result = 0;                                                      // [ESP+0xc]

    // the target (this+8), then the own ship (Sys+8), must be live.
    if (!IsWatcherLive(TargetWatcher()))
        return 0;
    SystemManager* const Sys = Owner;
    if (!IsWatcherLive(Sys->Watcher))
        return 0;

    // .
    if ((Field_0x10 & 0x30000000) != 0)
        return 2;

    // bAggressive = Sys+0xcc, the drop-mines damage reaction timer's
    // Active byte; the own ship's get_velocity() (ExternalIface +0x4), sqrt((z*z + y*y) + x*x)
    // must exceed 20.0f (the field; TEST AH,0x41 + JNZ leaves on not-greater).
    bool const bAggressive = Sys->Timers.Timers[7].Active != 0;
    Vector const Velocity = Sys->SelfTarget.Iface->get_velocity();
    double const Speed = std::sqrt(((double)Velocity.z * Velocity.z + (double)Velocity.y * Velocity.y) +
                                   (double)Velocity.x * Velocity.x);
    if (!(Speed > 20.0))
        return Result;

    // unless aggressive, the routine(&Sys->Timers, MineLaunchConeAngle,
    // mine_launch_range (Sys+0x1748)) must find a hostile in the forward arc.
    if (!bAggressive &&
        !Sys->Timers.HasTargetInForwardArc(MineLaunchConeAngle, Sys->PersonalityData.MineParams.MineLaunchRange))
        return Result;

    // the EquipStatus local (baseline reuses the velocity slot and clears
    // byte +0 & 0xf8 / dword +4 & 0xe0000000 -- every bit read below is a cleared one);
    // an empty array returns.
    EquipStatus Status{};
    if (MineDropperCount == 0)
        return Result;

    for (unsigned Index = 0; Index < (unsigned)MineDropperCount; ++Index)
    {
        FGunGroupSlot& Slot = MineDroppers[Index];                              // EDI = &Slot.MunitionKind

        // unless aggressive, Phase must be >= 0.0f (TEST AH,0x1 + JNZ
        // skips less or unordered) and bFireTick set.
        if (!bAggressive)
        {
            if (!(Slot.Phase >= 0.0f))
                continue;
            if (Slot.bFireTick == 0)
                continue;
        }

        // own ship get_equipment_status(+0x118) for the slot's hardpoint;
        // EquipFlags must have both bits 0x2 and 0x8.
        OwnShip()->get_equipment_status(Status, Slot.HardpointId);
        if ((Status.EquipFlags & 0xa) != 0xa)
            continue;

        // the result becomes 1 before the the field test.
        Result = 1;
        if (GNearFieldAimResetSuppressed != 0)
            continue;

        // IObjAffect slot_34(&HardpointId, 1, null, -1) on the own ship,
        // then Phase = -FireInterval when the slot's MunitionKind is nonzero.
        OwnShip()->slot_34(&Slot.HardpointId, 1u, nullptr, -1);
        if (Slot.MunitionKind != FGunGroupSlot::KindPlain)
            Slot.Phase = -Slot.FireInterval;
    }

    return Result;
}

// Common 0x062eb550 (the routine), item 1845, Commoneb550-062eb595. The
// sole caller is BuzzBehavior (module storage). baseline has no null test between the own-ship
// computation and the IObjAffect call (062eb567-062eb586); neither does the port.
void SystemManagerGunGroupManager::DropAllMines()
{
    if (MineDropperCount == 0)
        return;

    // own ship IObjAffect slot_34(&Id, 1, null, -1) per mine dropper, the
    // own ship re-read every pass, the count re-read and compared unsigned.
    unsigned Index = 0;
    do
    {
        OwnShip()->slot_34(&MineDroppers[Index].HardpointId, 1u, nullptr, -1);
        ++Index;
    } while (Index < (unsigned)MineDropperCount);
}

// (item 1833) and the routine (item 1829) used to be stood in for here by
// _ReturnGate (`return Handle != nullptr`, dropping the watcher retarget and the
// three notifications) and an empty RefreshWeaponSlot_TODO. Both are ported in
// common_sysmgr_gungroups.cpp -- Retarget and RandomizeSlotTiming -- and are called directly
// below since 2026-09-24.

// Common 0x062eb5a0 (the routine), item 1846, Commoneb5a0-062eb65d, RET 8.
bool SystemManagerGunGroupManager::ComputeGunRangeStats(float* OutAverage, float* OutMax)
{
    if (GunCount == 0)
        return false;

    // Count (EBP), Max [E-4], Sum [E-8] and the value local [E-0xc] all
    // start at zero; the value local is not reset per slot.
    int MatchCount = 0;
    float Max = 0.0f;
    float Sum = 0.0f;
    EquipmentVal Value{};

    unsigned Index = 0;
    do
    {
        // a plain gun asks the own ship's get_equipment_val(+0x11c)(&Value,
        // Id, EquipValRange, 0.0f).
        FGunGroupSlot const& Slot = Guns[Index];
        if (Slot.MunitionKind == FGunGroupSlot::KindPlain &&
            OwnShip()->get_equipment_val(Value, Slot.HardpointId, EquipValRange, 0.0f) == 0)
        {
            // Sum += Value (FSTP float); FCOMP Max, TEST AH,0x41 + JNZ --
            // only a greater value replaces Max.
            ++MatchCount;
            Sum = (float)((double)Value.Value + Sum);
            if (Value.Value > Max)
                Max = Value.Value;
        }

        ++Index;
    } while (Index < (unsigned)GunCount);

    // JLE on the count; FILD count, FDIVR Sum -> *OutAverage, Max ->
    // *OutMax, AL = 1.
    if (MatchCount <= 0)
        return false;

    *OutAverage = (float)((double)Sum / (double)MatchCount);
    *OutMax = Max;
    return true;
}

// Common 0x062eb6c0 (the routine), item 1848, Commoneb6c0-062eb8f4. The
// call at 062eb740 is IBehaviorManager::get_sys (analysis names the ICF fold
// CPlayerTradeOffer::GetTargetPlayerID). Structural twin: RefreshRepairManagerFromPersonality.
bool SystemManagerGunGroupManager::RefreshFromPersonality()
{
    // a null owner returns false.
    SystemManager* const Sys = Owner;
    if (Sys == nullptr)
        return false;

    // EDI = the config source: the owner itself unless the branch below swaps in the
    // formation leader's SystemManager.
    SystemManager* Src = Sys;

    // ExternalIface (+0x1580) is_player() (+0xBC), no null test. Then
    // FormationParams.bLeaderMakesMeTougher (Sys+0x1820) set and the gun-convergence
    // block's +0xC5 gate (Sys+0x1429) clear; 062eb6fb-062eb70a: the formation leader watched
    // at +0x1370, 0 or 8 skips -- GetFormationLeader reads exactly that gate and watcher.
    if (!Sys->SelfTarget.Iface->is_player() && Sys->PersonalityData.FormationParams.bLeaderMakesMeTougher != 0)
    {
        IObjRW* const Leader = Sys->GunConvergenceStateBytes.GetFormationLeader();
        if (Leader != nullptr)
        {
            // IObjInspect +0x150 cobject(); null, or class & 0x103 !=
            // 0x103 (not a CEqObj), skips.
            CObject const* const Obj = Leader->cobject();
            if (Obj != nullptr && ((unsigned)Obj->ObjectClass & 0x103) == 0x103)
            {
                // CEqObj::get_behavior_interface; non-null -> its
                // SystemManager becomes the config source and the owner's +0x14 latch is set.
                IBehaviorManager* const Iface = ((CEqObj*)Obj)->get_behavior_interface();
                if (Iface != nullptr)
                {
                    Src = Iface->get_sys();
                    Sys->Unknown14 = 1;
                }
            }
        }
    }

    pub::AI::Personality::GunUseStruct const& Gun = Src->PersonalityData.GunParams;
    pub::AI::Personality::MineUseStruct const& Mine = Src->PersonalityData.MineParams;
    pub::AI::Personality::MissileUseStruct const& Missile = Src->PersonalityData.MissileParams;

    // dword copies, in baseline order.
    GunFireIntervalTime = Gun.GunFireIntervalTime;                              // +0x1704
    GunFireIntervalVariance = Gun.GunFireIntervalVariancePercent;               // +0x1708
    AutoTurretIntervalTime = Gun.AutoTurretIntervalTime;                        // +0x1730
    GunFireBurstIntervalTime = Gun.GunFireBurstIntervalTime;                    // +0x170c
    GunFireBurstIntervalVariance = Gun.GunFireBurstIntervalVariancePercent;     // +0x1710
    AutoTurretBurstIntervalTime = Gun.AutoTurretBurstIntervalTime;              // +0x1734
    AutoTurretBurstIntervalVariance = Gun.AutoTurretBurstIntervalVariancePercent; // +0x173c
    GunFireNoBurstIntervalTime = Gun.GunFireNoBurstIntervalTime;                // +0x1714
    AutoTurretNoBurstIntervalTime = Gun.AutoTurretNoBurstIntervalTime;          // +0x1738
    GunFireAccuracyConeAngle = Gun.GunFireAccuracyConeAngle;                    // +0x1718

    // , on the x87 stack (double): R = gun_range_threshold_variance_percent
    // (+0x1728); NegR = float(-R) (FCHS; FSTP float). Frac = FILD rand() * float the field
    // (0x38000000 == 2^-15). ((R - NegR) * Frac + NegR + double the field (1.0)) *
    // gun_range_threshold (+0x1724), stored as a float local.
    float const SpreadRange = Gun.GunRangeThresholdVariancePercent;
    float const NegRange = -SpreadRange;
    double const Frac = (double)std::rand() * (double)3.0517578125e-05f;
    float const SpreadValue =
        (float)((((double)SpreadRange - (double)NegRange) * Frac + (double)NegRange + 1.0) * (double)Gun.GunRangeThreshold);

    // the routine(&local8, &local10) -- baseline reuses the R slot as the
    // first out-param -- and AL == 1 stores local10 * SpreadValue (float FMUL) into GunRange.
    float StatAvg = 0.0f, StatMax = 0.0f;
    if (ComputeGunRangeStats(&StatAvg, &StatMax))
        GunRange = (float)((double)StatMax * (double)SpreadValue);

    MissileLaunchRange = Missile.MissileLaunchRange;                                    // +0x174c
    MissileLaunchAllowOutOfRange = (float)Missile.bMissileLaunchAllowOutOfRange;        // +0x1750 byte, FILD

    // fire_style MULTIPLE (0) -> 1, SINGLE (1) -> 0, anything else keeps it.
    int const FireStyle = Gun.FireStyle;
    if (FireStyle == 0)
        bFireAllGuns = 1;
    else if (FireStyle == 1)
        bFireAllGuns = 0;

    MissileLaunchIntervalTime = Missile.MissileLaunchIntervalTime;                 // +0x1754
    MissileLaunchIntervalVariance = Missile.MissileLaunchIntervalVariancePercent;  // +0x1758
    MissileLaunchConeAngle = Missile.MissileLaunchConeAngle;                       // +0x175c
    MineLaunchInterval = Mine.MineLaunchInterval;                                  // +0x1740
    MineLaunchConeAngle = Mine.MineLaunchConeAngle;                                // +0x1744

    // signed `TEST; JLE` entry test, count re-read, signed `CMP; JL`.
    if (GunCount > 0)
    {
        int Index = 0;
        do
        {
            // the routine on this slot (item 1829).
            RandomizeSlotTiming(&Guns[Index]);
            ++Index;
        } while (Index < GunCount);
    }

    // the same over the mine droppers.
    if (MineDropperCount > 0)
    {
        int Index = 0;
        do
        {
            // the routine on this slot (item 1829).
            RandomizeSlotTiming(&MineDroppers[Index]);
            ++Index;
        } while (Index < MineDropperCount);
    }

    return true;
}

// Common 0x062eb910 (the routine), item 1849, Commoneb910-062eb9c0.
bool SystemManagerGunGroupManager::HasReadyWeaponSlot()
{
    if (GunCount == 0)
        return false;

    // `SUB ESP,0xc`: an EquipmentVal dword at +0x10 and an 8-byte EquipStatus at
    // +0x14. The status local used to be `unsigned char[4]`, which CELauncher::GetStatus
    // overran at +4 (CMN-BUG6).
    unsigned Index = 0;
    do
    {
        // EDI = &Guns[Index]; its first word is the equipment id.
        unsigned short const& SlotId = Guns[Index].HardpointId;

        // IObjInspect +0x11C get_equipment_val(Val, Id, EquipValMunitionKind, 0.0f).
        EquipmentVal Val{};
        int const CallResult = OwnShip()->get_equipment_val(Val, SlotId, EquipValMunitionKind, 0.0f);

        // nonzero result, or the returned dword outside [0,2] (signed
        // JL 0 / JG 2), skips the slot.
        int Classify;
        std::memcpy(&Classify, &Val, sizeof(Classify));
        if (CallResult == 0 && Classify >= 0 && Classify <= 2)
        {
            // EquipStatus's inlined ctor (Flags &= 0xF8, EquipFlags &=
            // ) on the reused stack slot each time; zero-init is the defined
            // superset. 062eb98d-062eb9a2: +0x118 get_equipment_status(Status, Id); its
            // result is not tested.
            EquipStatus Status{};
            (void)OwnShip()->get_equipment_status(Status, SlotId);

            // Destroyed and bit 2 clear -> BL = 1 and EBP = count, so the
            // INC/JC below leaves the loop: the first ready slot ends the scan with true.
            if ((Status.Flags & (EquipStatusFlagDestroyed | EquipStatusFlagDisabled)) == 0)
                return true;
        }

        // count re-read, unsigned compare.
        ++Index;
    } while (Index < (unsigned)GunCount);

    return false;
}

// Common 0x062eb670 (the routine), item 1847, Commoneb670-062eb6b1, RET 0xc
// (only the target argument is read).
bool SystemManagerGunGroupManager::RunFireControlTick(IObjRW* Target)
{
    if (bEnabled == 0)
        return false;

    // the routine = Retarget (item 1833).
    if (!Retarget(Target))
        return false;

    LastGunFireResult = UpdateGunFireState();
    UpdateMissileFireState();
    UpdateMineDropState();
    return true;
}

// Common 0x062ebb10 (the routine), item 1852, Commonebb10-062ebbb4.
void FCmDropperWatcher::SetActive(unsigned char NewState)
{
    // `CMP byte [ESI+8],BL; JZ` -- nothing to do.
    if (bActive == NewState)
        return;

    // `SUB ESP,8`: the frame's one local is an 8-byte EquipStatus. 062ebb24-062ebb41
    // is its inlined default ctor (the routine's body: Flags &= 0xF8, EquipFlags &=
    // ) over the uninitialised slot, run ONCE before the loop and reused for every
    // id. get_equipment_status -> CELauncher::GetStatus ORs into EquipFlags (+4), so this has
    // to be a whole EquipStatus -- the port used to pass a 1-byte local here (ASan
    // stack-buffer-overflow, CMN-BUG6). Zero-init is the defined superset of the bit clears;
    // only Flags bit 0 is read below, and GetStatus writes it on every successful call.
    EquipStatus Status{};

    // / 062ebb9d-062ebba6: EBP = index, EDI = &Ids[index]; Count is
    // re-read after every iteration, and a count <= 0 skips the loop (062ebb45).
    unsigned short* Id = Ids;
    for (int Index = 0; Index < Count; ++Index, ++Id)
    {
        // Owner's Watcher object (no null test in baseline).
        // IObjInspect +0x118 get_equipment_status(Status, *Id).
        IObjRW* const Obj = WatchedObject(Owner->Watcher);
        if (Obj->get_equipment_status(Status, *Id) != 0)    // JNZ
            continue;

        // `AND AL,1; CMP AL,BL; JZ` -- already in the requested state.
        if ((Status.Flags & EquipStatusFlagActive) == NewState)
            continue;

        // re-reads Owner and the watcher, then `LEA ECX,[obj+4]` -- the
        // IObjAffect sub-object -- and calls +0x4C with (Id, NewState, -1).
        IObjRW* const Obj2 = WatchedObject(Owner->Watcher);
        Obj2->slot_4c(Id, NewState != 0, -1);
    }

    // the new state is stored after the loop (also when the count was <= 0).
    bActive = NewState;
}

// Common 0x062eb9d0 (the routine), item 1850, Commoneb9d0-062eba61.
bool FCmDropperWatcher::Bind(SystemManager* NewOwner)
{
    // null owner, or a watcher target of 0 or 8 (i.e. no IObjRW).
    if (NewOwner == nullptr)
        return false;
    if (!IsWatcherLive(NewOwner->Watcher))
        return false;


    Owner = NewOwner;

    // ExternalIface (+0x1580) is_player() (+0xBC), no null test in
    // baseline. A player's droppers are left alone and the bind still reports success.
    if (NewOwner->SelfTarget.Iface->is_player())
        return true;                                    // MOV AL,1

    // AL is 0 here, so the state byte is cleared with it.
    bActive = 0;
    Timer = 0.0f;
    Count = 0;

    // re-reads Owner -> watcher object (no null test), then
    // IObjInspect +0x114 find_equipment(Ids, 0x40, EquipTypeCmDropper). Very strange: Ids
    // holds only 8 ushorts (this block sits at SystemManager+0x1344 and FGunConvergenceStateBlock
    // starts at +0x1364), yet baseline lets find_equipment write up to 0x40 ids. With 8 or more
    // countermeasure droppers the ids run on into the convergence block. Reproduced as baseline;
    // inside one SystemManager object.
    IObjRW* const Obj = WatchedObject(Owner->Watcher);
    int const Result = Obj->find_equipment(Ids, 0x40, EquipTypeCmDropper);

    // on success count the ids up to the first zero, at most 0x40.
    // EAX (the zero result) is the index, Count is bumped in step with it.
    if (Result == 0)
    {
        int Index = 0;
        unsigned short const* Cursor = Ids;
        do
        {
            if (*Cursor == 0)
                break;
            ++Count;
            ++Index;
            ++Cursor;
        } while (Index < 0x40);
    }


    SetActive(1);
    return true;
}

// Common 0x062eba70 (the routine), item 1851, Commoneba70-062ebaf9.
void FCmDropperWatcher::Update(float Dt)
{
    // `MOV EAX,[ESI]; ADD EAX,0x14fc; MOV EAX,[EAX+0x84]` is a single
    // load of SystemManager+0x1580 (ExternalIface), then is_player() (+0xBC), no null test.
    if (Owner->SelfTarget.Iface->is_player())
        return;

    // ECX = Owner, kept for every read below. Sys+0x17c4 / +0x17c8 are
    // PersonalityData (+0x1584) .CMParams (+0x240) .CountermeasureActiveTime / UnactiveTime.
    SystemManager* const Sys = Owner;
    pub::AI::Personality::CountermeasureUseStruct const& CmParams = Sys->PersonalityData.CMParams;

    // FLD Dt; FADD Timer; FST Timer; FCOMP [+0x17c8]; TEST AH,0x1; JNZ.
    // The compare is on the unrounded sum still on the FPU stack. C0 set (sum < limit, or
    // unordered) skips; fall-through (sum >= limit) zeroes the timer.
    double const Sum = (double)Dt + (double)Timer;
    Timer = (float)Sum;
    if (Sum >= (double)CmParams.CountermeasureUnactiveTime)
        Timer = 0.0f;

    // FLD Timer; FCOMP [+0x17c4]; TEST AH,0x5; JP. Parity even (C0 == C2:
    // greater/equal, or unordered) jumps on; fall-through is Timer < ActiveTime.
    if (Timer < CmParams.CountermeasureActiveTime)
    {
        SetActive(1);
        return;
    }

    // same test against +0x17c8; Timer >= UnactiveTime (or unordered) exits.
    if (!(Timer < CmParams.CountermeasureUnactiveTime))
        return;

    // watcher object (no null test) -> is_player(); not a player ->
    // SetActive(0).
    IObjRW* const Obj = WatchedObject(Sys->Watcher);
    if (!Obj->is_player())
        SetActive(0);
}
