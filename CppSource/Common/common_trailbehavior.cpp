// Author: Giperion.

#include "common_trailbehavior.h"

#include "common_aisteer.h"        // FAiTarget, GetIfaceVectorMagnitude, GetIfacePositionOrZero,
                                   // ClampedDot3, BearingAngleToIface, IsAlignedAndOutsideHeadingCone,
                                   // GetInterceptClearanceOfIface, GetObjectTypeOrZero
#include "common_csys.h"           // Transform, Acquire3DMathEngine
#include "common_iface.h"          // IObjInspect, IObjRW, IBehaviorManager, ObjInspectOf, ObjRwFromWatchable
#include "common_misc.h"           // PlayerTrailAvailable
#include "common_pub.h"            // pub::AI::ContentCallback, TrailUseStruct
#include "common_systemmanager.h"  // SystemManager -- what NavHelper points at
#include "x86math.h"               // I3DMathEngine::TransformMatrix (+0x2c)

#include <cmath>
#include <cstring>

#include "x87_trig.h" // X87CosF (ctor 062ca3c7, Slot3 062ca73b FCOS; FSTP float)

// TrailBehavior -- see common_trailbehavior.h. Every method re-ported from
// Common-10-08 (claude-code:cmn-ai5).

// (item 1798) is SystemManagerTimerTable::FindVerifiedObject.

namespace
{
    // .data:the field, the 3DMathEngine pointer Slot7 fetches lazily through the routine.
    I3DMathEngine* GTrailMathEngine = nullptr;

    // SystemManager+0x14fc, the owner's own FAiTarget record.
    FAiTarget* NavOf(SystemManager* Sys)
    {
        return &Sys->SelfTarget;
    }

    pub::AI::Personality::TrailUseStruct const& TrailParamsOf(SystemManager* Sys)
    {
        return Sys->PersonalityData.TrailParams;
    }

    IObjRW* WatchedObject(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // The inlined get_type refresh on the FAiTarget (`TEST [Sys+0x1579]; ... CALL [EDX+0x88];
    // MOV [EDI+0x7d],1` at module storage-062ca8c0): the flag is raised after the call.
    unsigned RefreshNavFlags(FAiTarget* Nav)
    {
        if (Nav->bFlagsValid == 0)
        {
            Nav->Iface->get_type(Nav->CachedFlags);
            Nav->bFlagsValid = 1;
        }
        return Nav->CachedFlags;
    }

    // `MOV EAX,[ExternalIface]; CALL [EAX+0xbc]`.
    bool OwnShipIsPlayer(SystemManager* Sys)
    {
        return Sys->SelfTarget.Iface->is_player();
    }

    // The [-1, 1] clamp set_direction applies to both axes (module storage-062cb27b): `TEST
    // AH,0x41; JNZ` sends <= -1 (and unordered) to -1, `TEST AH,0x5; JP` sends >= 1 to 1, and
    // the third compare re-tests the lower bound on a path where it can no longer hold.
    float ClampUnit(float Value)
    {
        if (!(Value > -1.0f))
            return -1.0f;
        if (!(Value < 1.0f))
            return 1.0f;
        return Value;
    }
}

// ----------------------------------------------------------------------------
// (module storage-062ca496).
// ----------------------------------------------------------------------------
TrailBehavior::TrailBehavior(int InTypeId)
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
    , MatchThrottle()               // the routine
    , Face()                        // the routine
    , Move()                        // the routine
    , Afterburner()                 // the routine
    , DirectiveTarget()             // , then the no-op set_pointer guard
    , LeaderId1dc(0)
    , bHasDirectiveTarget1e0(0)     // not written by baseline's ctor; SetContext clears it
    , Pad1e1{}
    , TrailOffset1e4(0.0f)
    , TrailDistance1e8(50.0f)       // 0x42480000
    , bLagging1ec(0)                // not written by baseline's ctor; Slot3 clears it
    , Pad1ed{}
    , FacingCos1f0(X87CosF(0.5235987901687622))   // FCOS, 062ca454 FSTP
    , LagTimer1f4(0.0f)             // not written by baseline's ctor; Slot3 clears it
    , AxisTimer1f8(0.0f)            // not written by baseline's ctor; Slot3 clears it
    , LagTime1fc(2.5f)              // 0x40200000
    , AxisPause200(5.0f)            // 0x40a00000
    , AxisTime204(3.0f)             // 0x40400000
    , AxisZ208(0.0f)
    , bAfterburner20c(0)
    , Pad20d{}
    , Scale210(1.0f)                // 0x3f800000
    , bAxisArmed214(0)
    , Pad215{}
    , SightTimer218(0.0f)           // not written by baseline's ctor; Slot3 clears it
    , SightTime21c(4.0f)            // 0x40800000
    , TrailExtent220(0.0f)          // not written by baseline's ctor; Slot7 writes it
    , Field224(0.0385f)             // overwritten below with the exact bits
    , Field228(400.0f)              // 0x43c80000
    , bDirectionSet22c(0)
    , Pad22d{}
    , DirectionX230(0.0f)
    , DirectionY234(0.0f)
    , DirectionZ238(0.0f)
    , bMatchThrottle23c(0)
    , Pad23d{}
    , Field240(20.0f)               // 0x41a00000
    , CatchUpRange244(1500.0f)      // 0x44bb8000
{
    ConstructBase(InTypeId);        // the routine
    unsigned const Field224Bits = 0x3d1de9e7u;
    std::memcpy(&Field224, &Field224Bits, sizeof(Field224));
}

// (the routine is the scalar-deleting thunk): the four set_pointer(0) calls are
// the members' own BaseWatcher destructors, then the routine (~Behavior).
TrailBehavior::~TrailBehavior()
{
}

// ----------------------------------------------------------------------------
// Slot 1, the branch (module storage-062ca63a).
// ----------------------------------------------------------------------------
bool TrailBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    bool const bOk = BindContext(Owner, Manager, Sys);              // the routine
    if (!bOk)
        return bOk;

