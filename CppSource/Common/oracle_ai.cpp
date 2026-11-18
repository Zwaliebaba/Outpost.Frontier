// Oracle instrumentation (P2.5 ai): the exported wrappers over Common's internal AI helpers
// for the suites of `Oracle.exe game-sim`, the PhySys query hook and the behaviour-manager
// reader of the `Oracle.exe sim` recorder (include/oracle_ai.h). Nothing in the game calls
// these; GOracleAiHooks stays zero outside the harness.

#include "oracle_ai.h"

// The include order of the PhySys translation units (oracle_sim_hooks.cpp).
#include "common_physys.h"
#include "common_cobject.h"
#include "common_iface.h"
#include "common_newchar.h"        // IStateGraph, pub::StateGraph::LoadFromFile / Free
#include "common_pub.h"            // pub::AI::Personality
#include "common_systemmanager.h"  // SystemManager, FAiTarget
#include "common_csys.h"           // Matrix, Transform
#include "common_aisteer.h"
#include "common_aisteer_internal.h"  // CmnAiSteer::SteeringFrameFromRelativeDir
#include "common_misc.h"           // RandomVector

#include <cstring>
#include <new>

FOracleAiHooks GOracleAiHooks = {};

namespace
{
    Vector ToVector(float const* In)
    {
        Vector Out;
        Out.x = In[0];
        Out.y = In[1];
        Out.z = In[2];
        return Out;
    }

    void FromVector(float* Out, Vector const& In)
    {
        Out[0] = In.x;
        Out[1] = In.y;
        Out[2] = In.z;
    }

    Matrix ToMatrix(float const* In)
    {
        Matrix Out;
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Column = 0; Column < 3; ++Column)
                Out.E[Row][Column] = In[Row * 3 + Column];
        }
        return Out;
    }

    void FromMatrix(float* Out, Matrix const& In)
    {
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Column = 0; Column < 3; ++Column)
                Out[Row * 3 + Column] = In.E[Row][Column];
        }
    }

    Transform ToTransform(float const* In)
    {
        Transform Out;
        Out.Rotation = ToMatrix(In);
        Out.Translation = ToVector(In + 9);
        return Out;
    }

    OrientedBox ToBox(float const* In)
    {
        OrientedBox Out;
        Out.Center = ToVector(In);
        Out.AxisX = ToVector(In + 3);
        Out.AxisY = ToVector(In + 6);
        Out.AxisZ = ToVector(In + 9);
        Out.HalfExtent = ToVector(In + 12);
        return Out;
    }

    void ToCapsule(float const* In, float& Radius, Vector& Start, Vector& Dir)
    {
        Radius = In[0];
        Start = ToVector(In + 1);
        Dir = ToVector(In + 4);
    }
}

int __cdecl OracleAiRaySphere(float const* Origin, float const* Dir, float const* Center, float Radius, float* OutT)
{
    return RaySphereIntersect(ToVector(Origin), ToVector(Dir), ToVector(Center), Radius, OutT) ? 1 : 0;
}

int __cdecl OracleAiRayAabb(float const* LocalOrigin, float const* LocalDir, float const* HalfExtent, float* TMin,
                            float* TMax)
{
    return RayAABBIntersect(ToVector(LocalOrigin), ToVector(LocalDir), ToVector(HalfExtent), *TMin, *TMax) ? 1 : 0;
}

int __cdecl OracleAiRayOrientedBox(float const* Origin, float const* Dir, float const* Box, int* OutHitType,
                                   float* OutPoints)
{
    Vector Points[2] = {ToVector(OutPoints), ToVector(OutPoints + 3)};
    OrientedBox const B = ToBox(Box);
    bool const bHit = RayOrientedBoxIntersect(ToVector(Origin), ToVector(Dir), B, OutHitType, Points);
    FromVector(OutPoints, Points[0]);
    FromVector(OutPoints + 3, Points[1]);
    return bHit ? 1 : 0;
}

