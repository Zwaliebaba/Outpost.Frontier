// Author: Giperion.

// Common.dll -- ThrustManager (SystemManager+0x18).
// CMN-SPLIT stage 1 (2026-10-14): split out of common_systemmanager.cpp unchanged.

#include "common_systemmanager.h"
#include "common_aisteer.h"   // SustainedThresholdTimer, ConstructGunConvergenceState, RefreshWeaponFireControlConfig
#include "common_behavior.h"  // Behavior_InvokeNodeSlotC (the routine)
#include "common_csys.h"      // Transform, Acquire3DMathEngine
#include "x86math.h"          // I3DMathEngine::TransposeTransform / TransformMatrix, FVector, FMatrix
#include "common_misc.h"      // SUBOBJ_ID_NONE
#include "common_iface.h"     // IBehaviorManager::Pathfind, IObjInspect, ObjRwFromWatchable
#include "common_pathfind.h"  // PathfindManager::IsCruiseUnobstructed (the routine)
#include "common_iustategraph.h" // ClearAiTargetVector, BuildAiTargetList (ScanGunGroupTargets)
#include "common_cobject.h"    // CEqObj::EquipManager / get_behavior_interface (ScanGunGroupTargets)
#include "common_cequip.h"     // CELauncher::cast / GetBarrelPosWS (ScanGunGroupTargets)
#include "common_cobjdb.h"     // IObjDB::FindById (AppendSystemManagerTimerListEntry)
#include "neuroncomponents.h"             // NEURONCOMPONENTS_API

#include <cmath>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <new>

// ============================================================================
// ThrustManager — SystemManager+0x18. See common_systemmanager.h for the
// layout derivation and for how it resolves the long-standing
// "SystemManager+0x18 size contradiction" (tracker items 1786/1796).
// ============================================================================

// 's ThrustManager initialiser block, Common
// (item 1785; the surrounding function stays blocked on the gun-hardpoint
// resolver family, but THIS block is self-contained and fully traced).
void InitThrustManager(ThrustManager* Self, SystemManager* Owner)
{
    Self->Owner = Owner;                    // MOV [ESI+0x18],ESI
    Self->MaxThrottle = 5.0f;               // 0x40a00000
    Self->AxisThrottle.x = 0.0f;
    Self->AxisThrottle.y = 0.0f;
    Self->AxisThrottle.z = 0.0f;
    Self->bCruiseEngaged = 0;               // [ESI+0x2c]
    Self->bCruiseUnobstructed = 1;          // [ESI+0x2d]
    Self->bEngineActive = 0;                // [ESI+0x2e]
    Self->bAutoLevel = 1;                   // [ESI+0x2f]
    Self->bAutoAvoidance = 1;               // [ESI+0x30]

    // baseline then re-reads Owner back out of the field it just wrote, resolves
    // Owner->Watcher through the -8 downcast, and clears bAutoAvoidance when the
    // watched object answers true to is_player() (vtable+0xbc).
    // Common.
    IObjRW* const Target =
        ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
    if (Target != nullptr && ObjInspectOf(Target)->is_player())
        Self->bAutoAvoidance = 0;
}

// ThrustManager::engage_cruise, module storage, baseline symbol
// `?engage_cruise@ThrustManager@@QAEX_N@Z` (tracker item 12724).
// Instruction-for-instruction:
// CMP BL,1                     ; BL == bEngage
// JNZ  -> body                 ; a DISENGAGE always runs the body...
// MOV AL,[ESI+0x15]            ; ...an ENGAGE only when bCruiseUnobstructed
// JZ   -> return               ;    is non-zero
// PUSH 1; CALL the routine    ; SetEngineActive(true)
// ... ObjRwFromWatchable(Owner->Watcher) ...
// CALL [EAX+0x5c]              ; IObjInspect::is_cruise_active(bool&, bool&)
// CMP [ESP+0x10],BL            ; out-param #1 (the param_1 slot, reused) vs bEngage
// CMP [ESI+0x14],BL            ; cached state vs bEngage
// JZ   -> return               ; both already agree: nothing to do
// PUSH -1; PUSH 0; PUSH BL
// CALL [EAX+0x50]              ; IObjAffect::slot_50(bEngage, 0, -1) -- EngageCruise
// MOV [ESI+0x14],BL            ; cache it