    // Every idea gets (Object = +0x14, Handle = +0x24, Context = Manager). Results unread.
    bHasDirectiveTarget1e0 = 0;
    Face.SetContext(OwnerContext, NavHelper, Manager);
    MatchThrottle.SetContext(OwnerContext, NavHelper, Manager);
    MatchThrottle.SetMaxBearing(3.14159274f);                       // module storage, 0x40490fdb
    MatchThrottle.SetCatchUpRange(CatchUpRange244);                 // the routine
    if (!ObjInspectOf(OwnerContext)->is_player())                   // [+0xbc] on the owner
        MatchThrottle.SetFlag3b(true);                              // the routine
    Move.SetContext(OwnerContext, NavHelper, Manager);
    Move.SetMode(1);                                                // the routine
    Afterburner.SetContext(OwnerContext, NavHelper, Manager);
    return bOk;
}

// ----------------------------------------------------------------------------
// Slot 3, the branch (module storage-062ca7a3).
// ----------------------------------------------------------------------------
void TrailBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    SetCruiseLock(1, 3);                                            // the routine

    // the owner's target, else the directive target.
    IObjRW* Leader = NavHelper->Targeting.GetTarget();
    if (Leader == nullptr)
        Leader = WatchedObject(DirectiveTarget);

    bLagging1ec = 0;
    LagTimer1f4 = 0.0f;
    AxisTimer1f8 = 0.0f;

    if (OwnShipIsPlayer(NavHelper))
    {
        // the owner's own cruise state into MatchThrottle's +0x38.
        bool bCruise = false;
        static_cast<IObjRW*>(OwnerContext)->is_cruise_active(bCruise);   // [+0x60]
        MatchThrottle.SetFlag38(bCruise);
    }

    bDirectionSet22c = 0;
    DirectionX230 = 0.0f;
    DirectionY234 = 0.0f;
    DirectionZ238 = 0.0f;
    SightTimer218 = 0.0f;

    if (Leader != nullptr)
    {
        Move.PublishTargetId(Leader->get_id());                     // the routine
        Face.PublishTargetId(Leader->get_id());
        LeaderId1dc = Leader->get_id();
    }
    else
    {
        Move.PublishTargetId(0);
        LeaderId1dc = 0;
    }

    // the personality's TrailUseStruct (Sys+0x16ac).
    pub::AI::Personality::TrailUseStruct const& P = TrailParamsOf(NavHelper);
    FacingCos1f0 = X87CosF(static_cast<double>(P.TrailLockConeAngle));   // FLD; FCOS; FSTP
    LagTime1fc = P.TrailBreakTime;
    AxisTime204 = P.TrailMinNoLockTime;
    AxisZ208 = P.TrailBreakRollThrottle;
    bAfterburner20c = P.bTrailBreakAfterburner;
    Scale210 = P.TrailMaxTurnThrottle;
    Face.SetScale94(P.TrailMaxTurnThrottle);                        // the routine
    bAxisArmed214 = 0;
    bMatchThrottle23c = 0;
}

