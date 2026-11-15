// Author: Giperion.

#include "ivp_controller.h"
#include "ivp_core.h"
#include "ivp_simulation_unit.h"
#include "ivp_real_object.h"  // IVP_Real_Object::environment / core (IVP_Controller_Motion ctor)

#include <cmath>
#include <cstdint>
#include <cstdlib>

// ============================================================================
// D-cluster-06348f60 pass (2026-09-10): the small family of free functions
// that drive an IVP_Controller's OWN associated-cores list (via its vtable
// slot 2, get_associated_controlled_cores(), see ivp_controller.h) to keep
// the affected IVP_Simulation_Unit(s) in sync whenever a controller is
// attached to, detached from, or otherwise changes the set of cores it
// governs. Ported from Common (implementation tracker items
// 2926/2927/2930/2945). Companion functions that instead take an
// IVP_Simulation_Unit* as their primary subject live in
// ivp_simulation_unit.cpp, not here.
// ============================================================================

// (Common, implementation tracker item 2926, 48
// insns). Two stack args: a Controller* and a second int/bool-shaped value
// (analysis's own param table only formally lists the first -- the second is a
// genuine, silently-undetected 2nd stack argument, confirmed by this port's
// own byte-for-byte trace of `MOV EDX,[ESP+0x14]` at Common,
// matching this project's known "XREF/param-table unreliable" precedent).
// For every core currently associated with Controller (backward walk):
// detaches Controller from it (IVP_Core::detach_controller, item 2931), then
// re-reads that core's OwningSimUnit (which detach_controller does NOT
// change -- only its controller-bucket membership) to track the last core
// processed. After the walk, if any core was found, flips 2 bits in the
// surviving unit's State (clear bit 0x200, set bit 0x100 -- an as-yet
// unidentified "controller topology changed" marker, reproduced verbatim
// without a confirmed name), then -- unless the 2nd argument is nonzero --
// runs the same "update last-motion-times if awake, else revive if asleep"
// dispatch every other function in this file's own header ends with.
void IVP_Controller_Detach_From_All_Cores(IVP_Controller* Controller, int SkipFinalWakeCheck)
{
    IVP_U_Vector<IVP_Core>* Cores = Controller->get_associated_controlled_cores();
    IVP_Simulation_Unit* SimUnit = nullptr;

    for (int i = static_cast<int>(Cores->n_elems) - 1; i >= 0; --i)
    {
        IVP_Core* Core = Cores->ElementAt(i);
        Core->detach_controller(Controller); // , item 2931
        SimUnit = Core->OwningSimUnit;        // re-read AFTER detach -- unchanged by it, just tracked here
    }

    if (SimUnit == nullptr)
    {
        return;
    }

    // Commonf9c-06348fa8: `AND EAX,0xfffffdff; OR EAX,0x100`.
    SimUnit->State = (SimUnit->State & 0xFFFFFDFFu) | 0x100u;

    if (SkipFinalWakeCheck != 0)
    {
        return;
    }

    if (static_cast<int8_t>(SimUnit->State & 0xFF) < 8)
    {
        IVP_Simulation_Unit_Update_Cores_Last_Motion_Time(SimUnit);
    }
    else
    {
        IVP_Core* FirstCore = SimUnit->Cores.ElementAt(0);
        IVP_Simulation_Unit_Revive_If_Sleeping(SimUnit, FirstCore->Environment);
    }
}

