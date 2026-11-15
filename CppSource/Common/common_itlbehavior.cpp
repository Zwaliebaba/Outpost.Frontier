// Author: Giperion.

#include "common_itlbehavior.h"
#include "common_cobject.h"  // CObject::ObjectClass, CShip::request_exit_tradelane
#include "common_systemmanager.h"  // SystemManager::Thrust -- TurnOff

// (implementation tracker item 1798) -- the NavHelper+0x34 "resolve and verify by id"
// lookup -- is SystemManagerTimerTable::FindVerifiedObject (common_sysmgr_timertable.cpp).


// InstantTradeLaneBehavior -- see common_itlbehavior.h for the full RTTI/
// layout writeup this pass relied on. Ported from Common
// (raw analysis is authoritative in this module over analysis's
// Common analysis -- see this marathon's "ground truth
// precedence" rule).

// ----------------------------------------------------------------------------
// (Common) -- constructor. Calls the
// shared TractorArm-family base-init helper the routine first (item 1529,
// already a REAL field-initialising base ctor -- it writes +0x04..+0x29 (see Behavior in common_behavior.h); the long-recorded "no-op stub" reading was wrong,
// same as DockBehavior's ctor), then installs both vtables and does nothing
// else. Every field is zero-initialized here anyway (documented gap vs.
// baseline, which leaves everything but the two vtable pointers untouched in
// this particular constructor -- see the header's class-level comment).
// (The block above documents InstantTradeLaneBehavior::InstantTradeLaneBehavior, defined just
// below this base ctor.)

// forwards its own type-id argument to the routine at module storage.
InstantTradeLaneBehaviorFields::InstantTradeLaneBehaviorFields(int InTypeId)
    : TypeId(InTypeId)
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
}

InstantTradeLaneBehavior::InstantTradeLaneBehavior(int InTypeId)
    : InstantTradeLaneBehaviorFields(InTypeId)
    , State30(0)
    , SubObjPtr34(nullptr)
    , LanePtr38(nullptr)
    , LaneStart3c()
    , LaneEnd48()
    , Flags54(0)
    , Flag55(0)
    , Flag56(0)
    , Pad57(0)
    , Field58(0)
{
}

// Scalar-deleting-destructor thunk (the routine, item 1320, ICF-shared
// boilerplate) calls thunk_FUN_062d2080 directly with no class-specific
// cleanup at all (Common) -- this class owns no
// BaseWatcher-shaped sub-objects to tear down, unlike DockBehavior.
InstantTradeLaneBehavior::~InstantTradeLaneBehavior()
{
    // baseline calls thunk_FUN_062d2080 (~Behavior, a vptr reset); the port's base destructor
    // runs implicitly. Nothing this class owns needs tearing down beyond that.
}

// Own vtable slot 1 (offset 0x04). Common (the branch).

// CLOSED 2026-09-17 (claude:common-largeship-itl) -- fully real, no deferral left. See the
// header for why this was renamed from ResetIfTargetQueryFailed and re-signed from two void
// parameters to three plus a bool return (baseline's `RET 0xc` and `MOV AL,1`/`XOR AL,AL`).

// Argument recovery from the raw bytes, since the old declaration had it wrong: at entry
// [ESP+4]/[ESP+8]/[ESP+0xc] are Param1/Param2/Param3; baseline reads Param3 into EAX and
// Param1 into EDX, pushes ESI, re-reads Param2 into ECX, then pushes EAX, ECX, EDX -- so
// receives them in their original order (module storage-062cee84).

// is Behavior::BindContext (Param1 owner -> +0x14, its id -> +0x08, Param3 ->
// +0x24 nav helper, Param2 -> +0x20 manager; false when Param1 or Param3 is null).
bool InstantTradeLaneBehavior::SetContext(void* Param1, void* Param2, void* Param3)
{
    bool const bInstalled = BindContext(Param1, Param2, Param3);     // the routine

    // `XOR ECX,ECX; CMP AL,CL; JZ the branch` -- the JZ skips the clears, so they run on
    // the TRUE path. The old port had this branch's polarity documented correctly but never
    // executed it, because it hard-coded the gate to false.
    if (bInstalled)
    {
        Flag55 = 0;
        Flag56 = 0;
        Flags54 = 0;
        SubObjPtr34 = nullptr;
        LanePtr38 = nullptr;
        Field58 = 0;
    }

    return bInstalled;
}

