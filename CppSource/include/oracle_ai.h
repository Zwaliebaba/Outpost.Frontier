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

// TargetManager (common_iface.h) without its owner pointer.
struct FOracleAiTargeting
{
    int AttackState;                // +0x08
    float AggressionRatio;          // +0x0C
    float SubtargetDistanceSq;      // +0x10
    unsigned int SubtargetId;       // +0x14
    int NumForcedTargets;           // +0x18
    unsigned int ForcedTargets[8];  // +0x1C
    float Anchor[3];                // +0x3C
    int bCommitted;                 // +0x48
    float CommitTimer;              // +0x4C
    int SelectionPriority;          // +0x50
    int NumCandidatesA;             // +0x54
    int NumCandidatesB;             // +0x58
    int NumSelected;                // +0x5C
    int bSelectedIsPlayer;          // +0x60
    int TargetedEnemyCount;         // +0x64
    int bOverClampKept;             // +0x68
    int bOverClampDropped;          // +0x69
};

// SystemManagerTimerTable (common_systemmanager.h) without the target list.
struct FOracleAiTimerTable
{
    unsigned int ScannerId;         // +0x08 ScannerEquipId
    float DecayRate;                // +0x20
    float Level;                    // +0x24
    int TimerActive[11];            // +0x28 Timers[i].Active
    float TimerValues[11][3];       // Timers[i].TriggerThreshold, Duration, Accumulator
    float DecayScale;               // +0xD8
    int MissileCount;               // the incoming-missile list (+0xE0), at most 16 copied
    unsigned int Missiles[16];      // its watched ids, in list order
    unsigned int BestMissile;       // +0xE8 BestCandidateWatcher's id
    float BestTime;                 // +0xF4 BestValueSoFar
};

// FGunGroupSlot.
struct FOracleAiGunSlot
{
    unsigned int Id;
    int bAutoTurret;
    int Seeker;
    int bFireTick;
    int Kind;
    float BurstTime;
    float NoBurstTime;
    float FireInterval;
    float Phase;
};

// FLauncherFireCacheSlot.
struct FOracleAiLineOfFire
{
    int bInUse;
    unsigned int TargetId;
    unsigned int Id;
    float Age;
    float RefreshTimer;
    int bNeedsRefresh;
    int bBlocked;
};

// SystemManagerGunGroupManager (common_systemmanager.h) without its pointers.
struct FOracleAiGunGroups
{
    int LastGunFireResult;          // +0x00
    unsigned int TargetId;          // +0x08 TargetWatcher's id
    unsigned int HoldMask;          // +0x10
    float AimOffset[3];             // +0x14
    float AimRefreshTimer;          // +0x20
    float CruiseHeldTime;           // +0x24
    float ScanTimer;                // +0x28
    float ScanPeriod;               // +0x2C
    int GunCount;                   // +0x830
    FOracleAiGunSlot Guns[64];      // +0x30
    int MineDropperCount;           // +0x1038
    FOracleAiGunSlot MineDroppers[64]; // +0x838
    float Params[19];               // +0x103C .. +0x1084: the 18 interval, launch and accuracy
                                    // floats in field order (Field_0x107C included), GunRange last
    float WideConeAngle;            // +0x1090
    int bFireAllGuns;               // +0x1088
    int bEnabled;                   // +0x1094
    FOracleAiLineOfFire LineOfFire[16]; // +0x1098
    float CmTimer;                  // +0x1218 CmDroppers.Timer
    int bCmActive;                  // CmDroppers.bActive
    int CmCount;                    // CmDroppers.Count (at most 8 ids copied)
    unsigned int CmIds[8];          // CmDroppers.Ids
};

// FaceBehavior (common_facebehavior.h): the Behavior prefix and its own fields.
struct FOracleAiFace
{
    int Mode;                       // +0x0C ModeSelector0xc
    int Result;                     // +0x10 UpdateResult10
    float DeltaTime;                // +0x1C
    int Flags28;                    // +0x28
    int Flag29;                     // +0x29
    int Kind;                       // +0x2C TargetKind2c
    unsigned int WatchedId;         // +0x30 Watcher30's id
    float Orient[9];                // +0x38
    float Target[3];                // +0x5C
    int Flag1b4;                    // +0x1B4
    float Steer[3];                 // +0x1B8 .. +0x1C0
    int Flag1c4;                    // +0x1C4
    int Flag1c5;                    // +0x1C5
    float Field1c8;                 // +0x1C8
    float Field1cc;                 // +0x1CC
    float Field1d0;                 // +0x1D0
    int Flag1d4;                    // +0x1D4
    float Field1d8;                 // +0x1D8
};

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
    // P2.5 combat: the system manager's combat state the fire control, the target manager and
    // the timer table carry between ticks (Sys->Targeting, Sys->Timers, Sys->GunGroups), the
    // manager's Flags1c8, and the face behaviour's own fields while it is current.
    int bSys;                       // Sys is set
    int Flags1c8;                   // IBehaviorManager::Flags1c8
    FOracleAiTargeting Targeting;   // Sys->Targeting
    FOracleAiTimerTable Timers;     // Sys->Timers without the target list
    FOracleAiGunGroups Guns;        // Sys->GunGroups
    int bFace;                      // the current behaviour is the face behaviour (15)
    FOracleAiFace Face;             // its fields
};

// Copies the manager's state; false (Out zeroed, StateGraphId -1) for a null manager.
COMMON_API bool __cdecl OracleAiReadManager(IBehaviorManager const* Manager, FOracleAiManager& Out);
// The CSimple id of a CObject (0 for null or an object that is not a CSimple).
COMMON_API unsigned int __cdecl OracleAiObjectId(CObject const* Object);

// Barrel 0's world position and direction of one of an equipped object's launchers
// (CELauncher::cast over the CEqObj's FindByID, as IsLineOfFireBlocked reads them): 1 with
// both written, 0 when the item is not a launcher.
COMMON_API int __cdecl OracleAiLauncherBarrel(IObjInspect const* Object, unsigned short Id, float* OutPos,
                                              float* OutDir);
// CShip::get_tgt_lead_fire_pos(Vector&) of a ship: 1 with Out written, 0 when it answers
// false, -1 for an object that is not a ship.
COMMON_API int __cdecl OracleAiShipLeadFirePos(IObjInspect const* Object, float* Out);
// CShip::get_targeted_enemy_count of a ship, -1 for an object that is not a ship.
COMMON_API int __cdecl OracleAiTargetedEnemyCount(IObjInspect const* Object);
