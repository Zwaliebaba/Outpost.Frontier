// Author: Giperion.

// Common.dll MatchThrottle2Idea (ctor 0x062d5aa0). See src/include/common_idea.h.

// CMN-SPLIT stage 1 (2026-10-14): split out of common_idea.cpp, code unchanged.

#include "common_idea.h"
#include "common_aisteer.h"   // BearingAngleToPoint, GetIfacePositionOrZero
#include "common_behavior.h"  // Behavior_InvokeManagerSlotD4 / D8, Behavior_QueryRadiusEffect / ...
#include "common_cequip.h"    // ThrustEquipType (IObjInspect::get_speed's 2nd argument)
#include "common_csys.h"      // Matrix / Transform
#include "common_iface.h"     // IObjInspect / IObjRW / IBehaviorManager / ObjRwFromWatchable
#include "common_pathfind.h"  // PathfindManager (IBehaviorManager::Pathfind)
#include "common_systemmanager.h" // SystemManager / ThrustManager (BaseIdea::Handle)
#include "x86math.h"          // I3DMathEngine::TransformMatrix

#include <cmath>
#include <cstring>

// ----------------------------------------------------------------------------
// MatchThrottle2Idea -- re-ported from the listing 2026-10-08 (claude-code:cmn-ai5).
// Handle is the SystemManager, Context the IBehaviorManager, Object the owner IObjRW.
// ----------------------------------------------------------------------------

namespace
{
    PathfindManager* MatchPathfindOf(void* Context)
    {
        return static_cast<IBehaviorManager*>(Context)->Pathfind;
    }

    IObjRW* MatchWatched(BaseWatcher const& Watcher)
    {
        return ObjRwFromWatchable(const_cast<Watchable*>(Watcher.target()));
    }

    // The shared tail of the routine / the routine: `MOV ESI,[[this+0xc]+0xc]`, the
    // watcher re-read with the `-8` downcast and NO null test (ECX == 0 faults in
    // baseline), slot 0 get_position, three dwords into +0x200.
    void MirrorWatchedPosition(void* Context, BaseWatcher const& Watcher)
    {
        PathfindManager* const Pathfind = MatchPathfindOf(Context);
        Pathfind->CachedTargetPosition = MatchWatched(Watcher)->get_position();
    }
}

// (module storage-062d5b48). BaseIdea's Object / Handle / Attached are
// zeroed (Context is not, BaseIdea's ctor zeroes it here -- the port's convention), the
// +0x14 watcher is zeroed (EH state 1), then the scalars. +0x28 and +0x4c/+0x50 are
// never written by baseline's ctor (Distance28 before SetWatchAnd*, Speed4c/50 before
// SetContext); the port zeroes them so the object is deterministic.
MatchThrottle2Idea::MatchThrottle2Idea()
    : BaseIdea()
    , Watcher()
    , Position{ 0.0f, 0.0f, 0.0f }
    , Distance28(0.0f)              // not written by baseline
    , Speed2c(0.0f)
    , ThrottleBias30(0.0f)
    , MaxBearing34(0.78539819f)     // 0x3f490fdb
    , Flag38(1)
    , Flag39(0)
    , Flag3a(0)
    , Flag3b(0)
    , Flag3c(1)
    , Flag3d(0)
    , Pad3e{}
    , ClosingFloor40(-200.0f)       // 0xc3480000
    , ClosingBias44(-0.5f)          // 0xbf000000
    , Field48(0.05f)                // 0x3d4ccccd
    , Speed4c(0.0f)                 // not written by baseline
    , Speed50(0.0f)                 // not written by baseline
    , ClosingWindow54(5.0f)         // 0x40a00000
    , CatchUpRange58(500.0f)        // 0x43fa0000
{
}

