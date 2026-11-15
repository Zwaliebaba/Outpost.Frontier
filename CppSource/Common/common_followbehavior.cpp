// Author: Giperion.

#include "common_followbehavior.h"
#include "common_iface.h" // IObjInspect / IObjAffect / ObjRwFromWatchable
#include "common_aisteer.h" // GetIfacePositionOrZero, IsWithinInterceptRangeOfIface,
                             // GetInterceptClearanceOfIface, GetIfaceVectorMagnitude,
                             // GetObjectTypeOrZero, SetFollowLeader
#include "common_systemmanager.h" // SystemManager::GunConvergenceStateBytes (NavHelper+0x1364)
#include "common_csys.h"  // Matrix, Transform, MatrixSetIdentity, Acquire3DMathEngine
#include "x86math.h"      // I3DMathEngine::TransformMatrix (vtable +0x2c), FVector, FMatrix

#include <cmath>

// (item 1798): the checked id lookup through the IObjDB at
// SystemManager::Timers+0x00, SystemManagerTimerTable::FindVerifiedObject.

// (module storage-062ccb62, RET 4).
FollowBehavior::FollowBehavior(int InTypeId)
    : BaseFollowerBehavior(InTypeId)
    , SpeedParam2a0(150.0f)          // 0x43160000
    , Enabled2a4(1)
    , Pad2a5{}
    , Distance2a8(0.0f)              // not written by baseline; the port zeroes it
    , Field2ac(150.0f)
    , Field2b0(2.0f)
    , Field2b4(400.0f)
    , Field2b8(100.0f)
{
    Flag281 = 0;
    Flag280 = 0;
    Offset290.x = 0.0f;
    Offset290.y = 0.0f;
    Offset290.z = 0.0f;
    Flag285 = 0;
}

// Slot 3, the routine (module storage-062cccdd).
void FollowBehavior::Slot3()
{
    BaseFollowerBehavior::Slot3();                                  // the routine
    IObjRW* const Lead = ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target()));
    if (Lead == nullptr)
        return;
    // IObjAffect slot 11 (+0x2c) on Lead+4, add_follow_follower(Owner).
    (void)ObjAffectOf(Lead)->add_follow_follower(static_cast<IObjRW*>(OwnerContext));
    // the watcher re-read, then the routine on Sys+0x1364.
    NavHelper->GunConvergenceStateBytes.SetFollowLeader(
        ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target())));
}

// Slot 4, the routine (module storage-062ccd21).
void FollowBehavior::Slot4()
{
    BaseFollowerBehavior::Slot4();                                  // the routine
    IObjRW* const Lead = ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target()));
    if (Lead == nullptr)
        return;
    // IObjAffect slot 12 (+0x30), remove_follow_follower(Owner).
    (void)ObjAffectOf(Lead)->remove_follow_follower(static_cast<IObjRW*>(OwnerContext));
    NavHelper->GunConvergenceStateBytes.SetFollowLeader(nullptr);   // the routine(0)
}

// Slot 5, the routine (module storage-062ccc07, RET 4, argument unread).
int FollowBehavior::Slot5(float Value)
{
    (void)Value;
    IObjRW* const Lead = ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target()));
    if ((Flags28 & 0x2) == 0 || Lead == nullptr)
    {
        ModeSelector0xc = (Flag29 != 0) ? 3 : 2;
        return ModeSelector0xc;
    }
    // the routine(Sys+0x14fc, Lead, Field2b4).
    if (IsWithinInterceptRangeOfIface(&NavHelper->SelfTarget, Lead, Field2b4))
        ModeSelector0xc = (Flag29 != 0) ? 3 : 1;
    else
        ModeSelector0xc = (Flag29 != 0) ? 3 : 0;
    return ModeSelector0xc;
}

// Slot 6, the branch (module storage-062ccc72, RET 4, argument unread).
EvaluateResult FollowBehavior::evaluate(unsigned int Context)
{
    (void)Context;
    IObjRW* Target = nullptr;
    (void)ObjInspectOf(OwnerContext)->get_target(Target);           // slot 0x74
    if (Target == nullptr)
        return EvalSkip;
    bool bInLane = false;
    (void)ObjInspectOf(OwnerContext)->is_using_tradelane(&bInLane); // slot 0x134
    if (bInLane)
        return EvalSkip;
    // the routine: any ship-class bit -> 1.
    if ((GetObjectTypeOrZero(ObjInspectOf(Target)) & 0x7f0000u) != 0)
        return EvalBlocked;                                         // EAX = 1
    return EvalSkip;
}

