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
#include "common_idea.h"           // MoveIdea

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

// ---------------------------------------------------------------------------
// The navigation suites (Oracle/oracle_ai_nav.cpp): an IObjInspect that answers from an
// FOracleAiObject, and the wrappers over the intercept and obstacle-avoidance helpers.
// ---------------------------------------------------------------------------

namespace
{
    float const Zero3[3] = {0.0f, 0.0f, 0.0f};

    Vector ToVec(float const* In)
    {
        Vector Out;
        Out.x = In[0];
        Out.y = In[1];
        Out.z = In[2];
        return Out;
    }

    // Every slot the helpers do not read fails (-1, 0, false or null). get_id answers 1.
    class FOracleFakeInspect : public IObjInspect
    {
    public:
        explicit FOracleFakeInspect(FOracleAiObject const& In) : Data(In)
        {
            for (int Row = 0; Row < 3; ++Row)
            {
                for (int Column = 0; Column < 3; ++Column)
                    Xform.Rotation.E[Row][Column] = In.Rotation[Row * 3 + Column];
            }
            Xform.Translation = ToVec(In.Position);
            Velocity = ToVec(In.Velocity);
            AngularVelocity = ToVec(In.AngularVelocity);
        }

        Vector const& get_position() const override { return Xform.Translation; }
        Vector get_velocity() const override { return Velocity; }
        Vector get_angular_velocity() const override { return AngularVelocity; }
        Transform const& get_transform() const override { return Xform; }
        Matrix const& get_orientation() const override { return Xform.Rotation; }
        Vector get_center_of_mass() const override { return ToVec(Zero3); }
        int get_sub_obj_center_of_mass(unsigned short, Vector&) const override { return -1; }
        long get_index() const override { return 0; }
        unsigned int const get_id() const override { return 1; }
        int get_good_id(unsigned&) const override { return -1; }
        int get_archetype_extents(Vector& Min, Vector& Max) const override
        {
            if (Data.bExtentsFail != 0)
            {
                Min = ToVec(Zero3);
                Max = ToVec(Zero3);
                return -5;
            }
            Min = ToVec(Data.ExtentMin);
            Max = ToVec(Data.ExtentMax);
            return 0;
        }
        int get_physical_radius(float& Radius, Vector& Center) const override { Radius = Data.Radius; Center = ToVec(Data.Center); return 0; }
        float get_mass() const override { return 0.0f; }
        bool is_targetable() const override { return false; }
        bool is_dying() const override { return false; }
        int get_status(float&, float&) const override { return -1; }
        int get_status(float&) const override { return -1; }
        int get_shield_status(float&, float&, bool&) const override { return -1; }
        int get_shield_status(float&, bool&) const override { return -1; }
        int get_throttle(float& OutThrottle) const override { OutThrottle = Data.Throttle; return 0; }
        int get_axis_throttle(Vector& OutThrottle) const override { OutThrottle = AngularVelocity; return 0; }
        int get_nudge_vec(Vector&) const override { return -1; }
        int get_strafe_dir(StrafeDir&) const override { return -1; }
        int is_cruise_active(bool&, bool&) const override { return -1; }
        int is_cruise_active(bool&) const override { return -1; }
        int are_thrusters_active(bool&) const override { return -1; }
        int get_attitude_towards(float&, IObjInspect const*) const override { return -1; }
        int get_attitude_towards_symmetrical(float&, IObjInspect const*, float&) const override { return -1; }
        int get_reputation(float&, uint const&) const override { return -1; }
        int get_target(IObjRW*&) const override { return -1; }
        int get_subtarget(unsigned short&) const override { return -1; }
        int get_subtarget_center_of_mass(Vector&) const override { return -1; }
        int get_rank(uint&) const override { return -1; }
        int get_affiliation(uint&) const override { return -1; }
        int get_type(uint& OutType) const override { OutType = Data.Type; return 0; }
        int get_base(uint&) const override { return -1; }
        int get_dock_target(uint&) const override { return -1; }
        int get_power(float&) const override { return -1; }
        int get_zone_props(unsigned long&) const override { return -1; }
        float get_scanner_interference() const override { return 0.0f; }
        int get_hold_left(float&) const override { return -1; }
        int enumerate_cargo(CargoEnumerator*) const override { return -1; }
        int get_data(void const*&) const override { return -1; }
        int get_formation_offset(Vector&) const override { return -1; }
        int get_formation_leader(IObjRW*&) const override { return -1; }
        int get_follow_offset(Vector&) const override { return -1; }
        int get_follow_leader(IObjRW*&) const override { return -1; }
        bool is_player() const override { return false; }
        int get_hardpoint(char const*, Vector*, Matrix*) const override { return -1; }
        bool has_dock_hardpoints() const override { return false; }
        int get_dock_hardpoints(int, TERMINAL_TYPE*, Transform*, Transform*, Transform*, float*) const override { return -1; }
        float get_time_to_accelerate(float, float, float, ThrustEquipType) const override { return 0.0f; }
        float get_distance_travelled(float, float, float, ThrustEquipType) const override { return 0.0f; }
        float get_projected_throttle(float, ThrustEquipType) const override { return 0.0f; }
        float get_speed(float, ThrustEquipType) const override { return Data.Speed; }
        float get_initial_speed_to_coast_distance(float, bool) const override { return 0.0f; }
        float get_time_to_accelerate_angularly(float, float, float) const override { return 0.0f; }
        float get_time_to_accelerate_angularly(float, float, float Dt, float MaxSpeed) const override
        {
            return Dt + MaxSpeed;
        }
        float get_angular_distance_travelled(float, float To, float Dt) const override { return To * Dt; }
        float get_angular_distance_travelled(float, float, float Dt, float MaxSpeed) const override
        {
            return Dt * MaxSpeed;
        }
        float get_angular_speed_XY(float, float) const override { return Data.AngularSpeedXY; }
        float get_angular_speed_Z(float Speed) const override { return Speed * Data.AngularSpeedXY; }
        float get_projected_axis_throttle_XY(float Speed) const override { return Speed * 0.5f; }
        float get_projected_axis_throttle_Z(float) const override { return 0.0f; }
        float get_max_bank_angle() const override { return 0.6f; }
        int get_scanlist(ScanList const*&, uint, bool) const override { return -1; }
        int get_tgt_lead_fire_pos(unsigned short const&, Vector&) const override { return -1; }
        int is_pointing_at(bool&, unsigned short const&, float) const override { return -1; }
        int can_point_at(bool&, unsigned short const&, Vector const&, float) const override { return -1; }
        int find_equipment(unsigned short* const, uint, uint) const override { return -1; }
        int get_equipment_status(EquipStatus&, unsigned short const&) const override { return -1; }
        int get_equipment_val(EquipmentVal&, unsigned short const&, EquipmentValType, float) const override { return -1; }
        int scan_cargo(IObjRW*, EquipDescVector&) const override { return -1; }
        int enumerate_subtargets(SubtargetEnumerator*) const override { return -1; }
        int get_lane_direction(Transform const&, bool*) const override { return -1; }
        int get_ring_side(Transform const&, bool*) const override { return -1; }
        int traverse_rings(uint&, bool) const override { return -1; }
        int is_using_tradelane(bool*) const override { return -1; }
        int get_lane_start(IObjInspect const*, IObjInspect const*&, Vector&, Vector&) const override { return -1; }
        bool generate_follow_offset(IObjInspect const*, Transform&) const override { return false; }
        int get_atmosphere_range(float& OutRange) const override { OutRange = Data.AtmosphereRange; return 0; }
        int get_toughness() const override { return -1; }
        int get_behavior_id() const override { return -1; }
        int get_formation_followers(IObjRW** const, uint) const override { return -1; }
        CObject const* cobject() const override { return nullptr; }

