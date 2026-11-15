// Author: Giperion.

#include "common_evadebehavior.h"

#include "common_aisteer.h"        // IsPointOutsideHeadingCone, IsWithinInterceptRangeOfIface,
                                   // GetIfaceVectorMagnitude
#include "common_csys.h"           // Transform
#include "common_iface.h"          // IObjInspect, IObjRW, ObjInspectOf, ObjRwFromWatchable
#include "common_iustategraph.h"   // FindClosestHostileTarget
#include "common_systemmanager.h"  // SystemManager -- what NavHelper points at

#include <cmath>
#include <cstdint>
#include <cstdlib>

#include "x87_trig.h" // X87CosF (Slot3, 062c8431 FCOS; FCHS; FSTP float)

// EvadeBehavior -- see common_evadebehavior.h. Every method re-ported from
// Common-10-08 (claude-code:cmn-ai4).

// (item 1798) is SystemManagerTimerTable::FindVerifiedObject.

namespace
{
    // == 0x38000000 == 1/32768, the rand() scale every pick below uses.
    float const GRandScale = 3.0517578125e-05f;

    // `[ESI+0x2a0]` with the family's `Ptr ? Ptr - 8 : 0` downcast.
    IObjRW* WatchedObject(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // `MOV EAX,[W]; TEST EAX,EAX; JZ; ADD EAX,-8; TEST EAX,EAX; JZ` -- watched and not the
    // 8 sentinel.
    bool IsWatching(BaseWatcher const& Watcher)
    {
        return WatchedObject(Watcher) != nullptr;
    }

    // The watched object (and only it) must lie outside the owner's heading cone:
    // `PUSH Cos; PUSH [ESI+0x14]; CALL [Watched->vtbl] (get_position); PUSH EAX;
    // CALL the routine`.
    bool WatchedOutsideCone(BaseWatcher const& Watcher, void* Owner, float Cos)
    {
        IObjRW* const Watched = WatchedObject(Watcher);
        return IsPointOutsideHeadingCone(Watched->get_position(),
                                         static_cast<IObjInspect const*>(static_cast<IObjRW*>(Owner)),
                                         Cos);
    }
}

// ----------------------------------------------------------------------------
// , module storage-062c786d.
// ----------------------------------------------------------------------------
EvadeBehavior::EvadeBehavior(int InTypeId)
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
    , Mode2c(0)
    , Dodge()                    // the routine
    , BreakAway()                // the routine
    , DodgeMissile()             // the routine
    , Face()                     // the routine
    , Afterburner()              // the routine
    , Watcher2a0()
    , DodgeTime2a8(0.0f)         // not written by the ctor; SetContext sets it
    , BreakTime2ac(3.0f)         // 0x40400000
    , Elapsed2b0(0.0f)           // not written by the ctor; SetContext sets it
    , Flag2b4(0)
    , Flag2b5(0)
    , Flag2b6(0)
    , Flag2b7(0)
    , ConeCos2b8(0.0f)           // (dword 0)
    , ConeCos2bc(0.707f)         // 0x3f34fdf4 (0.707f, not cos 45)
{
    ConstructBase(InTypeId);     // the routine
}

// (the routine is the scalar-deleting thunk): the five set_pointer(0)
// calls are the members' own BaseWatcher destructors (Watcher2a0, Face, DodgeMissile,
// BreakAway.Picker, Dodge.Picker -- reverse member order), then the routine (~Behavior).
EvadeBehavior::~EvadeBehavior()
{
}

// ----------------------------------------------------------------------------
// Slot 1, the branch (module storage-062c7aa3).
// ----------------------------------------------------------------------------
bool EvadeBehavior::SetContext(void* Owner, void* Manager, void* Sys)
{
    bool const bOk = BindContext(Owner, Manager, Sys);              // the routine
    if (!bOk)
        return bOk;

    // Every idea gets (Object = +0x14, Handle = +0x24, Context = Manager): `PUSH EBP
    // (Manager); PUSH [ESI+0x24]; PUSH [ESI+0x14]; CALL [vtbl]`. Results unread.
    Dodge.SetContext(OwnerContext, NavHelper, Manager);
    Dodge.ConeAngle = 1.5707964f;                                   // module storage, 0x3fc90fdb
    Dodge.DodgeDistance = 50.0f;                                    // module storage, 0x42480000
    Dodge.IntervalTime = 6.0f;                                      // module storage, 0x40c00000
    BreakAway.SetContext(OwnerContext, NavHelper, Manager);
    DodgeMissile.SetContext(OwnerContext, NavHelper, Manager);
    Face.SetContext(OwnerContext, NavHelper, Manager);
    Afterburner.SetContext(OwnerContext, NavHelper, Manager);

    DodgeTime2a8 = 10.0f;                                           // 0x41200000
    Elapsed2b0 = 0.0f;                                              // (dword 0)
    return bOk;
}

// ----------------------------------------------------------------------------
// (module storage-062c872a). r = rand() * (F04 + F08 + F0C + F10) / 32768
// with the sum stored to a float local first (062c86b1 FSTP float) and r kept on the
// x87 stack. Each `TEST AH,0x5; JP` skips on r >= bound (or unordered):
//   r < F04 -> 0;  r < F04 + F08 -> 1;  r < F04 + F08 + F0C -> 2 (the final `JNP`);  3.
// ----------------------------------------------------------------------------
int EvadeBehavior::PickDodgeStyle()
{
    pub::AI::Personality::EvadeDodgeUseStruct const& P = NavHelper->PersonalityData.EvadeDodgeParams;
    float const Sum = static_cast<float>(((static_cast<double>(P.EvadeDodgeStyleWeightWaggle) +
                                            P.EvadeDodgeStyleWeightWaggleRandom) +
                                           P.EvadeDodgeStyleWeightSlide) +
                                          P.EvadeDodgeStyleWeightCorkscrew);
    double const R = static_cast<double>(std::rand()) * Sum * GRandScale;

    double Bound = P.EvadeDodgeStyleWeightWaggle;
    if (R < Bound)
        return 0;
    Bound = Bound + P.EvadeDodgeStyleWeightWaggleRandom;
    if (R < Bound)
        return 1;
    Bound = Bound + P.EvadeDodgeStyleWeightSlide;
    if (R < Bound)
        return 2;
    return 3;
}

// ----------------------------------------------------------------------------
// Slot 3, the branch (module storage-062c85d5).
// ----------------------------------------------------------------------------
void EvadeBehavior::Slot3()
{
    Behavior::Slot3();                                              // the routine
    Mode2c = 0;
    SetCruiseLock(1, 3);                                            // the routine
    Elapsed2b0 = 0.0f;                                              // (dword 0)

    Dodge.Picker.SetWatchObject(WatchedObject(Watcher2a0));         // the routine
    BreakAway.Picker.SetWatchObject(WatchedObject(Watcher2a0));     // the routine

    if (NavHelper->SelfTarget.Iface->is_player())
    {
        // a 1.0f local, IObjInspect::get_throttle (+0x4c) on the owner
        // (result unread), then the routine on Dodge (+0x94) and module storage on
        // BreakAway (+0x40) with the local.
        float Throttle = 1.0f;
        static_cast<IObjInspect const*>(static_cast<IObjRW*>(OwnerContext))->get_throttle(Throttle);
        Dodge.Throttle = Throttle;
        BreakAway.Throttle = Throttle;
    }

    pub::AI::Personality const& Pers = NavHelper->PersonalityData;
    pub::AI::Personality::EvadeDodgeUseStruct const& D = Pers.EvadeDodgeParams;
    pub::AI::Personality::EvadeBreakUseStruct const& B = Pers.EvadeBreakParams;

    // seven one-line setters on Dodge, each a raw dword copy.
    Dodge.ConeAngle = D.EvadeDodgeConeAngle;                        // +0x3c <- Sys+0x1598
    Dodge.ConeAngleVariancePercent = D.EvadeDodgeConeAngleVariancePercent;       // +0x40 <- 0x159c
    Dodge.WaggleAxisConeAngle = D.EvadeDodgeWaggleAxisConeAngle;    // +0x44 <- 0x15a0
    Dodge.RollAngle = D.EvadeDodgeRollAngle;                        // +0x48 <- 0x15a4
    Dodge.IntervalTime = D.EvadeDodgeIntervalTime;                  // +0x34 <- 0x15a8
    Dodge.IntervalTimeVariancePercent = D.EvadeDodgeIntervalTimeVariancePercent; // +0x38 <- 0x15ac
    Dodge.DodgeDistance = D.EvadeDodgeDistance;                     // +0x4c <- 0x15b0

    DodgeTime2a8 = D.EvadeDodgeTime;                                // <- 0x15b4
    BreakTime2ac = B.EvadeBreakTime;                                // <- 0x15dc

    Dodge.TurnThrottle = D.EvadeDodgeTurnThrottle;                  // +0x90 <- 0x15bc
    Dodge.SlideThrottle = D.EvadeDodgeSlideThrottle;                // +0xc8 <- 0x15b8
    Dodge.VectorCc.x = D.EvadeDodgeCorkscrewTurnThrottle;           // +0xcc <- 0x15c0
    Dodge.VectorCc.z = D.EvadeDodgeCorkscrewRollThrottle;           // +0xd4 <- 0x15c4
    Dodge.bRollFlipDirection = D.bEvadeDodgeCorkscrewRollFlipDirection; // +0xd8 <- byte 0x15c8
    Dodge.Picker.SetWeights(D.EvadeDodgeDirectionWeightLeft, D.EvadeDodgeDirectionWeightRight,
                            D.EvadeDodgeDirectionWeightUp, D.EvadeDodgeDirectionWeightDown);  // the routine on +0x44

    int const Style = PickDodgeStyle();                             // the routine
    Dodge.DodgeStyle = Style;                                       // module storage +0xb4
    if (Style == 2)
    {
        // FLD double [module storage] (1.0471975803375244); FCOS;
        // FCHS; FSTP float [+0x2b8].
        ConeCos2b8 = -X87CosF(1.0471975803375244);
    }

    // the break-away fate. The sum is FLD F2C; FADD F28; FADD F30 into a
    // float local; r = rand() * sum / 32768 stays on the x87 stack. `FCOM [F30]; TEST
    // AH,0x5; JP` skips on r >= F30, so r < F30 gives 2; then `FLD F2C; FADD F30; FXCH;
    // FCOMPP; TEST AH,0x5; JP` -> r < F2C + F30 gives 0, otherwise 1.
    int Fate;
    {
        float const Sum = static_cast<float>((static_cast<double>(B.EvadeBreakStyleWeightOutrun) +
                                              B.EvadeBreakStyleWeightSideways) +
                                             B.EvadeBreakStyleWeightReverse);
        double const R = static_cast<double>(std::rand()) * Sum * GRandScale;
        if (R < B.EvadeBreakStyleWeightReverse)
            Fate = 2;
        else if (R < static_cast<double>(B.EvadeBreakStyleWeightOutrun) + B.EvadeBreakStyleWeightReverse)
            Fate = 0;
        else
            Fate = 1;
    }
    BreakAway.BreakStyle = Fate;                                    // module storage +0x34
    BreakAway.AttemptReverseTime = B.EvadeBreakAttemptReverseTime;  // +0x4c <- 0x1610
    BreakAway.ReverseDistance = B.EvadeBreakReverseDistance;        // +0x54 <- 0x1614
    BreakAway.TurnThrottle = B.EvadeBreakTurnThrottle;              // +0x94 <- 0x1600
    BreakAway.RollThrottle = B.EvadeBreakRollThrottle;              // +0x98 <- 0x15fc
    BreakAway.IntervalTime = B.EvadeBreakIntervalTime;              // +0x88 <- 0x15e0
    BreakAway.Picker.SetWeights(B.EvadeBreakDirectionWeightLeft, B.EvadeBreakDirectionWeightRight,
                                B.EvadeBreakDirectionWeightUp, B.EvadeBreakDirectionWeightDown);  // on +0x124

    // FLD F08; FMUL F0C; FSTP float local; r = rand() * local / 32768;
    // FLD F08; FSUB r -> the routine (+0x8c).
    {
        float const Spread = static_cast<float>(static_cast<double>(B.EvadeBreakAfterburnerDelay) *
                                                B.EvadeBreakAfterburnerDelayVariancePercent);
        double const R = static_cast<double>(std::rand()) * Spread * GRandScale;
        BreakAway.AfterburnerDelay = static_cast<float>(static_cast<double>(B.EvadeBreakAfterburnerDelay) - R);
    }

    if (NavHelper->SelfTarget.Iface->is_player())
    {
        // get_transform (+0xc) on ExternalIface, its +0x8/+0x14/+0x20
        // (column 2 of the rotation) negated into a local, handed to the routine.
        Transform const& Xf = NavHelper->SelfTarget.Iface->get_transform();
        Vector Forward;
        Forward.x = -Xf.Rotation.E[0][2];                           // +0x08
        Forward.y = -Xf.Rotation.E[1][2];                           // +0x14
        Forward.z = -Xf.Rotation.E[2][2];                           // +0x20
        Dodge.SetField58(Forward);
    }
}

// ----------------------------------------------------------------------------
// Slot 4, the routine (module storage-062c8623).
// ----------------------------------------------------------------------------
void EvadeBehavior::Slot4()
{
    Flag2b5 = 0;
    Dodge.Commit();
    BreakAway.Commit();
    DodgeMissile.Commit();
    Afterburner.Commit();
    Behavior::Slot4();                                              // JMP the routine
}

// ----------------------------------------------------------------------------
// Slot 5, the branch (module storage-062c7cf7).
// ----------------------------------------------------------------------------
int EvadeBehavior::Slot5(float Unused)
{
    (void)Unused;
    SystemManager* const Sys = NavHelper;
    int Result = 2;                                                 // [ESP+0x14]

    // JobParams.bForceAttackFormation (Sys+0x1958) and Timers[0].Active.
    if (Sys->PersonalityData.JobParams.bForceAttackFormation != 0 && NavHelper->Timers.IsTimer0Active())
    {
        Flag2b4 = 1;
        Elapsed2b0 = DodgeTime2a8;
        Result = 0;
    }

    if (SystemManagerTimerTableHasMissileReactionTarget(&Sys->Timers))   // the routine
    {
        ModeSelector0xc = Flag29 ? 3 : 0;
        return ModeSelector0xc;
    }
    if (Sys->GunConvergenceStateBytes.FollowerDirectiveGateA())       // the routine
    {
        ModeSelector0xc = Flag29 ? 3 : 2;
        return ModeSelector0xc;
    }

    // the closest hostile within EvadeDodge.EvadeActivateRange, watched.
    IObjInspect* const Closest = FindClosestHostileTarget(&Sys->Timers,
                                                          Sys->PersonalityData.EvadeDodgeParams.EvadeActivateRange);
    Watcher2a0.set(static_cast<Watchable const*>(static_cast<IObjRW*>(Closest)));

    if (IsWatching(Watcher2a0))
    {
        IObjRW* Victim = nullptr;
        WatchedObject(Watcher2a0)->get_target(Victim);              // +0x74
        if (Victim != nullptr)
        {
            // GetIfaceVectorMagnitude (the routine) FCOMP 1.0f; `TEST
            // AH,0x41; JNZ` skips when not greater.
            if (GetIfaceVectorMagnitude(WatchedObject(Watcher2a0)) > 1.0f &&
                WatchedOutsideCone(Watcher2a0, OwnerContext, ConeCos2bc))
            {
                // is the watched object's target this very ship?
                unsigned int const SelfId = Sys->SelfTarget.Iface->get_id();
                if (Victim->get_id() == SelfId)
                {
                    ModeSelector0xc = Flag29 ? 3 : 1;
                    return ModeSelector0xc;
                }
            }

            // ... or one of this ship's formation followers.
            IObjRW* Followers[0x10];
            int const Count = static_cast<IObjRW*>(OwnerContext)->get_formation_followers(Followers, 0x10);
            for (int Index = 0; Index < Count; ++Index)
            {
                IObjRW* const Follower = Followers[Index];
                if (Follower == nullptr)
                    continue;
                // the cone test against the FOLLOWER's heading.
                if (!IsPointOutsideHeadingCone(WatchedObject(Watcher2a0)->get_position(), Follower, ConeCos2bc))
                    continue;
                if (Victim->get_id() == Follower->get_id())
                {
                    ModeSelector0xc = Flag29 ? 3 : 1;
                    return ModeSelector0xc;
                }
            }
        }
    }

    // .
    ModeSelector0xc = Flag29 ? 3 : Result;
    return ModeSelector0xc;
}

// ----------------------------------------------------------------------------
// Slot 8, the routine (module storage-062c7d57).
// ----------------------------------------------------------------------------
void EvadeBehavior::Slot8()
{
    SystemManager* const Sys = NavHelper;
    Flag2b4 = 0;
    if (Sys->PersonalityData.JobParams.bForceAttackFormation != 0)
        Flag2b4 = NavHelper->Timers.IsTimer0Active() ? 1 : 0;    // the routine
    if (Flag2b4 != 0)
        Elapsed2b0 = DodgeTime2a8;
    if (Flag2b5 == 0)
        Flag2b5 = NavHelper->Timers.IsTimer1Active() ? 1 : 0;    // the routine
}

// ----------------------------------------------------------------------------
// Slot 7, the branch (module storage-062c824a). Switch table at 062c824c:
// {062c8030, 062c8219, 062c7fd2, 062c7f03} for Mode2c 1..4; the JA default (062c7ef6,
// rel32 -> 062c823f) is the shared `return 2`.
// ----------------------------------------------------------------------------
int EvadeBehavior::Slot7()
{
    SystemManager* const Sys = NavHelper;
    pub::AI::Personality const& Pers = Sys->PersonalityData;

    // a non-player ship evades only something it is watching, and only
    // while that object lies outside its own heading cone.
    if (!NavHelper->SelfTarget.Iface->is_player())
    {
        if (!IsWatching(Watcher2a0) || !WatchedOutsideCone(Watcher2a0, OwnerContext, ConeCos2bc))
        {
            UpdateResult10 = 2;
            return 2;
        }
    }

    int const OldMode = Mode2c;                                     // EDI
    if (SystemManagerTimerTableHasMissileReactionTarget(&Sys->Timers))
    {
        Mode2c = 4;
    }
    else if (NavHelper->Timers.IsTimer2Active())                 // the routine
    {
        Mode2c = 3;
    }
    // `FLD [2b0]; FCOMP [2a8]; TEST AH,0x5; JP` skips on
    // Elapsed >= DodgeTime (or unordered), so mode 1 needs Elapsed < DodgeTime.
    else if (Flag2b6 == 0 && Elapsed2b0 < DodgeTime2a8)
    {
        Mode2c = 1;
    }
    else
    {
        if (Flag2b7 != 0)
        {
            UpdateResult10 = 2;
            return 2;
        }
        // `FLD [2ac]; FADD [2a8]; FCOMP [2b0]; TEST AH,0x41; JNZ` fails
        // when not greater, so mode 2 needs BreakTime + DodgeTime > Elapsed.
        if (!(static_cast<double>(BreakTime2ac) + DodgeTime2a8 > Elapsed2b0))
        {
            UpdateResult10 = 2;
            return 2;
        }
        Mode2c = 2;
    }

    if (OldMode != Mode2c)
    {
        if (Mode2c == 1)
        {
            if (Pers.FormationParams.bBreakApartFormationOnEvadeDodge != 0)   // Sys+0x17fd
                NavHelper->GunConvergenceStateBytes.NotifyFormationLeader1(Pers.FormationParams.BreakFormationOnEvadeDodgeTime);
            if (Pers.FormationParams.bRegroupFormationOnEvadeDodge != 0)      // Sys+0x1804
                NavHelper->GunConvergenceStateBytes.NotifyFormationLeader2();
        }
        if (Mode2c == 2)
        {
            if (Pers.FormationParams.bBreakApartFormationOnEvadeBreak != 0)   // Sys+0x1805
                NavHelper->GunConvergenceStateBytes.NotifyFormationLeader1(Pers.FormationParams.BreakFormationOnEvadeBreakTime);
            if (Pers.FormationParams.bRegroupFormationOnEvadeBreak != 0)      // Sys+0x180c
                NavHelper->GunConvergenceStateBytes.NotifyFormationLeader2();
        }
        Slot4();                                                    // CALL [EAX+0x10]
    }

    Elapsed2b0 = Elapsed2b0 + DeltaTime;

    switch (Mode2c)
    {
    case 1:
    {
        if (IsWatching(Watcher2a0) && !WatchedOutsideCone(Watcher2a0, OwnerContext, ConeCos2b8))
        {
            // the threat is back inside the narrower cone -- stop
            // steering: zero position into the routine and the routine, then
            // (1.0f, 0), all on the ThrustManager (Sys+0x18).
            Vector Zero1;
            Zero1.x = 0.0f;
            Zero1.y = 0.0f;
            Zero1.z = 0.0f;
            Sys->Thrust.SetAxisThrottle(Zero1);
            Vector Zero2;
            Zero2.x = 0.0f;
            Zero2.y = 0.0f;
            Zero2.z = 0.0f;
            Sys->Thrust.SetNudgeDirection(Zero2);
            Sys->Thrust.SetThrottle(1.0f, 0);
            UpdateResult10 = 0;
            return 0;
        }

        if (Flag2b5 != 0)
        {
            pub::AI::Personality::EvadeDodgeUseStruct const& D = Pers.EvadeDodgeParams;
            // v = F14 * 3.0f (the field); `FCOMP pi/2 (the field);
            // TEST AH,0x41; JNZ` keeps v when not greater, otherwise v = pi/2.
            // The FST rounds into the local, but FCOMP tests the unrounded product.
            double const Angle = static_cast<double>(D.EvadeDodgeConeAngle) * 3.0f;
            Dodge.ConeAngle = (Angle > 1.5707964f) ? 1.5707964f : static_cast<float>(Angle);
            // the same clamp on F1C -> the routine (+0x44).
            double const Turn = static_cast<double>(D.EvadeDodgeWaggleAxisConeAngle) * 3.0f;
            Dodge.WaggleAxisConeAngle = (Turn > 1.5707964f) ? 1.5707964f : static_cast<float>(Turn);
            // `FLD F24; FCOM 1.0f; TEST AH,0x41; JNZ` -> not greater
            // doubles it (FADD ST0,ST0), greater halves it (FMUL 0.5f).
            double Interval = D.EvadeDodgeIntervalTime;
            if (Interval > 1.0)
                Interval = Interval * 0.5f;
            else
                Interval = Interval + Interval;
            Dodge.IntervalTime = static_cast<float>(Interval);
            Dodge.TurnThrottle = 1.0f;
            Dodge.SlideThrottle = 1.0f;
        }

        Dodge.Update(DeltaTime);

        // the routine(Sys+0x14fc, watched-or-null, 50.0f).
        if (IsWithinInterceptRangeOfIface(&Sys->SelfTarget,WatchedObject(Watcher2a0), 50.0f))
            Afterburner.Update(DeltaTime);
        else
            Afterburner.Commit();
        UpdateResult10 = 0;
        return 0;
    }

    case 2:
    {
        int const R = BreakAway.Update(DeltaTime);
        if (R != 0)
        {
            UpdateResult10 = 2;
            return 2;
        }
        UpdateResult10 = R;
        return R;
    }

    case 3:
    {
        if (IsWatching(Watcher2a0) && !WatchedOutsideCone(Watcher2a0, OwnerContext, 0.7f))
        {
            UpdateResult10 = 2;
            return 2;
        }
        BreakAway.BreakStyle = 2;                                   // module storage (EBP)
        int const R = BreakAway.Update(DeltaTime);
        if (R != 0)
        {
            UpdateResult10 = 2;
            return 2;
        }
        UpdateResult10 = R;
        return R;
    }

    case 4:
    {
        // The missile the timer table is tracking (Timers+0xe8, Sys+0x11c), and one of three
        // dodge modes chosen by the Personality::MissileReaction range it falls inside.
        void* const MissileRaw = *reinterpret_cast<void* const*>(Sys->Timers.BestCandidateWatcherBytes);
        int Mode;
        if (TimerTableMissileWithinReaction00(&Sys->Timers))        // the routine
            Mode = 1;
        else if (TimerTableMissileWithinReaction04(&Sys->Timers))   // the routine
            Mode = 2;
        else if (TimerTableMissileWithinReaction08(&Sys->Timers))   // the routine
            Mode = 0;
        else
        {
            UpdateResult10 = 1;
            return 1;
        }
        DodgeMissile.SetWatchObject(ObjRwFromWatchable(MissileRaw));
        DodgeMissile.Mode1c = Mode;                                 // module storage
        DodgeMissile.Update(DeltaTime);
        UpdateResult10 = 1;
        return 1;
    }

    default:
        UpdateResult10 = 2;
        return 2;
    }
}

// ----------------------------------------------------------------------------
// Slot 10, the branch (module storage-062c8680).
// ----------------------------------------------------------------------------
bool EvadeBehavior::Slot10(void* DirectiveOp)
{
    SystemManager* const Sys = NavHelper;
    void* Target;
    if (DirectiveOp != nullptr)
    {
        // the routine(Sys+0x34, Op+0xc).
        Target = Sys->Timers.FindVerifiedObject((unsigned const*)(static_cast<char*>(DirectiveOp) + 0xc));
    }
    else
    {
        // FindClosestHostileTarget(Sys+0x34, EvadeDodge.EvadeActivateRange).
        Target = FindClosestHostileTarget(&Sys->Timers, Sys->PersonalityData.EvadeDodgeParams.EvadeActivateRange);
    }
    // `Target ? Target + 8 : 0`, deduplicated set_pointer.
    Watcher2a0.set(Target != nullptr
                       ? reinterpret_cast<Watchable const*>(static_cast<char*>(Target) + 8)
                       : nullptr);
    Flags28 = static_cast<unsigned char>(Flags28 | 0x2);
    return true;
}

// ----------------------------------------------------------------------------
// Slot 21, the branch (module storage-062c78af).
// ----------------------------------------------------------------------------
void EvadeBehavior::Slot21(void* Arg)
{
    if (!IsWatching(Watcher2a0))
        return;
    // get_id (+0x20) on the watched object, compared with the argument.
    if (WatchedObject(Watcher2a0)->get_id() != static_cast<unsigned int>(reinterpret_cast<uintptr_t>(Arg)))
        return;
    Watcher2a0.set(nullptr);
}

// ----------------------------------------------------------------------------
// Slot 24, the branch: `Watched ? Watched - 8 : 0`.
// ----------------------------------------------------------------------------
IObjRW const* EvadeBehavior::get_target()
{
    return WatchedObject(Watcher2a0);
}
