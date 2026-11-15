// Author: Giperion.

#include "common_freeflightbehavior.h"
#include "common_systemmanager.h"  // SystemManager::Thrust (ThrustManager::bAutoAvoidance)

// FreeFlightBehavior -- see common_freeflightbehavior.h for the full
// RTTI/layout writeup this pass relied on. Ported from
// Common (raw analysis is authoritative in this module
// over analysis's Common analysis -- see this marathon's "ground
// truth precedence" rule).

// (module storage-062c6d2d, RET 4) -- constructor. CALL the routine
// (GotoBehavior's ctor) with the type id, install vtable 0639f5dc, then
// `MOV byte ptr [ESI+0x2eb],0x1`. Flag3fc is not written by baseline; it is value-initialised
// here and Slot3 clears it before anything could read it.
FreeFlightBehavior::FreeFlightBehavior(int InTypeId)
    : GotoBehavior(InTypeId)
    , Flag3fc(0)
    , Pad3fd{}
{
    Flag2eb = 1;
}

// (scalar deleting) / real body the routine: the same six watcher releases
// as GotoBehavior's the routine, then the routine (~Behavior) -- i.e. exactly what C++
// chaining into ~GotoBehavior() produces. Intentionally empty.
FreeFlightBehavior::~FreeFlightBehavior()
{
}

// Slot 3, the routine (module storage-062c6fbf).
void FreeFlightBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
    Flag365 = 1;
    Flag3fc = 0;
    SubObjectA.SetAngle38(3.14159274f);                             // , 0x40490fdb
    Field358 = 0.0f;
    SubObjectA.SetRange3c(0.0f);                                    // (bind_to_dc fold)
    SubObjectA.SetEnabled(false);                                   // the routine
    SubObjectA.SetField44(2);                                       // the routine
    // `PUSH 0x3` is SetCruiseLock's second argument; the CALL in between is
    // the ICF fold named EquipDesc::get_temporary (`MOV AL,[ECX+0x18]; RET`) on NavHelper+0x18,
    // i.e. ThrustManager::bAutoAvoidance. EAX's upper bytes are 0 (the routine left EAX = 2).
    SystemManager* const Sys = NavHelperPtr24;
    SetCruiseLock(Sys->Thrust.bAutoAvoidance, 3);                   // the routine
    Field2f4 = 1;
    Flag2ea = 1;
    Field2d8 = 0.0f;
    Field2dc = 0.0f;
    Field2e0 = 0.0f;
    ObjInspectOf(OwnerContextPtr14)->get_throttle(Field2e4);        // +0x4c
    ObjInspectOf(OwnerContextPtr14)->is_cruise_active(reinterpret_cast<bool&>(Flag2e8)); // +0x60
    // FCOMP 0.0f with TEST AH,0x5 + JP -> >= 0 or unordered keeps the
    // throttle and clears Flag2ec; strictly negative -> 1.0f and Flag2ec = 1.
    if (Field2e4 < 0.0f)
    {
        Field2e4 = 1.0f;
        Flag2ec = 1;
    }
    else
    {
        Flag2ec = 0;
    }
    // the destination point to 1e6f (0x49742400) on all three axes.
    Field304.x = 1000000.0f;
    Field304.y = 1000000.0f;
    Field304.z = 1000000.0f;
}

// Slot 4, the branch: `JMP the routine` -- Behavior's own slot-4 body, bypassing
// GotoBehavior::Slot4.
void FreeFlightBehavior::Slot4()
{
    Behavior::Slot4();
}

// Slot 6, the branch: `MOV EAX,1; RET 4` (shared by ICF with CruiseBehavior's slot 6 and
// NullBehavior's slot 5).
EvaluateResult FreeFlightBehavior::evaluate(unsigned int Context)
{
    (void)Context;
    return EvalBlocked;                                             // 1
}

// Slot 10, the branch (module storage-062c6ffc, RET 4, argument unread). Does not copy the
// op: Flags28 = (Flags28 & 0xfe) | 2, the steering triple and the throttle zeroed, Flag2e8
// cleared (from AL == 0 after the XOR), returns true.
bool FreeFlightBehavior::Slot10(void* DirectiveOp)
{
    (void)DirectiveOp;
    Flags28 = static_cast<unsigned char>((Flags28 & 0xfe) | 0x2);
    Field2d8 = 0.0f;
    Field2dc = 0.0f;
    Field2e0 = 0.0f;
    Field2e4 = 0.0f;
    Flag2e8 = 0;
    return true;
}

// Slot 15, the branch (module storage-062c7011, RET 4): Flag2e9 = argument, Flag2e8 = 0.
void FreeFlightBehavior::engage_engine(bool bOn)
{
    Flag2e9 = bOn ? 1 : 0;
    Flag2e8 = 0;
}
