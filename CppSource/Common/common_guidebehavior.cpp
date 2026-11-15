// Author: Giperion.

#include "common_guidebehavior.h"
#include "common_pathfind.h" // PathfindManager (IBehaviorManager+0x0C)
#include "common_systemmanager.h" // SystemManager::Targeting / ExternalIface (NotifyManagerGuideEvent)

// Update()'s steering/geometry leaves. ClampedDot3 (the routine),
// GetIfacePositionOrZero (the routine), GetInterceptClearanceOfIface (the routine)
// and GetSignedRadialClearanceToPoint (the routine) are all declared here; Distance3
// (the routine) and VecNormalize3 (the routine) arrive via common_ini.h, and
// ObjInspectOf/ObjRwFromWatchable via common_iface.h, both already pulled in by
// common_behavior.h.
#include "common_aisteer.h"

#include <cmath> // std::acos -- Update()'s 062cfa2e `_CIacos` call
#include <cstdint>
#include <cstring>

// (item 1798): the checked id lookup through the IObjDB at SystemManager+0x34,
// SystemManagerTimerTable::FindVerifiedObject.

// GuideBehavior -- see common_guidebehavior.h for the full RTTI/layout writeup this pass
// relied on. Ported from Common (raw analysis is authoritative in this
// module over analysis's Common analysis -- see this marathon's "ground truth
// precedence" rule).

// (Common) -- constructor. Calls the shared base-init
// field-initialising the routine (it writes +0x04..+0x29, see Behavior in common_behavior.h) first, zeroes WatcherA, default-constructs the two REAL embedded
// sub-objects (GotoOp @+0x68, FollowOp @+0xd8 -- both already-ported types, common_pub.h),
// zeroes WatcherB, and finally (implicitly, via C++ construction) installs the vtable,
// matching baseline's own `MOV dword ptr [ESI],the routine` followed by the five
// remaining scalar field writes.
GuideBehavior::GuideBehavior(int InTypeId)
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
    , WatcherA()
    , Field34(0.0f)
    , Flag38(1)
    , Pad39{}
    , Field3c(0.0f)
    , Field40(0)
    , Field44(0.0f)
    , Field48{}
    , Field54(0.0f)
    , Field58(0.0f)
    , Field5c(0.0f)
    , CachedPtr60(nullptr)
    , CachedPtr64(nullptr)
    , GotoOp()
    , FollowOp()
    , WatcherB()
    , Field104(10.0f)
    , Field108(0.0f)
    , Field10c(0.0f)
    , Field110(0.0f)
    , FieldLow114(-10.0f)
    , FieldHigh118(10.0f)
    , AngleThreshold11c(0.39269909f)
    , Flag120(0)
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
    // baseline: the ctor also performs `CMP dword ptr [this+0xfc],0; JZ skip; CALL
    // BaseWatcher::set_pointer(this+0xfc, nullptr)` right after installing the vtable --
    // CONFIRMED DEAD CODE (matching this marathon's LootBehavior ctor precedent): the
    // compared dword was JUST zeroed by this same ctor a few instructions earlier (WatcherB's
    // own zero-init) and nothing in between writes it, so the branch always takes the
    // "already null" path. Not reproduced here.
}

// Real (non-thunk) destructor path. the routine (Common) is the
// scalar-deleting-dtor thunk: it calls the routine (the REAL cleanup body, 205796-205835)
// then conditionally frees. the routine clears WatcherB (+0xfc) then WatcherA (+0x2c) --
// note baseline clears them in that order -- before tail-calling the shared
// (this,-1) effect (FULLPORT-DEFERRED, same family-shared teardown helper
// documented on every other sibling's own dtor).
GuideBehavior::~GuideBehavior()
{
    WatcherB.set(nullptr);
    WatcherA.set(nullptr);
}

// Slot 1, the branch: `JMP the routine`.
bool GuideBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    return BindContext(Owner, Manager, Sys);
}