// Own vtable slot 3 (offset 0x0c). Common
// (the routine, implementation tracker item 1445).
void InstantTradeLaneBehavior::TurnOff()
{
    // 2026-10-08 (claude-code:cmn-ai3), all 16 instructions: `[ESI+0x14]` null -> RET;
    // (0, 3) (Behavior::SetCruiseLock); +0x30 = 0; the routine
    // (Behavior::Slot3); ThrustManager::engage_cruise(false) on [ESI+0x24]+0x18.
    if (TargetPtr14 == nullptr)
        return;
    SetCruiseLock(0, 3);
    State30 = 0;
    Behavior::Slot3();
    NavHelperPtr24->Thrust.engage_cruise(false);
}

// Own vtable slot 4 (offset 0x10). Common
// (the routine).
void InstantTradeLaneBehavior::RequestExitTradeLane()
{
    // `TEST ECX,ECX; JZ the branch` -- the null-check on TargetPtr14 gates
    // the ENTIRE function body, including the the routine/SubObjPtr34-clear/
    // tail below (confirmed: the JZ target is the function's
    // final POP/RET, not a mid-function rejoin point).
    if (TargetPtr14 == nullptr)
        return;

    bool const bWasStatusSet = (Flags54 != 0);
    Flag55 = 0;
    Flag56 = 0;
    Flags28 |= 0x1;

    if (bWasStatusSet)
    {
        Flags54 = 0;

        // CLOSED 2026-09-17 (claude:common-largeship-itl). The old reason here --
        // "TargetPtr14's real interface is not identified, so this port cannot resolve a
        // genuine CShip*" -- is disproved. TargetPtr14 is an IObjRW* (see its field note in
        // the header), so vtbl+0x150 is IObjInspect::cobject(), returning a CObject const*;
        // and the "+0x4c" the old note described as an opaque status mask is simply
        // CObject::ObjectClass, which common_cobject.h has had typed and static_asserted at
        // that exact offset all along. baseline, module storage-062cef87:
        CObject const* const Obj = TargetPtr14->cobject();

        CShip* Ship = nullptr;
        if (Obj != nullptr && (Obj->ObjectClass & CObject::ClassShip) == CObject::ClassShip)
            Ship = (CShip*)Obj;

        // baseline reaches `MOV ECX,EAX; CALL CShip::request_exit_tradelane` on BOTH paths
        // (062cef83-062cef87), i.e. with a null `this` whenever cobject() is null or fails the
        // class test -- a deterministic null dereference, reproduced without a guard.
        Ship->request_exit_tradelane();
    }

    // the routine (Behavior::Slot4 -- Pathfind->Field_064 = 0, Flag29 = 0),
    // +0x34 = 0, the routine(0, 3) (Behavior::SetCruiseLock). Both calls were deferred
    // until 2026-10-08.
    Behavior::Slot4();
    SubObjPtr34 = nullptr;
    SetCruiseLock(0, 3);
}

// Own vtable slot 5 (offset 0x14). Common
// (the branch).
int InstantTradeLaneBehavior::UpdateMode()
{
    int Mode = 2;

    // `TEST EAX,EAX; JZ the branch` -- TargetPtr14==nullptr skips BOTH the
    // Flags54 check AND the SubObjPtr34 vcall below, falling straight
    // through to the Flag29 check.
    if (TargetPtr14 != nullptr)
    {
        if (Flags54 != 0)
            Mode = 1;

        if (SubObjPtr34 != nullptr)
        {
            // CLOSED 2026-09-17 (claude:common-largeship-itl). SubObjPtr34's interface IS
            // identified now -- it is an IObjInspect const* (proven by Update()'s state-0
            // case passing it as get_lane_start's `Self` argument), so vtbl+0x88 is
            // IObjInspect::get_type(uint& OutType), slot 34. baseline,
            // the call, then `MOV AL,[ESP+8]; TEST AL,AL; JNS` --
            // it reads only the out-parameter's LOW BYTE and branches on that byte's sign
            // bit, so the predicate is `(Type & 0x80) != 0`, not a signed test of the whole
            // dword as an earlier reading of this code had it. get_type's int return value
            // is discarded.
            unsigned SubObjectType = 0;
            SubObjPtr34->get_type(SubObjectType);
            if ((SubObjectType & 0x80u) != 0)
                Mode = 1;
        }
    }

    if (Flag29 != 0)
        Mode = 3;

    ModeSelector0xc = Mode;
    return Mode;
}