    private:
        FOracleAiObject Data;
        Transform Xform;
        Vector Velocity;
        Vector AngularVelocity;
    };

    // An FAiTarget over a fake, its caches cleared.
    struct FOracleTarget
    {
        FOracleFakeInspect Inspect;
        FAiTarget Target;

        explicit FOracleTarget(FOracleAiObject const& In) : Inspect(In)
        {
            std::memset(&Target, 0, sizeof(Target));
            Target.Init(nullptr, &Inspect);
        }
    };

    void ToSlot(float const* In, ObstacleAvoidance::Slot& Out)
    {
        Out.Timer = In[0];
        Out.Point = ToVector(In + 1);
        Out.Direction = ToVector(In + 4);
    }

    void FromSlot(float* Out, ObstacleAvoidance::Slot const& In)
    {
        Out[0] = In.Timer;
        FromVector(Out + 1, In.Point);
        FromVector(Out + 4, In.Direction);
    }
}

double __cdecl OracleAiLeadInterceptDistance(FOracleAiObject const* Elem, FOracleAiObject const* Target)
{
    FOracleTarget E(*Elem);
    FOracleFakeInspect const T(*Target);
    return EstimateLeadInterceptDistance(&E.Target, &T);
}

double __cdecl OracleAiInterceptClearance(FOracleAiObject const* Elem, FOracleAiObject const* Target, int bOfIface)
{
    FOracleTarget E(*Elem);
    FOracleTarget T(*Target);
    return bOfIface != 0 ? GetInterceptClearanceOfIface(&E.Target, &T.Inspect)
                         : GetInterceptClearance(&E.Target, &T.Target);
}