// Slot 3, the branch (module storage-062cf4eb).
void GuideBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    CachedPtr60 = BehaviorMgrPtr20->BehaviorDispatchTable[2];       // Manager+0x17c (Goto)
    CachedPtr64 = BehaviorMgrPtr20->BehaviorDispatchTable[17];      // Manager+0x1b8 (Follow)
    (void)CachedPtr60->Slot10(&GotoOp);
    CachedPtr60->Slot3();
    (void)CachedPtr64->Slot10(&FollowOp);
    // on the follow behavior itself (baseline writes its offsets directly):
    // +0x28c = 1, MatchThrottle.SetFlag39(1) (the routine), Move.SetField44(2) (the routine).
    BaseFollowerBehavior* const Follow = static_cast<BaseFollowerBehavior*>(CachedPtr64);
    Follow->Flag28c = 1;
    Follow->MatchThrottle.SetFlag39(true);
    Follow->Move.SetField44(2);
    CachedPtr64->Slot3();
    Field48 = GotoOp.Destination;                                   // (GotoOp+0x10)
    Field54 = GotoOp.Field_50;                                      // (GotoOp+0x50)
    Field108 = 0.0f;
    // |WatcherA position - Field48|, the sum ((dx*dx + dz*dz) + dy*dy) and
    // FSQRT on the x87 stack, one rounding into +0x10c.
    Vector Pos{ 0.0f, 0.0f, 0.0f };
    GetIfacePositionOrZero(&Pos, ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target())));
    double const Dx = static_cast<double>(Pos.x) - Field48.x;
    double const Dy = static_cast<double>(Pos.y) - Field48.y;
    double const Dz = static_cast<double>(Pos.z) - Field48.z;
    Flag120 = Flag38;
    Field58 = 0.0f;
    Field5c = 0.0f;
    Field10c = static_cast<float>(std::sqrt((Dx * Dx + Dz * Dz) + Dy * Dy));
}

// Slot 4, the routine (module storage-062cf51e).
void GuideBehavior::Slot4()
{
    if (CachedPtr60 != nullptr)
    {
        CachedPtr60->Slot4();
        Behavior_ReleaseDirectiveTarget(CachedPtr60);               // the routine
    }
    if (CachedPtr64 != nullptr)
    {
        CachedPtr64->Slot4();
        Behavior_ReleaseDirectiveTarget(CachedPtr64);               // the routine
    }
    Behavior::Slot4();                                              // JMP the routine
}

// Slot 5, the routine (module storage-062cf5d3, RET 4, argument unread).
int GuideBehavior::Slot5(float Value)
{
    (void)Value;
    if ((Flags28 & 0x2) != 0)
    {
        IObjRW* const Lead = ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target()));
        if (Lead != nullptr)
        {
            // the routine(Sys+0x14fc, Lead, Field34).
            if (IsWithinInterceptRangeOfIface(&NavHelperPtr24->SelfTarget, Lead, Field34))
                ModeSelector0xc = (Flag29 != 0) ? 3 : 1;
            else
                ModeSelector0xc = (Flag29 != 0) ? 3 : 0;
            return ModeSelector0xc;
        }
        BehaviorMgrPtr20->SetDirectiveResult(0xe, 1);               // the routine
    }
    ModeSelector0xc = (Flag29 != 0) ? 3 : 2;
    return ModeSelector0xc;
}

