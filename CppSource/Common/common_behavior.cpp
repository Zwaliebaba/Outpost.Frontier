// Author: Giperion.

// Common.dll -- Behavior (the AI behavior base class) and the Behavior_* helpers; also the two
// shared follower helpers FollowerExternalIfaceIsPlayer / FollowerPlayerShipOrNull, declared for
// the follower/formation TUs in common_behavior_internal.h.
// CMN-SPLIT stage 1 (2026-10-14): BaseFollowerBehavior moved to common_basefollowerbehavior.cpp,
// FormationBehavior to common_formationbehavior.cpp, unchanged.

#include "common_behavior.h"
#include "common_behavior_internal.h" // CmnBehavior:: -- the shared follower helpers defined here
#include "common_aisteer.h" // GetIfacePositionOrZero, IsWithinInterceptRangeOfIface
#include "common_systemmanager.h" // SystemManager -- BaseFollowerBehavior::NavHelper's real type
#include "common_misc.h" // FastInvSqrt
#include "common_csys.h"  // Matrix, Transform, MatrixSetIdentity, Acquire3DMathEngine
#include "common_iface.h" // IObjInspect / IObjRW / ObjInspectOf / ObjRwFromWatchable
#include "x86math.h"      // I3DMathEngine::TransformMatrix (+0x2c), FVector, FMatrix
#include "common_pathfind.h" // PathfindManager -- Behavior::Slot4 / Behavior::SetCruiseLock

#include <cmath>
#include <cstdint>
#include <cstring>

// MatchThrottle2Idea (formerly FormationMoveFx here) lives in common_idea.h/.cpp.

// (implementation tracker item 1765). See common_behavior.h for the full
// derivation. Generic null-guarded vtable-slot-0xd4 trampoline on an
// unidentified interface.
float Behavior_InvokeManagerSlotD4(IObjInspect const* Obj, float Value, ThrustEquipType Type)
{
    if (Obj == nullptr)
        return 0.0f;

    return Obj->get_projected_throttle(Value, Type);
}

// (item 1537). All five instructions, module storage `MOV AL,[ECX+0x28]`
// through the `RET` at module storage. See common_behavior.h for the receiver proof
// and for the three claims in the tracker note that did not survive re-measuring.
// No null test, as baseline: every call site has already called through the same pointer.
void Behavior_ReleaseDirectiveTarget(void* Target)
{
    unsigned char* const Flags28 = reinterpret_cast<unsigned char*>(Target) + 0x28;
    *Flags28 = static_cast<unsigned char>((*Flags28 & 0xfd) | 0x01);  // / 062d21c5
}

// (implementation tracker item 1762). See common_behavior.h for the full
// derivation -- byte-identical to Behavior_InvokeManagerSlotD4 above, just a
// different fixed vtable slot.
float Behavior_InvokeManagerSlotD8(IObjInspect const* Obj, float Value, ThrustEquipType Type)
{
    if (Obj == nullptr)
        return 0.0f;

    return Obj->get_speed(Value, Type);
}

// (item 1759), all 29 instructions (Common).
float Behavior_GetAngularDistanceTravelled(IObjInspect const* Obj, float From, float To, float Dt,
                                           float MaxSpeed)
{
    if (Obj == nullptr)
        return 0.0f;                                   // FLD the field
    // FCOMP 0.0; `TEST AH,0x41; JP` -- falls through (returns 0) when
    // C0 or C3 alone is set, i.e. MaxSpeed < 0 or MaxSpeed == 0.
    if (MaxSpeed < 0.0f || MaxSpeed == 0.0f)
        return 0.0f;
    // FCOMP double 0.001; `TEST AH,0x5; JP` -- the store runs for C0
    // alone, i.e. Dt < 0.001.
    if (static_cast<double>(Dt) < 0.001)
        Dt = 0.0f;
    return Obj->get_angular_distance_travelled(From, To, Dt, MaxSpeed);   // [EAX+0xec]
}

