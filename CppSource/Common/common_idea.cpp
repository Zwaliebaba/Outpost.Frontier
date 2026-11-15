// Author: Giperion.

// Common.dll BaseIdea family, shared part: BaseIdea, StopIdea, AfterburnerIdea, IdleIdea,
// the FDirectionPicker base of DodgeIdea / BreakAwayIdea, and GenericWeightedPick
// (the routine, the weighted pick over FDirectionPicker's tag + four weights).
// See src/include/common_idea.h for the RTTI/layout writeup. Ported from
// Common (raw analysis is authoritative in this module over
// analysis's Common -- most functions in this cluster have no analysis inspect).

// CMN-SPLIT stage 1 (2026-10-14): the other ideas went to common_moveidea.cpp,
// common_matchthrottleidea.cpp, common_faceidea.cpp, common_splinemoveidea.cpp,
// common_dodgeidea.cpp, common_breakawayidea.cpp and common_dodgemissileidea.cpp; the
// shared helpers to common_idea_internal.h. GenericWeightedPick moved here from
// common_aisteer.cpp. Code unchanged.

#include "common_idea.h"
#include "common_idea_internal.h"
#include "common_aisteer.h"   // BearingAngleToPoint, GetIfacePositionOrZero
#include "common_behavior.h"
#include "common_cequip.h"    // ThrustEquipType (IObjInspect::get_speed's 2nd argument)
#include "common_csys.h"      // Matrix / Transform
#include "common_iface.h"     // IObjInspect / IObjRW / IBehaviorManager / ObjRwFromWatchable
#include "common_pathfind.h"  // PathfindManager (IBehaviorManager::Pathfind)
#include "common_systemmanager.h" // SystemManager / ThrustManager (BaseIdea::Handle)
#include "x86math.h"          // I3DMathEngine::TransformMatrix

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace CmnIdea;

// ----------------------------------------------------------------------------
// BaseIdea
// ----------------------------------------------------------------------------

BaseIdea::BaseIdea()
    : Object(nullptr)
    , Handle(nullptr)
    , Context(nullptr)
    , Attached(0)
    , Pad11{}
{
}

// Slot 0 default implementation -- the branch (item 1617), Common
// 219718-219735. Used verbatim by 5 confirmed siblings (see common_idea.h's
// class-level comment); StopIdea/MoveIdea/FaceIdea each override this.
bool BaseIdea::SetContext(void* InObject, void* InHandle, void* InContext)
{
    if (InObject == nullptr || InHandle == nullptr)
    {
        return false;
    }

    Handle = (SystemManager*)InHandle;
    Context = InContext;
    Object = InObject;
    Attached = 1;
    return true;
}

// (Common), all 5 instructions; no null tests.
void BaseIdea::PublishTargetId(unsigned int Id)
{
    static_cast<IBehaviorManager*>(Context)->Pathfind->Field_064 = static_cast<int>(Id);
}

// ----------------------------------------------------------------------------
// StopIdea
// ----------------------------------------------------------------------------

StopIdea::StopIdea()
    : BaseIdea()
{
    // baseline (the routine-shaped tiny ctor when embedded, see MoveIdea's own
    // ctor below) only ever zeroes BaseIdea's own span (+0x04/+0x08/+0x10) and
    // installs the vtable -- both handled by the BaseIdea base subobject and
    // the compiler-generated vtable pointer respectively. Nothing else to do.
}

// Own vtable slot 0 override -- the branch, `MOV AL,1; RET 0xc`.
// Unconditionally succeeds; ignores all 3 incoming pointers.
bool StopIdea::SetContext(void* /*InObject*/, void* /*InHandle*/, void* /*InContext*/)
{
    return true;
}

// Own vtable slot 1 override -- the branch. FULLPORT-DEFERRED: this address
// sits far outside this cluster's own 0x062d6000-0x062da600 range and was not
// traced this pass.
int StopIdea::Update(float /*DeltaTime*/)
{
    return 0;
}

// ----------------------------------------------------------------------------
// AfterburnerIdea
// ----------------------------------------------------------------------------

AfterburnerIdea::AfterburnerIdea()
    : BaseIdea()
{
    // (Common, 7 instructions): zeroes
    // BaseIdea's own +0x04/+0x08/+0x10 span (handled by the BaseIdea base
    // subobject above) and installs the branch (handled implicitly by
    // the compiler via this class's own virtual overrides). No fields of its
    // own beyond BaseIdea -- see common_idea.h's class-level comment.
}

// Own vtable slot 1 override -- the routine (item 1658), Common
// 223762-223782, RET 4. Ported in full 2026-09-17: all 13 instructions.
// DeltaTime is accepted (RET 4) and never read on any path.
int AfterburnerIdea::Update(float /*DeltaTime*/)
{
    // `MOV EAX,[ESI+4]; TEST EAX,EAX; JNZ`, else `MOV EAX,3`.
    if (Object == nullptr)
    {
        return 3;
    }

    // Handle+0x18 recomputed for each call, never tested.
    // `PUSH 0x1` then the receiver; then `PUSH 0x0; PUSH 0x3f800000`, so the 1.0f
    // is the FIRST argument and the 0 the second.
    Handle->Thrust.SetThrustersActive(true);
    Handle->Thrust.SetThrottle(1.0f, 0);

    return 0; // `XOR EAX,EAX`
}