// Slot 10, the branch (module storage-062cfe15, RET 4). The argument is a
// pub::AI::DirectiveGuideOp; the FollowParams fields it reads (+0x8c/+0x90..+0x98) and the
// GotoParams fields (+0x68/+0x6c/+0x70) are taken by raw offset off the op.
bool GuideBehavior::Slot10(void* DirectiveOp)
{
    pub::AI::DirectiveGuideOp const* const Op = static_cast<pub::AI::DirectiveGuideOp const*>(DirectiveOp);
    unsigned char const* const OpBytes = static_cast<unsigned char const*>(DirectiveOp);
    CachedPtr60 = BehaviorMgrPtr20->BehaviorDispatchTable[2];
    CachedPtr64 = BehaviorMgrPtr20->BehaviorDispatchTable[17];
    // REP MOVSD of 0x1c and 9 dwords -- whole-object copies, vptr included.
    std::memcpy(static_cast<void*>(&GotoOp), &Op->GotoParams, sizeof(GotoOp));
    std::memcpy(static_cast<void*>(&FollowOp), &Op->FollowParams, sizeof(FollowOp));
    bool bResult = true;
    // WatcherB = the object GotoOp+0x1c names (the routine).
    IObjRW* const GotoTarget = NavHelperPtr24->Timers.FindVerifiedObject(
        (unsigned const*)((unsigned char*)this + 0x84));
    WatcherB.set(GotoTarget != nullptr ? static_cast<Watchable const*>(GotoTarget) : nullptr);
    Field104 = Op->Field_A4;
    AngleThreshold11c = Op->Field_A8;
    Field110 = static_cast<float>(Op->Field_AC);                    // FILD
    FieldHigh118 = Op->Field_B0;
    FieldLow114 = Op->Field_B4;
    if (*reinterpret_cast<int const*>(OpBytes + 0x68) != 0)
    {
        IObjRW* const Escort = NavHelperPtr24->Timers.FindVerifiedObject(
            (unsigned const*)(OpBytes + 0x68));
        WatcherA.set(Escort != nullptr ? static_cast<Watchable const*>(Escort) : nullptr);
        if (ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target())) != nullptr)
        {
            Field3c = *reinterpret_cast<float const*>(OpBytes + 0x6c);
            Field40 = *reinterpret_cast<unsigned const*>(OpBytes + 0x70);
            // |op+0x90..+0x98| on the x87 stack ((x*x + y*y) + z*z); not
            // greater than 0.0f (TEST AH,0x41 JNZ) -> +0x44 = op+0x8c, else len + 100.0f
            // (the field), one rounding.
            float const* const V = reinterpret_cast<float const*>(OpBytes + 0x90);
            double const Len = std::sqrt((static_cast<double>(V[0]) * V[0]
                                          + static_cast<double>(V[1]) * V[1])
                                         + static_cast<double>(V[2]) * V[2]);
            if (Len > 0.0)
                Field44 = static_cast<float>(Len + 100.0);
            else
                Field44 = *reinterpret_cast<float const*>(OpBytes + 0x8c);
            if (!CachedPtr60->Slot10(&GotoOp))
                bResult = false;
            if (!CachedPtr64->Slot10(&FollowOp))
                bResult = false;
        }
        else
        {
            bResult = false;
        }
    }
    // .
    Field34 = Op->Field_B8;
    Flag38 = Op->Field_A0;
    Flags28 = static_cast<unsigned char>((Flags28 & ~0x2u) | (bResult ? 0x2u : 0u));
    return bResult;
}

// Slot 21, the branch (module storage-062cf339, RET 4).
void GuideBehavior::Slot21(void* Arg)
{
    unsigned int const LeftId = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Arg));
    IObjRW* const Lead = ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target()));
    if (Lead == nullptr)
        return;
    if (Lead->get_id() == LeftId)                                   // slot 0x20
        WatcherA.set(nullptr);
}

// Slot 24, the branch: `MOV EAX,[ECX+0x2c]; TEST; JZ; ADD EAX,-8`.
IObjRW const* GuideBehavior::get_target()
{
    return ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target()));
}


// ----------------------------------------------------------------------------
// MOVED HERE 2026-09-17 (claude:w16-behavior): Update() (the branch) and
// NotifyManagerGuideEvent (the routine) used to live in src/Common/common_stubs.cpp.
// The stated reason -- "Update() calls IsIdAmongOwnerFormationFollowers, which is
// file-static there" -- was the only thing holding them; the routine's definition is
// now non-static and lived in common_behavior.cpp (declared in common_behavior.h; since CMN-GC an
// FGunConvergenceStateBlock member in common_gunconvergence.cpp), so both
// bodies sit in their own class's translation unit. Every other helper Update() calls
// (GetIfacePositionOrZero, GetInterceptClearanceOfIface, GetSignedRadialClearanceToPoint,
// Distance3, VecNormalize3, ClampedDot3, ObjInspectOf, ObjRwFromWatchable) was already
// header-declared and needed no change.

// Still open on this class: own vtable slots 3 and 10 (the branch Engage(),
// ApplyDirective(pub::AI::DirectiveGuideOp const&)) are NOT ported yet. Their
// former shared excuse -- "too many still-unidentified CachedPtr60/64 sub-object classes to
// safely vcall" -- is dead as of 2026-09-17: CachedPtr60/64 are sibling `Behavior*` objects
// out of IBehaviorManager::BehaviorDispatchTable (entries 2 and 18), see the field comment
// in common_guidebehavior.h. What remains genuinely unported for those two slots is
// (ApplyDirective's hardpoint-lookup-and-BaseWatcher-set helper) and the
// /the routine pair Engage() uses. TODO: both slots are ordinary next-pass
// work now, not blocked.
// ----------------------------------------------------------------------------