// ----------------------------------------------------------------------------
// Slot 4, the routine (module storage-062ca811).
// ----------------------------------------------------------------------------
void TrailBehavior::Slot4()
{
    bHasDirectiveTarget1e0 = 0;
    DirectiveTarget.set(nullptr);
    Face.Commit();
    MatchThrottle.SetFlag39(true);                                  // the routine
    MatchThrottle.Commit();
    Move.Commit();
    Afterburner.Commit();
    Behavior::Slot4();                                              // JMP the routine
}

// ----------------------------------------------------------------------------
// Slot 5, the branch (module storage-062ca936).
// ----------------------------------------------------------------------------
int TrailBehavior::Slot5(float Unused)
{
    (void)Unused;
    SystemManager* const Sys = NavHelper;
    int Result = 2;                                                 // EBP

    if (!Sys->GunConvergenceStateBytes.FollowerDirectiveGateA())     // the routine
    {
        IObjRW* const Leader = Sys->Targeting.GetTarget();
        if (Leader != nullptr)
        {
            // the routine on a fresh GetTarget(); `FCOMP 20.0f; TEST
            // AH,0x1; JNZ` leaves on less (or unordered).
            if (GetIfaceVectorMagnitude(Sys->Targeting.GetTarget()) >= 20.0f)
            {
                // the leader's type; a 0x380000 type gives up at once.
                // The local is not pre-set in baseline (get_type always writes it).
                unsigned LeaderType = 0;
                Leader->get_type(LeaderType);
                if ((LeaderType & 0x380000u) != 0)
                {
                    ModeSelector0xc = Flag29 ? 3 : Result;
                    return ModeSelector0xc;
                }
                if ((RefreshNavFlags(NavOf(Sys)) & 0x7f0000u) != 0)
                    Result = 1;
                // the routine(Leader, Owner); `FCOMP pi/2 (module storage);
                // TEST AH,0x5; JP` leaves on >= (or unordered).
                if (BearingAngleToIface(Leader, OwnerContext) < 1.5707964f)
                {
                    ModeSelector0xc = Flag29 ? 3 : 2;
                    return ModeSelector0xc;
                }
            }
        }
        if (bHasDirectiveTarget1e0 != 0)
            Result = 1;
    }

    ModeSelector0xc = Flag29 ? 3 : Result;
    return ModeSelector0xc;
}

// ----------------------------------------------------------------------------
// Slot 6, the branch (module storage-062caa2f).
// ----------------------------------------------------------------------------
EvaluateResult TrailBehavior::evaluate(unsigned int Context)
{
    (void)Context;
    if (!PlayerTrailAvailable)
        return EvalSkip;

    IObjRW* const Owner = static_cast<IObjRW*>(OwnerContext);
    IObjRW* Target = nullptr;
    Owner->get_target(Target);                                      // [+0x74]
    if (Target == nullptr)
        return EvalSkip;

    bool bInLane = false;
    Owner->is_using_tradelane(&bInLane);                            // [+0x134]
    if (bInLane)
        return EvalSkip;

    // get_type must succeed and name a ship-class type outside 0x200000.
    unsigned TargetType = 0;                                        // not pre-set in baseline
    if (Target->get_type(TargetType) != 0)                          // [+0x88]
        return EvalSkip;
    if ((TargetType & 0x7f0000u) == 0 || (TargetType & 0x200000u) != 0)
        return EvalSkip;

    // behaviors 0xe, 0x11 and 8 cannot be trailed.
    int const TargetBehavior = Target->get_behavior_id();           // [+0x148]
    if (TargetBehavior == 0xe || TargetBehavior == 0x11 || TargetBehavior == 8)
        return EvalSkip;

    // the routine re-reads the type the null-safe way.
    unsigned const TypeAgain = GetObjectTypeOrZero(Target);
    if ((TypeAgain & 0x7f0000u) == 0 || (TypeAgain & 0x600de0u) != 0)
        return EvalSkip;

    NavHelper->Targeting.RetargetOwner(Target, SUBOBJ_ID_NONE);     // the routine
    return EvalBlocked;                                             // , 1
}

