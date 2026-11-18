// Oracle instrumentation (P2.5 ai): entry points for the AI suites of `Oracle.exe game-sim`
// and the behaviour-tick recorder of `Oracle.exe sim` (design/sim-records.md, `ai.jsonl`).
//
// The AI helpers the suites check (common_airaycast.cpp, common_aisteer.cpp, the slerp of
// common_obstacleavoidance.cpp, pub::StateGraph::LoadFromFile) are internal to Common.dll.
// The OracleAi* functions below are thin exported wrappers over them, taking plain float
// arrays (a Vector is 3 floats, a Matrix 9 in row order, a Transform 12: the matrix, then
// the translation). Nothing in the game calls them.
//
// GOracleAiHooks is inert until the harness fills it. Its one hook site, marked
// "Oracle instrumentation (P2.5 ai)", is in Common/common_physys_query.cpp: the three PhySys
// queries the AI makes (FindRayCollisions, FindRayIntersect, FindSphereCollisions) report
// the query and its hits. The definitions are in Common/oracle_ai.cpp.

#pragma once

#include "common_exports.h"

struct CObject;
struct IObjDB;
struct IObjInspect;
struct IObjRW;
class IBehaviorManager;
class Vector;

// ---------------------------------------------------------------------------
// Wrappers for the suites (Oracle/oracle_ai.cpp).
// ---------------------------------------------------------------------------

// RaySphereIntersect; OutT is written on a hit.
COMMON_API int __cdecl OracleAiRaySphere(float const* Origin, float const* Dir, float const* Center, float Radius,
                                         float* OutT);
// RayAABBIntersect; TMin and TMax in and out.
COMMON_API int __cdecl OracleAiRayAabb(float const* LocalOrigin, float const* LocalDir, float const* HalfExtent,
                                       float* TMin, float* TMax);
// RayOrientedBoxIntersect over an OrientedBox of 15 floats (Center, AxisX, AxisY, AxisZ,
// HalfExtent). OutPoints holds 6 floats; the caller fills it first (hit type 1 leaves the
// second point as it was).
COMMON_API int __cdecl OracleAiRayOrientedBox(float const* Origin, float const* Dir, float const* Box,
                                              int* OutHitType, float* OutPoints);
// RayTransformedBoxNearestHit; Aabb is {MinX, MaxX, MinY, MaxY, MinZ, MaxZ}.
COMMON_API int __cdecl OracleAiRayTransformedBoxNearest(float const* Origin, float const* Dir, float const* Xform,
                                                        float const* Aabb, float* OutPoint, int* OutStartsInside);
// TransformedBoxesIntersect.
COMMON_API int __cdecl OracleAiBoxesIntersect(float const* XformA, float const* AabbA, float const* XformB,
                                              float const* AabbB);
// BuildPerpendicularBasis; Dir is normalised in place unless bAlreadyNormalized.
COMMON_API void __cdecl OracleAiPerpendicularBasis(float* Dir, int bAlreadyNormalized, float* Out1, float* Out2);
// RaySegmentCapsuleIntersect over a capsule of 7 floats (Radius, SegStart, SegDir); OutT
// holds 2 floats. Returns the hit count.
COMMON_API int __cdecl OracleAiCapsuleIntersect(float const* Origin, float const* Dir, float const* Capsule,
                                                float* OutT);
// RaySegmentCapsuleFirstHit.
COMMON_API int __cdecl OracleAiCapsuleFirstHit(float const* Origin, float const* Dir, float const* Capsule,
                                               float* OutPoint);
// PointInCapsule.
COMMON_API int __cdecl OracleAiPointInCapsule(float const* Point, float const* Capsule);
// ClampedDot3 (unrounded).
COMMON_API double __cdecl OracleAiClampedDot3(float const* A, float const* B);
// SweptSphereCollisionTime.
COMMON_API int __cdecl OracleAiSweptSphere(float RadiusA, float const* A0, float const* A1, float RadiusB,
                                           float const* B0, float const* B1, float* OutT);