// ===========================================================================
// GuideBehavior -- vtable slot 7 Update() (the branch, implementation tracker item 1461) and its
// private notification path NotifyManagerGuideEvent (the routine, item 1551).

// PORTED IN FULL 2026-09-17 (claude:common-guide) from Common
// 206092-206730 and 211770-211808. The class, its layout and the per-step reasoning live in
// src/include/common_guidebehavior.h; this file holds the bodies.

// MOVED HERE from src/Common/common_stubs.cpp on 2026-09-17 (claude:w16-behavior). The
// original placement note read "this file holds only the bodies, because Update() calls
// IsIdAmongOwnerFormationFollowers above, which is file-static here" -- that was the whole
// reason, and it is gone: the routine is now non-static with a declaration in
// common_guidebehavior.h (see that header's trailing TODO -- it should end up in
// common_behavior.h). Nothing else in these three bodies was file-local to common_stubs.cpp.

// SPOT-RE-VERIFIED against the raw listing while moving (the transcription was NOT taken on
// trust -- that is where this marathon's mis-assignments hide). The x87 predicates, which are
// the recurring defect class, are transcribed CORRECTLY here, and unusually the original pass
// named the mask AND the jump at each site rather than the predicate alone, which is what made
// them checkable. Confirmed byte-for-byte at 062cf72d-062cf74e:
//   FLD float ptr [ESP+0x34]   ; the the routine/Distance3 result
//   FCOMP float ptr [ESP+0x30] ; the the routine/GetSignedRadialClearanceToPoint result
//                              ; (pre-push [ESP+0x28], shifted by the two PUSHes at 062cf722)
//   FNSTSW AX; TEST AH,0x5; JP the branch   ; skips `MOV byte ptr [ESP+0x12],1`
// TEST AH,0x5 masks C0|C2, so PF is set for greater (0x00), equal (0x00) and unordered (0x05),
// and clear only for less (0x01) -- JP therefore jumps on NOT-less and the flag is set exactly
// when Distance3 < RadialClearance, which is what `bInsideRadial` below says. The
// TEST AH,0x41 + JP sites (062cfad2, and both tail tests) likewise jump on strictly-greater
// or unordered, since only 0x00 and 0x41 have even parity -- also transcribed correctly.

// The item's "GAP:AI-GUIDE-UPDATE -- not ported, too many unidentified helpers" marker was
// STALE, as this marathon's own standing lesson predicts. Every helper it named resolves
// today: the routine GetIfacePositionOrZero, the routine GetInterceptClearanceOfIface,
// GetSignedRadialClearanceToPoint, the routine Distance3, the routine
// VecNormalize3, the routine ClampedDot3, the routine IsIdAmongOwnerFormationFollowers,
// IBehaviorManager::SetDirectiveResult. The two "raw block vtbl" calls are
// IObjInspect slot 82 get_behavior_id (+0x148) and slot 44 get_formation_leader (+0xb0), and
// the "unidentified CachedPtr60/64 sub-object classes" are sibling `Behavior*` out of
// IBehaviorManager::BehaviorDispatchTable.
// ===========================================================================

// (item 1551). All four XREFs are in Update() below; the receiver is the
// IBehaviorManager (062cf9ae / 062cfa04 / 062cfb1a / 062cfb9d load [ESI+0x20] into ECX).
bool GuideBehavior::NotifyManagerGuideEvent(IBehaviorManager* BehaviorMgr, unsigned SelfId, unsigned WatchedId,
                                           int EventCode, float Clearance, Vector const* Point)
{
    // Sys (+0x8) Targeting.AttackState (+0x1434) nonzero and behavior 0xe,
    // Guide's own index in the dispatch table.
    if (BehaviorMgr->Sys->Targeting.AttackState == 0 || BehaviorMgr->CurrentBehaviorIndex != 0xe)
        return false;

    // ContentCbPtr (+0x110) slot 2 with the five arguments re-pushed in order.
    if (BehaviorMgr->ContentCbPtr != nullptr)
    {
        BehaviorMgr->ContentCbPtr->lead_object_event(
            SelfId, WatchedId, (pub::AI::ContentCallback::LeadProgressType)EventCode, Clearance, *Point);
        return true;
    }

    // EAX still holds Sys -- its ExternalIface->is_player() (+0xbc) for
    // effect, then false (the NotifyFollowEvent / NotifyTetherEvent fallback).
    (void)BehaviorMgr->Sys->SelfTarget.Iface->is_player();
    return false;
}

