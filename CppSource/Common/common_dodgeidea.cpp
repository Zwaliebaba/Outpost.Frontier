// Author: Giperion.

// Common.dll DodgeIdea (ctor 0x062d77d0, Update the branch). See src/include/common_idea.h.

// CMN-SPLIT stage 1 (2026-10-14): split out of common_idea.cpp (ctor, SetField58, Commit) and
// common_aisteer.cpp (Update), code unchanged.

#include "common_idea.h"
#include "common_idea_internal.h"
#include "common_aisteer.h"   // BearingAngleToPoint, GetIfacePositionOrZero
#include "common_aisteer_internal.h"
#include "common_behavior.h"
#include "common_cequip.h"    // ThrustEquipType (IObjInspect::get_speed's 2nd argument)
#include "common_csys.h"      // Matrix / Transform
#include "common_iface.h"     // IObjInspect / IObjRW / IBehaviorManager / ObjRwFromWatchable
#include "common_pathfind.h"  // PathfindManager (IBehaviorManager::Pathfind)
#include "common_systemmanager.h" // SystemManager / ThrustManager (BaseIdea::Handle)
#include "x86math.h"          // I3DMathEngine::TransformMatrix
#include "x87_trig.h"         // baseline's inline FSIN / FCOS

#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace CmnIdea;
using namespace CmnAiSteer;

// ----------------------------------------------------------------------------
// DodgeIdea -- common_buzzbehavior.h's "WatcherD". See common_idea.h for the
// identification and the two-sided size proof.
// ----------------------------------------------------------------------------

// (item 1616), Common, 85 instructions. Every
// constant below is the raw dword the ctor stores.
DodgeIdea::DodgeIdea()
    : BaseIdea()
    , Picker()
    , IntervalTime(5.0f)                      // , 062d781f
    , IntervalTimeVariancePercent(0.0f)                      // (dword 0)
    , ConeAngle(BitsToFloat(0x3f860a92u))  // NOT pi/3, see the header
    , ConeAngleVariancePercent(0.0f)                      // (dword 0)
    , WaggleAxisConeAngle(0.0f)                      // (dword 0)
    , RollAngle(0.0f)                      // (dword 0)
    , DodgeDistance(150.0f)                    // , 062d7839
    , Field50(0.0f)                      // (dword 0)
    , Field54(BitsToFloat(0x3e4ccccdu))  // 0.2f, 062d7843
    , Field58{}                          // not written by baseline's ctor
    , Field64(0)
    , Pad65{}
    , WorkingFrame(0.0f, 0.0f, 0.0f,     // , `MOV ECX,9; REP STOSD`
                   0.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 0.0f)
    , IntervalTimeLeft(0.0f)                      // (dword 0)
    , TurnThrottle(1.0f)
    , Throttle(0.0f)                      // not written by baseline's ctor
    , Field98(0)
    , Opaque99{}
    , DodgeStyle(1)
    , VectorB8{}                         // , three zeroed dwords
    , FieldC4(30.0f)                     // , 062d7905
    , SlideThrottle(1.0f)
    , VectorCc{ BitsToFloat(0x3dcccccdu), 0.0f, 1.0f } // 0.1f, 0, 1.0f
    , bRollFlipDirection(0)
    , FlagD9(0)
    , FlagDa(0)
    , PadDb(0)
    , FieldDc(1.0f)
{
    // The DirectionPicker sub-object's own initialization (+0x14..+0x33) is
    // FDirectionPicker's job -- see its constructor below; baseline inlines it here,
    // byte-identically with BreakAwayIdea's, except for Flag1c's constant.
    Picker.Flag1c = 0;
}

// Own vtable slot 1 -- the branch (item 1618): DodgeIdea::Update is defined at the end
// of this file; the steering helpers it shares with FaceIdea::Update are in
// common_aisteer.cpp (common_aisteer_internal.h).

