// Oracle instrumentation (P2.3 sim): the hooks of the scenario and replay oracle,
// `Oracle.exe sim` (design/adr/0003-simulation-oracle.md, design/reference-build.md
// section 10, record layouts in design/sim-records.md).
//
// Every hook is inert until the harness fills GOracleSimHooks. Nothing else writes it: the
// client, the dedicated server and rpclocal leave it zero, so each hook site is a test of a
// zero field or a null pointer and the game behaves exactly as before. Hook sites, each a
// few lines marked "Oracle instrumentation (P2.3 sim)":
//
//   Server/IServerImpl.cpp      Server_StepSystems (StepSystemId), StarSystem_Update (physics
//                               and collisions with no player), IServerImpl::ElapseTime (real
//                               dt), OracleSimLoadSystem (export, loads one system)
//   Server/server_gameobject.cpp  the three Timing::read timer stamps; GameObject::on_collision
//                               (CalcCollisionDamage in and out); Ship's IObjAffect setters,
//                               set_target, slot_50 and slot_54, EqObj::slot_34 and slot_4c
//                               (OnAffect, each inside an FOracleSimAffectScope)
//   Server/server_classes.cpp   StarSystem::munition_collision (OnMunitionHit);
//                               StarSystem_MpCollisionFilter (single-player collisions)
//   Common/common_physicalsystem.cpp  PhysicalSystem::Update (single-player stepping)
//   Common/common_behaviormanager.cpp IBehaviorManager::UpdateBehaviorTick (OnBehaviorTick)
//   Common/common_ceqobj.cpp    CEqObj::activate (OnDockActivate) and
//                               CEqObj::update_docking_animations (OnDockAnim), P2.4
//   IVP/ivp_physics/ivp_environment.cpp IVP_Environment_Simulate_Psi (OnPsi)
//   IVP/ivp_physics/ivp_simulation_unit.cpp IVP_Simulation_Unit_Process_One_Psi_Step
//                               (OnSimUnitPsi)
//   IVP/ivp_physics/ivp_core.cpp IVP_Core::push_core_linear / _angular_ws / _angular_cs
//                               (OnCorePush)
//
// The definitions and the readers below are in Common/oracle_sim_hooks.cpp.

#pragma once

#include "common_exports.h"

struct CObject;
struct DamageList;
struct IObjRW;
class IBehaviorManager;
class IVP_Core;
class IVP_Environment;
class IVP_Simulation_Unit;
class Vector;

// One IVP core as OracleSimReadCore copies it: the core state of a psi.jsonl record
// (design/sim-records.md). IVP_Core offsets and the reconstruction's names (ivp_core.h) in the
// comments, IVP's own names in parentheses where they differ.
struct FOracleSimCore
{
    unsigned int Flags;             // +0x00 Flags
    unsigned int MovementState;     // +0x60 MovementState
    unsigned int UnitState;         // OwningSimUnit (+0x15c)->State, 0 without a unit
    float UpperLimitRadius;         // +0x04 UpperLimitRadius
    float SpeedLimit;               // +0x08 SpeedLimit (max_surface_deviation)
    float Mass;                     // +0x20 Mass
    float InvMass;                  // +0x40 InvMass
    float RotInertia[3];            // +0x14 RotInertia
    float InvRotInertia[3];         // +0x34 InvRotInertia
    float RotSpeedDamp[3];          // +0x24 RotSpeedDamp
    float SpeedDamp;                // +0x44 SpeedDamp
    float DAngularLimitFactor;      // +0x48 DAngularLimitFactor
    int bSpinClipping;              // +0x4c SpinClipping != null
    float SpinClipping[3];          // *SpinClipping, zero when null
    float TimeOfLastPsi[2];         // +0x68 TimeOfLastPsi base, offset
    float IDeltaTime;               // +0x70 IDeltaTime
    float RotSpeedChange[3];        // +0x74 RotSpeedChange
    float SpeedChange[3];           // +0x84 SpeedChange
    float RotSpeed[3];              // +0x94 RotSpeed (core space)
    float Speed[3];                 // +0xa4 Speed (world)
    float Pos[3];                   // +0xb4 PosWorldFCore
    float DeltaWorld[3];            // +0xc4 DeltaWorldFCore
    float Quat[4];                  // +0xd4 QWorldFCore x y z w
    float QuatNext[4];              // +0xe4 QWorldFCoreNext x y z w
    float Matrix[12];               // +0xf4 MWorldFCore rows 0..2 (three floats each), then vv
    float RotAxis[3];               // +0x134 RotAxisOS (rotation_axis_world_space)
    float Margin;                   // +0x144 Margin (current_speed)
    float MaxSurfaceDeviation;      // +0x148 MaxSurfaceDeviation (abs_omega)
    float BoundingRadius;           // +0x14c BoundingRadius (max_surface_rot_speed)
    float TimeOfLastMotion[2];      // +0x160 TimeOfLastMotion
    float TimeOfLastLongMotion[2];  // +0x168 TimeOfLastLongMotion
    float QLastMotion[4];           // +0x170 QLastMotion
    float QLastLongMotion[4];       // +0x180 QLastLongMotion
    float PosLastMotion[3];         // +0x190 PosLastMotion
    float PosLastLongMotion[3];     // +0x1a0 PosLastLongMotion
};

