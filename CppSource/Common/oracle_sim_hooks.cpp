// Oracle instrumentation (P2.3 sim): the hook block and the state readers of the scenario and
// replay oracle (include/oracle_sim_hooks.h, design/adr/0003-simulation-oracle.md). Nothing in
// the game writes GOracleSimHooks, so every hook stays inert outside Oracle.exe sim; the
// readers are called only by the harness.

#include "oracle_sim_hooks.h"

// The include order of the PhySys translation units (common_physicalsystem.cpp), which reach
// the same IVP headers.
#include "common_physys.h"
#include "common_cobject.h"
#include "common_iface.h"
#include "common_newchar.h"   // IStateGraph
#include "common_timing.h"
#include "../IVP/ivp_collision/ivp_collision.h"
#include "../IVP/ivp_physics/ivp_core.h"
#include "../IVP/ivp_physics/ivp_real_object.h"
#include "../IVP/ivp_physics/ivp_environment.h"
#include "../IVP/ivp_physics/ivp_anomaly_manager.h"
#include "../IVP/ivp_physics/ivp_simulation_unit.h"
#include "../IVP/ivp_utility/ivp_random.h"

#include <cstring>

FOracleSimHooks GOracleSimHooks = {};

namespace
{
    void CopyVector(float* Out, Vector const& In)
    {
        Out[0] = In.x;
        Out[1] = In.y;
        Out[2] = In.z;
    }

    void CopyQuat(float* Out, IVP_U_Quat const& In)
    {
        Out[0] = In.x;
        Out[1] = In.y;
        Out[2] = In.z;
        Out[3] = In.w;
    }

    void CopyTime(float* Out, IVP_Time const& In)
    {
        Out[0] = In.time_base;
        Out[1] = In.time_offset;
    }
}

double __cdecl OracleSimTimingRead()
{
    if (GOracleSimHooks.bFixedClock)
        return GOracleSimHooks.ClockSeconds;
    return Timing::read();
}

void __cdecl OracleSimFixElapsed(double& RealDt)
{
    if (GOracleSimHooks.bFixedClock)
        RealDt = GOracleSimHooks.FrameSeconds;
}

float __cdecl OracleSimEnvDeltaTime(IVP_Environment const* Environment)
{
    return Environment != nullptr ? Environment->delta_time : 0.0f;
}

void __cdecl OracleSimReadEnv(IVP_Environment const* Environment, FOracleSimEnv& Out)
{
    std::memset(&Out, 0, sizeof(Out));
    if (Environment == nullptr)
        return;
    Out.DeltaTime = Environment->delta_time;
    Out.InvDeltaTime = Environment->inv_delta_time;
    CopyTime(Out.Time, Environment->current_time);
    Out.MovementCheck = Environment->next_movement_check;
    Out.IvpSeed = IVP_FastRandomSeed();
    if (Environment->anomaly_limits != nullptr)
    {
        Out.MaxVelocity = Environment->anomaly_limits->MaxVelocity;
        Out.MaxAngularVelocityPerPsi = Environment->anomaly_limits->MaxAngularVelocityPerPsi;
    }
}

IVP_Core* __cdecl OracleSimCoreOf(CObject const* Object)
{
    if (Object == nullptr || Object->BodyPtr == nullptr)
        return nullptr;
    return Object->BodyPtr->core;
}

IVP_Simulation_Unit* __cdecl OracleSimUnitOf(IVP_Core const* Core)
{
    return Core != nullptr ? Core->OwningSimUnit : nullptr;
}

int __cdecl OracleSimUnitCoreCount(IVP_Simulation_Unit const* Unit)
{
    return Unit != nullptr ? static_cast<int>(Unit->Cores.n_elems) : 0;
}

int __cdecl OracleSimUnitControllers(IVP_Simulation_Unit const* Unit, void const** Out, int Max)
{
    if (Unit == nullptr)
        return 0;
    // IVP_Simulation_Unit_Process_One_Psi_Step runs the buckets from the last to the first.
    int Count = 0;
    for (int Index = static_cast<int>(Unit->ControllerBuckets.n_elems) - 1; Index >= 0 && Count < Max;
         --Index)
        Out[Count++] = Unit->ControllerBuckets.elems[Index]->Controller;
    return Count;
}