int __cdecl OracleAiRayTransformedBoxNearest(float const* Origin, float const* Dir, float const* Xform,
                                             float const* Aabb, float* OutPoint, int* OutStartsInside)
{
    Vector Point = ToVector(OutPoint);
    bool bStartsInside = false;
    Transform const X = ToTransform(Xform);
    bool const bHit = RayTransformedBoxNearestHit(ToVector(Origin), ToVector(Dir), X, Aabb, &Point, &bStartsInside);
    FromVector(OutPoint, Point);
    *OutStartsInside = bStartsInside ? 1 : 0;
    return bHit ? 1 : 0;
}

int __cdecl OracleAiBoxesIntersect(float const* XformA, float const* AabbA, float const* XformB, float const* AabbB)
{
    Transform const A = ToTransform(XformA);
    Transform const B = ToTransform(XformB);
    return TransformedBoxesIntersect(A, AabbA, B, AabbB) ? 1 : 0;
}

void __cdecl OracleAiPerpendicularBasis(float* Dir, int bAlreadyNormalized, float* Out1, float* Out2)
{
    Vector D = ToVector(Dir);
    Vector A;
    Vector B;
    BuildPerpendicularBasis(&A, &B, &D, bAlreadyNormalized != 0);
    FromVector(Dir, D);
    FromVector(Out1, A);
    FromVector(Out2, B);
}

int __cdecl OracleAiCapsuleIntersect(float const* Origin, float const* Dir, float const* Capsule, float* OutT)
{
    float Radius = 0.0f;
    Vector Start;
    Vector SegDir;
    ToCapsule(Capsule, Radius, Start, SegDir);
    return RaySegmentCapsuleIntersect(ToVector(Origin), ToVector(Dir), Radius, Start, SegDir, OutT);
}

int __cdecl OracleAiCapsuleFirstHit(float const* Origin, float const* Dir, float const* Capsule, float* OutPoint)
{
    float Radius = 0.0f;
    Vector Start;
    Vector SegDir;
    ToCapsule(Capsule, Radius, Start, SegDir);
    Vector Point;
    bool const bHit = RaySegmentCapsuleFirstHit(ToVector(Origin), ToVector(Dir), Radius, Start, SegDir, &Point);
    FromVector(OutPoint, Point);
    return bHit ? 1 : 0;
}

int __cdecl OracleAiPointInCapsule(float const* Point, float const* Capsule)
{
    float Radius = 0.0f;
    Vector Start;
    Vector SegDir;
    ToCapsule(Capsule, Radius, Start, SegDir);
    return PointInCapsule(ToVector(Point), Radius, Start, SegDir) ? 1 : 0;
}

double __cdecl OracleAiClampedDot3(float const* A, float const* B)
{
    return ClampedDot3(ToVector(A), ToVector(B));
}

int __cdecl OracleAiSweptSphere(float RadiusA, float const* A0, float const* A1, float RadiusB, float const* B0,
                                float const* B1, float* OutT)
{
    return SweptSphereCollisionTime(RadiusA, ToVector(A0), ToVector(A1), RadiusB, ToVector(B0), ToVector(B1), OutT)
               ? 1
               : 0;
}

void __cdecl OracleAiOrientationBasis(float const* Reference, float const* Forward, float const* UpHint, float* Out)
{
    Matrix Result;
    Matrix const Ref = ToMatrix(Reference);
    Vector const Hint = UpHint != nullptr ? ToVector(UpHint) : Vector();
    BuildOrientationBasis(Result, Ref, ToVector(Forward), UpHint != nullptr ? &Hint : nullptr);
    FromMatrix(Out, Result);
}

int __cdecl OracleAiPerpendicularFrame(float const* Direction, float DotThreshold, float const* Reference, float* Out)
{
    Matrix Frame = ToMatrix(Out);
    Vector const Ref = Reference != nullptr ? ToVector(Reference) : Vector();
    bool const bOk = BuildPerpendicularFrame(ToVector(Direction), &Frame, DotThreshold,
                                             Reference != nullptr ? &Ref : nullptr);
    FromMatrix(Out, Frame);
    return bOk ? 1 : 0;
}