// NOTE on the out-param aliasing, which analysis renders confusingly: baseline
// passes `LEA EDX,[ESP+0x14]` -- the stack slot holding its OWN `bool param_1`
// argument -- as the FIRST out-param of is_cruise_active, so the callee
// OVERWRITES the incoming argument. That is harmless only because the value
// was already copied into EBX at function entry (062e90a2), and every later
// use reads EBX. Reproduced here with an explicit local, not by aliasing the
// parameter.
void ThrustManager::engage_cruise(bool bEngage)
{
    unsigned char const EngageByte = bEngage ? 1 : 0;

    if (EngageByte == 1 && bCruiseUnobstructed == 0)
        return;

    // . No Owner test: baseline dereferences `*this` here and in the callee (the
    // former `Owner == nullptr` early-out was invented; removed CMN-S2 AIS 2026-10-14).
    SetEngineActive(true);

    // the owner ship (`XOR ECX,ECX` arm at 062e90d3 dereferences 0) ->
    // is_cruise_active (+0x5c); the first out-param is the zeroed argument slot.
    bool CruiseActive = false;
    bool TradeLaneActive = false;  // baseline's `local_1` scratch byte, never read again
    (void)OwnerShip()->is_cruise_active(CruiseActive, TradeLaneActive);

    unsigned char const ActiveByte = CruiseActive ? 1 : 0;
    if (ActiveByte == EngageByte && bCruiseEngaged == EngageByte)
        return;

    // re-resolved the same way (`XOR EAX,EAX` arm at 062e90ff, no guard),
    // IObjAffect sub-object at +4, slot 0x50 (bEngage, 0, -1).
    ObjAffectOf(OwnerShip())->slot_50(EngageByte != 0, false, -1);

    bCruiseEngaged = EngageByte;
}

// , item 1826. `*(byte*)(this+0x17) = AL; RET 4`.
void ThrustManager::SetSteeringFallbackFlag(unsigned char Value)
{
    bAutoLevel = Value;
}

// (Common), three instructions:
//   MOV AL,byte ptr [ESP+4]; MOV byte ptr [ECX+0x18],AL; RET 4
// An ICF fold shared by CNudgeEngine::Activate and three unrelated setters; on a
// ThrustManager receiver (ECX = Sys+0x18, from update_current_behavior_auto_avoidance)
// the byte it writes is bAutoAvoidance, and AL still holds the argument on return.
bool ThrustManager::SetAutoAvoidance(bool bOn)
{
    bAutoAvoidance = bOn ? 1 : 0;
    return bOn;
}

// , item 1827 (module storage-062e932f, RET 0). No null tests anywhere: the
// former Sys / Sys->Owner / ExternalIface guards were invented (removed CMN-S2 AIS 2026-10-14).
bool ThrustManager::CheckSteeringGate()
{
    // [[this]+4] = the owning IBehaviorManager.
    if (Owner->Owner->get_camera_level_status())
        return true;

    // Sys+0x14fc+0x84 = ExternalIface -> is_player (+0xbc).
    if (Owner->SelfTarget.Iface->is_player())
    {
        // IBehaviorManager+0xc0, 10 or 7 -> true (AL = 1).
        int const BehaviorIndex = Owner->Owner->CurrentBehaviorIndex;
        if (BehaviorIndex == 10 || BehaviorIndex == 7)
            return true;
    }

    // the +0x17 byte as the result.
    return bAutoLevel != 0;
}

// ============================================================================
// The throttle / nudge / engine / thruster setters (items 1819-1825). Each one re-reads
// `[this]` and re-applies the -8 Watchable adjustment before every interface call, as
// baseline does; none of them null-tests the result beyond the entry gates noted.
// ============================================================================

IObjRW* ThrustManager::OwnerShip() const
{
    return ObjRwFromWatchable(const_cast<Watchable*>(Owner->Watcher.target()));
}