// Slot 0, the branch (module storage-062d5be6, RET 0xc).
bool MatchThrottle2Idea::SetContext(void* InObject, void* InHandle, void* InContext)
{
    if (InObject == nullptr || InHandle == nullptr)                 // / 062d5b63
        return false;

    Handle = (SystemManager*)InHandle;
    Object = InObject;
    Context = InContext;
    Attached = 1;

    IObjInspect const* const Owner = static_cast<IObjInspect const*>(static_cast<IObjRW*>(Object));

    // the two calls share one frame. `PUSH 4; PUSH 0; PUSH 4;
    // PUSH 1.0f; PUSH Object; CALL the routine` is get_speed(1.0f, Cruise); its result
    // is FSTP'd over the third pushed dword, `ADD ESP,8` drops two, and `PUSH 0; PUSH
    // Object; CALL the routine` reads (Object, 0, speed, 0, 4). FADD 300.0f
    // (module storage) and FSTP to +0x4c.
    float const CruiseSpeed = Behavior_InvokeManagerSlotD8(Owner, 1.0f, ThrustEquipCruise);
    Speed4c = static_cast<float>(
        static_cast<double>(Behavior_QueryRadiusEffect(Owner, 0.0f, CruiseSpeed, 0.0f, ThrustEquipCruise)) + 300.0f);

    // the same pair with type 1 (Engine), FADD 50.0f (module storage)
    // into +0x50.
    float const EngineSpeed = Behavior_InvokeManagerSlotD8(Owner, 1.0f, ThrustEquipEngine);
    Speed50 = static_cast<float>(
        static_cast<double>(Behavior_QueryRadiusEffect(Owner, 0.0f, EngineSpeed, 0.0f, ThrustEquipEngine)) + 50.0f);
    return true;                                                    // AL = BL = 1
}

// Slot 2, the branch (module storage-062d5cd0, RET 4). The argument is never read.
void MatchThrottle2Idea::Slot2(float DeltaTime)
{
    (void)DeltaTime;
    IObjRW* const Watched = MatchWatched(Watcher);
    if (Watched == nullptr || Object == nullptr)
        return;

    ThrustManager& Thrust = Handle->Thrust;
    if (Flag38 == 0)
    {
        Thrust.engage_cruise(false);
        return;
    }

    bool bCruise = false;
    Watched->is_cruise_active(bCruise);                             // [EAX+0x60]
    Vector const Velocity = Watched->get_velocity();                // [EAX+0x04]
    unsigned char const bEngaged = Thrust.bCruiseEngaged;           // [Handle+0x2c]

    if (bCruise)
    {
        // (y*y + z*z) + x*x against 900.0f (module storage);
        // `TEST AH,0x41; JNZ` leaves on not-greater.
        double const SpeedSq = (static_cast<double>(Velocity.y) * Velocity.y +
                                static_cast<double>(Velocity.z) * Velocity.z) +
                               static_cast<double>(Velocity.x) * Velocity.x;
        if (SpeedSq > 900.0f && bEngaged == 0)
            Thrust.engage_cruise(true);
        return;
    }

    if (bEngaged != 0 && Flag3d == 0)
        Thrust.engage_cruise(true);
}

// Slot 4, the branch (module storage-062d65ea, RET 0).
void MatchThrottle2Idea::Commit()
{
    SystemManager* const Sys = Handle;
    Vector const Zero{ 0.0f, 0.0f, 0.0f };
    Sys->Thrust.SetNudgeDirection(Zero);                            // the routine
    if (Flag39 == 0)
    {
        Sys->Thrust.SetThrottle(0.0f, 0);                           // the routine
        Sys->Thrust.engage_cruise(false);
    }
    Sys->Thrust.SetThrustersActive(false);                          // the routine
    MatchPathfindOf(Context)->CachedTargetPosition = Vector{ 0.0f, 0.0f, 0.0f };
}

// (module storage-062d665f, RET 8).
void MatchThrottle2Idea::SetWatchAndPosition(void const* Object2, Vector const& Point)
{
    // `raw = Object2 ? Object2 + 8 : 0; if (raw != [EDI]) set_pointer(raw)` == set().
    Watcher.set(Object2 != nullptr
        ? reinterpret_cast<Watchable const*>(static_cast<unsigned char const*>(Object2) + 8)
        : nullptr);
    Position = Point;
    Distance28 = 0.0f;
    MirrorWatchedPosition(Context, Watcher);
}

// (module storage-062d66fd, RET 8). Same as the routine except that the
// point is zeroed (three zero locals copied in) and the float lands in +0x28.
void MatchThrottle2Idea::SetWatchAndDistance(void const* Object2, float Distance)
{
    Watcher.set(Object2 != nullptr
        ? reinterpret_cast<Watchable const*>(static_cast<unsigned char const*>(Object2) + 8)
        : nullptr);
    Position = Vector{ 0.0f, 0.0f, 0.0f };
    Distance28 = Distance;
    MirrorWatchedPosition(Context, Watcher);
}