// The environment values of a psi.jsonl record, as OracleSimReadEnv copies them.
struct FOracleSimEnv
{
    float DeltaTime;                // delta_time (+0xac), the PSI length
    float InvDeltaTime;             // inv_delta_time (+0xb0)
    float Time[2];                  // current_time base, offset
    float MaxVelocity;              // anomaly_limits->MaxVelocity
    float MaxAngularVelocityPerPsi; // anomaly_limits->MaxAngularVelocityPerPsi
    int MovementCheck;              // next_movement_check (+0x118)
    unsigned IvpSeed;               // IVP_FastRandomFloat's seed (process wide)
};

// The IVP_Core::push_core_* calls OnCorePush reports.
enum EOracleSimPush
{
    OracleSimPushLinear = 0,     // push_core_linear(Direction, Magnitude)
    OracleSimPushAngularWs = 1,  // push_core_angular_ws(Axis, Magnitude), world space
    OracleSimPushAngularCs = 2   // push_core_angular_cs(Axis, Magnitude), core space
};

// One behaviour manager as OracleSimReadBehavior copies it.
struct FOracleSimBehavior
{
    IObjRW* Object;            // the owner (WatchablePtr - 8), null when unset
    unsigned int ObjectId;     // its get_id(), 0 when unset
    int BehaviorIndex;         // CurrentBehaviorIndex
    int StateGraphId;          // StateGraph->Id, -1 when there is none
    unsigned int Directive[3]; // DirectiveStorage as three dwords
    int bSuppressed;           // TickSuppressedFlag
};

// The IObjAffect slots OnAffect reports (common_iface.h slot numbers).
enum EOracleSimAffect
{
    OracleSimAffectThrottle = 0,      // Values[0] = throttle
    OracleSimAffectAxisThrottle = 1,  // Values[0..2]
    OracleSimAffectNudge = 2,         // Values[0..2]
    OracleSimAffectStrafe = 3,        // IntArg = StrafeDir
    OracleSimAffectFire = 13,         // IntArg = equipment count, Values = target position or none
    // P2.5 loot: slot_40 (the tractor): IntArg = equipment id, Values[0] = the target's id
    // (its bits; 0 for none).
    OracleSimAffectTractor = 16,
    // P2.5 goto/face (design/sim-records.md, `affect`):
    OracleSimAffectTarget = 18,       // set_target: IntArg = target id (0 none), Values[0] = sub-target
    OracleSimAffectEquip = 19,        // slot_4c: IntArg = equipment id, Values[0] = activate (1 or 0)
    OracleSimAffectCruise = 20,       // slot_50: IntArg = engage (1 or 0), Values[0] = the second flag
    OracleSimAffectThrusters = 21     // slot_54: IntArg = activate (1 or 0)
};

