// Author: Giperion.

#include "common_strafebehavior.h"

#include "common_csys.h"           // Transform, Matrix, Acquire3DMathEngine
#include "common_iface.h"          // IObjInspect, IObjRW, ObjRwFromWatchable, ObjInspectOf
#include "common_systemmanager.h"  // SystemManager -- what NavHelper actually points at
#include "common_misc.h"           // random_sphere_constrained_2
#include "common_behavior.h"       // Behavior_GetIfaceMaxScalarAttribute and friends
#include "common_aisteer.h"        // GetIfacePositionOrZero, GetObjectTypeOrZero, FAiTarget,
                                   // BearingAngleToIface
#include "common_cobjdb.h"         // IObjDB::GetZoneDb
#include "common_universe.h"       // Universe::ISystem::GetId
#include "common_physys.h"         // PhySys::FindRayCollisions
#include "x86math.h"               // I3DMathEngine::TransformXform
#include "common_aisteer_internal.h" // CmnAiSteer::InlineFastLength

#include <cmath>

// StrafeBehavior -- see common_strafebehavior.h. Ported from Common;
// every method re-traced 2026-10-08 (claude-code:cmn-ai3).

// is FAiTarget::GetFlags (common_aitarget.cpp).

// (item 1798) is SystemManagerTimerTable::FindVerifiedObject.

namespace
{
    // SelectStrafePoint's inline normalize (062c991d-062c995f, 062c9c16-062c9c6a): (z*z + y*y)
    // + x*x, FSQRT, `FDIVR [module storage]` (1.0f), the reciprocal kept on the FPU (double);
    // each component is rounded once by its own FSTP float. No zero-length guard, as baseline.
    void NormalizeInlineX87(Vector& V)
    {
        double const Inv = 1.0 / std::sqrt(((double)V.z * V.z + (double)V.y * V.y) + (double)V.x * V.x);
        V.x = (float)(V.x * Inv);
        V.y = (float)(V.y * Inv);
        V.z = (float)(V.z * Inv);
    }

    // The other normalize shape (062c9a0f-062c9a3b, 062c9a61-062c9a82): the routine leaves
    // sqrt((x*x + y*y) + z*z) unrounded in ST0, then `FDIVR 1.0f` and the same per-component
    // FMUL / FSTP float.
    void NormalizeByVectorLengthX87(Vector& V)
    {
        double const Inv = 1.0 / std::sqrt(((double)V.x * V.x + (double)V.y * V.y) + (double)V.z * V.z);
        V.x = (float)(V.x * Inv);
        V.y = (float)(V.y * Inv);
        V.z = (float)(V.z * Inv);
    }

    // The SystemManager's own watched object (SystemManager::Watcher, +0x08), minus 8, or a
    // null receiver -- baseline dispatches through it either way.
    IObjInspect const* WatchedSelf(SystemManager* Sys)
    {
        return ObjInspectOf(ObjRwFromWatchable(const_cast<Watchable*>(Sys->Watcher.target())));
    }

    // The SystemManager+0x14fc self record (an FAiTarget, see common_aitarget.cpp).
    FAiTarget* SelfRecord(SystemManager* Sys)
    {
        return &Sys->SelfTarget;
    }
}

// ----------------------------------------------------------------------------
// (module storage-062c95bc): the routine(TypeId); MoveIdea +0x70,
// FaceIdea +0xe8, AfterburnerIdea +0x1a8 (EH states 0/1/2 = the member order); vptr
// ; +0x60 = 2; +0x48 = 2. The port zeroes the fields baseline leaves to
// SetContext/Slot3 (+0x2c..+0x6c); every one is written there before a reader runs.
// ----------------------------------------------------------------------------
StrafeBehavior::StrafeBehavior(int InTypeId)
    : Behavior()
    , TypeId(InTypeId)
    , CachedHandle08(0)
    , ModeSelector0xc(0)
    , UpdateResult10(0)
    , OwnerContext(nullptr)
    , Opaque18{}
    , DeltaTime(0.0f)
    , BehaviorMgrPtr(nullptr)
    , NavHelper(nullptr)
    , Flags28(0)
    , Flag29(0)
    , Pad2a{}
    , TriggerRange2c(0.0f)
    , Opaque30{}
    , StrafeDistance34(0.0f)
    , StrafeDir38{}
    , ForwardExtent44(0.0f)
    , StrafeMode48(2)
    , StrafePoint4c{}
    , ScaleNear58(0.0f)
    , ScaleFar5c(0.0f)
    , Field60(2)
    , Field64(0)
    , Pad65{}
    , Field68(0)
    , Field6c(0)
    , SubObjectA()
    , SubObjectB()
    , SubObjectC()
{
    ConstructBase(InTypeId);   // Behavior's own field initialisation
}