// Slot 1, the branch (module storage-062d6531, RET 4). DeltaTime is never read.
// Frame names below are B+n for [ESP0+n], ESP0 = ESP after `SUB ESP,0x64`.
int MatchThrottle2Idea::Update(float DeltaTime)
{
    (void)DeltaTime;
    Flag3d = 0;
    IObjRW* const Watched = MatchWatched(Watcher);
    if (Watched == nullptr || Object == nullptr)
        return 3;

    IObjRW* const Owner = static_cast<IObjRW*>(Object);
    SystemManager* const Sys = Handle;
    FAiTarget* const Nav = &Handle->SelfTarget;

    // own cached centre minus the watched position, built through
    // Vec3Assign (the routine) from three FSTP'd floats into B+0x28.
    Nav->RefreshPos();
    Vector const& WatchedPos = Watched->get_position();             // [EAX]
    Vector Delta;
    Delta.z = Nav->CachedPos.z - WatchedPos.z;
    Delta.y = Nav->CachedPos.y - WatchedPos.y;
    Delta.x = Nav->CachedPos.x - WatchedPos.x;

    // the distance by the 0x5f3759df trick. (x*x + y*y) + z*z stays
    // on the x87 stack; FST [B+0x14] rounds it for the bit pattern; the seed goes to B+8;
    // DistSq * s * (1.5 - 0.5 * s * s * DistSq) is FSTP'd over B+0x14.
    double const DistSq = (static_cast<double>(Delta.x) * Delta.x + static_cast<double>(Delta.y) * Delta.y) +
                          static_cast<double>(Delta.z) * Delta.z;
    float const DistSqF = static_cast<float>(DistSq);
    int DistSqBits;
    std::memcpy(&DistSqBits, &DistSqF, sizeof(DistSqBits));
    int const SeedBits = 0x5f3759df - (DistSqBits >> 1);           // SAR ECX,1; SUB
    float Seed;
    std::memcpy(&Seed, &SeedBits, sizeof(Seed));
    float const Distance = static_cast<float>(
        ((1.5f - ((static_cast<double>(Seed) * Seed) * DistSq) * 0.5f) * Seed) * DistSq);   // B+0x14

    // Flag38 asks the watched ship whether it cruises / rides a lane
    // (slot 23 is_cruise_active(bool&, bool&) into B+2 / B+3, both pre-cleared).
    bool bWatchedCruise = false;
    bool bWatchedLane = false;
    if (Flag38 != 0)
        Watched->is_cruise_active(bWatchedCruise, bWatchedLane);

    // both speeds, (x*x + y*y) + z*z then FSQRT, FSTP float. The
    // watched one lands on B+8 (over the dead seed), the own one on B+0x10.
    Vector const WatchedVel = Watched->get_velocity();
    float const WatchedSpeed = static_cast<float>(std::sqrt(
        (static_cast<double>(WatchedVel.x) * WatchedVel.x + static_cast<double>(WatchedVel.y) * WatchedVel.y) +
        static_cast<double>(WatchedVel.z) * WatchedVel.z));
    Vector const OwnVel = Owner->get_velocity();
    float const OwnSpeed = static_cast<float>(std::sqrt(
        (static_cast<double>(OwnVel.x) * OwnVel.x + static_cast<double>(OwnVel.y) * OwnVel.y) +
        static_cast<double>(OwnVel.z) * OwnVel.z));

    // the throttle that matches the watched speed -- cruise type 4
    // only when cruising and not in a lane, engine type 1 otherwise. B+0xc.
    ThrustEquipType const MatchType = (bWatchedCruise && !bWatchedLane) ? ThrustEquipCruise : ThrustEquipEngine;
    float const MatchThrottle = Behavior_InvokeManagerSlotD4(Owner, WatchedSpeed, MatchType);

    // B+0x1c: the aim vector, from the own centre.
    Vector Aim;
    // `FLD [+0x28]; FCOMP 0; TEST AH,0x41; JNZ the branch`.
    if (Distance28 > 0.0f)
    {
        // the routine writes the watched physical centre into B+0x28
        // (Delta's slot, dead by now).
        Vector WatchedCenter;
        GetIfacePositionOrZero(&WatchedCenter, Watched);

        // Distance28 is parked at B+4; `Distance28 * 0.95 (double
        // ) FCOMPP Distance; TEST AH,0x41; JNZ` -- inside 95% of the
        // standoff the aim is zero.
        if (static_cast<double>(Distance28) * 0.95 > Distance)
        {
            Aim = Vector{ 0.0f, 0.0f, 0.0f };
        }
        else
        {
            // the standoff point. R = max(radius, atmosphere) of the watched
            // object (the routine) + own cached radius (B+0x18) + Distance28 (B+4).
            Nav->RefreshPos();
            float const OwnRadius = Nav->CachedRadius;
            float const Standoff = static_cast<float>(
                (static_cast<double>(Behavior_GetIfaceMaxScalarAttribute(Watched)) + OwnRadius) + Distance28);
            // unit vector from the watched centre to the own centre.
            Nav->RefreshPos();
            Vector Away;
            Away.z = Nav->CachedPos.z - WatchedCenter.z;
            Away.y = Nav->CachedPos.y - WatchedCenter.y;
            Away.x = Nav->CachedPos.x - WatchedCenter.x;
            Vector AwayDir;
            VecNormalize3(&Away, &AwayDir);
            // (Standoff * dir) FSTP'd, then + the watched centre.
            Aim.x = Standoff * AwayDir.x;
            Aim.y = Standoff * AwayDir.y;
            Aim.z = Standoff * AwayDir.z;
            Aim.x = Aim.x + WatchedCenter.x;
            Aim.y = Aim.y + WatchedCenter.y;
            Aim.z = Aim.z + WatchedCenter.z;
            // relative to the own centre.
            Nav->RefreshPos();
            Aim.x = Aim.x - Nav->CachedPos.x;
            Aim.y = Aim.y - Nav->CachedPos.y;
            Aim.z = Aim.z - Nav->CachedPos.z;
        }
        Position = WatchedCenter;                                   // (from B+0x28)
    }
    else
    {
        // the fixed point relative to the own centre.
        Nav->RefreshPos();
        Aim.x = Position.x - Nav->CachedPos.x;
        Aim.y = Position.y - Nav->CachedPos.y;
        Aim.z = Position.z - Nav->CachedPos.z;
    }

    // both forward vectors -- the watched one by the routine (B+0x40), the
    // own one from ExternalIface's transform (column 2 negated, Vec3Assign into B+0x4c).
    Vector WatchedFwd;
    GetNegatedIfaceDirection(&WatchedFwd, Watched);
    Transform const& OwnXf = Sys->SelfTarget.Iface->get_transform();
    Vector OwnFwd;
    OwnFwd.x = -OwnXf.Rotation.E[0][2];                             // +0x08
    OwnFwd.y = -OwnXf.Rotation.E[1][2];                             // +0x14
    OwnFwd.z = -OwnXf.Rotation.E[2][2];                             // +0x20

    float Throttle;                                                 // B+4
    int Mode;                                                       // EDI
    // (Own.x*W.x + Own.z*W.z) + Own.y*W.y. `TEST AH,0x41; JZ` takes
    // the closing-speed path when greater than 0, and so does Distance28 > 0.
    double const Facing = (static_cast<double>(OwnFwd.x) * WatchedFwd.x + static_cast<double>(OwnFwd.z) * WatchedFwd.z) +
                          static_cast<double>(OwnFwd.y) * WatchedFwd.y;
    if (!(Facing > 0.0) && !(Distance28 > 0.0f))
    {
        Throttle = 1.0f;
    }
    else
    {
        // closing = |Aim| * clamp(dot(WatchedFwd, unit(Aim))), i.e. how far
        // ahead of the owner, along the watched heading, the aim point lies.
        Vector AimDir;
        VecNormalize3(&Aim, &AimDir);                               // into B+0x58
        double const Cos = ClampedDot3(WatchedFwd, AimDir);
        double const AimLen = std::sqrt((static_cast<double>(Aim.x) * Aim.x + static_cast<double>(Aim.z) * Aim.z) +
                                        static_cast<double>(Aim.y) * Aim.y);
        float const Closing = static_cast<float>(AimLen * Cos);     // B+4

        // get_speed(1.0f, type) is called and its result popped
        // unread (FSTP ST0 at 062d61f5); Speed2c takes the matching cached speed.
        if (bWatchedCruise)
        {
            (void)Behavior_InvokeManagerSlotD8(Owner, 1.0f, ThrustEquipCruise);
            Speed2c = Speed4c;
            Mode = 4;
        }
        else
        {
            (void)Behavior_InvokeManagerSlotD8(Owner, 1.0f, ThrustEquipEngine);
            Speed2c = Speed50;
            Mode = 1;
        }

        ThrottleBias30 = 0.0f;
        // `FLD |Closing|; FLD [+0x54]; FCOMPP; TEST AH,0x5; JP` --
        // the bias block runs only when ClosingWindow54 < |Closing|.
        if (ClosingWindow54 < std::fabs(Closing))
        {
            ThrottleBias30 = static_cast<float>(static_cast<double>(Closing) / Speed2c);
            // three `TEST AH,0x41; JNZ` gates, each needs greater.
            if (static_cast<double>(WatchedSpeed) > 0.01 && OwnSpeed > WatchedSpeed && Closing > 0.0f)
            {
                // time to slow from the own speed to the watched one,
                // and the distance covered meanwhile (FST into +0x2c).
                ThrustEquipType const Type = static_cast<ThrustEquipType>(Mode);
                float const SlowTime = Behavior_QueryRadiusEffect(Owner, 0.0f, OwnSpeed, WatchedSpeed, Type);
                float const SlowDistance = Behavior_QueryFalloffEffect(Owner, 0.0f, OwnSpeed, SlowTime, Type);
                Speed2c = SlowDistance;
                // `FLD Closing; FCOMP; TEST AH,0x41; JP` falls through on
                // Closing <= SlowDistance; then `FCOMP 0.01; TEST AH,0x41; JNZ` needs
                // SlowDistance > 0.01 to drop the bias.
                if (Closing <= SlowDistance && static_cast<double>(SlowDistance) > 0.01)
                    ThrottleBias30 = 0.0f;
            }
            // `TEST AH,0x5; JP` falls through on Closing < ClosingFloor40.
            if (Closing < ClosingFloor40)
                ThrottleBias30 = ClosingBias44;
        }

        // FLD MatchThrottle; FADD bias; FST B+4; negative -> 0.
        Throttle = static_cast<float>(static_cast<double>(MatchThrottle) + ThrottleBias30);
        if (Throttle < 0.0f)
            Throttle = 0.0f;
    }

    // .
    if (bWatchedCruise)
    {
        Mode = 2;
        // `TEST AH,0x41; JNZ` needs MatchThrottle > 0.8f, then `TEST
        // AH,0x5; JP` falls through on Throttle < 0.8f.
        if (MatchThrottle > 0.8f && Throttle < 0.8f)
            Throttle = 0.8f;
    }
    else
    {
        Mode = 0;
    }

    // target behind the bearing cone and more than 200 out -> go straight
    // to the gate with the throttle (Mode 2) or with 0.
    if (BearingAngleToPoint(Owner, Position) > MaxBearing34 &&
        GetSignedRadialClearanceToPoint(Nav, Position) > 200.0f)
    {
        Sys->Thrust.SetThrottle((Mode == 2) ? Throttle : 0.0f, Mode);
    }
    else
    {
        // catch up when too far behind.
        if (Flag3c != 0 && Distance > CatchUpRange58)
        {
            Flag3d = 1;
            Throttle = 1.2f;
            Mode = 2;
        }
        // afterburner notify.
        if (Flag3b != 0 || Sys->Timers.IsTimer4Active())
        {
            if (!Sys->SelfTarget.Iface->is_player() && Mode == 0)   // / 062d63ba
                Sys->Thrust.SetThrustersActive(Throttle > 2.0f);
        }
        else
        {
            Sys->Thrust.SetThrustersActive(false);
        }
        Sys->Thrust.SetThrottle(Throttle, Mode);
    }

    // no nudge matching while PathfindManager::Field_214 bit 0 is set
    // or Flag3a is clear.
    if ((MatchPathfindOf(Context)->Field_214 & 1) != 0 || Flag3a == 0)
        return 0;

    // the watched ship's nudge vector (slot 21) and its length,
    // (z*z + y*y) + x*x, FST to B+0x18 and compared unrounded with 0.1f.
    Vector Nudge{ 0.0f, 0.0f, 0.0f };
    Watched->get_nudge_vec(Nudge);
    double const NudgeLenD = std::sqrt((static_cast<double>(Nudge.z) * Nudge.z + static_cast<double>(Nudge.y) * Nudge.y) +
                                       static_cast<double>(Nudge.x) * Nudge.x);
    float const NudgeLen = static_cast<float>(NudgeLenD);
    if (!(NudgeLenD > 0.1f))
    {
        Vector const Zero{ 0.0f, 0.0f, 0.0f };
        Sys->Thrust.SetNudgeDirection(Zero);
        return 0;
    }

    // watched speed for that nudge (slot 0xd8, type 3), the own
    // throttle for that speed (slot 0xd4, type 3), along the unit nudge, into
    // IObjAffect::set_nudge_vec (Object+4, slot 2). Direct vcalls, no null tests.
    float const NudgeSpeed = Watched->get_speed(NudgeLen, ThrustEquipTradelane);
    float const NudgeThrottle = Owner->get_projected_throttle(NudgeSpeed, ThrustEquipTradelane);
    Vector NudgeDir;
    VecNormalize3(&Nudge, &NudgeDir);
    Vector Out;
    Out.x = NudgeThrottle * NudgeDir.x;
    Out.y = NudgeThrottle * NudgeDir.y;
    Out.z = NudgeThrottle * NudgeDir.z;
    static_cast<IObjAffect*>(Owner)->set_nudge_vec(Out);
    return 0;
}
