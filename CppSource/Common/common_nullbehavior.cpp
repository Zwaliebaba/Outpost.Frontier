// Author: Giperion.

#include "common_nullbehavior.h"

// NullBehavior -- see common_nullbehavior.h for the full RTTI/layout writeup
// this pass relied on. Ported from Common (raw
// analysis is authoritative in this module over analysis's Common
// analysis -- see this marathon's "ground truth precedence" rule).

// No dedicated ctor function exists in baseline -- NullBehavior is only ever produced by the
// id-0 factory the routine (see class comment): `operator new(0x2c)`, then
// `CALL the routine` with the descriptor's own type id, then the vtable install that the
// factory does inline (`MOV [ESI],063a0244`, module storage) because there is no class-specific
// constructor to do it. This port's constructor stands in for that whole sequence.

// CORRECTED 2026-09-17 (claude:common-factory). The text that stood here called the routine a
// "documented no-op" and said "nothing observable differs since the routine touches no fields
// either way". Both halves are wrong: module storage-062d205a writes ten fields spanning
// +0x04..+0x29, two of them to nonzero constants. The class had no storage for any of them,
// which is what the field block added to the header this pass fixes.
NullBehavior::NullBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)
    , ModeSelector0xc(2)
    , UpdateResult10(1)
    , OwnerContext(nullptr)
    , Opaque18{}
    , DeltaTime(0.0f)
    , BehaviorMgrPtr(nullptr)
    , NavHelper(nullptr)
    , Flags28(0)
    , Flag29(0)
    , Pad2a{}
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
    // Every value above is the routine's own: +0x0c = 2 (module storage), +0x10 = 1
    // (module storage), everything else zero. See the class's field block for why this
    // storage had to exist before the factory could safely make one of these.
}

// Vtable slot 0 is the routine, the scalar-deleting-dtor thunk shared
// verbatim across 7 unrelated vtables (implementation tracker item 1320) -- NO
// NullBehavior-specific field cleanup exists anywhere in it.
NullBehavior::~NullBehavior()
{
}

// Own vtable slot 1 (offset 0x04). Common
// (the branch), 2 instructions -- unconditionally succeeds, touches
// nothing (see class comment for why this is notable versus every other
// sibling's own SetContext).
bool NullBehavior::SetContext(void* /*Param1*/, void* /*Param2*/, void* /*Param3*/)
{
    return true;
}

// Own vtable slot 7 (offset 0x1c). ICF-folded with the unrelated baseline
// symbol Archetype::Solar::get_class_type (Common), 2
// instructions: `MOV EAX,2; RET`.
int NullBehavior::Update()
{
    return 2;
}