// `set_pointer(0)` on +0xfc and +0x84 when set (FaceIdea's and MoveIdea's
// own BaseWatchers -- their member destructors) and the routine (~Behavior).
StrafeBehavior::~StrafeBehavior()
{
}

// ----------------------------------------------------------------------------
// Slot 1, the branch (Common).
// ----------------------------------------------------------------------------
bool StrafeBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    bool const bOk = BindContext(Owner, Manager, Sys);
    if (!bOk)
        return false;
    // `PUSH Manager; PUSH Sys; PUSH Owner; CALL [vtbl+0]` on each.
    SubObjectB.SetContext(Owner, Sys, Manager);
    SubObjectA.SetContext(Owner, Sys, Manager);
    SubObjectC.SetContext(Owner, Sys, Manager);
    Field60 = 2;
    StrafeMode48 = 2;
    Field64 = 0;
    Field6c = 0;
    Field68 = 999;                                                  // (0x3e7)
    return bOk;
}

// ----------------------------------------------------------------------------
// Slot 10, the branch (Common).
// ----------------------------------------------------------------------------
bool StrafeBehavior::Slot10(void* DirectiveOp)
{
    if (DirectiveOp != nullptr)
    {
        unsigned const* const OpTarget = (unsigned const*)(static_cast<char*>(DirectiveOp) + 0xc);
        if (NavHelper->Timers.FindVerifiedObject(OpTarget) != nullptr)
        {
            Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
            return true;
        }
    }
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    return false;
}

// ----------------------------------------------------------------------------
// Slot 5, the branch (Common).
// ----------------------------------------------------------------------------
int StrafeBehavior::Slot5(float Unused)
{
    (void)Unused;
    SystemManager* const Sys = NavHelper;
    int Result = 2;                                                 // EBX
    if (!Sys->GunConvergenceStateBytes.FollowerDirectiveGateA())   // the routine
    {
        IObjRW* const Target = Sys->Targeting.GetTarget();         // the routine
        if (Target != nullptr &&
            (GetObjectTypeOrZero(Target) & 0x70000) == 0)
        {
            // the self record's type flags, refreshed through its
            // +0x7d latch (the routine, FAiTarget::GetFlags).
            if ((SelfRecord(Sys)->GetFlags() & 0x70000) != 0)
            {
                // get_velocity (slot 1) and its squared length
                // ((x*x + y*y) + z*z) FCOMP 400.0f (the field); `TEST AH,0x41; JZ` skips
                // when greater, so <= 400 takes it.
                Vector const Velocity = Target->get_velocity();
                double const SpeedSq =
                    (static_cast<double>(Velocity.x) * Velocity.x + static_cast<double>(Velocity.y) * Velocity.y) +
                    static_cast<double>(Velocity.z) * Velocity.z;
                if (!(SpeedSq > 400.0))
                {
                    Result = 1;
                    Field60 = 1;
                }
            }
        }
    }
    // .
    if (Flag29 != 0)
    {
        ModeSelector0xc = 3;
        return 3;
    }
    ModeSelector0xc = Result;
    return Result;
}