// BuildOrientationBasis; UpHint may be null.
COMMON_API void __cdecl OracleAiOrientationBasis(float const* Reference, float const* Forward, float const* UpHint,
                                                 float* Out);
// BuildPerpendicularFrame; Reference may be null. Out is left as it was on false.
COMMON_API int __cdecl OracleAiPerpendicularFrame(float const* Direction, float DotThreshold, float const* Reference,
                                                  float* Out);
// SteeringFrameFromRelativeDir.
COMMON_API void __cdecl OracleAiSteeringFrame(float const* Reference, float const* RelDir, float const* UpHint,
                                              float* Out);
// RandomVector(Radius) (draws rand()).
COMMON_API void __cdecl OracleAiRandomVector(float Radius, float* Out);
// BuildRandomSpreadFrame (draws rand()).
COMMON_API void __cdecl OracleAiRandomSpreadFrame(float const* Reference, float SpreadAngle, float BankAngle,
                                                  float* Out);
// TransformVectorByMatrix.
COMMON_API void __cdecl OracleAiTransformVectorByMatrix(float const* M, float const* V, float* Out);
// RotateTowardBySlerp.
COMMON_API void __cdecl OracleAiRotateTowardBySlerp(float const* From, float const* To, float Angle, float* Out);
// TurnHelper::get_angular_throttle; returns the angle.
COMMON_API float __cdecl OracleAiAngularThrottle(float const* A, float const* B, float* OutThrottle);
// pub::StateGraph::Free, then pub::StateGraph::LoadFromFile(Path).
COMMON_API void __cdecl OracleAiLoadStateGraph(char const* Path);
// pub::StateGraph::Free, which Common does not export.
COMMON_API void __cdecl OracleAiFreeStateGraph();
// A default-constructed pub::AI::Personality, built in zeroed storage, copied to Out (0x3d8
// bytes); its two methods for the given values.
COMMON_API void __cdecl OracleAiDefaultPersonality(unsigned char* Out);
COMMON_API float __cdecl OracleAiToughnessRatio(int Type);
COMMON_API double __cdecl OracleAiRangeThresholdValue(int JobField00);

// ---------------------------------------------------------------------------
// Wrappers for the navigation suites (Oracle/oracle_ai_nav.cpp): intercepts and obstacle
// avoidance (common_aiintercept.cpp, common_obstacleavoidance.cpp). An object is an
// FOracleAiObject; Common/oracle_ai.cpp wraps it in an IObjInspect that answers from it
// (get_id 1, get_physical_radius, get_archetype_extents (zeroed and -5 when bExtentsFail),
// get_type, get_throttle, get_atmosphere_range, get_angular_speed_XY, get_speed) and fails
// every other slot. An FAiTarget argument is one over such an object with its caches clear.
// A null object pointer passes a null interface. A slot is 7 floats (Timer, Point, Direction).
// ---------------------------------------------------------------------------

struct FOracleAiObject
{
    float Position[3];
    float Rotation[9]; // row order
    float Velocity[3];
    float AngularVelocity[3];
    float Radius;
    float Center[3];
    float ExtentMin[3];
    float ExtentMax[3];
    unsigned int Type;
    int bExtentsFail;
    float Throttle;
    float AngularSpeedXY;
    float Speed;
    float AtmosphereRange;
};

COMMON_API double __cdecl OracleAiLeadInterceptDistance(FOracleAiObject const* Elem, FOracleAiObject const* Target);
// GetInterceptClearance, or GetInterceptClearanceOfIface when bOfIface.
COMMON_API double __cdecl OracleAiInterceptClearance(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                                     int bOfIface);
// IsWithinInterceptRange, or IsWithinInterceptRangeOfIface when bOfIface.
COMMON_API int __cdecl OracleAiWithinInterceptRange(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                                    float Threshold, int bOfIface);
COMMON_API int __cdecl OracleAiPointOutsideHeadingCone(float const* Point, FOracleAiObject const* Iface,
                                                       float CosThreshold);