// (implementation tracker item 1769). See common_behavior.h for the full
// derivation. No null-check of its own -- matches baseline, every confirmed
// caller pre-validates Iface.
float Behavior_GetIfaceMaxScalarAttribute(IObjInspect const* Iface)
{
    // Slot 0x2c is get_physical_radius(float& Radius, Vector& Center). The second out-parameter
    // used to be a bare `float Unused` -- four bytes catching a twelve-byte write.
    float Radius = 1.0f;
    Vector Center{ 0.0f, 0.0f, 0.0f };
    Iface->get_physical_radius(Radius, Center);

    float AtmosphereRange = 0.0f;
    Iface->get_atmosphere_range(AtmosphereRange);

    return (AtmosphereRange > Radius) ? AtmosphereRange : Radius;
}

// (implementation tracker item 1757). See common_behavior.h for the full
// derivation, hand-re-verified against Common.
float Behavior_QueryRadiusEffect(IObjInspect const* Obj, float Unused2, float Radius, float Falloff, ThrustEquipType Type)
{
    if (Obj == nullptr)
        return 0.0f;

    // `FCOMP 0.0f; TEST AH,0x5; JP` keeps the value on >= and on
    // unordered -- only a strict less-than clamps.
    if (Radius < 0.0f)
        Radius = 0.0f;
    if (Falloff < 0.0f)
        Falloff = 0.0f;

    // Early-out (no vtable call at all) when both Radius and Unused2 are
    // exactly 0.0f -- `TEST AH,0x44; JP` falls through only on equal (062e5891-062e58b9).
    if (Radius == 0.0f && Unused2 == 0.0f)
        return 0.0f;

    // Snap Radius to 0 below the double 0.001 (the field), `TEST AH,0x5; JP`.
    if ((double)Radius < 0.001)
        Radius = 0.0f;

    return Obj->get_time_to_accelerate(Unused2, Radius, Falloff, Type);
}

// (implementation tracker item 1758). See common_behavior.h for the full
// derivation, hand-re-verified against Common.
float Behavior_QueryFalloffEffect(IObjInspect const* Obj, float Unused2, float Radius, float Falloff, ThrustEquipType Type)
{
    if (Obj == nullptr)
        return 0.0f;

    // Early-out (no vtable call at all) when Falloff is strictly negative --
    // `FCOMP 0.0f; TEST AH,0x5; JP` continues on >= and on unordered.
    if (Falloff < 0.0f)
        return 0.0f;

    // Snap Radius to 0 below the same double 0.001 (the field) the
    // Radius-effect sibling above uses.
    if ((double)Radius < 0.001)
        Radius = 0.0f;

    return Obj->get_distance_travelled(Unused2, Radius, Falloff, Type);
}

// (implementation tracker item 1760), module storage-062e5aae. baseline inlines the
// slot calls of Behavior_InvokeManagerSlotD8 / QueryRadiusEffect / QueryFalloffEffect
// (their null tests and the 0/0 early-out drop away because the arguments are known);
// the clamps that remain are reproduced in baseline's order.
float Behavior_QueryChainedRadiusEffect(void* Obj, float Radius, int Selector)
{
    if (Obj == nullptr)
        return 0.0f;

    IObjInspect const* const Inspect = ObjInspectOf(Obj);
    ThrustEquipType const Type = (ThrustEquipType)Selector;

    // V = get_speed(1.0f, Sel) - 1.0 stays on the FPU; `< 0` -> 0.0
    // (TEST AH,0x5; JP); V < Radius (FCOM float, TEST AH,0x5; JP) -> 0.0f.
    double Value = (double)Inspect->get_speed(1.0f, Type) - 1.0;           // slot 0xd8
    if (Value < 0.0)
        Value = 0.0;
    if (Value < Radius)
        return 0.0f;

    // Radius (the parameter itself) snaps to 0 below double 0.001.
    if ((double)Radius < 0.001)
        Radius = 0.0f;

    // a copy of Radius and FST float V, each clamped `< 0` -> 0, the copy
    // snapped again below 0.001 -- QueryRadiusEffect's clamps on the inlined arguments.
    float RadiusCopy = Radius;
    float Falloff = (float)Value;
    if (RadiusCopy < 0.0f)
        RadiusCopy = 0.0f;
    if (Value < 0.0)
        Falloff = 0.0f;
    if ((double)RadiusCopy < 0.001)
        RadiusCopy = 0.0f;

    // get_time_to_accelerate(1.0f, RadiusCopy, Falloff, Sel), FSTP float.
    float const RadiusResult = Inspect->get_time_to_accelerate(1.0f, RadiusCopy, Falloff, Type);
    // get_distance_travelled(1.0f, Radius, RadiusResult, Sel), returned in ST0.
    return Inspect->get_distance_travelled(1.0f, Radius, RadiusResult, Type);
}