// ----------------------------------------------------------------------------
// Slot 3, the branch (Common).
// ----------------------------------------------------------------------------
void StrafeBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    SetCruiseLock(1, 3);                                            // the routine

    IObjInspect const* const Owner = ObjInspectOf(OwnerContext);
    SystemManager* const Sys = NavHelper;

    // . Step = 0.1f (0x3dcccccd), advanced by 0.1f (the field) after
    // each call; the FCOMP against 10.0 (`TEST AH,0x41; JZ` exits when greater) runs
    // first, then the call's result against pi/2 (`TEST AH,0x41; JNP` loops while less
    // or equal).
    float Step = 0.1f;
    float Angular = 0.0f;
    for (;;)
    {
        Angular = Behavior_GetAngularDistanceTravelled(Owner, 1.0f, 0.0f, 0.0f, Step);
        double const Next = static_cast<double>(Step) + static_cast<double>(0.1f);
        Step = static_cast<float>(Next);                            // FST [ESP+4]
        if (Next > 10.0)
            break;
        if (!(static_cast<double>(Angular) < 1.5707963705062866 ||
              static_cast<double>(Angular) == 1.5707963705062866))
            break;
    }

    // get_speed(1.0f, 1) via the routine, times (Step + 1.0).
    float const Speed = Behavior_InvokeManagerSlotD8(Owner, 1.0f, static_cast<ThrustEquipType>(1));
    TriggerRange2c = static_cast<float>(static_cast<double>(Speed) *
                                        (static_cast<double>(Step) + 1.0));

    // get_archetype_extents(Min, Max) (slot 10) on the watched object;
    // +0x44 = Min.z.
    Vector ExtentMin{ 0.0f, 0.0f, 0.0f };
    Vector ExtentMax{ 0.0f, 0.0f, 0.0f };
    (void)WatchedSelf(Sys)->get_archetype_extents(ExtentMin, ExtentMax);
    ForwardExtent44 = ExtentMin.z;

    // -column 2 of ExternalIface's rotation (slot 3, get_transform).
    Transform const& Xform = Sys->SelfTarget.Iface->get_transform();
    Vector const Column2 = MatrixColumn2(Xform.Rotation);
    Field64 = 0;
    StrafeDir38 = Vector{ -Column2.x, -Column2.y, -Column2.z };

    // Personality.StrafeParams (Sys+0x16c8/+0x16cc/+0x16d0).
    pub::AI::Personality::StrafeUseStruct const& Params = Sys->PersonalityData.StrafeParams;
    StrafeDistance34 = Params.StrafeRunAwayDistance;
    ScaleNear58 = Params.StrafeAttackThrottle;
    ScaleFar5c = Params.StrafeTurnThrottle;
    StrafeMode48 = 2;
}

// ----------------------------------------------------------------------------
// Slot 4, the routine (Common).
// ----------------------------------------------------------------------------
void StrafeBehavior::Slot4()
{
    SubObjectA.PublishTargetId(0);                                  // the routine
    SubObjectA.Commit();                                            // [EAX+0x10]
    SubObjectB.Commit();
    SubObjectC.Commit();
    Behavior::Slot4();                                              // JMP the routine
}