void __cdecl OracleAiSteeringFrame(float const* Reference, float const* RelDir, float const* UpHint, float* Out)
{
    Matrix Result;
    CmnAiSteer::SteeringFrameFromRelativeDir(Result, ToMatrix(Reference), ToVector(RelDir), ToVector(UpHint));
    FromMatrix(Out, Result);
}

void __cdecl OracleAiRandomVector(float Radius, float* Out)
{
    FromVector(Out, RandomVector(Radius));
}

void __cdecl OracleAiRandomSpreadFrame(float const* Reference, float SpreadAngle, float BankAngle, float* Out)
{
    Matrix Result;
    BuildRandomSpreadFrame(Result, ToMatrix(Reference), SpreadAngle, BankAngle);
    FromMatrix(Out, Result);
}

void __cdecl OracleAiTransformVectorByMatrix(float const* M, float const* V, float* Out)
{
    FromVector(Out, TransformVectorByMatrix(ToMatrix(M), ToVector(V)));
}

void __cdecl OracleAiRotateTowardBySlerp(float const* From, float const* To, float Angle, float* Out)
{
    FromVector(Out, RotateTowardBySlerp(ToVector(From), ToVector(To), Angle));
}

float __cdecl OracleAiAngularThrottle(float const* A, float const* B, float* OutThrottle)
{
    float Angle = 0.0f;
    Vector const Throttle = TurnHelper::get_angular_throttle(ToMatrix(A), ToMatrix(B), &Angle);
    FromVector(OutThrottle, Throttle);
    return Angle;
}

void __cdecl OracleAiLoadStateGraph(char const* Path)
{
    pub::StateGraph::Free();
    pub::StateGraph::LoadFromFile(Path);
}

void __cdecl OracleAiFreeStateGraph()
{
    pub::StateGraph::Free();
}

void __cdecl OracleAiDefaultPersonality(unsigned char* Out)
{
    // Zeroed storage first, so the padding the constructors never write reads as zero.
    alignas(pub::AI::Personality) unsigned char Storage[sizeof(pub::AI::Personality)];
    std::memset(Storage, 0, sizeof(Storage));
    pub::AI::Personality* const P = new (Storage) pub::AI::Personality();
    std::memcpy(Out, P, sizeof(pub::AI::Personality));
}

float __cdecl OracleAiToughnessRatio(int Type)
{
    pub::AI::Personality P;
    return P.get_toughness_ratio(static_cast<pub::AI::Personality::TOUGHNESS_TYPE>(Type));
}

double __cdecl OracleAiRangeThresholdValue(int JobField00)
{
    pub::AI::Personality P;
    P.JobParams.Field_00 = JobField00;
    return P.get_range_threshold_value();
}

unsigned int __cdecl OracleAiObjectId(CObject const* Object)
{
    if (Object == nullptr)
        return 0;
    if ((static_cast<unsigned int>(Object->ObjectClass) & 0x3u) != 0x3u)
        return 0;
    return static_cast<CSimple const*>(Object)->Id;
}

void __cdecl OracleAiReportRayQuery(int Kind, unsigned int SystemId, CObject const* Object, Vector const& From,
                                    Vector const& To, void const* Hits, int MaxHits, int HitCount)
{
    if (GOracleAiHooks.OnQuery == nullptr)
        return;
    PhySys::RayHit const* const RayHits = static_cast<PhySys::RayHit const*>(Hits);
    int const Count = (HitCount < 0) ? 0 : ((HitCount > 64) ? 64 : HitCount);
    FOracleAiHit Out[64];
    for (int Index = 0; Index < Count; ++Index)
    {
        PhySys::RayHit const& Hit = RayHits[Index];
        Out[Index].Object = Hit.Object;
        Out[Index].ObjectId = OracleAiObjectId(Hit.Object);
        FromVector(Out[Index].Point, Hit.Point);
        FromVector(Out[Index].Normal, Hit.Normal);
        Out[Index].PartId = Hit.PartId;
    }
    float const F[3] = {From.x, From.y, From.z};
    float const T[3] = {To.x, To.y, To.z};
    GOracleAiHooks.OnQuery(Kind, SystemId, Object, F, T, 0.0f, MaxHits, Count, Out);
}