// (Common, implementation tracker item 2927, 20
// insns). Single stack arg: Controller*. Looks only at the FIRST core in
// Controller's associated-cores list (elems[0], not a full walk -- no
// detach, nothing is mutated here), resolves ITS OwningSimUnit, and runs the
// same "update last-motion-times if awake, else revive if asleep" dispatch
// as the rest of this file. No-op if Controller currently has zero
// associated cores. Likely the "controller newly attached, refresh the
// affected unit" counterpart to item 2926's "controller detaching, refresh
// after removal" -- not independently confirmed (both functions' own
// callers, Commonbc7-1f/06285ca6-cff, sit outside this
// cluster's assigned address range and were not traced this pass).
void IVP_Controller_Wake_Or_Refresh_Associated_Sim_Unit(IVP_Controller* Controller)
{
    IVP_U_Vector<IVP_Core>* Cores = Controller->get_associated_controlled_cores();
    if (Cores->n_elems == 0)
    {
        return;
    }

    IVP_Core* FirstCore = Cores->ElementAt(0);
    IVP_Simulation_Unit* SimUnit = FirstCore->OwningSimUnit;

    if (static_cast<int8_t>(SimUnit->State & 0xFF) < 8)
    {
        IVP_Simulation_Unit_Update_Cores_Last_Motion_Time(SimUnit);
    }
    else
    {
        IVP_Core* UnitFirstCore = SimUnit->Cores.ElementAt(0);
        IVP_Simulation_Unit_Revive_If_Sleeping(SimUnit, UnitFirstCore->Environment);
    }
}

// (Common, implementation tracker item 2930, 71
// insns). GENUINELY two-parameter (thiscall ECX + one stack arg), but ECX
// is NOT used as a real `this` here -- ported as an explicit 2nd parameter.
// Full raw trace (re-derived twice this pass: once by hand-tracing the raw
// bytes, once cross-checked against Common's own analysis at
// decomp_line 201788, which independently produced the equivalent
// `the routine(*param_1_00)` at its very end):

//   - Stack arg = IVP_Controller* Controller (its vtable slot 2 is
//     dereferenced for get_associated_controlled_cores(), same as every
//     other function in this file).
//   - Original ECX (`EnvironmentPtrHolder` below) is read exactly ONCE, at
//     the very end (Common... region), and
//     dereferenced a single level to yield the IVP_Environment* passed to
//     Revive_If_Sleeping. implementation tracker's own local_4 XREF annotation
//     claims this stack slot is write-only (Common param table);
//     this port's own byte-accurate stack-frame trace shows that's WRONG --
//     it IS read back, matching the same "XREF unreliable" precedent this
//     project has hit repeatedly. This function's own single caller
//     (Commoncf0) sits outside this cluster's assigned
//     address range and was not traced this pass, so EnvironmentPtrHolder's
//     real-world provenance (almost certainly `&SomeCore->Environment` or
//     similar, by analogy with items 2926/2927 above) is not independently
//     confirmed -- an honest, documented gap, not a guess.

// Walks every core associated with Controller (backward). Cores with
// (Flags & 0xC) != 0 are skipped entirely (no register_controller call
// either). For every OTHER core: AND-accumulates its MovementState low byte
// into MovementByteAcc (init 8); tracks the previous iteration's
// OwningSimUnit as PrevSimUnit, and whenever two consecutive valid cores
// disagree on SimUnit, merges them (absorbing the newer one into
// PrevSimUnit and freeing the now-empty absorbed unit's vector buffers +
// memory -- same pattern IVP_Simulation_Unit_Detach_Core already uses
// inline), then unconditionally re-registers the core with the controller
// (IVP_Core::register_controller, item 2932). After the walk: if any merge
// happened, rebuilds PrevSimUnit's controller buckets from scratch; if the
// accumulated verdict ended up below 8 (at least one processed core is
// non-static), revives PrevSimUnit if it was asleep.
void IVP_Controller_Reconcile_Associated_Cores(IVP_Environment* const* EnvironmentPtrHolder, IVP_Controller* Controller)
{
    IVP_U_Vector<IVP_Core>* Cores = Controller->get_associated_controlled_cores();

    IVP_Simulation_Unit* PrevSimUnit = nullptr;
    bool MergeHappened = false;
    int MovementByteAcc = 8;

    for (int i = static_cast<int>(Cores->n_elems) - 1; i >= 0; --i)
    {
        IVP_Core* Core = Cores->ElementAt(i);
        if ((Core->Flags & 0xc) != 0)
        {
            continue; // dying/invalid core -- skipped entirely, not even re-registered
        }

        MovementByteAcc &= static_cast<int8_t>(Core->MovementState & 0xFF);
        IVP_Simulation_Unit* CurrentSimUnit = Core->OwningSimUnit;

        if (PrevSimUnit != nullptr && CurrentSimUnit != PrevSimUnit)
        {
            // Two neighbouring cores in the SAME controller's associated-list
            // disagree on their owning unit -- merge them (the routine).
            IVP_Simulation_Unit_Absorb_Cores_From(PrevSimUnit, CurrentSimUnit);
            if (CurrentSimUnit != nullptr)
            {
                // the routine (the unit's destructor
                // body: Clear_Controller_Buckets, then both vector buffers) and
                // operator delete (the routine), as in IVP_Simulation_Unit_Detach_Core.
                IVP_Simulation_Unit_Free_Vector_Buffers(CurrentSimUnit);
                ::operator delete(CurrentSimUnit);
            }
            MergeHappened = true;
        }
        else if (PrevSimUnit == nullptr)
        {
            PrevSimUnit = CurrentSimUnit;
        }

        Core->register_controller(Controller); // , item 2932
    }

    if (MergeHappened)
    {
        IVP_Simulation_Unit_Clear_Controller_Buckets(PrevSimUnit);
        IVP_Simulation_Unit_Rebuild_Controller_Buckets(PrevSimUnit);
    }

    if (MovementByteAcc < 8)
    {
        IVP_Simulation_Unit_Revive_If_Sleeping(PrevSimUnit, *EnvironmentPtrHolder);
    }
}