// Own vtable slot 7 (offset 0x1c), Update/dispatch entry point.
// Common (the branch), 66 instructions across a
// 7-way (State30 0-6) switch.
int InstantTradeLaneBehavior::Update()
{
    // `TEST byte ptr [ESI+0x28],1; JZ ...` -- early-out if the exit-tradelane
    // bit is already set.
    if (Flags28 & 0x1)
    {
        UpdateResult10 = 2;
        return UpdateResult10;
    }

    // `MOV ECX,[ESI+0x14]; TEST ECX,ECX; JNZ dispatch` -- no target yet.
    if (TargetPtr14 == nullptr)
    {
        UpdateResult10 = 3;
        return UpdateResult10;
    }

    // baseline: `MOV EDX,[ESI+0x30]; CMP EDX,6; JA caseD_1; JMP [EDX*4 + switchdata]`
    // (module storage-062ceff0). States 0..6; anything above 6 joins case 1.

    // THIS SWITCH IS REAL CODE AS OF 2026-09-17 (claude:common-largeship-itl). It had been one
    // FULLPORT-DEFERRED block on the grounds that state 0 dispatches "through `*this`'s own
    // vtable at byte offset 0x138, which exceeds this class's own 27-slot vtable, so the real
    // target is not resolvable". That was a register misread: ECX is loaded with TargetPtr14
    // at module storage (the guard above) and is NEVER reloaded before or inside the switch,
    // so 0x138 and 0x64 below are slots on TargetPtr14, not on this object. See the header.
    switch (State30)
    {
    case 0:
    {
        // .
        if (LanePtr38 == nullptr)
        {
            // `LEA EDX,[ESI+0x48]; PUSH; LEA EDX,[ESI+0x3c]; PUSH; LEA EDX,[ESP+0x10]; PUSH;
            //  MOV EDX,[ESI+0x34]; MOV [ESP+0x14],0; MOV EAX,[ECX]; PUSH EDX;
            //  CALL [EAX+0x138]` -- IObjInspect::get_lane_start(Self, OutLane, OutStart,
            // OutEnd), slot 78. The pushes are right-to-left, so OutEnd (+0x48) going on the
            // stack first makes it the LAST argument.
            IObjInspect const* OutLane = nullptr;
            int const LaneResult = TargetPtr14->get_lane_start(
                SubObjPtr34, OutLane, LaneStart3c, LaneEnd48);

            // `TEST EAX,EAX; JZ the branch` -- ZERO is the success path. (An earlier
            // summary of this function had the polarity inverted, calling result==0 the
            // "vcall failure".)
            if (LaneResult != 0)
            {
                UpdateResult10 = 3;
                return 3;
            }
            LanePtr38 = OutLane;
        }

        // also the direct target when LanePtr38 was already set.
        UpdateResult10 = 1;
        State30 = 3;
        Flag56 = 1;
        Flag55 = 0;
        return 1;
    }

    case 3:
    {
        // . `MOV EDX,[ECX+0x4]` reads the vtable pointer of the
        // IObjAffect sub-object at TargetPtr14+4; `ADD ECX,0x4` makes it the call's `this`.
        // Arguments, right-to-left off the stack: (SubObjPtr34, LanePtr38, 1, Field58).
        // That is IObjAffect slot 25, go_tradelane(EntranceRing, ExitRing, bFastExit, Speed)
        // (named 2026-09-24 from Server.dll's Ship body module storage).
        int const AffectResult = TargetPtr14->go_tradelane(SubObjPtr34, LanePtr38, true, Field58);

        // `TEST EAX,EAX; JNZ the branch` -- zero is success.
        if (AffectResult != 0)
        {
            // NOTE UpdateResult10 is set from EDI, which is still 1 here --
            // it is only zeroed on the success path below. (An earlier summary said this
            // path yields 0; it does not.)
            UpdateResult10 = 1;
            State30 = 6;
            return 1;
        }

        // is implementation tracker item 1561, ported for real (ready_unverified) as
        // IBehaviorManager::QueueNotification(IObjRW* Target, unsigned Extra, float Duration)
        // in common_behaviormanager.cpp -- the "unprocessed" note that used to stand here was stale.
        // baseline pushes EAX (== 0) twice then TargetPtr14, with ECX = BehaviorMgrPtr20, so
        // Extra == 0 and Duration == 0.0f (the zero dword is a valid 0.0f bit pattern) -- the
        // immediate path: TargetPtr14's formation followers get notification 0 now.
        // no null test on [ESI+0x20] before the call.
        BehaviorMgrPtr20->QueueNotification(TargetPtr14, 0, 0.0f);

        UpdateResult10 = 0;
        Flags54 = 1;
        State30 = 4;
        return 0;
    }

    case 5:
    case 6:
    {
        // . Both cases set Flags28 bit 0x1, call this object's OWN
        // vtable slot 4 (`CALL [EDX+0x10]` with ECX = ESI = this -- genuinely self-referential
        // here, unlike the switch's other dispatches), then write the two manager fields. The
        // only difference between them is the value handed to the routine: 2 for state 6,
        // 0 for state 5.
        // Captured BEFORE the call: baseline selects the constant at the case label
        // (`PUSH 0x2` in caseD_6 / `PUSH 0x0` in caseD_5) and only then converges on
        // , so the value must not be re-derived from State30 afterwards.
        int const ManagerArg118 = (State30 == 6) ? 2 : 0;

        Flags28 = static_cast<unsigned char>(Flags28 | 0x1);
        Slot4();                                                   // `CALL [EDX+0x10]`, virtual
        BehaviorMgrPtr20->SetDirectiveResult(9, ManagerArg118);
        UpdateResult10 = 2;
        return 2;
    }

    case 1:
    case 2:
    case 4:
    default:
        // caseD_1/2/4 and states above 6 (`JA caseD_1`) land on the branch with EDI = 1 from
        // (only cases 5/6 load EDI = 2, at 062cf0c5) -- still running.
        UpdateResult10 = 1;
        return 1;
    }
}