COMMON_API int __cdecl OracleAiAlignedOutsideHeadingCone(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                                         float CosThreshold);
COMMON_API double __cdecl OracleAiSignedRadialClearance(FOracleAiObject const* Elem, float const* Point);
COMMON_API int __cdecl OracleAiCoAlignedAndAhead(FOracleAiObject const* Target, FOracleAiObject const* Reference,
                                                 float CosThreshold);
COMMON_API double __cdecl OracleAiIfaceVectorMagnitude(FOracleAiObject const* Iface);
// TransformBoundingBoxCorners; Out holds 24 floats.
COMMON_API void __cdecl OracleAiBoxCorners(float const* Xform, float const* Min, float const* Max, float* Out);
COMMON_API void __cdecl OracleAiShapeBiasedAvoid(FOracleAiObject const* Elem, FOracleAiObject const* Target,
                                                 float* Out);
COMMON_API double __cdecl OracleAiTurnCircleCost(FOracleAiObject const* Iface, float const* Point, float AngularSpeed,
                                                 float LinearSpeed);
// ObstacleAvoidance_AverageActiveDirection over 25 slots; Out is left as it was on false.
COMMON_API int __cdecl OracleAiAverageActiveDirection(float const* Slots, float const* Xform, float* Out);
// ResolveObstacleCandidateSlot; Out (7 floats) is the slot, read first.
COMMON_API int __cdecl OracleAiResolveCandidateSlot(FOracleAiObject const* Candidate, float* Out);
// NudgeObstaclePathPoint; OutPoint is read first and written back. Returns the code.
COMMON_API int __cdecl OracleAiNudgePathPoint(float const* Slot, float const* Point, float const* Xform,
                                              int bExtraCheck, float* OutPoint);

// The movement ideas' helpers (common_moveidea.cpp, common_aisteer.cpp). For these the fake
// also answers get_axis_throttle (the record's angular velocity), the four-argument
// get_time_to_accelerate_angularly (Dt + MaxSpeed) and get_angular_distance_travelled
// (Dt * MaxSpeed), the three-argument get_angular_distance_travelled (To * Dt),
// get_angular_speed_Z (Speed * AngularSpeedXY), get_projected_axis_throttle_XY (Speed * 0.5f)
// and get_max_bank_angle (0.6f).
// MoveIdea::SetDirection / SetDirectionFrom (Basis is a Matrix) on a new MoveIdea; Out is
// its Direction.
COMMON_API void __cdecl OracleAiMoveSetDirection(float const* Dir, float* Out);
COMMON_API void __cdecl OracleAiMoveSetDirectionFrom(float const* Basis, float* Out);
// LimitAxisThrottleForTurn; Obj may be null.
COMMON_API void __cdecl OracleAiLimitAxisThrottle(FOracleAiObject const* Obj, float const* Command,
                                                  float const* TargetFrame, float TurnAngle, float ReferenceAngle,
                                                  int bBankBlend, float* Out);
// ComputeBankedSteeringFrame; Frame (9) and Bank (3) are read and written back.
COMMON_API int __cdecl OracleAiBankedSteeringFrame(FOracleAiObject const* Elem, float const* TargetDir, float* Frame,
                                                   float* Bank, int Mode);

// ---------------------------------------------------------------------------
// The behaviour-tick recorder (Oracle/oracle_sim_ai.cpp).
// ---------------------------------------------------------------------------

// The PhySys queries OnQuery reports.
enum EOracleAiQuery
{
    OracleAiQueryRay = 0,       // FindRayCollisions(SystemId, From, To)
    OracleAiQueryRayObject = 1, // FindRayIntersect(Object, From, To)
    OracleAiQuerySphere = 2     // FindSphereCollisions(SystemId, Center = From, Radius)
};

// One hit as OnQuery reports it: the object, and for a ray the point, the normal and the
// part id.
struct FOracleAiHit
{
    CObject const* Object;
    unsigned int ObjectId;   // the CSimple's Id, 0 for an object that is not one
    float Point[3];
    float Normal[3];
    unsigned int PartId;
};