// (implementation tracker item 1761). See common_behavior.h for the full
// derivation.
float Behavior_QueryFixedFalloffEffect(void* Obj, float Radius)
{
    if (Obj == nullptr)
        return 0.0f;
    if (Radius < 0.001f)
        return 0.0f;

    // = 0x447a0000 = 1000.0f; Selector fixed to literal 1
    // (Common/241086-241088).
    return Behavior_QueryFalloffEffect(ObjInspectOf(Obj), 0.0f, Radius, 1000.0f, ThrustEquipEngine);
}

// (implementation tracker item 1767). See common_behavior.h for the full
// derivation. New sibling vtable slot (0xdc) not shared with any other
// already-ported member of this family.
float Behavior_InvokeManagerSlotDC(IObjInspect const* Obj, float Value)
{
    if (Obj == nullptr)
        return 0.0f;
    if (Value == 0.0f)
        return 0.0f;

    return Obj->get_initial_speed_to_coast_distance(Value, true);
}

// (implementation tracker item 1763). Hoisted from common_gotobehavior.cpp
// 2026-09-17 at that lane's request -- see the header for the full writeup.
float Behavior_InvokeManagerSlotF0(IObjInspect const* Obj, float Speed, float MaxSpeed)
{
    if (Obj == nullptr)
        return 0.0f; // FLD .rdata:the field
    return Obj->get_angular_speed_XY(Speed, MaxSpeed);
}

// (implementation tracker item 1764), Common, 10 insns, cdecl:
// `MOV ECX,[ESP+4]; TEST; JNZ; FLD 0.0f (the field); RET` / `PUSH [ESP+8]; CALL
// [EAX+0xf4]; RET` -- IObjInspect::get_angular_speed_Z (slot 61) or 0.0f for a null object.
float Behavior_InvokeManagerSlotF4(IObjInspect const* Obj, float Speed)
{
    if (Obj == nullptr)
        return 0.0f;
    return Obj->get_angular_speed_Z(Speed);
}

// (implementation tracker item 1766), Common, 10 insns, cdecl:
// null Obj -> FLD 0.0f (the field); else `PUSH [ESP+8]; CALL [EAX+0xf8]; RET` --
// IObjInspect::get_projected_axis_throttle_XY (slot 62).
float Behavior_InvokeManagerSlotF8(IObjInspect const* Obj, float Value)
{
    if (Obj == nullptr)
        return 0.0f;
    return Obj->get_projected_axis_throttle_XY(Value);
}

// / 062e8c20 / 062e9030 / 062e9120 / 062e89a0 / 062e8ce0 (items 1819-1825, the
// former Behavior_* "ComponentThis" helpers) are ThrustManager members now
// (common_systemmanager.h, common_thrustmanager.cpp).

// (item 1774). See common_behavior.h for the derivation --
// a trivial `Node->vtbl[0xc](Node)` tail-thunk, not a real vtable dispatch on
// some unrelated object.
Transform const* Behavior_InvokeNodeSlotC(void* Node)
{
    // `MOV ECX,[ESP+4]; MOV EAX,[ECX]; JMP [EAX+0xc]` -- no null test.
    // Slot 0xc is get_transform(); slots 3 and 4 are COMDAT-folded onto one address, so the
    // pointer this hands back is equally the object's orientation Matrix (common_iface.h).
    return &ObjInspectOf(Node)->get_transform();
}


// ============================================================================
// Behavior -- the base class's own +0x1c per-tick delta.

// See the long comment on Behavior's declaration (common_behavior.h) for why this is an
// accessor pair over a raw offset and not a data member: nineteen classes in this port
// re-declare Behavior's whole +0x04..+0x29 block on themselves, and every one of their
// offsetof static_asserts depends on sizeof(Behavior) staying 4. Giving the base a member
// would slide all nineteen by 0x28 bytes in one edit, so the migration is an open TODO for a
// wave that can touch all of them at once -- not something to sneak in from six files.