void __cdecl OracleAiReportSphereQuery(unsigned int SystemId, Vector const& Center, float Radius,
                                       CObject* const* Objects, int MaxObjects, int Count)
{
    if (GOracleAiHooks.OnQuery == nullptr)
        return;
    int const N = (Count < 0) ? 0 : ((Count > 64) ? 64 : Count);
    FOracleAiHit Out[64];
    std::memset(Out, 0, sizeof(Out));
    for (int Index = 0; Index < N; ++Index)
    {
        Out[Index].Object = Objects[Index];
        Out[Index].ObjectId = OracleAiObjectId(Objects[Index]);
    }
    float const C[3] = {Center.x, Center.y, Center.z};
    GOracleAiHooks.OnQuery(OracleAiQuerySphere, SystemId, nullptr, C, C, Radius, MaxObjects, N, Out);
}

bool __cdecl OracleAiReadManager(IBehaviorManager const* Manager, FOracleAiManager& Out)
{
    std::memset(&Out, 0, sizeof(Out));
    Out.StateGraphId = -1;
    if (Manager == nullptr)
        return false;
    // The same `W ? W - 8 : 0` downcast UpdateBehaviorTick guards with (common_iface.h).
    if (Manager->WatchablePtr != nullptr && static_cast<char*>(Manager->WatchablePtr) - 8 != nullptr)
    {
        Out.Object = ObjRwFromWatchable(Manager->WatchablePtr);
        Out.ObjectId = Out.Object->get_id();
    }
    Out.ObjectDb = Manager->ObjectDb;
    Out.BehaviorIndex = Manager->CurrentBehaviorIndex;
    if (Manager->StateGraph != nullptr)
        Out.StateGraphId = Manager->StateGraph->Id;
    std::memcpy(Out.Directive, Manager->DirectiveStorage, sizeof(Out.Directive));
    Out.ManeuverMask = Manager->ManeuverMask;
    Out.bLockManeuvers = Manager->LockManeuversByte != 0 ? 1 : 0;
    Out.bDirty = Manager->BehaviorDirty != 0 ? 1 : 0;
    Out.bSuppressed = Manager->TickSuppressedFlag != 0 ? 1 : 0;
    Out.TimerA = Manager->BehaviorTimerA;
    Out.TimerB = Manager->BehaviorTimerB;
    Out.TimerC = Manager->BehaviorTimerC;
    Out.AttackOverrideTimer = Manager->AttackOverrideTimer;
    Out.AttackOverridePeriod = Manager->AttackOverridePeriod;
    Out.InSightsTargetId = Manager->InSightsTargetId;
    Out.Field118 = Manager->Field118;
    Out.Field11c = Manager->Field11c;
    Out.Field120 = Manager->Field120;
    for (int Index = 0; Index < 21; ++Index)
    {
        Out.ScoreWeights[Index] = Manager->BehaviorScores[Index].Weight;
        Out.ScoreBehaviors[Index] = Manager->BehaviorScores[Index].BehaviorIndex;
    }
    SystemManager const* const Sys = Manager->Sys;
    if (Sys != nullptr)
    {
        FAiTarget const* const Begin = Sys->Timers.Targets.Begin;
        FAiTarget const* const End = Sys->Timers.Targets.End;
        int const Count = (Begin != nullptr && End != nullptr && End > Begin) ? static_cast<int>(End - Begin) : 0;
        Out.TargetCount = Count;
        for (int Index = 0; Index < Count && Index < 64; ++Index)
            Out.Targets[Index] = Begin[Index].Iface;
        Out.bPersonality = 1;
        std::memcpy(Out.Personality, &Sys->PersonalityData, sizeof(Out.Personality));
    }
    return true;
}