// (item 1825), module storage-062e92c6, RET 4.
void ThrustManager::SetEngineActive(bool bActive)
{
    // `head == 0` or `head - 8 == 0` -> return.
    if (OwnerShip() == nullptr)
        return;

    // SelfTarget.Iface (Owner+0x1580) is_player (+0xbc); a non-player whose
    // cached byte already equals bActive returns.
    bool const bPlayer = Owner->SelfTarget.Iface->is_player();
    if (!bPlayer && bEngineActive == (unsigned char)bActive)
        return;

    // find_equipment (+0x114) for one engine id; Id starts as
    // SUBOBJ_ID_NONE (module storage == 0, loaded at 062e91f7). Unguarded receiver.
    unsigned short Id = SUBOBJ_ID_NONE;
    ObjInspectOf(OwnerShip())->find_equipment(&Id, 1, EquipTypeEngine);

    if (Id != SUBOBJ_ID_NONE)
    {
        // get_equipment_status (+0x118) into the 8-byte EquipStatus
        // whose Flags low three bits baseline clears up front (062e9209).
        EquipStatus Status{};
        ObjInspectOf(OwnerShip())->get_equipment_status(Status, Id);
        unsigned char const StatusByte = Status.Flags;

        // Gate on the status byte just written (analysis conflates it with bActive): not
        // destroyed and the active bit differs from the request.
        if ((StatusByte & EquipStatusFlagDestroyed) == 0 &&
            ((StatusByte & EquipStatusFlagActive) != 0) != bActive)
        {
            // IObjAffect (+4) slot_4c(&Id, bActive, -1), unguarded.
            ObjAffectOf(OwnerShip())->slot_4c(&Id, bActive, -1);
        }
    }

    bEngineActive = (unsigned char)bActive;
}

// (item 1820), module storage-062e8cd5, RET 4.
void ThrustManager::SetAxisThrottle(Vector const& Wanted)
{
    // `head == 0` or `head - 8 == 0` -> return.
    if (OwnerShip() == nullptr)
        return;

    // get_axis_throttle (+0x50) into AxisThrottle; nonzero -> return.
    if (ObjInspectOf(OwnerShip())->get_axis_throttle(AxisThrottle) != 0)
        return;

    // Epsilon = the field raw bits (~1.049e-6f).
    unsigned int const EpsilonBits = 0x358637BEu;
    float Epsilon;
    std::memcpy(&Epsilon, &EpsilonBits, sizeof(Epsilon));

    // each difference and its square stay on the FPU (double); `FCOMP
    // float eps; TEST AH,0x41; JP` -- greater or unordered counts as moved.
    double const DeltaX = (double)Wanted.x - AxisThrottle.x;
    double const DeltaY = (double)Wanted.y - AxisThrottle.y;
    double const DeltaZ = (double)Wanted.z - AxisThrottle.z;
    if (DeltaX * DeltaX <= Epsilon && DeltaY * DeltaY <= Epsilon && DeltaZ * DeltaZ <= Epsilon)
        return;

    // Re-resolved, unguarded: IObjAffect (+4) slot 1 set_axis_throttle(Wanted).
    ObjAffectOf(OwnerShip())->set_axis_throttle(Wanted);

    AxisThrottle = Wanted;
}

// (item 1823), RET 4.
void ThrustManager::SetStrafeDir(StrafeDir Dir)
{
    SetEngineActive(true);

    // head-8 (or 0) and get_strafe_dir (+0x58) with no null test.
    StrafeDir Current = StrafeNone;
    if (ObjInspectOf(OwnerShip())->get_strafe_dir(Current) != 0)
        return;
    if (Current == Dir)
        return;

    // Re-resolved, unguarded: IObjAffect (+4) slot 3 set_strafe_dir(Dir).
    ObjAffectOf(OwnerShip())->set_strafe_dir(Dir);
}

// (item 1824), RET 4.
void ThrustManager::SetThrustersActive(bool bActive)
{
    // head-8 (or 0) and is_cruise_active (+0x5c) with no null test.
    bool bCruise = false;
    bool bTradeLane = false;
    (void)ObjInspectOf(OwnerShip())->is_cruise_active(bCruise, bTradeLane);
    if (bCruise && !bTradeLane)
        return;

    if (bActive)
        SetEngineActive(true);

    // re-resolved, are_thrusters_active (+0x64).
    bool bThrusters = false;
    (void)ObjInspectOf(OwnerShip())->are_thrusters_active(bThrusters);
    if (bThrusters == bActive)
        return;

    // Re-resolved a third time, unguarded: IObjAffect (+4) slot_54(bActive, -1).
    ObjAffectOf(OwnerShip())->slot_54(bActive, -1);
}