int __cdecl OracleAiWithinInterceptRange(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                         float Threshold, int bOfIface)
{
    FOracleTarget E(*Elem);
    FOracleTarget T(*Target);
    bool const bIn = bOfIface != 0 ? IsWithinInterceptRangeOfIface(&E.Target, &T.Inspect, Threshold)
                                   : IsWithinInterceptRange(&E.Target, &T.Target, Threshold);
    return bIn ? 1 : 0;
}

int __cdecl OracleAiPointOutsideHeadingCone(float const* Point, FOracleAiObject const* Iface, float CosThreshold)
{
    if (Iface == nullptr)
        return IsPointOutsideHeadingCone(ToVector(Point), nullptr, CosThreshold) ? 1 : 0;
    FOracleFakeInspect const I(*Iface);
    return IsPointOutsideHeadingCone(ToVector(Point), &I, CosThreshold) ? 1 : 0;
}

int __cdecl OracleAiAlignedOutsideHeadingCone(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                              float CosThreshold)
{
    FOracleTarget E(*Elem);
    if (Target == nullptr)
        return IsAlignedAndOutsideHeadingCone(&E.Target, nullptr, CosThreshold) ? 1 : 0;
    FOracleFakeInspect const T(*Target);
    return IsAlignedAndOutsideHeadingCone(&E.Target, &T, CosThreshold) ? 1 : 0;
}

double __cdecl OracleAiSignedRadialClearance(FOracleAiObject const* Elem, float const* Point)
{
    FOracleTarget E(*Elem);
    return GetSignedRadialClearanceToPoint(&E.Target, ToVector(Point));
}

int __cdecl OracleAiCoAlignedAndAhead(FOracleAiObject const* Target, FOracleAiObject const* Reference,
                                      float CosThreshold)
{
    FOracleTarget T(*Target);
    FOracleTarget R(*Reference);
    return IsTargetCoAlignedAndAhead(&T.Target, &R.Target, CosThreshold) ? 1 : 0;
}

double __cdecl OracleAiIfaceVectorMagnitude(FOracleAiObject const* Iface)
{
    if (Iface == nullptr)
        return GetIfaceVectorMagnitude(nullptr);
    FOracleFakeInspect const I(*Iface);
    return GetIfaceVectorMagnitude(&I);
}

void __cdecl OracleAiBoxCorners(float const* Xform, float const* Min, float const* Max, float* Out)
{
    Vector Corners[8];
    Transform const X = ToTransform(Xform);
    TransformBoundingBoxCorners(X, ToVector(Min), ToVector(Max), Corners);
    for (int Index = 0; Index < 8; ++Index)
        FromVector(Out + Index * 3, Corners[Index]);
}

void __cdecl OracleAiShapeBiasedAvoid(FOracleAiObject const* Elem, FOracleAiObject const* Target, float* Out)
{
    FOracleTarget E(*Elem);
    Vector Result;
    if (Target == nullptr)
    {
        ComputeShapeBiasedAvoidDirection(&Result, &E.Target, nullptr);
    }
    else
    {
        FOracleFakeInspect T(*Target);
        ComputeShapeBiasedAvoidDirection(&Result, &E.Target, &T);
    }
    FromVector(Out, Result);
}

