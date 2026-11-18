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

// The system manager's combat parts without an object world (Oracle/oracle_ai_combat.cpp).
// SustainedThresholdTimer::Update on {active, threshold, duration, accumulator}; Out gets
// {active, accumulator}; returns the result.
COMMON_API int __cdecl OracleAiThresholdTimer(float const* In, float Dt, float Level, float* Out);
// RandomizeSlotTiming on slot 0 of a freshly constructed gun-group manager whose eleven
// interval parameters are Params (gun fire interval, its variance, auto-turret interval, gun
// burst, its variance, gun no-burst, auto-turret burst, auto-turret no-burst, auto-turret
// variance, missile interval, its variance); Out gets {burst, no burst, interval, phase}.
// Kind 4 (the cruise disruptor) reads the owner's personality and is not supported.
COMMON_API void __cdecl OracleAiGunSlotTiming(float const* Params, int Kind, int bAutoTurret, float* Out);
// UpdateTimers(Dt) on a fresh gun-group manager with one gun {burst, no burst, interval,
// phase} of the kind, the aim refresh timer and one claimed line-of-fire cache slot {age,
// refresh timer, needs refresh}; Out gets {phase, fire tick, aim timer, in use, age, refresh
// timer, needs refresh}.
COMMON_API void __cdecl OracleAiGunGroupStep(int Kind, float const* Slot, float AimTimer, float const* Cache,
                                             float Dt, float* Out);

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
};

// Copies the manager's state; false (Out zeroed, StateGraphId -1) for a null manager.
COMMON_API bool __cdecl OracleAiReadManager(IBehaviorManager const* Manager, FOracleAiManager& Out);
// The CSimple id of a CObject (0 for null or an object that is not a CSimple).
COMMON_API unsigned int __cdecl OracleAiObjectId(CObject const* Object);