// The offset itself is baseline's, from two independent places: the constructor the routine
// zeroes a dword at +0x1c (module storage), and IBehaviorManager::UpdateBehaviorTick stores a
// float there through a bare Behavior* immediately before dispatching Slot7
// (module storage-062d336a, `FLD [ESI+0xd4]` / `FSTP [EDI+0x1c]` / `CALL [EAX+0x1c]`).
// ============================================================================

float Behavior::GetTickDelta() const
{
    return *reinterpret_cast<float const*>(reinterpret_cast<char const*>(this) + 0x1c);
}

void Behavior::SetTickDelta(float Delta)
{
    *reinterpret_cast<float*>(reinterpret_cast<char*>(this) + 0x1c) = Delta;
}

// Vtable slot 2 (+0x08), 0x0629afd0: `MOV EAX,[ESP+4]; MOV [ECX+0x18],EAX; RET 4`. Same
// raw-offset device as SetTickDelta -- +0x18 is declared on each subclass (see common_behavior.h).
void Behavior::Slot2(IObjDB* ObjectDb)
{
    *reinterpret_cast<IObjDB**>(reinterpret_cast<char*>(this) + 0x18) = ObjectDb;
}

// Vtable slot 23 (+0x5c), the branch: `AND byte ptr [ECX+0x28],0xfd; RET`. Shared by all 21
// Behavior vtables. Callers: IBehaviorManager::set_current_directive (one entry, then all 21).
void Behavior::cancel_notify()
{
    unsigned char* const Flags28 = reinterpret_cast<unsigned char*>(this) + 0x28;
    *Flags28 = static_cast<unsigned char>(*Flags28 & 0xfd);
}

// Same raw-offset device, same reason, for the type id the routine parks at +0x04 -- see the
// declaration's comment in common_behavior.h for the factory-side evidence. Read-only: the
// storage is written by whichever derived class declares it, from its own `int TypeId`
// constructor argument.
int Behavior::GetTypeId() const
{
    return *reinterpret_cast<int const*>(reinterpret_cast<char const*>(this) + 0x04);
}

// Same raw-offset device again, for the NavHelper pointer at +0x24 -- see the declaration's
// comment in common_behavior.h. Read-only; the binder the routine is what writes it.
void* Behavior::GetNavHelper() const
{
    return *reinterpret_cast<void* const*>(reinterpret_cast<char const*>(this) + 0x24);
}

// Vtable slot 3 (+0x0c), the routine (Common), 6 instructions:
// `MOV EAX,1; MOV [ECX+0xc],EAX; MOV [ECX+0x10],EAX; AND byte [ECX+0x28],0xfe;
// MOV dword [ECX+0x1c],0; RET`.
void Behavior::Slot3()
{
    char* const Self = reinterpret_cast<char*>(this);
    *reinterpret_cast<int*>(Self + 0x0c) = 1;
    *reinterpret_cast<int*>(Self + 0x10) = 1;
    unsigned char* const Flags28 = reinterpret_cast<unsigned char*>(Self + 0x28);
    *Flags28 = static_cast<unsigned char>(*Flags28 & 0xfe);
    *reinterpret_cast<float*>(Self + 0x1c) = 0.0f;                   // (dword 0)
}

// Vtable slot 4 (+0x10), the routine (Common), 8 instructions:
// `EAX = [[ECX+0x20]+0xc]; if (EAX) [EAX+0x64] = 0; byte [ECX+0x29] = 0; RET`. The manager
// pointer itself is not null-tested.
void Behavior::Slot4()
{
    char* const Self = reinterpret_cast<char*>(this);
    IBehaviorManager* const Manager = *reinterpret_cast<IBehaviorManager* const*>(Self + 0x20);
    PathfindManager* const Pathfind = Manager->Pathfind;
    if (Pathfind != nullptr)
        Pathfind->Field_064 = 0;
    *reinterpret_cast<unsigned char*>(Self + 0x29) = 0;
}