// Slot 10, the routine (module storage-062cd4b9, RET 4).
bool FollowBehavior::Slot10(void* Param)
{
    if (Param != nullptr)
    {
        unsigned char* P = static_cast<unsigned char*>(Param);

        // `the routine(NavHelper+0x34, Param+0xc)` -- the object whose id
        // Param+0xc holds, looked up through the IObjDB SystemManager::Timers keeps at +0x00, and
        // only if the found object reports that same id. The watcher then points at its
        // Watchable (`Candidate ? Candidate + 8 : 0`), set_pointer only when that differs.
        // Until 2026-09-24 the lookup was a hardcoded null ("item 1798 is unprocessed" -- it
        // has been ported since 2026-09-1x, now SystemManagerTimerTable::FindVerifiedObject in common_sysmgr_timertable.cpp), and the null was passed to
        // set() without the +8.
        IObjRW* const Candidate = NavHelper->Timers.FindVerifiedObject((unsigned const*)(P + 0xc));
        LeadWatcher.set(static_cast<Watchable*>(Candidate));

        if (ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target())) != nullptr)
        {
            float const* SrcVec = reinterpret_cast<float const*>(P + 0x14);
            Offset290.x = SrcVec[0];
            Offset290.y = SrcVec[1];
            Offset290.z = SrcVec[2];
            // (x*x + y*y) + z*z and FSQRT on the x87 stack, rounded to
            // float once by the FSTP.
            Distance2a8 = static_cast<float>(std::sqrt(
                (static_cast<double>(Offset290.x) * Offset290.x
                 + static_cast<double>(Offset290.y) * Offset290.y)
                + static_cast<double>(Offset290.z) * Offset290.z));
            SpeedParam2a0 = *reinterpret_cast<float const*>(P + 0x10);

            // IObjAffect slot 10 (+0x28) on Owner+4, set_follow_offset, no
            // null test.
            (void)ObjAffectOf(OwnerContext)->set_follow_offset(Offset290);

            // the watcher is re-read, then the routine on NavHelper+0x1364
            // with `Watched ? Watched - 8 : 0` (SetFollowLeader, common_gunconvergence.cpp).
            NavHelper->GunConvergenceStateBytes.SetFollowLeader(
                ObjRwFromWatchable(const_cast<Watchable*>(LeadWatcher.target())));

            Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
            return true;
        }

        Field2b4 = *reinterpret_cast<float const*>(P + 0x20);
    }

    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    return false;
}

// ============================================================================
// The two virtuals BaseFollowerBehavior adds to Behavior's 27 (vtable offsets 0x6c and 0x70).
// Ported 2026-09-17 (claude:common-behavior-layout) -- see common_behavior.h for how both
// signatures were recovered and common_followbehavior.h for each slot's own summary.
// ============================================================================

namespace
{

// The `LeadWatcher.target() - 8` downcast every one of these bodies opens with. baseline's own
// form is `MOV EAX,[this+0x2c]; TEST EAX,EAX; JZ zero; ADD EAX,-0x8` -- note it does NOT
// special-case the 0x8 "watching nothing" sentinel here, because 8 - 8 is 0 anyway.
IObjRW* LeaderObjectOf(BaseWatcher const& Watcher)
{
    return ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Watcher.target())));
}

// Commoncd236-062cd251 and 062cd2cc-062cd2f3 -- the SAME lazy per-frame refill,
// inlined twice into Slot27: when Flags1578[2] (NavHelper+0x14fc+0x7e) is clear, set it FIRST
// (062cd24a / 062cd2ec) and then ExternalIface (+0x14fc+0x84) slot 0x2c,
// IObjInspect::get_physical_radius(+0x14fc+0x18 Radius, +0x14fc+0xc Center). No null tests
// in baseline.
void RefreshNavPhysicalBounds(SystemManager* Nav)
{
    if (Nav->SelfTarget.bPosValid != 0)
        return;
    Nav->SelfTarget.bPosValid = 1;
    (void)Nav->SelfTarget.Iface->get_physical_radius(Nav->SelfTarget.CachedRadius,
                                                                Nav->SelfTarget.CachedPos);
}

} // namespace