// ============================================================================
// IVP_Controller_Independent / IVP_Controller_Motion (2026-09-25, claude-code:h-ivp).
// Declared in ivp_controller.h with their RTTI and vftable evidence. Until this pass the
// Motion share lived folded inside IVP_Controller_Golem's constructor/destructor in
// src/Common/common_physys.cpp, and Motion's own slot 4 (the branch) was not ported.
// ============================================================================

// the one IVP_U_Vector<IVP_Core> every independent controller hands out from
// slot 2. baseline constructs it empty in the static initializer the branch -> the routine
// (module storage-06348783: memsize = n_elems = 0, elems = 0) and destroys it at exit through
// the atexit entry the routine -> the branch. The only other references to it are the slot-2
// body below (module storage) and those two; nothing ever adds an element.
static IVP_U_Vector<IVP_Core> G_IndependentControlledCores;

// Slot 2, the branch (module storage-06285a65): `MOV EAX,.data:the field; RET`.
IVP_U_Vector<IVP_Core>* IVP_Controller_Independent::get_associated_controlled_cores()
{
    return &G_IndependentControlledCores;
}

// (module storage-06342d3a, RET 0x8). The compiler supplies the vptr stamp
// (module storage, module storage) and the SEH frame that unwinds the (empty) Independent
// base if register_controller throws (state 0 at module storage).
IVP_Controller_Motion::IVP_Controller_Motion(IVP_Real_Object* InBody,
                                             IVP_Controller_Motion_Parms const& Parms)
{
    // zero vector at +0x04 and identity quaternion at +0x14.
    // The hesse words +0x10, +0x30 and +0x40 are never written, as in baseline.
    CurrentTargetPosition.set(0.0f, 0.0f, 0.0f);   // / 06342ca8 / 06342cab
    CurrentTargetQuat.x = 0.0f;     // / 06342cb1 / 06342cae
    CurrentTargetQuat.y = 0.0f;
    CurrentTargetQuat.z = 0.0f;
    CurrentTargetQuat.w = 1.0f;      // , 0x3f800000

    // . Traced through the x87 stack, which is what makes the order
    // non-obvious: FLD parms+0x14 pushes first, FLD parms+0x10 pushes second, so the FIRST
    // FSTP (to +0x24) pops parms+0x10 and the second (to +0x28) pops parms+0x14. The z
    // component goes the integer route (MOV ECX,[EAX+0x18] -> [ESI+0x2c]).
    MaxTranslationForce.set(Parms.MaxTranslationForce[0],
                            Parms.MaxTranslationForce[1],
                            Parms.MaxTranslationForce[2]);

    // parms+0x20 is FLD'd three times and popped into +0x34/+0x38/
    // +0x3c -- one scalar broadcast across all three axes, NOT three separate inputs.
    MaxTorque.set(Parms.MaxTorque, Parms.MaxTorque, Parms.MaxTorque);

    ForceFactor = Parms.ForceFactor;
    DampFactor = Parms.DampFactor;
    TorqueFactor = Parms.TorqueFactor;
    AngularDampFactor = Parms.AngularDampFactor;  // /06342d08

    // +0x18 of the body is IVP_Object::environment and +0x98 is
    // IVP_Real_Object::core. No null test on the body (module storage reads [EAX+0x18]
    // straight away).
    Environment = InBody->environment;
    Body = InBody;
    Core = InBody->core;

    // the routine(this, this->Core).
    IVP_Controller_Manager_Add_Controller_To_Core(this, Core);
}

