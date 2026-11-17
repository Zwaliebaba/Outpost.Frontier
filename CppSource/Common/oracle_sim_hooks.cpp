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

bool __cdecl OracleSimReadCore(CObject const* Object, FOracleSimCore& Out)
{
    std::memset(&Out, 0, sizeof(Out));
    if (Object == nullptr || Object->BodyPtr == nullptr)
        return false;
    IVP_Core const* const Core = Object->BodyPtr->core;
    if (Core == nullptr)
        return false;

    CopyVector(Out.Pos, Core->PosWorldFCore);
    Out.Quat[0] = Core->QWorldFCore.x;
    Out.Quat[1] = Core->QWorldFCore.y;
    Out.Quat[2] = Core->QWorldFCore.z;
    Out.Quat[3] = Core->QWorldFCore.w;
    CopyVector(Out.Speed, Core->Speed);
    CopyVector(Out.RotSpeed, Core->RotSpeed);
    CopyVector(Out.SpeedChange, Core->SpeedChange);
    CopyVector(Out.RotSpeedChange, Core->RotSpeedChange);
    CopyVector(Out.DeltaWorld, Core->DeltaWorldFCore);
    Out.Mass = Core->Mass;
    Out.InvMass = Core->InvMass;
    CopyVector(Out.RotInertia, Core->RotInertia);
    CopyVector(Out.InvRotInertia, Core->InvRotInertia);
    Out.SpeedDamp = Core->SpeedDamp;
    CopyVector(Out.RotSpeedDamp, Core->RotSpeedDamp);
    Out.UpperLimitRadius = Core->UpperLimitRadius;
    Out.SpeedLimit = Core->SpeedLimit;
    Out.IDeltaTime = Core->IDeltaTime;
    Out.TimeOfLastPsi[0] = Core->TimeOfLastPsi.time_base;
    Out.TimeOfLastPsi[1] = Core->TimeOfLastPsi.time_offset;
    Out.MovementState = Core->MovementState;
    Out.Flags = Core->Flags;
    if (Core->SpinClipping != nullptr)
    {
        Out.bSpinClipping = 1;
        CopyVector(Out.SpinClipping, *Core->SpinClipping);
    }
    if (Core->Environment != nullptr)
    {
        Out.EnvDeltaTime = Core->Environment->delta_time;
        Out.EnvTime[0] = Core->Environment->current_time.time_base;
        Out.EnvTime[1] = Core->Environment->current_time.time_offset;
    }
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