// Commonccf83-062cd043, inlined a second time verbatim at 062cd0a3-062cd164 (the
// first copy opens Slot27's watch block, the second its point block). Toggle Enabled2a4 when
// the leader crosses the follow range and report the transition to the manager. baseline re-reads
// LeadWatcher (`MOV EAX,[EBP+0x2c]` + the `- 8`) for every use, and so does this.
void FollowBehavior::UpdateInterceptEngageState()
{
    // the routine (IsWithinInterceptRangeOfIface) with the float
    // Field2b8 + SpeedParam2a0 (`FLD; FADD; FSTP [ESP]` -- one float rounding).
    bool const bInRange = IsWithinInterceptRangeOfIface(
        &NavHelper->SelfTarget, LeaderObjectOf(LeadWatcher),
        Field2b8 + SpeedParam2a0);

    if (bInRange)
    {
        // already engaged -> nothing.
        if (Enabled2a4 != 0)
            return;
        Enabled2a4 = 1;
        IObjRW* const Leader = LeaderObjectOf(LeadWatcher);
        // NavHelper+0x14fc+0x84 == ExternalIface, slot 8 get_id().
        unsigned const OwnId = NavHelper->SelfTarget.Iface->get_id();
        // the float and the status are pushed for the routine BEFORE the
        // leader's own zero-argument slot 8 get_id() runs; they stay on the stack under it.
        float const Range = Field2b8 + SpeedParam2a0;
        unsigned const LeaderId = ObjInspectOf(Leader)->get_id();
        // BehaviorMgrPtr (+0x20), the routine, result discarded.
        (void)BehaviorMgrPtr->NotifyFollowEvent(OwnId, LeaderId,
                                                pub::AI::ContentCallback::FollowStatus1, Range);
        return;
    }

    // already disengaged -> nothing.
    if (Enabled2a4 == 0)
        return;
    Enabled2a4 = 0;
    IObjRW* const Leader = LeaderObjectOf(LeadWatcher);
    unsigned const OwnId = NavHelper->SelfTarget.Iface->get_id();
    unsigned const LeaderId = ObjInspectOf(Leader)->get_id();
    // raw SpeedParam2a0 dword and status 2.
    (void)BehaviorMgrPtr->NotifyFollowEvent(OwnId, LeaderId,
                                            pub::AI::ContentCallback::FollowStatus2,
                                            SpeedParam2a0);
}