// (item 1819), RET 8.
bool ThrustManager::SetThrottle(float Throttle, int Mode)
{
    // Gate: the owner's watcher head must be neither 0 nor 8.
    if (OwnerShip() == nullptr)
        return false;

    // is_player (+0xbc) on the re-resolved ship; the answer is discarded.
    (void)ObjInspectOf(OwnerShip())->is_player();

    float Clamped = (Throttle > MaxThrottle) ? MaxThrottle : Throttle;
    int const EffectiveMode = (bCruiseUnobstructed == 0 && Mode == 2) ? 0 : Mode;

    if (EffectiveMode != 0 && EffectiveMode != 2)
        return true;   // baseline's `SUB EAX,2; JNZ <epilogue>` for any other mode

    if (EffectiveMode == 2)
    {
        // Clamp into [0.0, 1.2] (the field / the field).
        if (Clamped > 1.2f) Clamped = 1.2f;
        if (Clamped < 0.0f) Clamped = 0.0f;

        // is_cruise_active (+0x5c).
        bool bCruise = false;
        bool bTradeLane = false;
        (void)ObjInspectOf(OwnerShip())->is_cruise_active(bCruise, bTradeLane);

        // Cruising outside a trade lane: the set_throttle commit moves to the tail.
        bool const bDeferCommitToTail = (bCruise && !bTradeLane);
        if (!bDeferCommitToTail)
            ObjAffectOf(OwnerShip())->set_throttle(Clamped);

        if (Clamped > 0.0f && bCruiseUnobstructed != 0)
        {
            SetEngineActive(true);

            bool bCruiseNow = false;
            bool bTradeLaneNow = false;
            (void)ObjInspectOf(OwnerShip())->is_cruise_active(bCruiseNow, bTradeLaneNow);

            if (!(bCruiseNow && bCruiseEngaged == 1))
            {
                ObjAffectOf(OwnerShip())->slot_50(true, false, -1);
                bCruiseEngaged = 1;
            }
        }

        if (bDeferCommitToTail)
            ObjAffectOf(OwnerShip())->set_throttle(Clamped);                // / 062e8b52
        return true;
    }

    // EffectiveMode == 0: clamp into [-1.0, 1.0] (the field / the field).
    if (Clamped < -1.0f) Clamped = -1.0f;
    else if (Clamped > 1.0f) Clamped = 1.0f;

    SetEngineActive(true);

    // is_cruise_active (+0x5c).
    bool bCruise = false;
    bool bTradeLane = false;
    (void)ObjInspectOf(OwnerShip())->is_cruise_active(bCruise, bTradeLane);

    if (!(!bCruise && bCruiseEngaged == 0))
    {
        ObjAffectOf(OwnerShip())->slot_50(false, false, -1);
        bCruiseEngaged = 0;
    }

    SetEngineActive(true);

    ObjAffectOf(OwnerShip())->set_throttle(Clamped);
    return true;
}

// (item 1821), Common, RET 4; all 133 instructions traced.
void ThrustManager::SetNudgeDirection(Vector const& WorldDir)
{
    SetEngineActive(true);                                                  // PUSH 1

    // a copy of WorldDir; when (y*y + z*z) + x*x is above 1.0f (TEST
    // AH,0x41 / JNZ keeps <=, == and unordered) it is replaced by the routine's result.
    Vector Local = WorldDir;
    if ((double)Local.y * Local.y + (double)Local.z * Local.z + (double)Local.x * Local.x > 1.0)
    {
        Vector Normalized;
        Local = *VecNormalize3(&Local, &Normalized);
    }

    // the owner ship's transform (the routine), then the 3DMathEngine's
    // TransposeTransform (+0x38, the field is this function's engine cache) of Local into
    // ship space -- the nudge vector to apply.
    Transform const* const Frame = Behavior_InvokeNodeSlotC(OwnerShip());
    Vector Nudge;
    Acquire3DMathEngine()->TransposeTransform(reinterpret_cast<FVector&>(Nudge),
                                              reinterpret_cast<FMatrix const&>(Frame->Rotation),
                                              reinterpret_cast<FVector const&>(Local));

    // the current nudge (IObjInspect slot 21, +0x54); a nonzero result
    // returns without committing.
    Vector Current;
    if (ObjInspectOf(OwnerShip())->get_nudge_vec(Current) != 0)
        return;

    // commit when any axis differs: (New - Current)^2 against 1e-6f
    // (the field); TEST AH,0x41 / JP commits on > and unordered, the last axis's JNP
    // returns on <= .
    double const DeltaX = (double)Nudge.x - Current.x;
    double const DeltaY = (double)Nudge.y - Current.y;
    double const DeltaZ = (double)Nudge.z - Current.z;
    float const Epsilon = 1.0000001111620804e-06f;
    if (!(DeltaX * DeltaX <= Epsilon) || !(DeltaY * DeltaY <= Epsilon) || !(DeltaZ * DeltaZ <= Epsilon))
    {
        // IObjAffect slot 2 (+0x08) set_nudge_vec; no null test.
        ObjAffectOf(OwnerShip())->set_nudge_vec(Nudge);
    }
}