// ----------------------------------------------------------------------------
// Slot 7, the branch (Common), 354 instructions.
// ----------------------------------------------------------------------------
int StrafeBehavior::Slot7()
{
    SystemManager* const Sys = NavHelper;

    // no target -> 3.
    IObjRW* const Target = Sys->Targeting.GetTarget();
    if (Target == nullptr)
    {
        UpdateResult10 = 3;
        return 3;
    }

    SubObjectA.PublishTargetId(Target->get_id());                   // / 062c9d55

    // a ship-class target, a ship-class self and target speed squared
    // <= 400 (`TEST AH,0x41; JNZ` continues on C0 or C3); otherwise 3.
    if ((GetObjectTypeOrZero(Target) & 0x70000) != 0 ||
        (SelfRecord(Sys)->GetFlags() & 0x70000) == 0)
    {
        UpdateResult10 = 3;
        return 3;
    }
    {
        Vector const Velocity = Target->get_velocity();
        double const SpeedSq =
            (static_cast<double>(Velocity.x) * Velocity.x + static_cast<double>(Velocity.y) * Velocity.y) +
            static_cast<double>(Velocity.z) * Velocity.z;
        if (SpeedSq > 400.0)
        {
            UpdateResult10 = 3;
            return 3;
        }
    }

    // .
    FAiTarget* const Self = SelfRecord(Sys);
    int Result = 1;                                                 // [ESP+0x20] = 1
    Self->RefreshPos();                                             // the routine
    Vector const& SelfPos = Self->CachedPos;                        // EBP = record + 0xc

    if (StrafeMode48 == 0)
    {
        // MoveIdea scale by the bearing to the target (pi/8 split,
        // `TEST AH,0x5; JP` takes the far value when >=).
        double const Bearing = BearingAngleToIface(OwnerContext, Target); // unrounded ST0, FCOMP at 062c9f3b
        if (Bearing < 0.39269909262657166)
            SubObjectA.SetScale40(ScaleNear58);
        else
            SubObjectA.SetScale40(ScaleFar5c);

        // Ahead = Transform * (0, 0, ForwardExtent44 * 1.2) through
        // I3DMathEngine::TransformXform (+0x28). baseline fetches the pointer through slot 4
        // (get_orientation, `CALL [EDX+0x10]` at 062c9f8c), which is ICF-folded with slot 3
        // get_transform and returns the whole Transform -- TransformXform reads its
        // translation at +0x24. The port asks slot 3 for the same object explicitly.
        Vector const Local{ 0.0f, 0.0f,
                            static_cast<float>(static_cast<double>(ForwardExtent44) * 1.2) };
        Transform const& Xform = Sys->SelfTarget.Iface->get_transform();
        Vector Ahead{ 0.0f, 0.0f, 0.0f };
        Acquire3DMathEngine()->TransformXform(reinterpret_cast<FVector&>(Ahead),
                                              reinterpret_cast<FTransform const&>(Xform),
                                              reinterpret_cast<FVector const&>(Local));

        // RayEnd = Ahead + (-column 2) * 10000.0f (the field).
        Matrix const& Orientation = Sys->SelfTarget.Iface->get_orientation();
        Vector const Column2 = MatrixColumn2(Orientation);
        Vector const Backward{ -Column2.x, -Column2.y, -Column2.z };
        Vector const Reach{ Backward.x * 10000.0f, Backward.y * 10000.0f, Backward.z * 10000.0f };
        Vector const RayEnd{ Ahead.x + Reach.x, Ahead.y + Reach.y, Ahead.z + Reach.z };

        // Sys->Owner->ObjectDb->GetZoneDb()->GetId() is the PhySys
        // system id; one hit at most.
        IBehaviorManager* const Manager = Sys->Owner;
        Universe::ISystem const* const System = Manager->ObjectDb->GetZoneDb();
        PhySys::RayHit Hit;
        int const HitCount = PhySys::FindRayCollisions(System->GetId(), Ahead, RayEnd, &Hit, 1);

        Vector Gap{ 0.0f, 0.0f, 0.0f };
        if (HitCount > 0)
        {
            Gap = Vector{ Ahead.x - Hit.Point.x, Ahead.y - Hit.Point.y, Ahead.z - Hit.Point.z };
        }
        else
        {
            Vector const& TargetPos = Target->get_position();
            Gap = Vector{ SelfPos.x - TargetPos.x, SelfPos.y - TargetPos.y, SelfPos.z - TargetPos.z };
        }

        // the inlined 0x5f3759df fast length of len^2 against TriggerRange2c; `TEST AH,0x5; JP`
        // skips when >=.
        // The squared length stays on the x87 stack: only its FST image feeds the magic, and
        // the Newton step and the final FMULP run on the unrounded value (no store before the
        // FCOMP at 062ca15e).
        double const GapSq = (static_cast<double>(Gap.z) * Gap.z + static_cast<double>(Gap.y) * Gap.y)
                           + static_cast<double>(Gap.x) * Gap.x;
        double const GapLength = CmnAiSteer::InlineFastLength(GapSq);
        if (GapLength < TriggerRange2c)
            SelectStrafePoint(1, Target);
        Result = 0;
    }
    else if (StrafeMode48 == 1)
    {
        // Toward = (TargetPos - SelfPos) / (inlined 0x5f3759df fast length).
        Vector const& TargetPos = Target->get_position();
        Vector Toward{ TargetPos.x - SelfPos.x, TargetPos.y - SelfPos.y, TargetPos.z - SelfPos.z };
        // (z*z + y*y) + x*x on the FPU; the fast length runs on the unrounded sum and only
        // its result is FSTP'd (062c9e1f-062c9e77).
        double const TowardSq = (static_cast<double>(Toward.z) * Toward.z
                                 + static_cast<double>(Toward.y) * Toward.y)
                              + static_cast<double>(Toward.x) * Toward.x;
        float const Length = static_cast<float>(CmnAiSteer::InlineFastLength(TowardSq)); // FSTP [ESP+0x10]
        double const Inv = 1.0 / static_cast<double>(Length);       // FLD 1.0; FDIV
        Toward.x = static_cast<float>(Toward.x * Inv);
        Toward.y = static_cast<float>(Toward.y * Inv);
        Toward.z = static_cast<float>(Toward.z * Inv);

        // dot(-column 2 of get_orientation (slot 4), Toward) FCOMP 0.0
        // (double the field); `TEST AH,0x5; JP` falls through only when less.
        Matrix const& Orientation = Sys->SelfTarget.Iface->get_orientation();
        Vector const Column2 = MatrixColumn2(Orientation);
        Vector const Backward{ -Column2.x, -Column2.y, -Column2.z };
        // the products and sums stay on the FPU (extended).
        double const Facing = (static_cast<double>(Backward.z) * Toward.z
                               + static_cast<double>(Backward.y) * Toward.y)
                            + static_cast<double>(Backward.x) * Toward.x;
        if (Facing < 0.0)
        {
            // (the routine(Target) + StrafeDistance34) FCOMP Length;
            // `TEST AH,0x5; JP` falls through only when less.
            double const Reach = static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Target)) +
                                 static_cast<double>(StrafeDistance34);
            if (Reach < static_cast<double>(Length))
                SelectStrafePoint(0, Target);
        }
    }
    else if (StrafeMode48 == 2)
    {
        SelectStrafePoint(0, ObjInspectOf(Target));
    }

    // tick the MoveIdea and the FaceIdea (slot 1) with the delta.
    (void)SubObjectA.Update(DeltaTime);
    (void)SubObjectB.Update(DeltaTime);
    UpdateResult10 = Result;
    return Result;
}