// Own vtable slot 10 (offset 0x28). Common
// (the branch).
bool InstantTradeLaneBehavior::AttachDirective(void* Directive)
{
    // CLOSED 2026-09-17 (claude:common-largeship-itl) -- all 37 instructions are real now.
    // The GAP:AI-MANEUVER-EXECUTOR tag and the "the routine is unprocessed" reason were
    // BOTH stale: implementation tracker item 1798 was closed on 2026-09-11 and the function is
    // defined for real as SystemManagerTimerTable::FindVerifiedObject (common_sysmgr_timertable.cpp). baseline (module storage-062cf14a):
    //   Flags28 &= ~0x1;                                              (`AND DL,0xfe`)
    //   SubObjPtr34 = the routine(NavHelperPtr24 + 0x34, Directive + 0xc);
    //   LanePtr38   = the routine(NavHelperPtr24 + 0x34, Directive + 0x10);
    //   Field58     = *(float*)(Directive + 0x14);   (a dword copy; go_tradelane's Speed)
    //   Flags28 = SubObjPtr34 ? (Flags28 | 0x2) : (Flags28 & ~0x2);
    //   return SubObjPtr34 != nullptr;
    // Both lookups use this+0x24 (the NavHelper), NOT this+0x20 -- re-confirmed against the
    // raw bytes this pass (`MOV ECX,[ESI+0x24]; ADD ECX,0x34` at module storage/062cf105 and
    // again at module storage/062cf117). Easy to conflate: both are plain pointer fields at
    // adjacent offsets.
    Flags28 &= ~0x1;

    char* const DirectiveBytes = static_cast<char*>(Directive);
    SystemManagerTimerTable const* const LookupThis = &NavHelperPtr24->Timers;

    SubObjPtr34 = LookupThis->FindVerifiedObject((unsigned const*)(DirectiveBytes + 0xc));
    LanePtr38 = LookupThis->FindVerifiedObject((unsigned const*)(DirectiveBytes + 0x10));

    Field58 = *reinterpret_cast<float*>(DirectiveBytes + 0x14);

    if (SubObjPtr34 != nullptr)
        Flags28 |= 0x2;
    else
        Flags28 &= ~0x2;

    return SubObjPtr34 != nullptr;
}