// (module storage-062d205a), all 16 instructions, the vptr store aside.
void Behavior::ConstructBase(int TypeId)
{
    char* const Self = reinterpret_cast<char*>(this);
    *reinterpret_cast<int*>(Self + 0x04) = TypeId;
    *reinterpret_cast<unsigned int*>(Self + 0x08) = 0;
    *reinterpret_cast<void**>(Self + 0x14) = nullptr;
    *reinterpret_cast<void**>(Self + 0x24) = nullptr;
    *reinterpret_cast<void**>(Self + 0x18) = nullptr;
    *reinterpret_cast<unsigned int*>(Self + 0x1c) = 0;
    *reinterpret_cast<unsigned char*>(Self + 0x29) = 0;
    unsigned char* const Flags28 = reinterpret_cast<unsigned char*>(Self + 0x28);
    *Flags28 = static_cast<unsigned char>(*Flags28 & 0xfc);
    *reinterpret_cast<int*>(Self + 0x0c) = 2;
    *reinterpret_cast<int*>(Self + 0x10) = 1;
}

// (Common), all 24 instructions: Owner null or Sys
// null -> AL = 0; else [+0x14] = Owner, `CALL [[Owner]+0x20]` (IObjInspect::get_id),
// [+0x24] = Sys, [+0x08] = the id, [+0x20] = Manager, AL = 1. RET 0xc.
bool Behavior::BindContext(void* Owner, void* Manager, void* Sys)
{
    if (Owner == nullptr || Sys == nullptr)
        return false;
    char* const Self = reinterpret_cast<char*>(this);
    *reinterpret_cast<void**>(Self + 0x14) = Owner;
    unsigned int const Id = ObjInspectOf(Owner)->get_id();
    *reinterpret_cast<void**>(Self + 0x24) = Sys;
    *reinterpret_cast<unsigned int*>(Self + 0x08) = Id;
    *reinterpret_cast<void**>(Self + 0x20) = Manager;
    return true;
}

// Vtable slot 9 (+0x24): `MOV EAX,[ECX+0x10]; RET`.
int Behavior::Slot9()
{
    return *reinterpret_cast<int const*>(reinterpret_cast<char const*>(this) + 0x10);
}

// (Common): tail-jump to the routine with
// this = [[this+0x20]+0xc] and both stack arguments unchanged; no null tests.
void Behavior::SetCruiseLock(int Value, int Mask)
{
    IBehaviorManager* const Manager =
        *reinterpret_cast<IBehaviorManager* const*>(reinterpret_cast<char const*>(this) + 0x20);
    Manager->Pathfind->SetCruiseLock(Value, static_cast<unsigned int>(Mask));
}

// ----------------------------------------------------------------------------
// Behavior vtable slot 16 (+0x40) -- the routine, implementation tracker item 1535, 54
// instructions, hand-translated from Common. PORTED 2026-09-17
// (claude:w23-stubsweep2); it was `virtual void set_afterburner(bool) {}` before, which is
// what eighteen behavior-family vtable slots and IBehaviorManager's own afterburner entry
// point were all reaching.

// baseline, instruction by instruction:
// MOV EAX,[ESI+0x24] / ADD EAX,0x8 / MOV EAX,[EAX]   ; NavHelper+0x8 list head
// the two out-bytes at [ESP+7] and [ESP+6] are pre-cleared
// head == 0 -> ECX = 0, else ECX = head - 8          ; the Watchable adjustment
// CALL [ECX+0x5c] with (&[ESP+7], &[ESP+6])          ; is_cruise_active(a, b)
//             -- push order is right-to-left, so [ESP+7] is parameter ONE (OutCruise) and
//             [ESP+6] is parameter TWO (OutTradeLane).
// if (OutCruise != 0 && OutTradeLane == 0) -> RET, do nothing at all.
//             (`TEST AL,AL; JZ 062d2150` on OutCruise, then `TEST AL,AL; JZ 062d219c` on
//             OutTradeLane -- the SECOND jump is the one that returns, so the do-nothing
//             case is "cruising but not in a trade lane".)
// ECX = [ESI+0x24] + 0x18 (ComponentThis); AL = [ESP+0xc], i.e. this method's
//             own dword argument, read as a byte.
// argument == 0 -> the branch: Thrust.SetThrustersActive(false); RET.
// argument != 0 -> Thrust.SetThrustersActive(true), then re-load
//             ComponentThis and, on OutTradeLane, Thrust.SetThrottle(1.0f,
//             2) at 062d217a, else Thrust.SetThrottle(1.0f, 0) at
// . The pushed 0x3f800000 is raw 1.0f in both arms; the mode literal is
//             the only thing that differs.