// ----------------------------------------------------------------------------
// (module storage-062c9d10), implementation tracker item 1365, 387 instructions
// (Common). Re-checked 2026-10-08: the port's null tests on NavHelper,
// the watched object and its sub-object-centre receiver were not baseline's and are gone.
// ----------------------------------------------------------------------------
void StrafeBehavior::SelectStrafePoint(int Mode, IObjInspect const* Target)
{
    SystemManager* const Sys = NavHelper;

    if (Mode == 1)
    {
        // this ship (SystemManager::Watcher's object), slot 0.
        Vector const SelfPos = WatchedSelf(Sys)->get_position();

        // the same slot 0 on Target.
        Vector const TargetPos = Target->get_position();

        // the routine, i.e. max(physical radius, atmosphere range).
        float const TargetExtent = Behavior_GetIfaceMaxScalarAttribute(Target);

        // Delta = SelfPos - TargetPos.
        Vector const Delta{ SelfPos.x - TargetPos.x, SelfPos.y - TargetPos.y, SelfPos.z - TargetPos.z };

        // (z*z + y*y) + x*x summed on the FPU, rounded once by
        // `FST float [ESP+0x38]` (the copy at [ESP+0x18] seeds the 0x5f3759df guess).
        float const DeltaLenSq =
            (float)(((double)Delta.z * Delta.z + (double)Delta.y * Delta.y) + (double)Delta.x * Delta.x);

        // ToTarget = TargetPos - SelfPos, normalized exactly.
        Vector ToTarget{ TargetPos.x - SelfPos.x, TargetPos.y - SelfPos.y, TargetPos.z - SelfPos.z };
        NormalizeInlineX87(ToTarget);

        // /062c98f3 plus module storage-062c9981: guess = 0x5f3759df - (bits >> 1);
        // ((guess*guess)*S*0.5f) subtracted from 1.5f, times guess, times S -- kept on the FPU
        // (double) and compared unrounded.
        double const Distance = CmnAiSteer::InlineFastLength(DeltaLenSq);

        // `FCOMP float [ESP+0x34]; TEST AH,0x5; JP` -- the jump is
        // taken on >= and unordered, the fall-through is strictly less than.
        if (Distance < TargetExtent)
        {
            // 0x3e20d97c == 0.15707964f.
            StrafeDir38 = random_sphere_constrained_2(ToTarget, 0.0f, 0.15707964f);
        }
        else
        {
            // cross(ToTarget, (0,1,0)) with the multiplications by 1
            // folded and the ones by 0.0f (the field) kept: (y*0 - z, z*0 - x*0, x - y*0).
            StrafeDir38 = Vector{ ToTarget.y * 0.0f - ToTarget.z,
                                  ToTarget.z * 0.0f - ToTarget.x * 0.0f,
                                  ToTarget.x - ToTarget.y * 0.0f };
            NormalizeByVectorLengthX87(StrafeDir38);
            StrafeDir38 = random_sphere_constrained_2(StrafeDir38, 0.0f, 0.34906587f);
        }

        // +0x3c (StrafeDir38.y) = 0, renormalize (062c9a61 the routine).
        StrafeDir38.y = 0.0f;
        NormalizeByVectorLengthX87(StrafeDir38);

        // the routine scales by StrafeDistance34, the routine
        // adds SelfPos.
        StrafePoint4c = Vector{ SelfPos.x + StrafeDir38.x * StrafeDistance34,
                                SelfPos.y + StrafeDir38.y * StrafeDistance34,
                                SelfPos.z + StrafeDir38.z * StrafeDistance34 };

        SubObjectA.SetPosition(StrafePoint4c);   // , the routine
        SubObjectB.SetPosition(StrafePoint4c);   // , the routine

        // the self record's position cache (Flags1578[2]).
        if (Sys->SelfTarget.bPosValid == 0)
        {
            Sys->SelfTarget.bPosValid = 1;
            Sys->SelfTarget.Iface
                ->get_physical_radius(Sys->SelfTarget.CachedRadius, Sys->SelfTarget.CachedPos);
        }

        // radius times the double 5.0 (the field).
        SubObjectA.SetRange3c(static_cast<float>(static_cast<double>(Sys->SelfTarget.CachedRadius) * 5.0));

        StrafeMode48 = Mode;
        return;
    }

    if (Mode == 0)
    {
        // the routine (GetIfacePositionOrZero).
        Vector TargetPos{ 0.0f, 0.0f, 0.0f };
        GetIfacePositionOrZero(&TargetPos, const_cast<IObjInspect*>(Target));
        StrafePoint4c = TargetPos;

        // this ship's own interface (Sys+0x1580) get_subtarget.
        IObjInspect const* const SelfExternal = Sys->SelfTarget.Iface;
        unsigned short SubtargetId = 0;
        SelfExternal->get_subtarget(SubtargetId);

        if (SubtargetId != SUBOBJ_ID_NONE)
        {
            // asked a second time; the second answer is used.
            unsigned short SubtargetIdAgain = 0;
            SelfExternal->get_subtarget(SubtargetIdAgain);

            // slot 6 (+0x18) on the watched object; non-zero skips.
            Vector SubCenter{ 0.0f, 0.0f, 0.0f };
            int const SubCenterFailed =
                WatchedSelf(Sys)->get_sub_obj_center_of_mass(SubtargetIdAgain, SubCenter);

            if (SubCenterFailed == 0)
            {
                // unit direction from the subtarget centre to
                // Target's centre of mass (slot 5), normalized exactly.
                Vector const TargetCoM = Target->get_center_of_mass();
                Vector Toward{ TargetCoM.x - SubCenter.x, TargetCoM.y - SubCenter.y, TargetCoM.z - SubCenter.z };
                NormalizeInlineX87(Toward);

                // -column 2 of this ship's rotation.
                Transform const& SelfXform = SelfExternal->get_transform();
                Vector const Column2 = MatrixColumn2(SelfXform.Rotation);
                Vector Backward{ 0.0f, 0.0f, 0.0f };
                VecNegate3(&Column2, &Backward);

                // the dot summed on the FPU (double), FCOMP double 0.2
                // (the field); `TEST AH,0x41; JNZ` skips on less, equal and unordered, the
                // fall-through is strictly greater.
                double const Facing =
                    ((double)Backward.z * Toward.z + (double)Backward.y * Toward.y) + (double)Backward.x * Toward.x;
                if (Facing > 0.2)
                    StrafePoint4c = SubCenter;
            }
        }

        // .
        SubObjectA.SetField44(2);                // , PUSH 0x2
        SubObjectA.SetRange3c(1.0f);             // fold, PUSH 0x3f800000
        SubObjectA.SetPosition(StrafePoint4c);
        SubObjectB.SetPosition(StrafePoint4c);
    }

    // .
    StrafeMode48 = Mode;
}