// Shared body of Update()'s eight guide-event sites (062cf90b-062cf913, 062cf96d-062cf975,
// , 062cf9f5-062cfa09, 062cfaa7-062cfaaf, 062cfb0b-062cfb1f,
// , 062cfb8e-062cfba2). All eight push the same five arguments in the same
// order; only the event code differs.
void GuideBehavior::FireGuideEvent(IObjRW* Watched, FAiTarget* NavElem, int EventCode,
                                   float Clearance)
{
    unsigned const SelfId = NavElem->Iface->get_id();   // baseline: parked in EBX first
    unsigned const WatchedId = ObjInspectOf(Watched)->get_id();
    NotifyManagerGuideEvent(BehaviorMgrPtr20, SelfId, WatchedId, EventCode, Clearance, &Field48);
}

int GuideBehavior::Update()
{
    // ---- 062cf5f0-062cf62e: entry guards -------------------------------------------------
    // baseline tests `[this+0x2c] == 0` and then `[this+0x2c] - 8 == 0` separately; both
    // collapse into one null test on the adjusted pointer, because ObjRwFromWatchable maps
    // null -> null and the literal sentinel 8 -> null identically.
    IObjRW* const ObjA = ObjRwFromWatchable(const_cast<Watchable*>(WatcherA.target()));
    if (ObjA == nullptr)
    {
        // only THIS exit clears the flag bit; the two below deliberately do not.
        Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
        UpdateResult10 = 3;
        return 3;
    }
    if (OwnerContextPtr14 == nullptr)
    {
        UpdateResult10 = 3;
        return 3;
    }
    // re-reads +0x2c and re-runs both tests. Nothing between can change it,
    // so the port evaluates ObjA once.

    FAiTarget* const NavElem = &NavHelperPtr24->SelfTarget;

    // ---- 062cf630-062cf6bd: refresh the guide destination point --------------------------
    // baseline reads the mode at raw offset +0x74, which is GotoOp + 0x0C.
    int const GotoMode = GotoOp.Field_0C;
    if (GotoMode == 0)
    {
        IObjRW* const ObjB = ObjRwFromWatchable(const_cast<Watchable*>(WatcherB.target()));
        if (ObjB == nullptr)
        {
            // .
            Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
            BehaviorMgrPtr20->SetDirectiveResult(0xe, 1);
            UpdateResult10 = 2;
            return 2;
        }
        // is `TEST ECX,ECX; JNZ` on the mode dword that 062cf635 already proved to be
        // zero -- dead code in baseline, reproduced as nothing rather than as an impossible branch.
        Vector WatchedBPos;
        GetIfacePositionOrZero(&WatchedBPos, ObjB);
        Field48 = WatchedBPos;                      // , a 3-dword copy
    }
    else if (GotoMode == 1)
    {
        // raw +0x78 == GotoOp + 0x10 == GotoOp.Destination.
        Field48 = GotoOp.Destination;
    }
    // any other mode: Field48 keeps last tick's value (baseline falls straight through).

    // raw +0xb8 == GotoOp + 0x50.
    Field54 = GotoOp.Field_50;

    // ---- 062cf6c9-062cf729: this tick's three steering measurements ----------------------
    Vector WatchedAPos;
    GetIfacePositionOrZero(&WatchedAPos, ObjA);
    // / 062cf71a: both ST0 returns are rounded by `FSTP float`.
    float const Clearance = static_cast<float>(GetInterceptClearanceOfIface(NavElem, ObjA));
    float const RadialClearance = static_cast<float>(GetSignedRadialClearanceToPoint(NavElem, Field48));
    float const Dist = (float)Distance3(&WatchedAPos, &Field48);                 // , FSTP float at 062cf729

    // . `TEST AH,0x5` with `JP` jumps on NOT-less, so the fall-through that
    // sets the byte is strictly-less. baseline keeps both of these in stack bytes [ESP+0x12] and
    // [ESP+0x13]; they gate the event table further down.
    bool const bInsideRadial = (Dist < RadialClearance);
    bool bLeaderIsSelf = false;

    // ---- 062cf74e-062cf7c2: "is the escorted object formed up on me?" --------------------
    if (ObjInspectOf(ObjA)->get_behavior_id() == 0xa) // vtbl +0x148 == IObjInspect slot 82
    {
        IObjRW* Leader = nullptr;                    // baseline zeroes [ESP+0x18] before the call
        ObjInspectOf(ObjA)->get_formation_leader(Leader); // vtbl +0xb0 == slot 44; result unused
        if (Leader != nullptr)
        {
            unsigned const NavSelfId = NavElem->Iface->get_id();
            if (ObjInspectOf(Leader)->get_id() == NavSelfId)
            {
                Flag120 = 0;
                bLeaderIsSelf = true;
            }
        }
    }

    // ---- 062cf7c4-062cf7ef: ... or listed among my own formation followers ---------------
    if (NavHelperPtr24->GunConvergenceStateBytes.IsIdAmongOwnerFormationFollowers(
            ObjInspectOf(ObjA)->get_id()))
    {
        Flag120 = 0;
    }

    // ---- 062cf7f1-062cf8b2: sample the escort's progress, at most once per Field104 -------
    // `TEST AH,0x41` + `JNZ` guards a strictly-greater test: baseline falls through to the
    // sampling block when Field108 > Field104 and jumps to the accumulate arm otherwise.
    bool bSampled = false;
    if (Field108 > Field104)
    {
        // .
        Field58 = Field10c - Dist;
        Field108 = 0.0f; // baseline stores an integer zero, which is 0.0f's bit pattern

        // the delta from the escorted ship's own position to the guide point.
        Vector Delta;
        Delta.x = Field48.x - WatchedAPos.x;
        Delta.y = Field48.y - WatchedAPos.y;
        Delta.z = Field48.z - WatchedAPos.z;

        // vtbl +0xc == IObjInspect slot 3 get_transform; Transform::Rotation
        // sits at +0x00, so baseline's [EAX+0x08]/[EAX+0x14]/[EAX+0x20] are E[0][2]/E[1][2]/E[2][2],
        // i.e. the matrix's third column, negated. (Same reading common_gotobehavior.cpp already uses
        // for GotoBehavior::Update's own facing extraction.)
        Matrix const& Rotation = ObjInspectOf(ObjA)->get_transform().Rotation;
        Vector NegForward;
        NegForward.x = -Rotation.E[0][2];
        NegForward.y = -Rotation.E[1][2];
        NegForward.z = -Rotation.E[2][2];

        // . the routine's own prologue reads param_2 as the source and writes
        // param_1, and returns param_1 -- so baseline's (E-0xc, &Delta) is VecNormalize3's
        // (Out, Src), which this port spells (Src, Out).
        Vector NormDelta;
        VecNormalize3(&Delta, &NormDelta);
        Field5c = std::acos(ClampedDot3(NegForward, NormDelta));          // + _CIacos

        Field10c = Dist;
        bSampled = true;
    }
    else
    {
        // .
        Field108 = Field108 + DeltaTime1c;
    }

    bool bSkipRadialTermination = false;

    if (Flag120 != 0)
    {
        // ---- 062cf8c2-062cfa13: event table for the "escort is following me" state -------
        if (bSampled)
        {
            if (Field58 < FieldLow114) // , `TEST AH,5` + `JP` == not-less
            {
                if (!bInsideRadial)
                    FireGuideEvent(ObjA, NavElem, 8, Clearance);
                Flag120 = 0; // , reached whether or not the event fired
            }
            else if (Field58 > FieldHigh118)
            {
                if (Field5c > AngleThreshold11c)
                {
                    if (!bInsideRadial)
                        FireGuideEvent(ObjA, NavElem, 6, Clearance);
                    Flag120 = 0;
                }
                else
                {
                    // . The ONE arm that neither checks bInsideRadial nor
                    // clears Flag120 -- verified against the raw listing, not a transcription
                    // slip: it jumps straight to the branch, over the branch.
                    FireGuideEvent(ObjA, NavElem, 5, Clearance);
                }
            }
            else if (Clearance < Field44)
            {
                if (!bInsideRadial)
                    FireGuideEvent(ObjA, NavElem, 7, Clearance);
                Flag120 = 0;
            }
        }

        // ---- 062cfa15-062cfa51: hand the flying to the right sibling ---------------------
        if (Flag120 != 0)
        {
            // Still following: keep ticking CachedPtr64. baseline discards the slot-7 result here
            // (unlike the mirror-image branch below, which tests it against 2).
            *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(CachedPtr64) + 0x1c) = DeltaTime1c;
            CachedPtr64->Slot7();
        }
        else
        {
            // Transition to leading: park CachedPtr64, push GotoOp into CachedPtr60, engage it.
            CachedPtr64->Slot4();
            CachedPtr60->Slot10(&GotoOp);
            CachedPtr60->Slot3();
        }
    }
    else
    {
        // ---- 062cfa56-062cfba5: event table for the "escort is not following me" state ---
        if (bSampled)
        {
            if (bLeaderIsSelf) // -> the branch
            {
                FireGuideEvent(ObjA, NavElem, 4, Clearance);
            }
            else if (Field58 < FieldLow114 && !bInsideRadial)
            {
                FireGuideEvent(ObjA, NavElem, 3, Clearance);
            }
            else
            {
                // interleaves two compares through one AH: the first FNSTSW
                // captures (Field58 vs FieldHigh118) and feeds `TEST AH,0x41`/`JNZ`; the second
                // FNSTSW (which does not disturb EFLAGS) captures (Clearance vs Field3c) and
                // feeds the following `TEST AH,0x5`/`JP` and, at 062cfb3d, a `TEST AH,0x1`
                // reading that same compare's C0 bit.
                bool const bFarAhead = (Field58 > FieldHigh118);
                bool const bInsideField3c = (Clearance < Field3c);          // TEST AH,0x5: C0 alone
                bool const bField3cC0 = !(Clearance >= Field3c);            // TEST AH,0x1: less OR unordered

                if (bFarAhead)
                {
                    if (bInsideField3c)
                    {
                        // `TEST AH,0x41` + `JP` JUMPS on greater or
                        // unordered, so the event fires only when Field5c <= AngleThreshold11c.
                        if (Field5c <= AngleThreshold11c)
                        {
                            FireGuideEvent(ObjA, NavElem, 1, Clearance);
                            Flag120 = 1;
                        }
                    }
                    else if (bInsideRadial) // -> the branch
                    {
                        FireGuideEvent(ObjA, NavElem, 4, Clearance);
                    }
                }
                else if (!bField3cC0 && !bInsideRadial) // , C0 also set on unordered
                {
                    FireGuideEvent(ObjA, NavElem, 2, Clearance);
                    Flag120 = 1; // , shared with event 1 via the branch
                }
            }
        }

        // ---- 062cfba7-062cfc0e: hand the flying to the right sibling ---------------------
        if (Flag120 != 0)
        {
            // Transition to following: park CachedPtr60, push FollowOp into CachedPtr64, engage it.
            CachedPtr60->Slot4();
            CachedPtr64->Slot10(&FollowOp);
            CachedPtr64->Slot3();
        }
        else
        {
            // Still leading: tick CachedPtr60. A result of 2 skips the radial termination test.
            *reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(CachedPtr60) + 0x1c) = DeltaTime1c;
            if (CachedPtr60->Slot7() == 2)
                bSkipRadialTermination = true;
        }
    }

    // ---- 062cfbc7-062cfc66: is the directive still running? -------------------------------
    // Both `TEST AH,0x41` + `JP` tests below JUMP on strictly-greater (and on unordered) --
    // that is the opposite jump from the `JNZ` form used earlier in this function, and getting
    // the two confused is exactly the bug class this module has been burned by before.
    if (!bSkipRadialTermination)
    {
        // FCOMP of the unrounded ST0 return; `!(a <= b)` keeps the
        // unordered case on the jump side, as JP does.
        if (!(GetSignedRadialClearanceToPoint(NavElem, Field48) <= Field54))
        {
            UpdateResult10 = 1;
            return 1;
        }
    }
    // raw +0xc8 == GotoOp + 0x60.
    // FCOMP of the unrounded ST0 return; JP is taken on greater OR
    // unordered, hence `!(a <= b)`.
    if (!(GetInterceptClearanceOfIface(NavElem, ObjA) <= GotoOp.Field_60))
    {
        UpdateResult10 = 1;
        return 1;
    }

    // .
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    BehaviorMgrPtr20->SetDirectiveResult(0xe, 0);
    UpdateResult10 = 2;
    return 2;
}