// (module storage-0634902e, cdecl): IVP_Controller_Manager::add_controller_to_core
// -- `MOV EAX,[ESP+4]; MOV ECX,[ESP+8]; PUSH EAX; CALL the routine`, no null test.
void IVP_Controller_Manager_Add_Controller_To_Core(IVP_Controller* Controller, IVP_Core* Core)
{
    Core->register_controller(Controller);
}

// (module storage-0634903e, cdecl): IVP_Controller_Manager::remove_controller_from_core
// -- same shape, `CALL the routine`.
void IVP_Controller_Manager_Remove_Controller_From_Core(IVP_Controller* Controller, IVP_Core* Core)
{
    Core->detach_controller(Controller);
}

// (module storage-0634301d): stamp 063a53b4 (compiler), then
// (this, [this+0x5c]) -> the routine = IVP_Core::detach_controller(this) with no
// test on the core, then the IVP_Controller stamp 0639b64c at module storage (compiler; the
// empty Independent level leaves no stamp of its own). Its scalar deleting wrapper is
// (063a53b4[6]).
IVP_Controller_Motion::~IVP_Controller_Motion()
{
    IVP_Controller_Manager_Remove_Controller_From_Core(this, Core);
}

// Vftable slot 0, the branch (module storage-06342fcb): `TEST ECX,ECX; JZ out; MOV EAX,[ECX];
// PUSH 1; CALL [EAX+0x18]; RET 4` -- delete this through the virtual deleting destructor; the
// core argument is ignored. `this` is never null in a C++ member call, so the TEST has no
// counterpart. Inherited verbatim by the golem (063a53dc[0]) and both leaves (0639c1c8[0],
// [0]). baseline does not clear Core first: the destructor detaches from the dying core.
void IVP_Controller_Motion::core_is_going_to_be_deleted_event(IVP_Core* DeletedCore)
{
    (void)DeletedCore;
    delete this;
}

// Vftable slot 5, the branch (module storage-06285a75): `MOV EAX,0x1f4; RET` = 500.
// PhySys::Controller's own slot 5 returns 0x5dc (1500). No IVP_CONTROLLER_PRIORITY enumerator
// is confirmed for 500, so the literal is used.
IVP_CONTROLLER_PRIORITY IVP_Controller_Motion::get_controller_priority()
{
    return (IVP_CONTROLLER_PRIORITY)500;
}