// ----------------------------------------------------------------------------
// Slot 8, the branch (module storage-062caa59).
// ----------------------------------------------------------------------------
void TrailBehavior::Slot8()
{
    if (bMatchThrottle23c != 0)
        MatchThrottle.Slot2(DeltaTime);                             // [EDX+0x8]
}

// ----------------------------------------------------------------------------
// Slot 7, the branch (module storage-062cb10f). Frame names B+n are [ESP0+n], ESP0 = ESP
// after `SUB ESP,0x68`.
// ----------------------------------------------------------------------------
int TrailBehavior::Slot7()
{
    if (OwnerContext == nullptr)
    {
        UpdateResult10 = 3;
        return 3;
    }

    SystemManager* const Sys = NavHelper;
    if (Sys->Timers.IsTimer0Active())                              // the routine
    {
        UpdateResult10 = 2;
        return 2;
    }

    // B+2: set when the leader comes from the TargetManager rather than the directive.
    bool bTrailTarget = false;
    IObjRW* Leader = nullptr;                                       // EBX
    if (bHasDirectiveTarget1e0 != 0 && WatchedObject(DirectiveTarget) != nullptr)
    {
        Leader = WatchedObject(DirectiveTarget);
    }
    else
    {
        // .
        if (Sys->Targeting.GetTarget() == nullptr)
        {
            UpdateResult10 = 2;
            return 2;
        }
        Leader = Sys->Targeting.GetTarget();
        // an AI owner gives up on a target slower than 20 (the routine;
        // `TEST AH,0x5; JP` stays on >= or unordered).
        if (!OwnShipIsPlayer(Sys) && Leader != nullptr &&
            GetIfaceVectorMagnitude(Sys->Targeting.GetTarget()) < 20.0f)
        {
            UpdateResult10 = 3;
            return 3;
        }
        bTrailTarget = true;
    }
    if (Leader == nullptr)
    {
        UpdateResult10 = 2;
        return 2;
    }

    // everyone watches the leader's id.
    MatchThrottle.PublishTargetId(Leader->get_id());                // the routine
    Face.PublishTargetId(Leader->get_id());
    Move.PublishTargetId(Leader->get_id());
    if (Leader->get_id() != LeaderId1dc)
        Slot3();                                                    // [EAX+0xc]
    LeaderId1dc = Leader->get_id();

    if (bTrailTarget && !OwnShipIsPlayer(Sys))
    {
        FAiTarget* const Nav = NavOf(Sys);
        Nav->RefreshPos();

        // unit vector from the own centre to the leader's centre.
        Vector LeaderCenter;
        GetIfacePositionOrZero(&LeaderCenter, Leader);              // into B+0x20
        Vector ToLeader;
        ToLeader.x = LeaderCenter.x - Nav->CachedPos.x;
        ToLeader.y = LeaderCenter.y - Nav->CachedPos.y;
        ToLeader.z = LeaderCenter.z - Nav->CachedPos.z;
        Vector ToLeaderDir;
        VecNormalize3(&ToLeader, &ToLeaderDir);                     // into B+0x38

        // the own forward (column 2 negated) and the facing cosine (B+4).
        Transform const& OwnXf = Sys->SelfTarget.Iface->get_transform();
        Vector OwnFwd;
        OwnFwd.x = -OwnXf.Rotation.E[0][2];
        OwnFwd.y = -OwnXf.Rotation.E[1][2];
        OwnFwd.z = -OwnXf.Rotation.E[2][2];
        float const FacingCos = static_cast<float>(ClampedDot3(ToLeaderDir, OwnFwd));

        // the "face the axis" pause.
        if (bAxisArmed214 != 0 && bLagging1ec == 0 &&
            static_cast<double>(AxisPause200) + AxisTime204 > AxisTimer1f8)          // `TEST AH,0x41; JNZ`
        {
            if (AxisTimer1f8 > AxisTime204)
            {
                AxisTimer1f8 = static_cast<float>(static_cast<double>(AxisTimer1f8) + DeltaTime);
                bAxisArmed214 = 0;
                Vector Axis;
                Axis.x = 0.0f;
                Axis.y = 0.0f;
                Axis.z = AxisZ208;
                Face.SetAxis(Axis);                                 // the routine
                Face.Update(DeltaTime);
                UpdateResult10 = 1;
                return 1;
            }
            AxisTimer1f8 = static_cast<float>(static_cast<double>(DeltaTime) + AxisTimer1f8);
        }
        else
        {
            AxisTimer1f8 = 0.0f;
        }

        // lagging behind (facing away) -- turn to the axis for LagTime1fc.
        // `TEST AH,0x5; JP` stays on FacingCos < FacingCos1f0 and on LagTimer1f4 < LagTime1fc.
        if (bLagging1ec != 0 && FacingCos < FacingCos1f0 && LagTimer1f4 < LagTime1fc)
        {
            LagTimer1f4 = static_cast<float>(static_cast<double>(DeltaTime) + LagTimer1f4);
            Vector Axis;
            Axis.x = 0.0f;
            Axis.y = 0.0f;
            Axis.z = AxisZ208;
            Face.SetAxis(Axis);
            if (bAfterburner20c != 0)
                Afterburner.Update(DeltaTime);
            Face.Update(DeltaTime);
            Move.SetMode(0);                                        // the routine
            Move.SetScale40(1.0f);                                  // module storage
            Move.SetDirectionFrom(&Sys->SelfTarget.Iface->get_transform());   // the routine
            Move.SetAngle38(3.14159274f);                           // module storage
            Move.Update(DeltaTime);
            UpdateResult10 = 0;
            return 0;
        }

        // .
        Afterburner.Commit();
        LagTimer1f4 = 0.0f;
        if (FacingCos > FacingCos1f0)                               // `TEST AH,0x41; JNZ`
        {
            bLagging1ec = 1;
            bAxisArmed214 = 1;
        }
        else
        {
            bLagging1ec = 0;
        }

        // once the leader has stayed in sight SightTime21c seconds, tell the
        // content callback (IBehaviorManager+0x110, ContentCallback slot 3).
        if (bLagging1ec != 0)
        {
            bool const bWasBelow = SightTimer218 < SightTime21c;    // `TEST AH,0x5; JP` -> CL
            SightTimer218 = static_cast<float>(static_cast<double>(DeltaTime) + SightTimer218);   // FST
            if (SightTimer218 > SightTime21c && bWasBelow)
            {
                pub::AI::ContentCallback* const Callback = Sys->Owner->ContentCbPtr;   // [[Sys+4]+0x110]
                if (Callback != nullptr)
                    Callback->ship_in_sights_event(Leader->get_id());   // [ESI+0xc]
            }
        }
        else
        {
            SightTimer218 = 0.0f;
        }
    }

    // the trail point, TrailOffset1e4 behind the leader along its own axis.
    if (!OwnShipIsPlayer(Sys))
        TrailDistance1e8 = TrailParamsOf(Sys).TrailDistance;        // Sys+0x16c4
    TrailOffset1e4 = static_cast<float>(
        static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Leader)) + TrailDistance1e8);
    TrailExtent220 = static_cast<float>(
        static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Leader)) + TrailOffset1e4);

    bool bOwnCruise = false;                                        // B+3, never read again
    static_cast<IObjRW*>(OwnerContext)->is_cruise_active(bOwnCruise);   // [+0x60]

    Matrix const LeaderRotation = Leader->get_transform().Rotation; // the routine, REP MOVSD 9
    Vector LeaderCenter;
    GetIfacePositionOrZero(&LeaderCenter, Leader);                  // into B+0x2c

    Vector Local;                                                   // B+8
    Local.x = 0.0f;
    Local.y = 0.0f;
    Local.z = TrailOffset1e4;
    if (GTrailMathEngine == nullptr)
        GTrailMathEngine = Acquire3DMathEngine();
    Vector Offset;                                                  // B+0x20
    GTrailMathEngine->TransformMatrix(reinterpret_cast<FVector&>(Offset),
                                      reinterpret_cast<FMatrix const&>(LeaderRotation),
                                      reinterpret_cast<FVector const&>(Local));   // [EDX+0x2c]
    Vector TrailPoint;                                              // B+8
    TrailPoint.x = Offset.x + LeaderCenter.x;
    TrailPoint.y = Offset.y + LeaderCenter.y;
    TrailPoint.z = Offset.z + LeaderCenter.z;

    // the MatchThrottle2Idea drives when the leader flies the same way and
    // the own ship lies outside its forward cone (the routine with cos 0).
    bMatchThrottle23c = IsAlignedAndOutsideHeadingCone(NavOf(Sys), Leader, 0.0f) ? 1 : 0;
    BaseIdea* Driver;
    if (bMatchThrottle23c != 0)
    {
        MatchThrottle.SetWatchAndPosition(Leader, TrailPoint);      // the routine
        Driver = &MatchThrottle;
    }
    else
    {
        Move.SetMode(0);                                            // the routine
        Move.SetScale40(1.0f);
        Move.SetWatchObject(Leader);                                // the routine
        Move.SetAngle38(3.14159274f);
        Move.SetRange3c(TrailDistance1e8);                          // module storage
        Driver = &Move;
    }
    Driver->Update(DeltaTime);                                      // [EDX+0x4]

    // the routine for effect (FSTP ST0) unless a player owner has the
    // camera-level status cleared.
    if (!OwnShipIsPlayer(Sys) || BehaviorMgrPtr->get_camera_level_status())
        (void)GetInterceptClearanceOfIface(NavOf(Sys), Leader);

    // face the leader, upright when the timer table asks for it.
    Face.SetWatchObject(Leader);                                    // the routine
    if (Sys->Timers.IsTimer5Active())                              // the routine
    {
        Vector Up;
        Up.x = 0.0f;
        Up.y = 0.0f;
        Up.z = 1.0f;
        Face.SetUpVector(Up);                                       // the routine
    }
    Face.Update(DeltaTime);
    UpdateResult10 = 1;
    return 1;
}