struct FOracleSimHooks
{
    // Non-zero: Server_StepSystems steps this one system and no other, and StarSystem_Update
    // runs its multiplayer physics and collision pass although the system has no player.
    unsigned int StepSystemId;
    // PhysicalSystem::Update takes the single-player branch (one clamped step per call)
    // while the server otherwise runs in multiplayer mode.
    bool bSinglePlayerStepping;
    // StarSystem_MpCollisionFilter takes its single-player branch (every pair collides) while
    // the server otherwise runs in multiplayer mode (P2 load draws: the collide scenario).
    bool bSinglePlayerCollisions;
    // The timer stamps read ClockSeconds and IServerImpl::ElapseTime's real dt is
    // FrameSeconds, instead of the performance counter.
    bool bFixedClock;
    double ClockSeconds;
    double FrameSeconds;

    // Phase 0 before the simulation units of a PSI, phase 1 after the next-PSI matrices are
    // committed (before hull managers and contacts).
    void(__cdecl* OnPsi)(IVP_Environment* Environment, int Phase);
    // IVP_Simulation_Unit_Process_One_Psi_Step: phase 0 on entry (only EventSim->sim_unit is
    // set), phase 1 before it returns. bMovementCheck (phase 1) is whether the environment's
    // movement-check countdown came to zero at this unit in this PSI. Unit is only an
    // identity at phase 1 (the unit may have been rebalanced).
    void(__cdecl* OnSimUnitPsi)(IVP_Environment* Environment, IVP_Simulation_Unit* Unit, int Phase,
                                bool bMovementCheck);
    // IVP_Core::push_core_linear, push_core_angular_ws and push_core_angular_cs on entry
    // (Kind an EOracleSimPush).
    void(__cdecl* OnCorePush)(IVP_Core* Core, int Kind, Vector const* Direction, float Magnitude);
    // GameObject::on_collision: Collision::CalcCollisionDamage(Speed, OtherMass, OwnMass)
    // and its result, with the velocities the speed came from.
    void(__cdecl* OnCollisionDamage)(unsigned int SourceId, unsigned short SourceSubObj,
                                     unsigned int TargetId, unsigned short TargetSubObj,
                                     Vector const& OwnVelocity, Vector const& OtherVelocity,
                                     float Speed, float OtherMass, float OwnMass, float Damage);
    // StarSystem::munition_collision after the target's munition_hit, before the damage list
    // is applied.
    void(__cdecl* OnMunitionHit)(unsigned int TargetId, unsigned int ShooterId,
                                 unsigned int MunitionArchId, unsigned short SubObjId,
                                 Vector const& HitPos, DamageList const& Damage);
    // IBehaviorManager::UpdateBehaviorTick: phase 0 on entry, phase 1 on every exit.
    void(__cdecl* OnBehaviorTick)(IBehaviorManager const* Manager, float Dt, int Phase);
    // Ship's IObjAffect setters and EqObj::slot_34, on entry.
    void(__cdecl* OnAffect)(unsigned int ObjectId, int Slot, int IntArg, float const* Values,
                            int Count);
    // The number of instrumented IObjAffect calls in progress (FOracleSimAffectScope), so
    // OnAffect can tell an outer call from one an outer call makes (depth 0 is the caller's).
    int AffectDepth;
    // P2.4 (docking points): CEqObj::activate before it returns (the index as given, the
    // request and the ObjActivateResult), and CEqObj::update_docking_animations for each point
    // with a channel, before the point is processed (its active flag, GetStoredDuration,
    // GetScriptDuration and LastDuration).
    void(__cdecl* OnDockActivate)(CObject const* Object, unsigned Index, bool bOn, int Result);
    void(__cdecl* OnDockAnim)(CObject const* Object, unsigned Index, bool bActive, float Duration,
                              float Position, float LastDuration);
};

