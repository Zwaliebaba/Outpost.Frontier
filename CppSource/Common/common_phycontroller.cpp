// Author: Giperion.

#include "common_physys.h"

#include "common_archetype.h"
#include "common_cobject.h"
#include "common_cobjdb.h"   // IObjDB::GetZoneDb (PhySys::GenerateCollisions)
#include "common_id.h"
#include "common_universe.h" // Universe::ISystem (PhySys::GenerateCollisions)
#include "common_net.h"  // IsMPServer -- CRemotePhysicsSimulation::tracef (module storage)
#include "common_phyarch.h"
#include "common_misc.h"     // SinglePlayer
#include "common_server.h"

#include "neuroncomponents.h"
#include "NeuronRuntime.h"
#include "x86math.h"  // I3DMathEngine::MatrixToQuaternion, FQuaternion, FMatrix
#include "../IVP/ivp_collision/ivp_collision.h" // IVP_Collision::get_objects (PhyCollisionStateManager)
#include "../IVP/ivp_physics/ivp_core.h"
#include "../IVP/ivp_physics/ivp_real_object.h"
#include "../IVP/ivp_physics/ivp_environment.h"
#include "../IVP/ivp_physics/ivp_controller.h"
#include "../IVP/ivp_physics/ivp_material.h"
#include "../IVP/ivp_physics/ivp_listener.h"
#include "../IVP/ivp_physics/ivp_mindist_settings.h"
#include "../IVP/ivp_physics/ivp_mindist.h" // IVP_Synapse_Real (PhySys_CalcClosestPointOnSynapse)
#include "common_csys.h"  // Acquire3DMathEngine
// PhySys::CalcClosestPointOnSynapse (the routine, near GetClosestPhantomIntruderPos
// below) walks baseline's compact half-edge mesh directly -- IVP_Compact_Edge /
// IVP_Compact_Triangle / IVP_Compact_Ledge and IVP_U_Float_Point.
#include "../IVP/ivp_compact_builder/ivp_compact_ledge.h"
#include "../IVP/ivp_compact_builder/ivp_surface_builder_pointsoup.h" // Cleanup, the box phantom's hull
#include "../IVP/ivp_compact_builder/ivp_surface_builder_ledge_soup.h" // the box phantom's surface
#include "../IVP/ivp_physics/ivp_surman_polygon.h" // PhySys::PhySurfaceManager's base
#include "../IVP/ivp_collision/ivp_ray_solver.h"   // PhySys::RaySolver_MaskedSurface's base
#include "../IVP/ivp_collision/ivp_sphere_query.h" // PhysicalSystem::FindSphereCollisions (06350740)

#include <assert.h>
#include <float.h>   // FLT_MAX (PhySys::GetClosestPhantomIntruderPos, the field)
#include <math.h>
#include <new>
#include <stdarg.h>  // va_list -- CRemotePhysicsSimulation::tracef is __cdecl varargs
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>

#include <mmsystem.h>  // timeGetTime (PhysicalSystem::Update, module storage); winmm is linked by common_playeraccount.cpp


// Win32 macro conflict: PhySys::GetCurrentTime vs windows.h GetCurrentTime()
#ifdef GetCurrentTime
#undef GetCurrentTime
#endif

#include "common_physys_internal.h"

using namespace CmnPhySys;

// ============================================================================
// PhySys::Controller
// ============================================================================

// memsize = n_elems = 0, elems = 0 (the vector's own constructor),
// then the vtable. +0x0c is not written; the port zeroes it.
PhySys::Controller::Controller()
    : PadC(0)
{
}

// the dwords +0x04 / +0x08 / +0x0c copied as they are (the core
// array is shared, not duplicated).
PhySys::Controller::Controller(Controller const& Other)
    : PadC(Other.PadC)
{
    Cores.memsize = Other.Cores.memsize;
    Cores.n_elems = Other.Cores.n_elems;
    Cores.elems = Other.Cores.elems;
}

// with cores registered, the routine(this, 1) takes the controller
// off every core and the vector is emptied; the vector member's own destructor (the same
// Clear, 06285bf1..06285c17) then runs again.
PhySys::Controller::~Controller()
{
    if (Cores.n_elems != 0)
    {
        IVP_Controller_Detach_From_All_Cores(this, 1);
        Cores.Clear();
    }
}

// the same three dwords, no self test.
PhySys::Controller& PhySys::Controller::operator=(Controller const& Other)
{
    Cores.memsize = Other.Cores.memsize;
    Cores.n_elems = Other.Cores.n_elems;
    Cores.elems = Other.Cores.elems;
    PadC = Other.PadC;
    return *this;
}

// the destructor's first block.
void PhySys::Controller::core_is_going_to_be_deleted_event(IVP_Core* Core)
{
    (void)Core;
    if (Cores.n_elems != 0)
    {
        IVP_Controller_Detach_From_All_Cores(this, 1);   // (this, 1)
        Cores.Clear();
    }
}

IVP_CONTROLLER_PRIORITY PhySys::Controller::get_controller_priority()
{
    // MOV EAX,0x5dc / RET, nothing else.
    return IVP_CP_MOTION;
}

IVP_U_Vector<IVP_Core>* PhySys::Controller::get_associated_controlled_cores()
{
    // (ICF): LEA EAX,[this+4].
    return &Cores;
}

// . ECX for the callee is elems[0]->Environment->+0x90, which
// does not read.
void PhySys::Controller::wakeup()
{
    if (Cores.n_elems != 0)
        IVP_Controller_Wake_Or_Refresh_Associated_Sim_Unit(this);
}

// . No null tests.
void __cdecl PhySys::Controller::Register(Controller* Ctrl, CObject* Object)
{
    // the object's core (+0x54 -> +0x98) joins the vector
    // (the routine grows it when n_elems >= memsize).
    IVP_Core* const Core = Object->BodyPtr->core;
    Ctrl->Cores.Add(Core);

    // the routine with ECX = Core->Environment->+0x90 -- the
    // environment's one-pointer box that holds the environment itself -- and Ctrl.
    IVP_Environment* const* const Holder = &Core->Environment->controller_manager->environment;
    IVP_Controller_Reconcile_Associated_Cores(Holder, Ctrl);

    // the routine(Ctrl), ECX the same box (not read).
    IVP_Controller_Wake_Or_Refresh_Associated_Sim_Unit(Ctrl);
}

// core_is_going_to_be_deleted_event's body on Ctrl. No null test.
void __cdecl PhySys::Controller::UnRegister(Controller* Ctrl)
{
    if (Ctrl->Cores.n_elems != 0)
    {
        IVP_Controller_Detach_From_All_Cores(Ctrl, 1);   // (Ctrl, 1)
        Ctrl->Cores.Clear();
    }
}

// (thiscall RET 8): ECX = Cores.elems[0] (+0x08 -> [0]) -- only the
// first registered core, no loop and no test -- and the routine (IVP_Core::push_core_linear)
// with a float copy of Force and Dt.
void PhySys::Controller::push(Vector const& Force, float Dt)
{
    IVP_Core* const Core = Cores.elems[0];
    Vector const Copy = Force;
    Core->push_core_linear(&Copy, Dt);
}

// (thiscall RET 8): the same on elems[0] through the routine
// (IVP_Core::push_core_angular_ws, which rotates by MWorldFCore.vimult first).
void PhySys::Controller::rotate(Vector const& Torque, float Dt)
{
    IVP_Core* const Core = Cores.elems[0];
    Vector const Copy = Torque;
    Core->push_core_angular_ws(&Copy, Dt);
}