// Own vtable slot 4 override -- the branch (module storage), Common
// 223806-223810, 5 instructions, bare `RET`. The "let off" counterpart of
// Update's `(..., 1)`. No null test on Handle.
void AfterburnerIdea::Commit()
{
    Handle->Thrust.SetThrustersActive(false); // /062da578
}

// ----------------------------------------------------------------------------
// IdleIdea (2026-10-08, claude-code:cmn-ai2)
// ----------------------------------------------------------------------------

// (Common), 7 instructions: BaseIdea's span zeroed by
// the base subobject, vtable 063a054c by the compiler.
IdleIdea::IdleIdea()
    : BaseIdea()
{
}

// (module storage-062d9bd7), all instructions traced; no null test on Handle.
int IdleIdea::Update(float /*DeltaTime*/)
{
    if (Object == nullptr)
        return 3;

    ThrustManager* const Thrust = &Handle->Thrust;
    Thrust->SetThrottle(0.0f, 2);                                    // (PUSH 2; PUSH 0)
    Thrust->SetThrottle(0.0f, 0);
    Vector Zero;
    Zero.x = 0.0f;
    Zero.y = 0.0f;
    Zero.z = 0.0f;
    Thrust->SetAxisThrottle(Zero);                                   // the routine
    Zero.x = 0.0f;                                                   // re-zeroed, 062d9b9a-062d9baa
    Zero.y = 0.0f;
    Zero.z = 0.0f;
    Thrust->SetNudgeDirection(Zero);                                 // the routine
    Thrust->SetStrafeDir(StrafeNone);                                // the routine
    Thrust->SetThrustersActive(false);                               // the routine
    return 0;
}

// ----------------------------------------------------------------------------
// FDirectionPicker -- the shared second base. Its whole initialization is the
// +0x14..+0x33 prologue DodgeIdea's and BreakAwayIdea's constructors share
// byte-for-byte (see common_idea.h); Flag1c is set by each derived ctor.
// ----------------------------------------------------------------------------
FDirectionPicker::FDirectionPicker()
    : LastDirection(0)   // / 062d89aa
    , WeightLeft(1.0f)   // / 062d899e
    , WeightRight(1.0f)  // / 062d89a1
    , WeightUp(1.0f)     // / 062d89a4
    , WeightDown(1.0f)   // block / 062d89a7
    , Watcher()       // / 062d899b-062d89c9
    , Flag1c(0)       // each derived ctor overwrites this with its own constant
    , Pad1d{}
{
}

// , Common module storage-062d8904 (12 instructions, RET 4). The
// receiver register is the Idea; +0x28 / +0x30 are this picker's Watcher / Flag1c.
void FDirectionPicker::SetWatchObject(void const* Object)
{
    // Object ? Object + 8 : null (IObjRW -> Watchable).
    Watchable const* const Target =
        (Object != nullptr)
            ? reinterpret_cast<Watchable const*>(static_cast<unsigned char const*>(Object) + 8)
            : nullptr;
    Watcher.set(Target);   // CMP EAX,[ECX]; JZ; set_pointer
    Flag1c = 1;            // `MOV byte [ESI+0x30],0x1`, unconditional
}

// , module storage-062d76cc: [ECX+4..+0x10] = the four arguments in order.
void FDirectionPicker::SetWeights(float Left, float Right, float Up, float Down)
{
    WeightLeft = Left;
    WeightRight = Right;
    WeightUp = Up;
    WeightDown = Down;
}

// , item 1615. See common_aisteer.h for the declaration comment.
int GenericWeightedPick(int* State)
{
    // local copies of the four weights, the current tag's zeroed.
    int const Tag = State[0];
    float const* const W = reinterpret_cast<float const*>(State + 1);
    float const W1 = (Tag == 1) ? 0.0f : W[0];
    float const W2 = (Tag == 2) ? 0.0f : W[1];
    float const W3 = (Tag == 3) ? 0.0f : W[2];
    float const W4 = (Tag == 4) ? 0.0f : W[3];

    // FILD rand(); FLD W4; FADD W3; FADD W2; FADD W1; FMULP;
    // FMUL the field (2^-15). The draw stays on the FPU stack, never rounded.
    double const Draw = ((double)rand() * ((((double)W4 + W3) + W2) + W1)) * (1.0 / 32768.0);

    // Each test is FCOM/FCOMPP + `TEST AH,0x5; JP`: the jump (to the next test)
    // is taken on not-less or unordered, so a NaN draw ends at 4. The running
    // sums are built fresh each time, also unrounded: W2+W1, then (W3+W2)+W1.
    int NewTag;
    if (Draw < W1)
        NewTag = 1;
    else if (Draw < (double)W2 + W1)
        NewTag = 2;
    else if (Draw < ((double)W3 + W2) + W1)
        NewTag = 3;
    else
        NewTag = 4;

    State[0] = NewTag;
    return NewTag;
}