// , module storage-062d88dd: three dword copies into +0x58, then
// `MOV byte [ECX+0x64],1`.
void DodgeIdea::SetField58(Vector const& Value)
{
    Field58 = Value;
    Field64 = 1;
}

// Own vtable slot 4 -- the branch, module storage-062d88a8, 28 instructions.
void DodgeIdea::Commit()
{
    IntervalTimeLeft = 0.0f;   // (dword 0)

    // then 062d8873-062d888b: baseline zeroes the SAME 12-byte
    // stack scratch twice, once before each call, and passes it to both; each call
    // recomputes Handle + 0x18 and none tests it.
    Vector ZeroVector{ 0.0f, 0.0f, 0.0f };
    Handle->Thrust.SetNudgeDirection(ZeroVector);

    ZeroVector.x = 0.0f;
    ZeroVector.y = 0.0f;
    ZeroVector.z = 0.0f;
    Handle->Thrust.SetAxisThrottle(ZeroVector);

    Handle->Thrust.SetThrustersActive(false);

    Field50 = 0.0f;   // (dword 0)
}

namespace
{
    // The two in-place row rotations DodgeIdea::Update's caseD_0 composes (loops
    // / 062d7ff6 / 062d80b4 / 062d8137 for (X,Z) and the branch /
    // / 062d8077 / 062d80f8 for (Y,Z)). Cosine and sine stay on the x87
    // stack (double), except in the negated-angle arms, which FSTP the cosine to
    // float first -- those callers pass the rounded value. Each row's old X/Y is
    // kept on the stack, Z is re-read from memory, and each result is FSTP'd:
    //   X' = X*C - S*Z,  Z' = C*Z + X*S
    void RotateFrameXZ(Matrix& M, double C, double S)
    {
        for (int Row = 0; Row < 3; ++Row)
        {
            double const X = M.E[Row][0];
            M.E[Row][0] = static_cast<float>(X * C - S * M.E[Row][2]);
            M.E[Row][2] = static_cast<float>(C * M.E[Row][2] + X * S);
        }
    }

    //   Y' = S*Z + Y*C,  Z' = C*Z - Y*S
    void RotateFrameYZ(Matrix& M, double C, double S)
    {
        for (int Row = 0; Row < 3; ++Row)
        {
            double const Y = M.E[Row][1];
            M.E[Row][1] = static_cast<float>(S * M.E[Row][2] + Y * C);
            M.E[Row][2] = static_cast<float>(C * M.E[Row][2] - Y * S);
        }
    }
}

// / the routine (the byte flags at Timers+0x68 / +0x78) are
// SystemManagerTimerTable::IsTimer4Active / IsTimer5Active (common_sysmgr_timertable.cpp).

// (formerly BlendNudgeIntoPredictedDirection) is ThrustManager::BlendNudgeDirection
// (common_thrustmanager.cpp).