COMMON_API extern FOracleSimHooks GOracleSimHooks;

// Timing::read(), or ClockSeconds while the clock is fixed.
COMMON_API double __cdecl OracleSimTimingRead();
// Replaces RealDt by FrameSeconds while the clock is fixed.
COMMON_API void __cdecl OracleSimFixElapsed(double& RealDt);
// The PSI length (delta_time) of an environment OnPsi reports.
COMMON_API float __cdecl OracleSimEnvDeltaTime(IVP_Environment const* Environment);
// Copies an environment's PSI values and anomaly limits (Out zeroed for a null environment).
COMMON_API void __cdecl OracleSimReadEnv(IVP_Environment const* Environment, FOracleSimEnv& Out);
// The IVP core of Object's body, null when it has none.
COMMON_API IVP_Core* __cdecl OracleSimCoreOf(CObject const* Object);
// The core's simulation unit (OwningSimUnit), null for a null core.
COMMON_API IVP_Simulation_Unit* __cdecl OracleSimUnitOf(IVP_Core const* Core);
// The number of cores in a unit, 0 for a null unit.
COMMON_API int __cdecl OracleSimUnitCoreCount(IVP_Simulation_Unit const* Unit);
// The unit's controllers (IVP_Controller*, the bucket keys) in the order its PSI runs them
// (the last bucket first), at most Max; their count.
COMMON_API int __cdecl OracleSimUnitControllers(IVP_Simulation_Unit const* Unit, void const** Out, int Max);
// The CObject of a unit's core Index (its first object's client_data), null when there is none.
COMMON_API CObject* __cdecl OracleSimUnitCoreObject(IVP_Simulation_Unit const* Unit, int Index);
// Copies a core; false (Out zeroed) for a null core.
COMMON_API bool __cdecl OracleSimReadCore(IVP_Core const* Core, FOracleSimCore& Out);
// Copies the manager's state; false (Out zeroed, StateGraphId -1) for a null manager.
COMMON_API bool __cdecl OracleSimReadBehavior(IBehaviorManager const* Manager, FOracleSimBehavior& Out);
// The behaviour manager of a CEqObj (null for anything else or when it has none).
COMMON_API IBehaviorManager const* __cdecl OracleSimBehaviorOf(CObject* Object);

// An instrumented IObjAffect call in progress: constructed before OnAffect, so OnAffect sees
// its own call at AffectDepth 1.
struct FOracleSimAffectScope
{
    FOracleSimAffectScope() { ++GOracleSimHooks.AffectDepth; }
    ~FOracleSimAffectScope() { --GOracleSimHooks.AffectDepth; }
    FOracleSimAffectScope(FOracleSimAffectScope const&) = delete;
    FOracleSimAffectScope& operator=(FOracleSimAffectScope const&) = delete;
};

// UpdateBehaviorTick's hook: OnBehaviorTick(phase 0) when constructed, phase 1 when destroyed.
struct FOracleSimBehaviorScope
{
    IBehaviorManager const* Manager;
    float Dt;

    FOracleSimBehaviorScope(IBehaviorManager const* InManager, float InDt) : Manager(InManager), Dt(InDt)
    {
        if (GOracleSimHooks.OnBehaviorTick != nullptr)
            GOracleSimHooks.OnBehaviorTick(Manager, Dt, 0);
    }

    ~FOracleSimBehaviorScope()
    {
        if (GOracleSimHooks.OnBehaviorTick != nullptr)
            GOracleSimHooks.OnBehaviorTick(Manager, Dt, 1);
    }

private:
    FOracleSimBehaviorScope(FOracleSimBehaviorScope const&);
    FOracleSimBehaviorScope& operator=(FOracleSimBehaviorScope const&);
};