// Vftable slot 4, the branch (module storage-06342fb0, RET 0x8). Read instruction by
// instruction, x87 stack tracked. EDI = EventSim (`MOV EDI,[ESP+0x68]` at module storage);
// EventSim+0x00 = dt and +0x04 = 1/dt. The same PD servo as IVP_Controller_Golem's slot 4
// (the branch, common_physys.cpp) MINUS its extrapolation step, its body-shift correction,
// its error accumulator and both teleport gates, and with ONE term that differs: the angular
// damping uses AngularDampFactor (+0x50, module storage) where the golem uses DampFactor.
// baseline never dispatches this body on a live golem (the golem overrides slot 4), but the
// class is concrete (063a53b4 has no _purecall) so the slot must exist.
void IVP_Controller_Motion::do_simulation_controller(IVP_Event_Sim* EventSim,
                                                     IVP_U_Vector<IVP_Core>* AffectedCores)
{
    (void)AffectedCores;  // RET 0x8 pops it; the body never reads it

    float const Dt = EventSim->delta_time;     // `FMUL [EDI]`
    float const IDt = EventSim->i_delta_time;  // [EDI+4], spilled over the arg slot at 06342dd6

    // Delta = (CurrentTargetPosition - Core's world position) *
    // ForceFactor. Core+0x124..+0x12c is MWorldFCore.vv (read as [Core+0x128], then with
    // EAX = Core+0xa4 as [EAX+0x88] and [EAX+0x80]). ForceFactor stays in ST0; each component
    // (target - vv) * ForceFactor is formed on the FPU and rounded once by its FSTP.
    IVP_U_Point Delta;
    Delta.k[0] = (float)(((double)CurrentTargetPosition.k[0] - Core->MWorldFCore.vv.k[0]) * ForceFactor);
    Delta.k[1] = (float)(((double)CurrentTargetPosition.k[1] - Core->MWorldFCore.vv.k[1]) * ForceFactor);
    Delta.k[2] = (float)(((double)CurrentTargetPosition.k[2] - Core->MWorldFCore.vv.k[2]) * ForceFactor);

    // FLD DampFactor; FMUL dt; FCHS; FSTP float argument ->
    // (this = &Delta, v1 = &Delta, v2 = &Core->Speed (Core+0xa4), f).
    // (module storage-0628be8e) forms f*v2[i] + v1[i] on the FPU and rounds once.
    float const NegDampDt = -(DampFactor * Dt);
    Delta.k[0] = (float)((double)NegDampDt * Core->Speed.x + Delta.k[0]);
    Delta.k[1] = (float)((double)NegDampDt * Core->Speed.y + Delta.k[1]);
    Delta.k[2] = (float)((double)NegDampDt * Core->Speed.z + Delta.k[2]);

    // per-axis force clamp, walked z -> x (ECX = &Delta.z,
    // EDX = &MaxTranslationForce.z, both SUB 4, EBP counts 3), (1/dt)^2 kept live in ST0.
    //   FLD Mass; FMUL Delta[i]; FMUL (1/dt)^2; FABS; FCOMP Max[i]; `TEST AH,0x1` + JNZ skip
    //     -> C0 (strictly less, or unordered) skips, so the clamp runs on >=.
    //   FLD Delta[i]; FCOMP 0.0f (the field) -> C0 = Delta < 0;
    //   FLD InvMass; FMUL dt; FMUL dt; FMUL Max[i] (on the FPU); `TEST AH,0x1` + JZ -> FCHS
    //   when C0; FSTP float.
    {
        double const IDtSquared = (double)IDt * IDt;
        for (int Axis = 2; Axis >= 0; --Axis)
        {
            double const AxisForce = fabs((double)Core->Mass * Delta.k[Axis] * IDtSquared);
            if (AxisForce >= MaxTranslationForce.k[Axis])
            {
                double Clamped = (double)Core->InvMass * Dt * Dt * MaxTranslationForce.k[Axis];
                if (Delta.k[Axis] < 0.0f)
                    Clamped = -Clamped;
                Delta.k[Axis] = (float)Clamped;
            }
        }
    }

    // the routine(this = &Core->Speed, v1 = &Core->Speed,
    // v2 = &Delta, f = 1/dt) -> Speed[i] = float((1/dt)*Delta[i] + Speed[i]).
    Core->Speed.x = (float)((double)IDt * Delta.k[0] + Core->Speed.x);
    Core->Speed.y = (float)((double)IDt * Delta.k[1] + Core->Speed.y);
    Core->Speed.z = (float)((double)IDt * Delta.k[2] + Core->Speed.z);

    // the routine (set_invert_unit of CurrentTargetQuat), then
    // (this = Rel, q1 = the inverse, q2 = Core->QWorldFCoreNext at Core+0xe4),
    // then the routine on Rel writing the rotation-error vector -- the same three calls, in
    // the same order, as the golem's slot 4 (see IVP_DoubleAsinVector there).
    IVP_U_Quat InvTargetQuat;
    InvTargetQuat.set_invert_unit(&CurrentTargetQuat);

    IVP_U_Quat RelativeQuat;
    RelativeQuat.set_mult_quat(&InvTargetQuat, &Core->QWorldFCoreNext);

    IVP_U_Point RelativeQuatXYZ;
    RelativeQuatXYZ.set(RelativeQuat.x, RelativeQuat.y, RelativeQuat.z);
    IVP_U_Point RotError;
    IVP_DoubleAsinVector(&RelativeQuatXYZ, &RotError);

    // ((w*w' + z*z') + y*y') + x*x', Core quaternion times
    // CurrentTargetQuat, summed on the FPU, then FCOMP 0.0f.
    double const QuatDot =
        (((double)Core->QWorldFCoreNext.w * CurrentTargetQuat.w
          + (double)Core->QWorldFCoreNext.z * CurrentTargetQuat.z)
         + (double)Core->QWorldFCoreNext.y * CurrentTargetQuat.y)
        + (double)Core->QWorldFCoreNext.x * CurrentTargetQuat.x;

    // scale = (1/dt) * TorqueFactor stays in ST0; `TEST AH,0x1` + JZ
    // to module storage, whose first instruction is FCHS -- so the scale is NEGATED when the
    // dot is non-negative (C0 clear; a NaN dot sets C0 and keeps the sign), exactly as in the
    // golem. Both arms then form x, y, z = float(RotError[i] * scale).
    double RotScale = (double)IDt * TorqueFactor;
    if (QuatDot >= 0.0)
        RotScale = -RotScale;

    RotError.k[0] = (float)(RotError.k[0] * RotScale);
    RotError.k[1] = (float)(RotError.k[1] * RotScale);
    RotError.k[2] = (float)(RotError.k[2] * RotScale);

    // FLD AngularDampFactor (+0x50); FCHS; then per axis
    // float((-AngularDampFactor) * RotSpeed[i] (Core+0x94/+0x98/+0x9c) + RotError[i]).
    float RotDelta[3];
    RotDelta[0] = (float)(-(double)AngularDampFactor * Core->RotSpeed.x + RotError.k[0]);
    RotDelta[1] = (float)(-(double)AngularDampFactor * Core->RotSpeed.y + RotError.k[1]);
    RotDelta[2] = (float)(-(double)AngularDampFactor * Core->RotSpeed.z + RotError.k[2]);

    // torque clamp, z -> x. EBP = Core - this, so [ECX+EBP-0x20] with
    // ECX = &MaxTorque[i] (this+0x34+4i) is Core+0x14+4i = RotInertia[i], and [ECX+EBP] is
    // Core+0x34+4i = InvRotInertia[i].
    //   FLD 1/dt; FMUL RotInertia[i]; FMUL RotDelta[i]; FLD ST0; FABS; FCOMP MaxTorque[i];
    //   `TEST AH,0x1` + JNZ skip (pop) -> clamp on >=;
    //   FCOMP 0.0f on the SIGNED torque; FLD InvRotInertia[i]; FMUL dt; FMUL MaxTorque[i];
    //   FCHS when C0 (torque < 0); FSTP float. All products stay on the FPU.
    {
        float const RotInertiaAxis[3] = {Core->RotInertia.x, Core->RotInertia.y,
                                         Core->RotInertia.z};
        float const InvRotInertiaAxis[3] = {Core->InvRotInertia.x, Core->InvRotInertia.y,
                                            Core->InvRotInertia.z};
        for (int Axis = 2; Axis >= 0; --Axis)
        {
            double const AxisTorque = (double)IDt * RotInertiaAxis[Axis] * RotDelta[Axis];
            if (fabs(AxisTorque) >= MaxTorque.k[Axis])
            {
                double Clamped = (double)InvRotInertiaAxis[Axis] * Dt * MaxTorque.k[Axis];
                if (AxisTorque < 0.0)
                    Clamped = -Clamped;
                RotDelta[Axis] = (float)Clamped;
            }
        }
    }

    // RotSpeed[i] = RotDelta[i] + RotSpeed[i].
    Core->RotSpeed.x = RotDelta[0] + Core->RotSpeed.x;
    Core->RotSpeed.y = RotDelta[1] + Core->RotSpeed.y;
    Core->RotSpeed.z = RotDelta[2] + Core->RotSpeed.z;
}