bool __cdecl OracleSimReadCore(IVP_Core const* Core, FOracleSimCore& Out)
{
    std::memset(&Out, 0, sizeof(Out));
    if (Core == nullptr)
        return false;

    Out.Flags = Core->Flags;
    Out.MovementState = Core->MovementState;
    if (Core->OwningSimUnit != nullptr)
        Out.UnitState = Core->OwningSimUnit->State;
    Out.UpperLimitRadius = Core->UpperLimitRadius;
    Out.SpeedLimit = Core->SpeedLimit;
    Out.Mass = Core->Mass;
    Out.InvMass = Core->InvMass;
    CopyVector(Out.RotInertia, Core->RotInertia);
    CopyVector(Out.InvRotInertia, Core->InvRotInertia);
    CopyVector(Out.RotSpeedDamp, Core->RotSpeedDamp);
    Out.SpeedDamp = Core->SpeedDamp;
    Out.DAngularLimitFactor = Core->DAngularLimitFactor;
    if (Core->SpinClipping != nullptr)
    {
        Out.bSpinClipping = 1;
        CopyVector(Out.SpinClipping, *Core->SpinClipping);
    }
    CopyTime(Out.TimeOfLastPsi, Core->TimeOfLastPsi);
    Out.IDeltaTime = Core->IDeltaTime;
    CopyVector(Out.RotSpeedChange, Core->RotSpeedChange);
    CopyVector(Out.SpeedChange, Core->SpeedChange);
    CopyVector(Out.RotSpeed, Core->RotSpeed);
    CopyVector(Out.Speed, Core->Speed);
    CopyVector(Out.Pos, Core->PosWorldFCore);
    CopyVector(Out.DeltaWorld, Core->DeltaWorldFCore);
    CopyQuat(Out.Quat, Core->QWorldFCore);
    CopyQuat(Out.QuatNext, Core->QWorldFCoreNext);
    for (int Row = 0; Row < 3; ++Row)
    {
        for (int Column = 0; Column < 3; ++Column)
            Out.Matrix[Row * 3 + Column] = Core->MWorldFCore.rows[Row].k[Column];
    }
    for (int Index = 0; Index < 3; ++Index)
        Out.Matrix[9 + Index] = Core->MWorldFCore.vv.k[Index];
    CopyVector(Out.RotAxis, Core->RotAxisOS);
    Out.Margin = Core->Margin;
    Out.MaxSurfaceDeviation = Core->MaxSurfaceDeviation;
    Out.BoundingRadius = Core->BoundingRadius;
    CopyTime(Out.TimeOfLastMotion, Core->TimeOfLastMotion);
    CopyTime(Out.TimeOfLastLongMotion, Core->TimeOfLastLongMotion);
    CopyQuat(Out.QLastMotion, Core->QLastMotion);
    CopyQuat(Out.QLastLongMotion, Core->QLastLongMotion);
    CopyVector(Out.PosLastMotion, Core->PosLastMotion);
    CopyVector(Out.PosLastLongMotion, Core->PosLastLongMotion);
    return true;
}

bool __cdecl OracleSimReadBehavior(IBehaviorManager const* Manager, FOracleSimBehavior& Out)
{
    std::memset(&Out, 0, sizeof(Out));
    Out.StateGraphId = -1;
    if (Manager == nullptr)
        return false;
    Out.BehaviorIndex = Manager->CurrentBehaviorIndex;
    std::memcpy(Out.Directive, Manager->DirectiveStorage, sizeof(Out.Directive));
    if (Manager->StateGraph != nullptr)
        Out.StateGraphId = Manager->StateGraph->Id;
    Out.bSuppressed = Manager->TickSuppressedFlag != 0 ? 1 : 0;
    // The same `W ? W - 8 : 0` downcast UpdateBehaviorTick guards with (common_iface.h).
    if (Manager->WatchablePtr != nullptr && static_cast<char*>(Manager->WatchablePtr) - 8 != nullptr)
    {
        Out.Object = ObjRwFromWatchable(Manager->WatchablePtr);
        Out.ObjectId = Out.Object->get_id();
    }
    return true;
}

IBehaviorManager const* __cdecl OracleSimBehaviorOf(CObject* Object)
{
    if (Object == nullptr)
        return nullptr;
    if ((static_cast<unsigned int>(Object->ObjectClass) & 0x103u) != 0x103u)
        return nullptr;
    return static_cast<CEqObj*>(Object)->get_behavior_interface();
}
