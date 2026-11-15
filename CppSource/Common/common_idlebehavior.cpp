// Author: Giperion.

#include "common_idlebehavior.h"

// IdleBehavior -- see common_idlebehavior.h for the full RTTI/layout writeup
// this pass relied on. Ported from Common (raw
// analysis is authoritative in this module over analysis's Common
// analysis -- see this marathon's "ground truth precedence" rule).

// ----------------------------------------------------------------------------
// (Common) -- constructor. Calls the
// shared TractorArm-family base-init helper the routine first (item 1529,
// already a REAL field-initialising base ctor -- it writes +0x04..+0x29 (see Behavior in common_behavior.h); the long-recorded "no-op stub" reading was wrong),
// then default-constructs the still-unidentified IdleWatcher sub-object
// (FULLPORT-DEFERRED, see ConstructIdleWatcher), and finally (implicitly,
// via the base class list + this being a real C++ object) installs the
// vtable -- baseline's own `MOV dword ptr [ESI], the routine` needs no
// manual equivalent.
IdleBehavior::IdleBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)
    , ModeSelector0xc(0)
    , UpdateResult10(0)
    , OwnerContextPtr14(nullptr)
    , Opaque18{}
    , DeltaTime1c(0.0f)
    , BehaviorMgrPtr20(nullptr)
    , NavHelperPtr24(nullptr)
    , Flags28(0)
    , Flag29(0)
    , Pad2a{}
    , Watcher2c()   // IdleIdea's ctor (identified 2026-10-08)
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
}

// (Common) is MSVC's scalar-deleting-
// destructor boilerplate: it tail-calls a shared teardown helper
// (thunk_FUN_062d2080) and conditionally frees the block -- NO
// IdleBehavior-specific field cleanup exists anywhere in it (confirmed: this
// exact thunk address is shared verbatim across 7 unrelated vtables,
// implementation tracker item 1320, matching the same "empty real dtor" precedent
// already documented for InstantTradeLaneBehavior). Nothing for this port's
// destructor to do.
IdleBehavior::~IdleBehavior()
{
}

// Own vtable slot 1 (offset 0x04). Common
// (the branch), 28 instructions.
bool IdleBehavior::SetContext(void* Param1, void* Param2, void* Param3)
{
    // baseline: `CALL the routine(this, Param1, Param2, Param3)` -- the shared
    // Behavior-family context initializer (see common_dockbehavior.cpp's
    // DockBehavior::SetContext for the fully-traced body; reproduced inline
    // here again since IdleBehavior does not share a header with
    // DockBehavior and the two call sites' field targets, while offset-
    // identical, are genuinely different C++ members):
    //   if (Param1 == nullptr || Param3 == nullptr) return false;
    //   OwnerContextPtr14 = Param1;
    //   CachedHandle08 = Param1->vtbl[0x20](Param1);
    //   NavHelperPtr24 = Param3;
    //   BehaviorMgrPtr20 = Param2;
    //   return true;
    // 2026-10-08: module storage the routine -> Behavior::BindContext; on success
    // (062cd5b1-062cd5c0) Watcher2c's slot 0 (BaseIdea::SetContext) with
    // (OwnerContextPtr14, NavHelperPtr24, Param2); AL = the binder's result.
    bool const bOk = BindContext(Param1, Param2, Param3);
    if (bOk)
        Watcher2c.SetContext(OwnerContextPtr14, NavHelperPtr24, Param2);
    return bOk;
}

// Own vtable slot 3 (offset 0x0c). Common
// (the branch) -- a pure tail-call to the shared the routine with `this`
// unchanged. See common_idlebehavior.h for why this specific call site's
// body is safe to reproduce despite implementation tracker item 1533's global
// `simplified` status.
void IdleBehavior::ResetTickState()
{
    Behavior::Slot3();   // `JMP the routine` -- baseline's slot 3 IS Behavior's body
}

// Own vtable slot 4 (offset 0x10). Common
// (the routine), 8 instructions.
void IdleBehavior::Disengage()
{
    Watcher2c.Commit();     // `CALL [EAX+0x10]` on +0x2c (IdleIdea: bare RET)
    Behavior::Slot4();      // `JMP the routine`
}

// Own vtable slot 5 (offset 0x14). Common
// (the branch), 7 instructions: EAX = byte +0x29 ? 3 : 1; [+0xc] = EAX; RET 4.
int IdleBehavior::SetModeFromFlags()
{
    int const Mode = (Flag29 != 0) ? 3 : 1;
    ModeSelector0xc = Mode;
    return Mode;
}

// Own vtable slot 7 (offset 0x1c). Common
// (the routine), 18 instructions. Confirmed 1:1 against both raw analysis and
// Common.
int IdleBehavior::Update()
{
    if (OwnerContextPtr14 == nullptr)
    {
        UpdateResult10 = 3;
        return UpdateResult10;
    }

    // Watcher2c slot 1 (IdleIdea::Update) with DeltaTime1c; the
    // result is discarded.
    Watcher2c.Update(DeltaTime1c);

    UpdateResult10 = 1;
    return UpdateResult10;
}

// Own vtable slot 10 (offset 0x28). Common
// (the branch), 3 instructions -- ICF-shared with FormationBehavior's own
// slot 10 (BaseFollowerBehavior_DefaultRequestFlag, common_behavior.h/.cpp);
// see common_idlebehavior.h's class comment for why this is reproduced as
// its own method rather than a cast to that unrelated type.
bool IdleBehavior::RequestFlag()
{
    Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
    return true;
}