// (item 1822), Common (analysis's `.c` for this address is
// the artefact the old `unaff_ESI`/`unaff_EDI` blocker was built on and was not used).

// , raw bytes 00 00 c0 3f == 0x3fc00000 == 1.5f. Lives in .data, but its
// complete xref list (Common) is three reads, all inside this function, and no
// write anywhere in the module: a fixed tuning constant, not runtime state.
static float const NudgeBlendScale = 1.5f;

// , raw bytes a4 70 7d 3f == 0x3f7d70a4 == 0.99f exactly.
static float const NudgeAlignmentThreshold = 0.99f;

void ThrustManager::BlendNudgeDirection(Vector const& WorldDir, float DeltaTime)
{
    // the argument's three floats are copied into this frame; baseline never
    // writes back through the argument.
    Vector Working = WorldDir;

    // the -8 adjustor on the owner's watcher target (a null head yields a
    // null receiver, which baseline calls through unguarded), then get_nudge_vec (+0x54).
    Vector NudgeLocal;
    ObjInspectOf(OwnerShip())->get_nudge_vec(NudgeLocal);

    // TransformMatrix (3DMathEngine slot 0x2c, the the field lazy
    // singleton) of the nudge by SelfTarget.Iface's (Owner+0x1580) get_transform(). No null
    // tests.
    Vector NudgeWorld;
    {
        Matrix const& Orientation = Owner->SelfTarget.Iface->get_transform().Rotation;
        Acquire3DMathEngine()->TransformMatrix(reinterpret_cast<FVector&>(NudgeWorld),
                                               reinterpret_cast<FMatrix const&>(Orientation),
                                               reinterpret_cast<FVector const&>(NudgeLocal));
    }

    // the routine on Working then on the nudge, the routine(NormNudge,
    // NormWorking) FCOMP 0.99f; TEST AH,0x5 / JP jumps over the zeroing, so only "less" zeroes
    // the nudge.
    {
        Vector NormWorking;
        Vector NormNudge;
        VecNormalize3(&Working, &NormWorking);
        VecNormalize3(&NudgeWorld, &NormNudge);
        if (ClampedDot3(NormNudge, NormWorking) < (double)NudgeAlignmentThreshold)
            NudgeWorld = Vector{ 0.0f, 0.0f, 0.0f };
    }

    // both lengths stay on the x87 stack, summed (y,x,z) for Working and
    // (y,z,x) for the nudge; FLD ST1 / FCOMPP / TEST AH,0x41 / JNZ skips the blend unless
    // |Working| > |Nudge|.
    double const LenWorking = std::sqrt(((double)Working.y * Working.y +
                                         (double)Working.x * Working.x) +
                                        (double)Working.z * Working.z);
    double const LenNudge = std::sqrt(((double)NudgeWorld.y * NudgeWorld.y +
                                       (double)NudgeWorld.z * NudgeWorld.z) +
                                      (double)NudgeWorld.x * NudgeWorld.x);
    if (LenWorking > LenNudge)
    {
        // , roundings exactly where baseline FSTPs:
        //   x: (float)((float)(K*Wx*Dt) * Len), then (float)(+ Nx)
        //   y: (float)(K*Wy*Dt*Len), then (float)(Ny +)
        //   z: (float)(K*Wz) [FSTP to [F+0x30]], * Dt * Len unrounded, then (float)(Nz +)
        double const K = NudgeBlendScale;
        float const XScaled = (float)((K * Working.x) * DeltaTime);
        float const XTerm = (float)(XScaled * LenWorking);
        float const YTerm = (float)(((K * Working.y) * DeltaTime) * LenWorking);
        float const ZScaled = (float)(K * Working.z);
        double const ZTerm = ((double)ZScaled * DeltaTime) * LenWorking;
        Working.x = (float)((double)XTerm + NudgeWorld.x);
        Working.y = (float)((double)NudgeWorld.y + YTerm);
        Working.z = (float)((double)NudgeWorld.z + ZTerm);
    }

    // commit through the same `this`.
    SetNudgeDirection(Working);
}