struct FOracleAiHooks
{
    // A PhySys query after it ran: its arguments and its hits (Hits[0..HitCount)).
    void(__cdecl* OnQuery)(int Kind, unsigned int SystemId, CObject const* Object, float const* From,
                           float const* To, float Radius, int MaxHits, int HitCount, FOracleAiHit const* Hits);
};

COMMON_API extern FOracleAiHooks GOracleAiHooks;

// Called by the PhySys queries when OnQuery is set: converts the RayHit array (Hits, of
// PhySys::RayHit) or the object array (Objects) and calls OnQuery.
COMMON_API void __cdecl OracleAiReportRayQuery(int Kind, unsigned int SystemId, CObject const* Object,
                                               Vector const& From, Vector const& To, void const* Hits,
                                               int MaxHits, int HitCount);
COMMON_API void __cdecl OracleAiReportSphereQuery(unsigned int SystemId, Vector const& Center, float Radius,
                                                  CObject* const* Objects, int MaxObjects, int Count);

// One behaviour manager as OracleAiReadManager copies it.
struct FOracleAiManager
{
    IObjRW* Object;                 // the owner, null when unset
    unsigned int ObjectId;          // its get_id()
    IObjDB* ObjectDb;               // IBehaviorManager::ObjectDb
    int BehaviorIndex;              // CurrentBehaviorIndex
    int StateGraphId;               // StateGraph->Id, -1 without one
    unsigned int Directive[3];      // DirectiveStorage
    unsigned int ManeuverMask;      // +0xB8
    int bLockManeuvers;             // +0xBC
    int bDirty;                     // BehaviorDirty
    int bSuppressed;                // TickSuppressedFlag
    float TimerA;                   // BehaviorTimerA
    float TimerB;                   // BehaviorTimerB
    float TimerC;                   // BehaviorTimerC
    float AttackOverrideTimer;      // +0xE0
    float AttackOverridePeriod;     // +0xE4
    unsigned int InSightsTargetId;  // +0xE8
    int Field118;                   // +0x118
    int Field11c;                   // +0x11C
    unsigned int Field120;          // +0x120, the fallback behaviour
    float ScoreWeights[21];         // BehaviorScores[i].Weight
    int ScoreBehaviors[21];         // BehaviorScores[i].BehaviorIndex
    int TargetCount;                // Sys->Timers.Targets size (at most 64 are copied)
    IObjInspect* Targets[64];       // their Iface
    int bPersonality;               // Sys is set
    unsigned char Personality[0x3d8]; // Sys->PersonalityData
    // P2.5 goto/face: the pathfinder's clocks and override record (IBehaviorManager::Pathfind),
    // and the thrust manager's cached flags (Sys->Thrust).
    int bPathfind;                  // Pathfind is set
    float PathfindTimers[3];        // Timer1E0, Timer1E8, Timer20C
    float PathfindPeriods[3];       // Period1E4, Period1EC, Period210
    int PathfindRecord;             // Field_214
    float PathfindGoal[3];          // Field_22C
    float PathfindCap;              // Field_23C
    int PathfindKind;               // Field_240
    float PathfindTarget[3];        // CachedTargetPosition
    unsigned int PathfindTargetId;  // Field_064
    int PathfindReaction;           // Reaction.State
    int bProbeActive;               // Avoidance.Active
    int bProbeContact;              // Avoidance.HasContact
    int ThrustFlags;                // Thrust: bCruiseEngaged 1, bCruiseUnobstructed 2,
                                    // bEngineActive 4, bAutoLevel 8, bAutoAvoidance 16
};

// Copies the manager's state; false (Out zeroed, StateGraphId -1) for a null manager.
COMMON_API bool __cdecl OracleAiReadManager(IBehaviorManager const* Manager, FOracleAiManager& Out);
// The CSimple id of a CObject (0 for null or an object that is not a CSimple).
COMMON_API unsigned int __cdecl OracleAiObjectId(CObject const* Object);
