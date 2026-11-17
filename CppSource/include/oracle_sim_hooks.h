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
//                               (CalcCollisionDamage in and out); Ship's IObjAffect setters and
//                               EqObj::slot_34 (OnAffect)
//   Server/server_classes.cpp   StarSystem::munition_collision (OnMunitionHit)
//   Common/common_physicalsystem.cpp  PhysicalSystem::Update (single-player stepping)
//   Common/common_behaviormanager.cpp IBehaviorManager::UpdateBehaviorTick (OnBehaviorTick)
//   IVP/ivp_physics/ivp_environment.cpp IVP_Environment_Simulate_Psi (OnPsi)
//
// The definitions and the readers below are in Common/oracle_sim_hooks.cpp.

#pragma once

#include "common_exports.h"

struct CObject;
struct DamageList;
struct IObjRW;
class IBehaviorManager;
class IVP_Environment;
class Vector;

// One IVP core as OracleSimReadCore copies it (ivp_core.h field names in the comments).
struct FOracleSimCore
{
    float Pos[3];              // PosWorldFCore
    float Quat[4];             // QWorldFCore, x y z w
    float Speed[3];            // Speed (world)
    float RotSpeed[3];         // RotSpeed (core space)
    float SpeedChange[3];      // SpeedChange
    float RotSpeedChange[3];   // RotSpeedChange
    float DeltaWorld[3];       // DeltaWorldFCore
    float Mass;                // Mass
    float InvMass;             // InvMass
    float RotInertia[3];       // RotInertia
    float InvRotInertia[3];    // InvRotInertia
    float SpeedDamp;           // SpeedDamp
    float RotSpeedDamp[3];     // RotSpeedDamp
    float UpperLimitRadius;    // UpperLimitRadius
    float SpeedLimit;          // SpeedLimit
    float IDeltaTime;          // IDeltaTime
    float TimeOfLastPsi[2];    // TimeOfLastPsi base, offset
    unsigned int MovementState; // MovementState
    unsigned int Flags;        // Flags
    int bSpinClipping;         // SpinClipping != null
    float SpinClipping[3];     // *SpinClipping, zero when null
    float EnvDeltaTime;        // Environment->delta_time, the PSI length
    float EnvTime[2];          // Environment->current_time base, offset
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
    OracleSimAffectFire = 13          // IntArg = equipment count, Values = target position or none
};

struct FOracleSimHooks
{
    // Non-zero: Server_StepSystems steps this one system and no other, and StarSystem_Update
    // runs its multiplayer physics and collision pass although the system has no player.
    unsigned int StepSystemId;
    // PhysicalSystem::Update takes the single-player branch (one clamped step per call)
    // while the server otherwise runs in multiplayer mode.
    bool bSinglePlayerStepping;
    // The timer stamps read ClockSeconds and IServerImpl::ElapseTime's real dt is
    // FrameSeconds, instead of the performance counter.
    bool bFixedClock;
    double ClockSeconds;
    double FrameSeconds;

    // Phase 0 before the simulation units of a PSI, phase 1 after the next-PSI matrices are
    // committed (before hull managers and contacts).
    void(__cdecl* OnPsi)(IVP_Environment* Environment, int Phase);
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
};

COMMON_API extern FOracleSimHooks GOracleSimHooks;

// Timing::read(), or ClockSeconds while the clock is fixed.
COMMON_API double __cdecl OracleSimTimingRead();
// Replaces RealDt by FrameSeconds while the clock is fixed.
COMMON_API void __cdecl OracleSimFixElapsed(double& RealDt);
// The PSI length (delta_time) of an environment OnPsi reports.
COMMON_API float __cdecl OracleSimEnvDeltaTime(IVP_Environment const* Environment);
// Copies the IVP core of Object's body; false (Out zeroed) when it has none.
COMMON_API bool __cdecl OracleSimReadCore(CObject const* Object, FOracleSimCore& Out);
// Copies the manager's state; false (Out zeroed, StateGraphId -1) for a null manager.
COMMON_API bool __cdecl OracleSimReadBehavior(IBehaviorManager const* Manager, FOracleSimBehavior& Out);
// The behaviour manager of a CEqObj (null for anything else or when it has none).
COMMON_API IBehaviorManager const* __cdecl OracleSimBehaviorOf(CObject* Object);

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