// Own vtable slot 22 (offset 0x58). Common
// (the branch).
void InstantTradeLaneBehavior::HandleTargetEvent(void* Event)
{
    // baseline's tagged-event shape: `struct { int Tag; void* Payload; } const*`.
    // Only proceeds when Tag == 4 and Payload->FirstByte == 0.
    struct TaggedEvent { int Tag; unsigned char* Payload; };
    TaggedEvent const* Ev = static_cast<TaggedEvent const*>(Event);

    if (Ev->Tag != 4 || Ev->Payload[0] != 0)                         // , no null test
        return;

    // CLOSED 2026-09-17 (claude:common-largeship-itl). TargetPtr14 is an IObjRW*, so
    // vtbl+0x20 is IObjInspect::get_id() -- the very slot the family's own shared context
    // initializer (item 1532) reaches through the same offset on the same kind of pointer.
    // baseline, module storage-062cf1f0.
    if (TargetPtr14->get_id() != *reinterpret_cast<unsigned const*>(Ev->Payload + 0xc))
        return;

    // `MOV dword ptr [ESI+0x30],5` -- State30=5 is set UNCONDITIONALLY once
    // both guards above pass, regardless of SubObjPtr34 (confirmed via raw
    // analysis: the SubObjPtr34 null-check comes AFTER this write, gating only
    // the vcall block below, not the State30 assignment itself).
    State30 = 5;

    if (SubObjPtr34 == nullptr)
        return;

    // . SubObjPtr34's vtbl+0x20 is also IObjInspect::get_id(); the
    // id it returns is stashed in a stack slot whose ADDRESS is then handed to the
    // IObjAffect sub-object at TargetPtr14+4, slot 0x60, as (2, &SavedId, 0). baseline reuses
    // its own incoming parameter slot as that scratch (`MOV [ESP+0xc],0` at module storage
    // pre-zeroes it), which is why the old reading saw a "localbuf" with no declaration.
    // Slot 24's signature is declared in common_iface.h since 2026-09-24 (Server.dll's Ship
    // body module storage, RET 0xc).
    unsigned SavedId = SubObjPtr34->get_id();
    (void)TargetPtr14->slot_60(2, &SavedId, 0);
}

// IObjAffect::RequestCB override -- own single secondary-vtable slot.
// Common (the branch), 47 instructions.
bool InstantTradeLaneBehavior::OnResult(int /*Param1*/, int /*Param2*/, int Param3)
{
    // baseline (outer_this recovered via a further -0x18 adjustment from the
    // already-adjusted RequestCB `this`, i.e. outer+0x14 relative to the
    // secondary vtable's own base -- confirmed against TargetPtr14's offset):
    Flag55 = 0;

    // `CMP EAX,-1; JZ the branch` -- Param3 != -1 is the FALL-THROUGH path
    // (not the jump target); this is the simple, fully self-contained branch.
    if (Param3 != -1)
    {
        Flag56 = 1;
        State30 = 3;
        return false;
    }

    // Param3 == -1 (the branch):
    State30 = 6;

    // baseline also re-tests `Flag55 != 0` here (`CMP [ESI+0x29],BL; JNZ ...`),
    // but Flag55 was unconditionally zeroed two instructions above with no
    // write in between -- that comparison is PROVABLY always-false (dead
    // code in baseline itself), so only Flag56 actually gates the block below.
    if (Flag56 != 0)
    {
        // CLOSED 2026-09-17 (claude:common-largeship-itl). Two corrections to the old note.
        // (1) The slot is 0x20, not 0x8: module storage reads `MOV ECX,[ESI+0x8]` -- and
        //     since ESI here is the ADJUSTED RequestCB `this` (outer + 0x2c), [ESI+0x8] is
        //     outer+0x34, i.e. SubObjPtr34 itself; the call is `CALL [EAX+0x20]`, which on
        //     an IObjInspect const* is get_id(). The "0x8" in the old note was the field
        //     offset being mistaken for the vtable slot.
        // (2) Both owning interfaces ARE identified now: SubObjPtr34 is IObjInspect const*
        //     and TargetPtr14 (reached at [ESI-0x18] == outer+0x14) is IObjRW*, whose
        //     IObjAffect sub-object sits at +4. Same (2, &SavedId, 0) shape as
        //     HandleTargetEvent()'s call, with the id written into the scratch slot first
        //     (`MOV [ESP+0x14],EAX` at module storage / module storage-062cf1a2 here).
        // Both calls are unguarded in baseline (module storage-062cf1a2).
        unsigned SavedId = SubObjPtr34->get_id();
        (void)TargetPtr14->slot_60(2, &SavedId, 0);

        // baseline then unconditionally clears Flag55/Flag56.
        Flag55 = 0;
        Flag56 = 0;
    }

    if (Flags54 != 0)
    {
        Flags54 = 0;
        State30 = 6;
    }

    return false;
}
