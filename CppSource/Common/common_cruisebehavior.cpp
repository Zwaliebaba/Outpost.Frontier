// Author: Giperion.

#include "common_cruisebehavior.h"
#include "common_systemmanager.h"  // SystemManager::Thrust -- ThrustManager::engage_cruise

// CruiseBehavior -- see common_cruisebehavior.h for the full RTTI/layout
// writeup this pass relied on. Ported from Common (raw
// analysis is authoritative in this module over analysis's Common
// analysis -- see this marathon's "ground truth precedence" rule).

// ----------------------------------------------------------------------------
// (Common) -- constructor, 9 instructions,
// the smallest in the whole TractorArm family so far. Calls the shared
// base-init helper the routine(this) (it writes +0x04..+0x29, see Behavior in common_behavior.h) and installs the vtable; no field is
// zeroed, no sub-object is constructed. Every field is left to SetContext().
CruiseBehavior::CruiseBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)   // baseline leaves this uninitialized until SetContext(); zeroed
                                 // here for well-defined C++ semantics (documented deviation).
    , ModeSelector0xc(0)
    , UpdateResult10(0)
    , OwnerContextPtr14(nullptr)
    , Opaque18{}
    , BehaviorMgrPtr20(nullptr)
    , NavHelperPtr24(nullptr)
    , Flags28(0)
    , Flag29(0)
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
}

// Vtable slot 0 is the routine, the scalar-deleting-dtor thunk shared
// verbatim across 7 unrelated vtables (implementation tracker item 1320, already
// documented in common_idlebehavior.cpp) -- NO CruiseBehavior-specific field
// cleanup exists anywhere in it.
CruiseBehavior::~CruiseBehavior()
{
}

// Own vtable slot 1 (offset 0x04). Common
// (the branch) -- a pure `JMP the routine` tail-call, i.e. this method IS
// the shared Behavior-family context initializer with no CruiseBehavior-
// specific wrapper logic at all (see class comment for the cross-sibling ICF
// evidence this gives for 0x0639f650/0x0639feb8 too).
bool CruiseBehavior::SetContext(void* Param1, void* Param2, void* Param3)
{
    // `JMP the routine` -- Behavior::BindContext.
    return BindContext(Param1, Param2, Param3);
}

// Own vtable slot 3 (offset 0x0c). Common
// (the routine), 9 instructions.
void CruiseBehavior::Engage()
{
    Behavior::Slot3();                                               // the routine
    // ThrustManager::engage_cruise(true) on NavHelperPtr24 + 0x18.
    NavHelperPtr24->Thrust.engage_cruise(true);
}

// Own vtable slot 4 (offset 0x10). Common
// (the routine), 10 instructions, fully self-contained.
void CruiseBehavior::Disengage()
{
    // ThrustManager::engage_cruise(false) on NavHelperPtr24 + 0x18.
    NavHelperPtr24->Thrust.engage_cruise(false);
    Behavior::Slot4();                                               // JMP the routine
}

// Own vtable slot 5 (offset 0x14). Common
// (the branch), 7 instructions -- ICF-shared byte-identical body with
// IdleBehavior::SetModeFromFlags (common_idlebehavior.cpp); reproduced here
// as CruiseBehavior's own method rather than cast to that unrelated type.
int CruiseBehavior::SetModeFromFlags()
{
    int const Mode = (Flag29 != 0) ? 3 : 1;
    ModeSelector0xc = Mode;
    return Mode;
}

// Own vtable slot 7 (offset 0x1c). Common
// (the branch), 8 instructions, fully self-contained -- the simplest
// Update() anywhere in this family so far.
int CruiseBehavior::Update()
{
    UpdateResult10 = (OwnerContextPtr14 != nullptr) ? 1 : 3;
    return UpdateResult10;
}