// Own vtable slot 27 (offset 0x6c) -- the branch, item 1407, Common.
void FollowBehavior::Slot27()
{
    // Frame: S = ESP after `SUB ESP,0x9c`. Locals: S+0x0 Rotated (later the forward column and
    // then the aim point), S+0xf the cruise bool, S+0x10 Speed, S+0x14 TargetPoint, S+0x20
    // Delta / products, S+0x2c Weight, S+0x30 LeaderPos, S+0x3c LeaderBasis (3x3), S+0x60
    // Probe (Transform), S+0x90 Projected.

    // `[+0x2c]` zero, or zero after the `- 8`, returns at once.
    if (LeaderObjectOf(LeadWatcher) == nullptr)
        return;

    // OwnerContext (+0x14) slot 24 (+0x60), IObjInspect::is_cruise_active
    // (bool&), on a stack byte zeroed first. Nothing reads the byte afterwards; the call is
    // issued for whatever it does, and its int result is dropped.
    bool bCruiseActive = false;
    (void)ObjInspectOf(OwnerContext)->is_cruise_active(bCruiseActive);

    // the routine (Behavior_InvokeNodeSlotC == the leader's
    // get_transform()) and `MOVSD.REP` of nine dwords -- the 3x3 only -- into S+0x3c.
    // Speed = SpeedParam2a0 (raw dword).
    Matrix LeaderBasis = Behavior_InvokeNodeSlotC(LeaderObjectOf(LeadWatcher))->Rotation;
    float Speed = SpeedParam2a0;

    // the routine (GetIfacePositionOrZero) into S+0x30.
    Vector LeaderPos;
    GetIfacePositionOrZero(&LeaderPos, LeaderObjectOf(LeadWatcher));

    // the routine (identity) on S+0x60 and three zero dwords at S+0x84.
    Transform Probe;
    MatrixSetIdentity(Probe.Rotation);
    Probe.Translation.x = 0.0f;
    Probe.Translation.y = 0.0f;
    Probe.Translation.z = 0.0f;

    // `FLD c; FLD ST0; FMUL ST1; FCOMP 0.0f (the field); TEST AH,0x41;
    // JP the branch` per component, x then y then z. JP is taken on greater or unordered, so
    // the offset counts as present unless every square compares <= 0. The squares live on the
    // x87 stack (double here) -- a float square of a tiny component would underflow to 0.
    double const OffX = Offset290.x;
    double const OffY = Offset290.y;
    double const OffZ = Offset290.z;
    bool const bHasOffset = !(OffX * OffX <= 0.0) || !(OffY * OffY <= 0.0) || !(OffZ * OffZ <= 0.0);

    Vector TargetPoint;
    bool bWatchBlock = false; // AL at the branch

    // only with a zero offset -- the leader's slot 79 (+0x13c),
    // IObjInspect::generate_follow_offset(OwnerContext, Probe).
    bool const bGenerated =
        !bHasOffset &&
        ObjInspectOf(LeaderObjectOf(LeadWatcher))
            ->generate_follow_offset(ObjInspectOf(OwnerContext), Probe);

    if (bGenerated)
    {
        // Probe's translation -> TargetPoint, Probe's 3x3 -> LeaderBasis,
        // Speed = 50.0f (0x42480000); `XOR AL,AL` at the branch.
        TargetPoint = Probe.Translation;
        LeaderBasis = Probe.Rotation;
        Speed = 50.0f;
        bWatchBlock = false;
    }
    else
    {
        // (zero offset, AL = 1) and 062ccefd-062ccf64 (offset present,
        // `XOR AL,AL`) are the same code: the field / the routine 3DMathEngine, slot 0x2c
        // TransformMatrix(S+0x0, LeaderBasis, Offset290), then the routine (a three-float
        // Vector store) of Rotated + LeaderPos per component (`FLD; FADD; FSTP` -- one float
        // rounding each), copied to TargetPoint.
        Vector Rotated;
        Acquire3DMathEngine()->TransformMatrix(reinterpret_cast<FVector&>(Rotated),
                                               reinterpret_cast<FMatrix const&>(LeaderBasis),
                                               reinterpret_cast<FVector const&>(Offset290));
        TargetPoint.x = Rotated.x + LeaderPos.x;
        TargetPoint.y = Rotated.y + LeaderPos.y;
        TargetPoint.z = Rotated.z + LeaderPos.z;
        bWatchBlock = !bHasOffset;
    }

    // BL = 1; cleared only by the weight clamp below.
    bool bUpdateMatchThrottle = true;

    // / 062cd098-062cd164: the engage toggle opens both blocks.
    UpdateInterceptEngageState();

    if (bWatchBlock)
    {
        // the routine on MatchThrottle (+0x34) with (leader, Speed).
        MatchThrottle.SetWatchAndDistance(LeaderObjectOf(LeadWatcher), Speed);
        // the routine on Face (+0x90) with the leader; JMP the branch.
        Face.SetWatchObject(LeaderObjectOf(LeadWatcher));
    }
    else
    {
        // the routine on MatchThrottle with (leader, &TargetPoint).
        MatchThrottle.SetWatchAndPosition(LeaderObjectOf(LeadWatcher), TargetPoint);

        // the routine (GetInterceptClearanceOfIface) compared with
        // Distance2a8 + 100.0f (the field) kept on the x87 stack; `FXCH; FCOMPP` puts the
        // clearance on the left. `TEST AH,0x5; JP the branch` leaves the fall-through only
        // for clearance < Distance2a8 + 100.
        double const Clearance = GetInterceptClearanceOfIface(
            &NavHelper->SelfTarget, LeaderObjectOf(LeadWatcher));
        if (!(Clearance < static_cast<double>(Distance2a8) + 100.0))
        {
            // the routine on Face with &TargetPoint; JMP the branch.
            Face.SetPosition(TargetPoint);
        }
        else
        {
            // the routine (GetIfaceVectorMagnitude), `FST` to S+0x2c, then
            // the unrounded ST0 against 1.0f (the field); `TEST AH,0x5; JP` falls through
            // only on less.
            double const LeaderSpeed = GetIfaceVectorMagnitude(LeaderObjectOf(LeadWatcher));
            float Weight = static_cast<float>(LeaderSpeed);
            if (LeaderSpeed < 1.0)
            {
                // Weight = 1.0f, BL = 0, the routine(0), MatchThrottle's
                // slot 4 (+0x10) Commit, the routine(Flag28c's raw byte).
                Weight = 1.0f;
                bUpdateMatchThrottle = false;
                MatchThrottle.SetFlag39(0);
                MatchThrottle.Commit();
                MatchThrottle.SetFlag39(Flag28c);
            }

            // column 2 of LeaderBasis (S+0x44 / S+0x50 / S+0x5c) to S+0x0.
            Vector const Forward{ LeaderBasis.E[0][2], LeaderBasis.E[1][2], LeaderBasis.E[2][2] };

            RefreshNavPhysicalBounds(NavHelper);

            // Delta = CachedPhysicalCenter - TargetPoint (`FLD; FSUB; FSTP`
            // per component), the routine (VecCosineProject3)(S+0x90, &Delta, &Forward).
            Vector const Delta{ NavHelper->SelfTarget.CachedPos.x - TargetPoint.x,
                                NavHelper->SelfTarget.CachedPos.y - TargetPoint.y,
                                NavHelper->SelfTarget.CachedPos.z - TargetPoint.z };
            Vector Projected;
            Vector const* const ProjectedOut = VecCosineProject3(&Projected, &Delta, &Forward);

            // |*EAX| as (x*x + y*y) + z*z and FSQRT on the x87 stack, one
            // `FSTP` to Speed (S+0x10). 062cd2b3-062cd2c4: Speed < 0.0f -> 0.
            double const PX = ProjectedOut->x;
            double const PY = ProjectedOut->y;
            double const PZ = ProjectedOut->z;
            Speed = static_cast<float>(std::sqrt((PX * PX + PY * PY) + PZ * PZ));
            if (Speed < 0.0f)
                Speed = 0.0f;

            RefreshNavPhysicalBounds(NavHelper);

            // , off the x87 stack: Scalar = (Weight * Field2b0 +
            // CachedPhysicalRadius) + Speed stays unrounded. The x and y products (-col2 *
            // Scalar) are stored to float (S+0x20 / S+0x24) before TargetPoint is added; the z
            // product stays on the stack and is added unrounded. the routine on Face.
            double const Scalar =
                (static_cast<double>(Weight) * Field2b0 + NavHelper->SelfTarget.CachedRadius) + Speed;
            float const ProductX = static_cast<float>(-static_cast<double>(Forward.x) * Scalar);
            float const ProductY = static_cast<float>(-static_cast<double>(Forward.y) * Scalar);
            double const ProductZ = -static_cast<double>(Forward.z) * Scalar;
            Vector const Aim{ ProductX + TargetPoint.x,
                              ProductY + TargetPoint.y,
                              static_cast<float>(ProductZ + TargetPoint.z) };
            Face.SetPosition(Aim);
        }
    }

    // (062cd371-062cd37d): MatchThrottle's slot 1 Update(DeltaTime +0x1c) unless
    // BL was cleared; the int result is dropped.
    if (bUpdateMatchThrottle)
        (void)MatchThrottle.Update(DeltaTime);

    // the routine again with the re-read leader; `FSTP ST0` drops it.
    (void)GetInterceptClearanceOfIface(&NavHelper->SelfTarget,
                                       LeaderObjectOf(LeadWatcher));
    // Face's slot 1 Update(DeltaTime).
    (void)Face.Update(DeltaTime);
}

// Own vtable slot 28 (offset 0x70) -- the branch, item 1409, Common.
Vector FollowBehavior::Slot28()
{
    // the 0xc-byte local that 062cd506-062cd521 copies through the hidden
    // return-buffer pointer (`RET 0x4`).
    Vector Out;

    // the routine (Behavior_InvokeNodeSlotC) on the `- 8` leader.
    Transform const* const LeaderXform = Behavior_InvokeNodeSlotC(LeaderObjectOf(LeadWatcher));

    // the field / the routine 3DMathEngine, slot 0x2c
    // TransformMatrix(&Out, LeaderXform (its 3x3 is at offset 0), &Offset290). No null tests.
    Acquire3DMathEngine()->TransformMatrix(reinterpret_cast<FVector&>(Out),
                                           reinterpret_cast<FMatrix const&>(LeaderXform->Rotation),
                                           reinterpret_cast<FVector const&>(Offset290));
    return Out;
}