// baseline dereferences [this+0x24] with no null test, and its `head == 0` arm calls through a
// null receiver; both are reproduced as written (no guards).

// NAME CAVEAT: `set_afterburner` is this slot's inherited positional name and this pass did
// NOT re-derive it. The body is a cruise/throttle-gate poke, not an afterburner write, so the
// name is worth re-checking against the pub::AI interface -- but renaming slot 16 would touch
// IBehaviorManager and every subclass, which is not this pass's scope.
void Behavior::set_afterburner(bool bOn)
{
    void* const NavHelper = GetNavHelper();

    // both out-bytes pre-cleared, then slot 0x5c on the -8 adjusted head.
    void* const Head = *reinterpret_cast<void* const*>(static_cast<char*>(NavHelper) + 0x8);
    bool bCruise = false;
    bool bTradeLane = false;
    (void)ObjInspectOf(ObjRwFromWatchable(Head))->is_cruise_active(bCruise, bTradeLane);

    // the single early-out.
    if (bCruise && !bTradeLane)
        return;

    ThrustManager* const Thrust = &((SystemManager*)NavHelper)->Thrust;

    if (!bOn)
    {
        // .
        Thrust->SetThrustersActive(false);
        return;
    }

    // .
    Thrust->SetThrustersActive(true);

    // 1.0f either way, mode 2 while in a trade lane, mode 0 otherwise.
    Thrust->SetThrottle(1.0f, bTradeLane ? 2 : 0);
}

// ----------------------------------------------------------------------------
// Behavior vtable slot 17 (+0x44) -- the routine, implementation tracker item 1536, Common
// 210877-210887. PORTED 2026-09-17 (claude:w23-stubsweep2). The entire baseline body is
//     MOV ECX,[ECX+0x24] ; ADD ECX,0x18 ; JMP the routine
// and the routine is ThrustManager::SetStrafeDir. The argument is one dword (the routine's
// three exits all `RET 4`).
void Behavior::slide_strafe_burst(StrafeDir Dir)
{
    ((SystemManager*)GetNavHelper())->Thrust.SetStrafeDir(Dir);
}

// ============================================================================
// Shared follower helpers (common_behavior_internal.h, namespace CmnBehavior). Neither is a
// baseline function: both are inlined instruction sequences, on the SystemManager and on a
// CObject. The SystemManager+0x1364 accessors that used to sit here (the routine, 062ec680,
// , 062ecad0, 062ecb20, 062ecc00, 062ecc50, 062ecc60, 062ecce0, 062ecd30) are
// FGunConvergenceStateBlock members in common_gunconvergence.cpp (CMN-GC 2026-10-14).
// ============================================================================

// `MOV EAX,[Sys+0x1580]; MOV EDX,[EAX]; CALL [EDX+0xbc]` (IObjInspect::is_player). baseline
// tests neither pointer (module storage-062ecb35, 062ecbc7-062ecbd1, 062cb76a-062cb784).
bool CmnBehavior::FollowerExternalIfaceIsPlayer(SystemManager* Sys)
{
    return Sys->SelfTarget.Iface->is_player();
}

// `(*(uint*)(CObject + 0x4c) & 0x503) == 0x503` -- the port's established
// "[obj+0x4c] & mask == mask" object-class test (common_cobject.h documents the
// 0x103 variant of the same idiom). Used at module storage and 062cb7e2 to reduce
// a cobject() result to "a player-flavoured ship, else null".
CObject const* CmnBehavior::FollowerPlayerShipOrNull(CObject const* Object)
{
    if (Object == nullptr)
        return nullptr;
    unsigned int const Class = *reinterpret_cast<unsigned int const*>(
        reinterpret_cast<char const*>(Object) + 0x4c);
    return ((Class & 0x503u) == 0x503u) ? Object : nullptr;
}