// ----------------------------------------------------------------------------
// Slot 10, the branch (module storage-062cb16c).
// ----------------------------------------------------------------------------
bool TrailBehavior::Slot10(void* DirectiveOp)
{
    if (DirectiveOp != nullptr)
    {
        unsigned char* const Op = static_cast<unsigned char*>(DirectiveOp);
        TrailDistance1e8 = *reinterpret_cast<float const*>(Op + 0x10);   // DirectiveTrailOp+0x10
        void* const Target = NavHelper->Timers.FindVerifiedObject((unsigned const*)(Op + 0xc));
        if (Target != nullptr)
        {
            DirectiveTarget.set(reinterpret_cast<Watchable const*>(static_cast<char*>(Target) + 8));
            bHasDirectiveTarget1e0 = 1;
            Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
            return true;
        }
    }
    Flags28 = static_cast<unsigned char>(Flags28 & 0xfd);
    return false;
}

// ----------------------------------------------------------------------------
// Slot 12, the routine (module storage-062cb29c).
// ----------------------------------------------------------------------------
void TrailBehavior::set_direction(Vector const& Dir)
{
    bDirectionSet22c = 1;
    DirectionX230 = -Dir.y;
    DirectionZ238 = 0.0f;
    DirectionY234 = -Dir.x;

    // the routine; `FCOM 0.0 (double); TEST AH,0x41; JNZ` rescales only
    // a strictly positive factor, by 1/factor kept on the x87 stack.
    float const Factor = BehaviorMgrPtr->get_turn_sensitivity_factor();
    if (Factor > 0.0f)
    {
        double const Rescale = 1.0 / static_cast<double>(Factor);
        DirectionX230 = static_cast<float>(Rescale * DirectionX230);
        DirectionY234 = static_cast<float>(Rescale * DirectionY234);
    }
    DirectionX230 = ClampUnit(DirectionX230);
    DirectionY234 = ClampUnit(DirectionY234);

    // a positive z also sets the trail distance (`TEST AH,0x41; JNZ`).
    if (Dir.z > 0.0f)
        TrailDistance1e8 = Dir.z;
}

// ----------------------------------------------------------------------------
// Slot 14, the branch: forwards to MatchThrottle2Idea::SetFlag38 (the routine).
// ----------------------------------------------------------------------------
void TrailBehavior::set_cruise(bool bOn)
{
    MatchThrottle.SetFlag38(bOn);
}