double __cdecl OracleAiTurnCircleCost(FOracleAiObject const* Iface, float const* Point, float AngularSpeed,
                                      float LinearSpeed)
{
    FOracleFakeInspect I(*Iface);
    return EstimateTurnCircleApproachCost(&I, ToVector(Point), AngularSpeed, LinearSpeed);
}

int __cdecl OracleAiAverageActiveDirection(float const* Slots, float const* Xform, float* Out)
{
    ObstacleAvoidance::Slot S[25];
    for (int Index = 0; Index < 25; ++Index)
        ToSlot(Slots + Index * 7, S[Index]);
    Vector Result = ToVector(Out);
    Transform const X = ToTransform(Xform);
    bool const bOk = ObstacleAvoidance_AverageActiveDirection(S, X, Result);
    FromVector(Out, Result);
    return bOk ? 1 : 0;
}

int __cdecl OracleAiResolveCandidateSlot(FOracleAiObject const* Candidate, float* Out)
{
    FOracleFakeInspect I(*Candidate);
    ObstacleAvoidance::Slot Slot{};
    ToSlot(Out, Slot);
    // ResolveObstacleCandidateSlot takes the IObjRW a scan list holds and calls only its
    // IObjInspect slots; IObjInspect is IObjRW's first base, at the same address.
    bool const bOk = ResolveObstacleCandidateSlot(static_cast<IObjRW*>(static_cast<IObjInspect*>(&I)), Slot);
    FromSlot(Out, Slot);
    return bOk ? 1 : 0;
}

int __cdecl OracleAiNudgePathPoint(float const* Slot, float const* Point, float const* Xform, int bExtraCheck,
                                   float* OutPoint)
{
    ObstacleAvoidance::Slot S{};
    ToSlot(Slot, S);
    Vector Result = ToVector(OutPoint);
    Transform const X = ToTransform(Xform);
    int const Code = NudgeObstaclePathPoint(S, ToVector(Point), X, &Result, bExtraCheck != 0);
    FromVector(OutPoint, Result);
    return Code;
}

void __cdecl OracleAiMoveSetDirection(float const* Dir, float* Out)
{
    MoveIdea Idea;
    Idea.SetDirection(ToVector(Dir));
    FromVector(Out, Idea.Direction);
}

void __cdecl OracleAiMoveSetDirectionFrom(float const* Basis, float* Out)
{
    Matrix const M = ToMatrix(Basis);
    MoveIdea Idea;
    Idea.SetDirectionFrom(&M);
    FromVector(Out, Idea.Direction);
}

void __cdecl OracleAiLimitAxisThrottle(FOracleAiObject const* Obj, float const* Command, float const* TargetFrame,
                                       float TurnAngle, float ReferenceAngle, int bBankBlend, float* Out)
{
    Matrix const Frame = ToMatrix(TargetFrame);
    Vector Result;
    if (Obj == nullptr)
    {
        LimitAxisThrottleForTurn(Result, nullptr, ToVector(Command), &Frame, TurnAngle, ReferenceAngle,
                                 bBankBlend != 0);
    }
    else
    {
        FOracleFakeInspect const I(*Obj);
        LimitAxisThrottleForTurn(Result, &I, ToVector(Command), &Frame, TurnAngle, ReferenceAngle, bBankBlend != 0);
    }
    FromVector(Out, Result);
}

int __cdecl OracleAiBankedSteeringFrame(FOracleAiObject const* Elem, float const* TargetDir, float* Frame,
                                        float* Bank, int Mode)
{
    FOracleTarget E(*Elem);
    Matrix Working = ToMatrix(Frame);
    Vector BankVector = ToVector(Bank);
    bool const bSteered = ComputeBankedSteeringFrame(&E.Target, ToVector(TargetDir), Working, BankVector,
                                                     static_cast<unsigned char>(Mode));
    FromMatrix(Frame, Working);
    FromVector(Bank, BankVector);
    return bSteered ? 1 : 0;
}