// DodgeIdea::Update -- the branch, item 1618, vtable 063a04c8 slot 1. Re-traced
// 2026-10-08 (cmn-ai10) against Common and made a DodgeIdea member
// (it was the raw-offset free function SteeringTaskLarge_Update). Frame: S = ESP after
// SUB 0x1fc / PUSH EBX,EBP,ESI,EDI; Dt is [S+0x210]; [S+0x70] caches
// &Manager->Pathfind->Field_214 (EBX in both tails); [S+0x3c] is the get_angular_throttle
// out-angle, zeroed at entry; [S+0x10] carries the throttle command of the cruise tail.
int DodgeIdea::Update(float DeltaTime)
{
    // no Object -> 3.
    if (Object == nullptr)
        return 3;

    SystemManager* const Sys = Handle;
    IBehaviorManager* const Manager = static_cast<IBehaviorManager*>(Context);
    FAiTarget* const NavRecord = &Sys->SelfTarget;

    // FLD IntervalTimeLeft; FSUB Dt; FST IntervalTimeLeft; FCOMP 0.0f; TEST AH,0x41; JP
    // caseD_4. The jump is taken on greater or unordered, so the reroll block runs only when
    // the unrounded difference is <= 0.
    double const TimerLeft = static_cast<double>(IntervalTimeLeft) - static_cast<double>(DeltaTime);
    PathfindManager* const Pathfind = Manager->Pathfind;
    IntervalTimeLeft = static_cast<float>(TimerLeft);
    float TurnAngle3c = 0.0f;                              // , [S+0x3c]
    if (TimerLeft <= 0.0)
    {
        // IntervalTimeLeft = IntervalTime - (IntervalTimeVariancePercent * IntervalTime) * (rand() * 2^-15),
        // one rounding (the field == 0x38000000).
        double const Draw8c = static_cast<double>(rand()) * (1.0 / 32768.0);
        IntervalTimeLeft = static_cast<float>(static_cast<double>(IntervalTime) -
                                     (static_cast<double>(IntervalTimeVariancePercent) * IntervalTime) * Draw8c);

        // Field50 = (rand() * Field54) * 2^-15 + 1.0, one rounding.
        Field50 = static_cast<float>((static_cast<double>(rand()) * Field54) * (1.0 / 32768.0) + 1.0);

        // thunk_FUN_062ddcd0 on ECX = Manager->Pathfind, OutDir [S+0x64].
        Vector ProbeDirection{};
        if (Pathfind->GetUserZoneApproachDirection(&ProbeDirection) == 0)
        {
            // BuildOrientationBasis(S+0x74, get_transform(), &ProbeDirection,
            // null), the routine(S+0xe0, S+0x74, 0x3e490fdb (pi/16), 0.0f), 9-dword copy into
            // WorkingFrame, then JMP caseD_4 -- the first switch does not run.
            Matrix Basis;
            BuildOrientationBasis(Basis, *IfaceOrientation(Sys), ProbeDirection, nullptr);
            BuildRandomSpreadFrame(WorkingFrame, Basis, 0.19634955f  , 0.0f);
        }
        else
        {
            // . Both Field98 arms open with FAiTarget::RefreshPos inlined on
            // Sys+0x14fc (gate Sys+0x157a). What follows is dead in baseline and not ported:
            //   Field98 set: Vec3Sub(S+0x4c, &CachedPos, this+0x9c), the routine(S+0x24,
            //     S+0x4c, this+0xa8), Vec3Add(S+0x40, EAX, this+0x9c);
            //   Field98 clear: get_transform(), Vector ctor of -column 2 at S+0x4c.
            // All four helpers are pure, and every one of those slots is rewritten before any
            // read on every path below: S+0x24 (caseD_0's up hint, the caseD_2 tail's cross
            // product, caseD_3's TransformVectorByMatrix out), S+0x40 (caseD_0's up hint,
            // the caseD_2 nudge), S+0x4c (caseD_0's forward, the caseD_2 tail's up hint);
            // no other path reads them.
            RefreshCachedIfacePosition(Sys);

            // CMP DodgeStyle,3; JA caseD_4; jump table 062d87f8.
            switch (DodgeStyle)
            {
            case 0:
            {
                // caseD_0 062d7e35-062d7e75: two draws, each rounded once.
                float const Angle1 = static_cast<float>(
                    (1.0 - (static_cast<double>(rand()) * ConeAngleVariancePercent) * (1.0 / 32768.0)) * ConeAngle);
                double const Draw2 = static_cast<double>(rand()) * (1.0 / 32768.0);
                double const Spread2 = Draw2 * WaggleAxisConeAngle;
                float const Angle2 = static_cast<float>((Spread2 + Spread2) - WaggleAxisConeAngle);

                // up hint (0, 1, 0); Field64 set -> forward Field58,
                // else -column 2 of a first get_transform(); BuildOrientationBasis on a
                // second get_transform() straight into WorkingFrame.
                Vector const UpHint{ 0.0f, 1.0f, 0.0f };
                if (Field64 != 0)
                {
                    BuildOrientationBasis(WorkingFrame, *IfaceOrientation(Sys), Field58, &UpHint);
                }
                else
                {
                    Matrix const& Current = *IfaceOrientation(Sys);
                    Vector const Forward{ -Current.E[0][2], -Current.E[1][2], -Current.E[2][2] };
                    BuildOrientationBasis(WorkingFrame, *IfaceOrientation(Sys), Forward, &UpHint);
                }

                // the routine on the DirectionPicker base; DEC EAX;
                // CMP EAX,3; JA caseD_4.
                switch (GenericWeightedPick(&Picker.LastDirection))
                {
                case 1:   // XZ by Angle1, then YZ by Angle2
                    RotateFrameXZ(WorkingFrame, X87Cos(static_cast<double>(Angle1)),
                                  X87Sin(static_cast<double>(Angle1)));
                    RotateFrameYZ(WorkingFrame, X87Cos(static_cast<double>(Angle2)),
                                  X87Sin(static_cast<double>(Angle2)));
                    break;
                case 2:   // XZ by -Angle1 with its cosine FSTP'd to float
                {
                    double const Neg = -static_cast<double>(Angle1);
                    float const CosNeg = X87CosF(Neg);
                    RotateFrameXZ(WorkingFrame, CosNeg, X87Sin(Neg));
                    RotateFrameYZ(WorkingFrame, X87Cos(static_cast<double>(Angle2)),
                                  X87Sin(static_cast<double>(Angle2)));
                    break;
                }
                case 3:   // YZ by Angle1, then XZ by Angle2
                    RotateFrameYZ(WorkingFrame, X87Cos(static_cast<double>(Angle1)),
                                  X87Sin(static_cast<double>(Angle1)));
                    RotateFrameXZ(WorkingFrame, X87Cos(static_cast<double>(Angle2)),
                                  X87Sin(static_cast<double>(Angle2)));
                    break;
                case 4:   // YZ by -Angle1 with its cosine FSTP'd to float
                {
                    double const Neg = -static_cast<double>(Angle1);
                    float const CosNeg = X87CosF(Neg);
                    RotateFrameYZ(WorkingFrame, CosNeg, X87Sin(Neg));
                    RotateFrameXZ(WorkingFrame, X87Cos(static_cast<double>(Angle2)),
                                  X87Sin(static_cast<double>(Angle2)));
                    break;
                }
                default:
                    break;
                }
                break;
            }

            case 1:
            {
                // caseD_1 062d7bfd-062d7c21: one draw, rounded once into [S+0x10].
                float const Angle1 = static_cast<float>(
                    (1.0 - (static_cast<double>(rand()) * ConeAngleVariancePercent) * (1.0 / 32768.0)) * ConeAngle);

                // the basis built into [S+0x74], UpHint null on both arms.
                Matrix Basis;
                if (Field64 != 0)
                {
                    BuildOrientationBasis(Basis, *IfaceOrientation(Sys), Field58, nullptr);
                }
                else
                {
                    // forward = -column 2 ([S+0x14]); a watched object
                    // (+0x28 non-null and not 8) replaces it with the routine's result,
                    // copied (Flag1c set) or negated through the Vector ctor (Flag1c clear).
                    Matrix const& Current = *IfaceOrientation(Sys);
                    Vector Forward{ -Current.E[0][2], -Current.E[1][2], -Current.E[2][2] };
                    IObjRW* const Watched =
                        ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Picker.Watcher.target())));
                    if (Watched != nullptr)
                    {
                        Vector Direction;
                        GetNegatedIfaceDirection(&Direction, Watched);
                        if (Picker.Flag1c != 0)
                            Forward = Direction;
                        else
                            Forward = Vector{ -Direction.x, -Direction.y, -Direction.z };
                    }
                    BuildOrientationBasis(Basis, *IfaceOrientation(Sys), Forward, nullptr);
                }

                // is_player (ExternalIface slot 0xbc).
                if (!Sys->SelfTarget.Iface->is_player())
                {
                    // -> the branch: spread the basis, bank RollAngle.
                    BuildRandomSpreadFrame(WorkingFrame, Basis, Angle1, RollAngle);
                    break;
                }
                // the routine(Sys+0x18), then the routine(&Field58,
                // [S+0xe0], 0x3f7fbe77 (0.999f), null); both true -> spread that frame, bank 0.
                Matrix Perpendicular;
                if (Sys->Thrust.CheckSteeringGate() &&
                    BuildPerpendicularFrame(Field58, &Perpendicular, 0.999f, nullptr))
                {
                    BuildRandomSpreadFrame(WorkingFrame, Perpendicular, Angle1, 0.0f);
                    break;
                }
                // spread a fresh get_transform(), bank RollAngle.
                BuildRandomSpreadFrame(WorkingFrame, *IfaceOrientation(Sys), Angle1, RollAngle);
                break;
            }

            case 2:
            {
                // caseD_2 062d7b4d-062d7bf8: RandomVector(1.0f) into [S+0x14]; the dot with
                // VectorB8 summed z, y, x on the x87 stack; FCOMP 0.7f (the field);
                // TEST AH,0x41; JNZ keeps the draw on less, equal or unordered.
                Vector const Draw = RandomVector(1.0f);
                double const Dot = (static_cast<double>(Draw.z) * VectorB8.z +
                                    static_cast<double>(Draw.y) * VectorB8.y) +
                                   static_cast<double>(Draw.x) * VectorB8.x;
                if (Dot > static_cast<double>(0.7f))
                    VectorB8 = Vector{ -Draw.x, -Draw.y, -Draw.z };
                else
                    VectorB8 = Draw;
                break;
            }

            case 3:
                // caseD_3 062d8160-062d8172.
                if (bRollFlipDirection != 0)
                    VectorCc.z = -VectorCc.z;
                break;

            default:
                break;
            }
        }
    }

    // caseD_4 062d8178-062d820f: Pathfind->Field_214 == 2 -> WorkingFrame =
    // BuildOrientationBasis(get_transform(), Field_22C - CachedPos (each FSUB FSTP'd), null).
    if (Pathfind->Field_214 == 2)
    {
        RefreshCachedIfacePosition(Sys);
        Vector const RecomputeDir = Sub(Pathfind->Field_22C, CachedIfacePosition(Sys));
        BuildOrientationBasis(WorkingFrame, *IfaceOrientation(Sys), RecomputeDir, nullptr);
    }

    // [S+0x10] = Throttle; with a watched object (+0x28 non-null and
    // not 8) and a non-player ship, the clearance blend may replace it.
    float ThrottleCommand = Throttle;
    IObjRW* const Target = ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Picker.Watcher.target())));
    if (Target != nullptr && !Sys->SelfTarget.Iface->is_player())
    {
        // the routine, FSTP float [S+0x20].
        float const Clearance = static_cast<float>(GetInterceptClearanceOfIface(NavRecord, Target));
        if (Picker.Flag1c != 0)
        {
            IObjInspect const* const SelfIface = ObjInspectOf(Object);
            // the routine(Target, 1.0f, 1) FSTP'd into [S+0x10].
            float Value = Behavior_InvokeManagerSlotD8(ObjInspectOf(Target), 1.0f, ThrustEquipEngine);

            // Limit = Field50 * DodgeDistance stays on the stack. FCOMPP
            // (Limit + 5.0) against Clearance, TEST AH,0x5 / JP: only "less" scales by
            // FieldDc; else FCOMPP (Limit - 5.0), TEST AH,0x41 / JNZ: only "greater" asks
            // the own ship's speed; otherwise Value is unchanged.
            double const Limit = static_cast<double>(Field50) * DodgeDistance;
            if (Limit + 5.0 < static_cast<double>(Clearance))
                Value = static_cast<float>(static_cast<double>(Value) * FieldDc);
            else if (Limit - 5.0 > static_cast<double>(Clearance))
                Value = Behavior_InvokeManagerSlotD8(SelfIface, 1.0f, ThrustEquipEngine);

            // FCOMP double 50.0, TEST AH,0x5 / JP: only "less" clamps.
            if (static_cast<double>(Value) < 50.0)
                Value = 50.0f;

            // the routine(Object, Value, 1) FSTP'd into [S+0x10].
            ThrottleCommand = Behavior_InvokeManagerSlotD4(SelfIface, Value, ThrustEquipEngine);
        }
    }

    // the routine(Sys+0x34) -> FlagDa = 1, the routine(Sys+0x18, 1),
    // [S+0x10] = 1.0f; else a set FlagDa sends the routine(Sys+0x18, 0), and FlagDa = 0.
    if (Sys->Timers.IsTimer4Active())
    {
        FlagDa = 1;
        Sys->Thrust.SetThrustersActive(true);
        ThrottleCommand = 1.0f;
    }
    else
    {
        if (FlagDa != 0)
            Sys->Thrust.SetThrustersActive(false);
        FlagDa = 0;
    }

    // the routine(Sys+0x18, [S+0x10], 0); the result is not used.
    Sys->Thrust.SetThrottle(ThrottleCommand, 0);

    // second dispatch on DodgeStyle (table 062d8818: 0 and 1 share a body).
    switch (DodgeStyle)
    {
    case 0:
    case 1:
    {
        // get_angular_throttle(get_transform(), WorkingFrame, &[S+0x3c])
        // into [S+0x14].
        Vector AxisThrottle = TurnHelper::get_angular_throttle(*IfaceOrientation(Sys), WorkingFrame,
                                                               &TurnAngle3c);
        // each component * TurnThrottle, FSTP'd to float.
        AxisThrottle.x = AxisThrottle.x * TurnThrottle;
        AxisThrottle.y = AxisThrottle.y * TurnThrottle;
        AxisThrottle.z = AxisThrottle.z * TurnThrottle;

        // ComputeBankedSteeringFrame(Sys+0x14fc, get_ship_up_direction(),
        // WorkingFrame, &[S+0x14], 1).
        Vector const Up = Manager->get_ship_up_direction();
        ComputeBankedSteeringFrame(NavRecord, Up, WorkingFrame, AxisThrottle, 1);

        // the routine(Out, Sys->Watcher object, [S+0x14] by value,
        // WorkingFrame, 0.0f, 0x3ec90fdb (pi/8), 1), copied back into [S+0x14].
        IObjRW* const TurnObject =
            ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Sys->Watcher.target())));
        Vector Limited;
        LimitAxisThrottleForTurn(Limited, ObjInspectOf(TurnObject), AxisThrottle, &WorkingFrame,
                                 0.0f, 0.3926991f  , true);
        AxisThrottle = Limited;

        // the routine(Sys+0x34) -> [S+0x14] = the routine(get_transform(),
        // (0, 0, 1)).
        if (Sys->Timers.IsTimer5Active())
            AxisThrottle = TransformVectorByMatrix(*IfaceOrientation(Sys), Vector{ 0.0f, 0.0f, 1.0f });

        // / the branch: the routine(Sys+0x18, &[S+0x14]).
        Sys->Thrust.SetAxisThrottle(AxisThrottle);
        return 0;
    }

    case 2:
    {
        // caseD_2 062d839a: CMP [Pathfind+0x214],0; JNZ caseD_4 (return 0, no refresh call).
        if (Pathfind->Field_214 != 0)
            return 0;

        // |column2 . VectorB8| (summed z, y, x on the stack) FCOMP
        // double 0x3FEFFF2E40000000 (== (double)0.9999f); TEST AH,0x5 / JP: only "less"
        // takes the nudge arm.
        Matrix const& Current = *IfaceOrientation(Sys);
        double const Dot = (static_cast<double>(Current.E[2][2]) * VectorB8.z +
                            static_cast<double>(Current.E[1][2]) * VectorB8.y) +
                           static_cast<double>(Current.E[0][2]) * VectorB8.x;
        if (std::fabs(Dot) < static_cast<double>(0.9999f))
        {
            // on a second get_transform(), C = column 2; the cross
            // (C.z*W.y - C.y*W.z, C.x*W.z - C.z*W.x, C.y*W.x - C.x*W.y) on the stack, each
            // FSTP'd into [S+0x24]; the routine([S+0xa4], [S+0x24]); SlideThrottle * each
            // component into [S+0x40]; the routine(Sys+0x18, &[S+0x40], Dt).
            Matrix const& Second = *IfaceOrientation(Sys);
            double const Cx = Second.E[0][2];
            double const Cy = Second.E[1][2];
            double const Cz = Second.E[2][2];
            Vector const Cross{ static_cast<float>(Cz * VectorB8.y - Cy * VectorB8.z),
                                static_cast<float>(Cx * VectorB8.z - Cz * VectorB8.x),
                                static_cast<float>(Cy * VectorB8.x - Cx * VectorB8.y) };
            Vector Normalized;
            VecNormalize3(&Cross, &Normalized);
            Vector const Nudge{ SlideThrottle * Normalized.x, SlideThrottle * Normalized.y,
                                SlideThrottle * Normalized.z };
            Sys->Thrust.BlendNudgeDirection(Nudge, DeltaTime);
        }
        else
        {
            // column 0 of a second get_transform() into [S+0x58],
            // (Sys+0x18, &[S+0x58]).
            Matrix const& Second = *IfaceOrientation(Sys);
            Vector const Right{ Second.E[0][0], Second.E[1][0], Second.E[2][0] };
            Sys->Thrust.SetNudgeDirection(Right);
        }

        // up hint (0, 1, 0) at [S+0x4c], -column 2 of a fresh
        // get_transform() at [S+0x64], the routine([S+0x74], another get_transform(),
        // [S+0x64], [S+0x4c]).
        Vector const UpHint{ 0.0f, 1.0f, 0.0f };
        Matrix const& Current2 = *IfaceOrientation(Sys);
        Vector const NegForward{ -Current2.E[0][2], -Current2.E[1][2], -Current2.E[2][2] };
        Matrix Frame;
        SteeringFrameFromRelativeDir(Frame, *IfaceOrientation(Sys), NegForward, UpHint);

        // get_angular_throttle(get_transform(), Frame, &[S+0x3c]) into
        // [S+0x14]; no TurnThrottle scaling on this path.
        Vector AxisThrottle = TurnHelper::get_angular_throttle(*IfaceOrientation(Sys), Frame,
                                                               &TurnAngle3c);

        // .
        Vector const Up = Manager->get_ship_up_direction();
        ComputeBankedSteeringFrame(NavRecord, Up, Frame, AxisThrottle, 1);

        // the same limit as case 0/1 against the local Frame.
        IObjRW* const TurnObject =
            ObjRwFromWatchable(const_cast<void*>(static_cast<void const*>(Sys->Watcher.target())));
        Vector Limited;
        LimitAxisThrottleForTurn(Limited, ObjInspectOf(TurnObject), AxisThrottle, &Frame,
                                 0.0f, 0.3926991f  , true);
        AxisThrottle = Limited;

        // -> the branch.
        if (Sys->Timers.IsTimer5Active())
            AxisThrottle = TransformVectorByMatrix(*IfaceOrientation(Sys), Vector{ 0.0f, 0.0f, 1.0f });

        Sys->Thrust.SetAxisThrottle(AxisThrottle);
        return 0;
    }

    case 3:
    {
        // caseD_3 062d87a1-062d87e4: the routine([S+0x24], get_transform(), VectorCc by
        // value), then the routine(Sys+0x18, &[S+0x24]).
        Vector Transformed = TransformVectorByMatrix(*IfaceOrientation(Sys), VectorCc);
        Sys->Thrust.SetAxisThrottle(Transformed);
        return 0;
    }

    default:
        // DodgeStyle above 3 -> caseD_4, return 0 without the refresh call.
        return 0;
    }
}
