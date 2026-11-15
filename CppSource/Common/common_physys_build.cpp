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
// PhySys::PhySurfaceManager / PhySys::RaySolver_MaskedSurface
// ============================================================================

// PhySurfaceManager (vtable module storage, RTTI COL 063a715c, operator new(0xc)) is the
// surface manager of every polygon body PhySys builds: IVP_SurfaceManager_Polygon
// (0639c2dc) over the part's compact surface plus the owning CObject at +0x08. Its
// constructor is inlined at both allocation sites, BuildPartIVP_Real (module storage..
// ) and the DetachInstance builder the routine (module storage..0628a165):
// `[+4] = Part->Surface; [+8] = Object; [+0] = 0639c258`. It overrides slots 4, 5, 6 and
// the destructor (slot 9, 06289340: the base destructor 0634ad60, then free on flag 1);
// every other slot is the base's.

// The three overrides hide the ledges of parts that are no longer attached: a ledge's
// client data (IVP_Compact_Ledge +0x04) is its part id, and a ledge survives only while
// Owner->part_to_inst(part id) != -1 -- the test baseline inlines at 06285ec3..06285efd,
// and 06285fbe..06285fea (CObject::part_to_inst, module storage).
namespace PhySys
{
    class PhySurfaceManager : public IVP_SurfaceManager_Polygon
    {
    public:
        PhySurfaceManager(const IVP_Compact_Surface* Surface, CObject* InOwner)
            : IVP_SurfaceManager_Polygon(Surface)
            , Owner(InOwner)
        {
        }

        // thunk_FUN_0634ad60 (the base destructor), then free on flag 1.
        ~PhySurfaceManager() override {}

        // Slot 4, 06285f10 (RET 0x18).
        void get_all_ledges_within_radius(const float* Center3, float Radius, void* Param3,
                                          void* Param4, void* Param5,
                                          IVP_SurfaceManager_OutList* OutList) override;
        // Slot 5, 06285fa0 (RET 4).
        void get_all_terminal_ledges(IVP_SurfaceManager_OutList* OutList) override;
        // Slot 6, 06285e80 (RET 8): Param1 = IVP_Ray_Solver*, Param2 = IVP_Real_Object*.
        void insert_all_ledges_hitting_ray(void* Param1, void* Param2) override;

        CObject* Owner; // +0x08
    };
    static_assert(sizeof(PhySurfaceManager) == 0x0c, "PhySys::PhySurfaceManager is operator new(0xc)");

    // vtable module storage (RTTI COL 063a6bf4), one slot (06285ec0). Built on the stack by
    // PhySurfaceManager::insert_all_ledges_hitting_ray: the IVP_Ray_Solver_Os constructor
    // (the routine), then +0x50 = the surface manager, then the vtable.
    class RaySolver_MaskedSurface : public IVP_Ray_Solver_Os
    {
    public:
        RaySolver_MaskedSurface(IVP_Ray_Solver* RaySolver, IVP_Real_Object* Object, PhySurfaceManager* InManager)
            : IVP_Ray_Solver_Os(RaySolver, Object)
            , Manager(InManager)
        {
        }

        // Slot 0, 06285ec0 (RET 4).
        int check_ray_against_compact_ledge_os(const IVP_Compact_Ledge* Ledge) override;

        PhySurfaceManager* Manager; // +0x50
    };
    static_assert(sizeof(RaySolver_MaskedSurface) == 0x54, "PhySys::RaySolver_MaskedSurface size");
}

namespace
{
    // The inlined part test: Owner->part_to_inst(Ledge's part id) != -1.
    bool PhySys_LedgeIsAttached(CObject const* Owner, IVP_Compact_Ledge const* Ledge)
    {
        return Owner->part_to_inst((unsigned)Ledge->ClientData) != -1;
    }

    // The removal loops of slots 4 and 5 (06285f3a..06285f98 / 06285faf..0628600d): walk the
    // list from the back and close up every detached ledge in place (`elems[j] = elems[j+1]`
    // up to count - 1, then count -= 1).
    void PhySys_DropDetachedLedges(CObject const* Owner, IVP_SurfaceManager_OutList* OutList)
    {
        for (int32_t Index = OutList->Count - 1; Index >= 0; --Index)
        {
            IVP_Compact_Ledge const* const Ledge = (IVP_Compact_Ledge const*)OutList->Elems[Index];
            if (PhySys_LedgeIsAttached(Owner, Ledge))
                continue;
            for (int32_t Move = Index; Move < OutList->Count - 1; ++Move)
                OutList->Elems[Move] = OutList->Elems[Move + 1];
            --OutList->Count;
        }
    }
}

// the base query (0634ab90) with the same six arguments, then the filter.
void PhySys::PhySurfaceManager::get_all_ledges_within_radius(const float* Center3, float Radius, void* Param3,
                                                             void* Param4, void* Param5,
                                                             IVP_SurfaceManager_OutList* OutList)
{
    IVP_SurfaceManager_Polygon::get_all_ledges_within_radius(Center3, Radius, Param3, Param4, Param5, OutList);
    PhySys_DropDetachedLedges(Owner, OutList);
}

// the base query (0634ac50), then the filter.
void PhySys::PhySurfaceManager::get_all_terminal_ledges(IVP_SurfaceManager_OutList* OutList)
{
    IVP_SurfaceManager_Polygon::get_all_terminal_ledges(OutList);
    PhySys_DropDetachedLedges(Owner, OutList);
}

// instead of the base's plain IVP_Ray_Solver_Os (0634ab60), a masked one
// over this manager's compact surface (the routine on +0x04).
void PhySys::PhySurfaceManager::insert_all_ledges_hitting_ray(void* Param1, void* Param2)
{
    // The IVP base declares the slot with void* parameters (ivp_surman_polygon.h).
    RaySolver_MaskedSurface Solver((IVP_Ray_Solver*)Param1, (IVP_Real_Object*)Param2, this);
    Solver.check_ray_against_compact_surface(compact_surface);
}

// a ledge of a detached part reports no hit (0); any other goes to
// IVP_Ray_Solver_Os's own test (06349fd0).
int PhySys::RaySolver_MaskedSurface::check_ray_against_compact_ledge_os(const IVP_Compact_Ledge* Ledge)
{
    if (!PhySys_LedgeIsAttached(Manager->Owner, Ledge))
        return 0;
    return IVP_Ray_Solver_Os::check_ray_against_compact_ledge_os(Ledge);
}

// ?GetRoot@@YAJJ@Z (module storage), defined in common_hierarchy.cpp next to the CObject*
// overload; no header declares it.
COMMON_API long __cdecl GetRoot(long Inst);

namespace
{
    // The bHasInertia block of the template builders (06289237..06289279, 0628a57c..0628a5de,
    // ): an explicit inertia box replaces the automatic one. +0x30 receives
    // the never-written fourth float of the staging IVP_U_Float_Point (UB); the port keeps the
    // constructor's value there.
    void PhySys_FillTemplateInertia(IVP_RealObjectConstructionTemplate& Template, Vector const& Inertia)
    {
        Template.AutoCheckRotInertia = 0;          // +0x20
        Template.InertiaBoxVector.x = Inertia.x;   // +0x24
        Template.InertiaBoxVector.y = Inertia.y;   // +0x28
        Template.InertiaBoxVector.z = Inertia.z;   // +0x2c
        Template.RotInertiaClampFactor = 0.0f;     // +0x34
    }

    // The CreateParms inputs BuildPartIVP_Real (062891bf..06289203) and the routine
    // (0628a553..0628a59d) copy. Both also store into +0x48 the never-written fourth float of
    // the IVP_U_Float_Point the damping triple is staged in (UB); the port keeps the
    // constructor's 0.
    void PhySys_FillTemplateDamping(IVP_RealObjectConstructionTemplate& Template, PhySys::CreateParms const& Parms)
    {
        Template.MassInput = Parms.Mass;                        // +0x1c <- +0x00
        Template.SpeedDampInput = Parms.LinearDamping;          // +0x38 <- +0x04
        Template.RotSpeedDampInputX = Parms.AngularDamping[0];  // +0x3c <- +0x08
        Template.RotSpeedDampInputY = Parms.AngularDamping[1];  // +0x40 <- +0x0c
        Template.RotSpeedDampInputZ = Parms.AngularDamping[2];  // +0x44 <- +0x10
    }

    // , baseline's BuildPartIVP_Real (module storage..0628933a, cdecl).
    void PhySys_BuildPartIVP_Real(CObject* Object, PhyArch::Part const* Part, PhySys::CreateParms const& Parms)
    {
        // / 06289300..0628933a
        if (Part->Surface == nullptr)
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kPhySysSource, 0x215,
                  "BuildPartIVP_Real(): No surface input for part_arch");
            return;
        }

        // the part (CObject+0x58).
        Object->PhyPart = Part;
        // the surface manager (inlined constructor).
        Object->SurfaceManager = new PhySys::PhySurfaceManager(Part->Surface, Object);
        // the collision state manager, unconditionally.
        Object->CollisionStateMgr = new PhySys::PhyCollisionStateManager(Object);


        IVP_RealObjectConstructionTemplate Template;
        Template.set_short_name(Parms.Name);

        PhySys_FillTemplateDamping(Template, Parms);
        Template.FlagsHi = Parms.bFlag1c;                       // +0x0c <- MOVZX +0x1c

        // outside single player the mass centre is pinned to the
        // object origin -- an identity IVP_U_Matrix (eh-vector constructor the routine over
        // the three 0x10 rows, then the routine: all 0x40 bytes zero, 1.0f on the diagonal)
        // becomes the template's override (+0x50).
        IVP_U_Matrix ComOverride;
        if (!SinglePlayer())
        {
            memset(&ComOverride, 0, sizeof(ComOverride));
            ComOverride.rows[0].k[0] = 1.0f;
            ComOverride.rows[1].k[1] = 1.0f;
            ComOverride.rows[2].k[2] = 1.0f;
            Template.OptionalComOverride = &ComOverride;
        }


        if (Parms.bHasInertia != 0)
            PhySys_FillTemplateInertia(Template, Parms.Inertia);

        // the routine find; a miss only reports.
        std::map<unsigned, PhysicalSystem>::iterator SystemIt = G_PhysicalSystems.find(Parms.CollisionGroupId);
        if (SystemIt == G_PhysicalSystems.end())
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kPhySysSource, 0x211,
                  "Tried to create physics before Ipion was initialized.");
            return;
        }
        SystemIt->second.CreatePolygonBody(Object, &Template);
    }

    // The part-map entry {part id, instance, not a shield part} BuildIVP_Real and its walk
    // append (the routine / the inlined insert at 06288c0b..06288dff).
    void PhySys_AppendPart(CObject* Object, unsigned PartId, long Instance)
    {
        FPartMapEntry Entry;
        Entry.PartId = PartId;
        Entry.EngineInstanceId = Instance;
        Entry.bShieldPart = 0;
        Object->PartMap.push_back(Entry);
    }

    // (module storage..06288e53, cdecl): one engine instance of BuildIVP_Real's
    // walk. A free ('!fxd') part becomes an object of its own; any other instance stays with
    // Owner. Then the instance's children, owned by whichever of the two it went to.
    void PhySys_BuildInstanceIVP_Real(CObject* Owner, long Instance, PhySys::CreateParms const& Parms)
    {
        IEngine* const Engine = NeuronRuntime::Engine;

        // the part id (the CRC half of IEngine +0x68).
        FEngineName Name;
        Engine->GetInstanceName(&Name, PhySys_InstanceHandle(Instance));
        unsigned const PartId = Name.Crc;


        PhyArch::Part const* const Part = Owner->Archetype->get_phy_part(PartId);
        CObject* RecursionOwner = Owner;
        if (Part != nullptr && Part->bNotFixed == 0)
        {

            CObject* const New = CObject::Alloc(CObject::ClassObject);   // Alloc(1)
            New->open(Owner->Archetype);                                 // vtbl +0x4c
            Engine->SetInstanceTransform(PhySys_InstanceHandle(Instance), New); // IEngine +0x70
            PhySys_SetInstanceObject(Instance, New);                     // IEngine +0x7c
            PhySys_AppendPart(New, PartId, Instance);
            PhySys_BuildPartIVP_Real(New, Part, Parms);
            PhySys::AttachGolemController(New, Parms);
            RecursionOwner = New;
        }
        else
        {

            PhySys_SetInstanceObject(Instance, Owner);
            PhySys_AppendPart(Owner, PartId, Instance);
        }


        for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
             Child = PhySys_RelatedInstance(Instance, 1, Child))
        {
            PhySys_BuildInstanceIVP_Real(RecursionOwner, Child, Parms);
        }
    }
}

// ============================================================================
// PhySys::BuildIVP_Real
// ============================================================================

// .
void __cdecl PhySys::BuildIVP_Real(CObject* Object, CreateParms const& Parms)
{
    long const Instance = Object->EngineInstanceId;


    PhySys_SetInstanceObject(Instance, Object);

    // the root part id, registered in the object's part map.
    FEngineName Name;
    NeuronRuntime::Engine->GetInstanceName(&Name, PhySys_InstanceHandle(Instance));
    PhySys_AppendPart(Object, Name.Crc, Instance);


    PhyArch::Part const* Part = Object->Archetype->get_phy_part(Name.Crc);
    if (Part == nullptr)
        Part = PhyArch::GetDefaultPart();
    PhySys_BuildPartIVP_Real(Object, Part, Parms);

    // / 06288ae0..06288b18: no body, no children.
    if (Object->BodyPtr == nullptr)
    {
        char const* const PartName = (Name.Text != nullptr) ? Name.Text : "";
        FDUMP(0x100001, "%s(%d) : *** ERROR: BuildIVP_Real(): failed to create ivp_real for part (%s) in file (%s).",
              kPhySysSource, 0x27a, PartName, Object->Archetype->DaArchetype.Value);
        return;
    }

    // the parms the child parts are built with -- a copy without
    // +0x1c, with the golem-child (type 1) or default (type 2) mass and damping.
    CreateParms ChildParms = Parms;
    ChildParms.bFlag1c = 0;
    if (Parms.Type == 1)
    {
        ChildParms.Mass = GOLEM_CHILD_MASS;
        ChildParms.LinearDamping = GOLEM_CHILD_LINEAR_DAMP;
        ChildParms.AngularDamping[0] = GOLEM_CHILD_ANGULAR_DAMP.x;
        ChildParms.AngularDamping[1] = GOLEM_CHILD_ANGULAR_DAMP.y;
        ChildParms.AngularDamping[2] = GOLEM_CHILD_ANGULAR_DAMP.z;
    }
    else if (Parms.Type == 2)
    {
        ChildParms.Mass = 1.0f;
        ChildParms.LinearDamping = DEFAULT_LINEAR_DAMPING;
        ChildParms.AngularDamping[0] = DEFAULT_ANGULAR_DAMPING.x;
        ChildParms.AngularDamping[1] = DEFAULT_ANGULAR_DAMPING.y;
        ChildParms.AngularDamping[2] = DEFAULT_ANGULAR_DAMPING.z;
    }


    for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
         Child = PhySys_RelatedInstance(Instance, 1, Child))
    {
        PhySys_BuildInstanceIVP_Real(Object, Child, ChildParms);
    }


    if (Parms.bEnableCollisions != 0)
        Object->enable_collisions_r(true);
}

// ============================================================================
// PhySys::BuildIVP_Sphere
// ============================================================================

// .
void __cdecl PhySys::BuildIVP_Sphere(CObject* Object, float Radius, CreateParms const& Parms)
{
    // the construction template on the stack (the routine; its
    // destructor thunk_FUN_06348670 runs at module storage and on the unwind path), named
    // after CreateParms+0x1e (the routine).
    IVP_RealObjectConstructionTemplate Template;
    Template.set_short_name(Parms.Name);

    // the template's physical inputs from CreateParms.
    Template.MassInput = Parms.Mass;                        // +0x1c <- +0x00
    Template.FlagsHi = Parms.bFlag1c;                       // +0x0c <- MOVZX +0x1c
    Template.SpeedDampInput = Parms.LinearDamping;          // +0x38 <- +0x04
    Template.RotSpeedDampInputX = Parms.AngularDamping[0];  // +0x3c <- +0x08
    Template.RotSpeedDampInputY = Parms.AngularDamping[1];  // +0x40 <- +0x0c
    Template.RotSpeedDampInputZ = Parms.AngularDamping[2];  // +0x44 <- +0x10
    // UB in baseline: +0x48 receives the never-written fourth float of the stack
    // IVP_U_Float_Point the three damping values were staged in (local_68). The port keeps
    // the constructor's zero.

    // the system named by CreateParms+0x14 (lower_bound + key test).
    // No end() test: a miss builds into the map head node's unconstructed storage
    // (PhySys_FindSystem: very strange, reproduced on purpose).
    PhysicalSystem& System = PhySys_FindSystem(Parms.CollisionGroupId);


    if (Template.Material == nullptr)
        Template.Material = G_PhySysMaterial;

    // the routine(System, &Object->WorldPosition,
    // &Object->Orientation, Radius, &Template); the body, the system id and the back link.
    IVP_Ball* const Body = System.CreateBallBody(Object->WorldPosition, Object->Orientation, Radius, &Template);
    Object->BodyPtr = Body;
    Object->PhysicsSystemId = System.SystemId;
    Body->client_data = Object;

    // with CreateParms+0x1d set and a body, a collision state
    // manager (operator new(0x14)) at Object+0x84, enabled immediately.
    if (Parms.bEnableCollisions != 0 && Object->BodyPtr != nullptr)
    {
        PhyCollisionStateManager* const Manager = new PhyCollisionStateManager(Object);
        Object->CollisionStateMgr = Manager;
        Manager->enable_collisions(true, true);
    }

    // IEngine slot +0x7c SetInstanceUserData(instance, Object).
    PhySys_SetInstanceObject(Object->EngineInstanceId, Object);
}

// ============================================================================
// PhySys::Attach / Detach
// ============================================================================

// . analysis's analysis of this cluster is corrupted (the
// recursive helpers' stack arguments are misread as globals / floats); everything below
// follows the listing. The two IVP calls are
// (ParentBody, ChildBody, -1.0f) -- IVP_Merge_Object_Into_Core (Attach / weld);
// (Body, &Template)              -- IVP_Split_Object_From_Core (the routine).

namespace
{
    // The {object, desired collision state} records Attach / Detach save while they pull a
    // subtree apart (8-byte elements, appended with the routine = insert(end, 1, value)).
    using FCollisionSaveList = std::vector<std::pair<CObject*, bool>>;

    // (module storage..06284adb, cdecl). Also inlined, with the same shape, in
    // (06284b3d..06284b71), the routine (06284c21..06284c55) and PhySys::Attach
    // (06284ce9..06284d28).
    void PhySys_LinkBodies(CObject* Parent, CObject* Child)
    {
        IVP_Real_Object* const ParentBody = Parent->BodyPtr;
        if (ParentBody == nullptr)
            return;
        IVP_Real_Object* const ChildBody = Child->BodyPtr;
        if (ChildBody == nullptr)
            return;


        strncpy(ChildBody->name, ParentBody->name, 8);

        // the routine(ParentBody, ChildBody, -1.0f).
        IVP_Merge_Object_Into_Core(ParentBody, ChildBody, -1.0f);


        Child->PhysicsControlMode = 2;
    }

    // (module storage..0628490e, cdecl): detach Object's body from the core it
    // shares, with DetachParms as the new core's mass / damping / inertia.
    void PhySys_DetachBody(CObject* Object, PhySys::DetachParms const& Parms)
    {
        IVP_Real_Object* const Body = Object->BodyPtr;
        if (Body != nullptr)
        {
            // the template (the routine, destroyed by
            // thunk_FUN_06348670 at 062848f6 and on unwind), named only for a non-empty name.
            IVP_RealObjectConstructionTemplate Template;
            if (Parms.Name[0] != 0)
                Template.set_short_name(Parms.Name);

            // . UB in baseline: +0x30 receives the never-written fourth
            // float of the staging IVP_U_Float_Point (local_68); the port keeps the ctor's value.
            Template.MassInput = Parms.Mass;                            // +0x1c <- +0x18
            if (Parms.bHasInertia != 0)
                PhySys_FillTemplateInertia(Template, Parms.Inertia);    // +0x20..+0x34

            // . +0x48 receives the same never-written float (UB).
            Template.SpeedDampInput = Parms.LinearDamping;              // +0x38 <- +0x00
            Template.RotSpeedDampInputX = Parms.AngularDamping.x;       // +0x3c <- +0x04
            Template.RotSpeedDampInputY = Parms.AngularDamping.y;       // +0x40 <- +0x08
            Template.RotSpeedDampInputZ = Parms.AngularDamping.z;       // +0x44 <- +0x0c
            Template.Material = G_PhySysMaterial;                       // +0x18 <- the field
            Template.FlagsHi = 0;                                       // +0x0c

            // a ball (object_type 3) keeps its radius.
            if (Body->object_type == 3)
                Template.Field_0x4c = Body->field_94;

            // the routine(Body, &Template).
            IVP_Split_Object_From_Core(Body, &Template);
        }


        Object->PhysicsControlMode = 0;
    }

    // (module storage..06284bb2, cdecl): walk ObjId's children; an instance of
    // another object (not null, not PrevRef) is linked onto NewParent and becomes PrevRef for
    // its own subtree, any other instance is descended into with PrevRef unchanged.
    void PhySys_ReparentSubtree(long ObjId, CObject* NewParent, CObject* PrevRef)
    {

        if (ObjId == -1)
            return;


        for (long ChildId = PhySys_RelatedInstance(ObjId, 1, -1); ChildId != -1;
             ChildId = PhySys_RelatedInstance(ObjId, 1, ChildId))
        {
            CObject* const ChildObject = PhySys_InstanceObject(ChildId);
            if (ChildObject != nullptr && ChildObject != PrevRef)
            {
                PhySys_LinkBodies(NewParent, ChildObject);               // inlined 06284b3d..06284b71
                PhySys_ReparentSubtree(ChildId, NewParent, ChildObject);
            }
            else
            {
                PhySys_ReparentSubtree(ChildId, NewParent, PrevRef);
            }
        }
    }

    // (module storage..06284c97, cdecl): the direct children of Container's
    // instance (the instance id is read once), each linked onto NewParent unless it is
    // Container's own (or nobody's), then PhySys_ReparentSubtree below it. Caller:
    // PhySys_SplitInstance (the routine) with NewParent == Container.
    void PhySys_LinkAllDirectChildren(CObject* NewParent, CObject* Container)
    {

        long const ObjId = Container->EngineInstanceId;
        if (ObjId == -1)
            return;


        for (long ChildId = PhySys_RelatedInstance(ObjId, 1, -1); ChildId != -1;
             ChildId = PhySys_RelatedInstance(ObjId, 1, ChildId))
        {
            CObject* const ChildObject = PhySys_InstanceObject(ChildId);
            if (ChildObject != nullptr && ChildObject != Container)
            {
                PhySys_LinkBodies(NewParent, ChildObject);               // inlined 06284c21..06284c55
                PhySys_ReparentSubtree(ChildId, NewParent, ChildObject);
            }
            else
            {
                PhySys_ReparentSubtree(ChildId, NewParent, Container);
            }
        }
    }

    // (module storage..062847d4, cdecl): walk ObjId's children. Every instance
    // of another object (not null, not Ref) has its desired collision state saved, its
    // collisions switched off and its body detached with default parms, and becomes Ref for
    // its own subtree; any other instance is descended into with Ref unchanged.
    void PhySys_DisableCollisionsAndDetachSubtree(long ObjId, CObject* Ref, FCollisionSaveList& SaveList)
    {

        if (ObjId == -1)
            return;


        for (long ChildId = PhySys_RelatedInstance(ObjId, 1, -1); ChildId != -1;
             ChildId = PhySys_RelatedInstance(ObjId, 1, ChildId))
        {
            CObject* const ChildObject = PhySys_InstanceObject(ChildId);
            if (ChildObject != nullptr && ChildObject != Ref)
            {
                // {object, desired state}, false without a body.
                bool const bDesired = (ChildObject->BodyPtr != nullptr)
                    ? ChildObject->CollisionStateMgr->get_desired_collision_state()
                    : false;
                SaveList.push_back(std::make_pair(ChildObject, bDesired));


                if (ChildObject->BodyPtr != nullptr)
                    ChildObject->CollisionStateMgr->enable_collisions(false, true);

                // the default parms on the stack. Name[1..7] and the
                // bytes +0x14 / +0x1d..+0x1f are not written by baseline and not read by
                // ; the port zeroes them.
                PhySys::DetachParms Defaults = {};
                Defaults.LinearDamping = PhySys::DEFAULT_LINEAR_DAMPING;
                Defaults.AngularDamping = PhySys::DEFAULT_ANGULAR_DAMPING;
                Defaults.Name[0] = 0;
                Defaults.Mass = 1.0f;
                Defaults.bHasInertia = 0;
                Defaults.Inertia.x = 0.0f;
                Defaults.Inertia.y = 0.0f;
                Defaults.Inertia.z = 0.0f;
                PhySys_DetachBody(ChildObject, Defaults);


                PhySys_DisableCollisionsAndDetachSubtree(ChildId, ChildObject, SaveList);
            }
            else
            {

                PhySys_DisableCollisionsAndDetachSubtree(ChildId, Ref, SaveList);
            }
        }
    }

    // The restore loop Attach (06284dbd..06284dea) and Detach (062849f0..06284a23) inline:
    // every saved object that has a body gets its saved state back, immediately.
    void PhySys_RestoreCollisionStates(FCollisionSaveList const& SaveList)
    {
        for (size_t Index = 0; Index < SaveList.size(); ++Index)
        {
            CObject* const Saved = SaveList[Index].first;
            if (Saved->BodyPtr != nullptr)
                Saved->CollisionStateMgr->enable_collisions(SaveList[Index].second, true);
        }
    }
}

// . Child's subtree is pulled apart (collision states saved), Child is
// linked onto Parent, then every other object hanging off Child's instance, and the saved
// collision states come back.
void __cdecl PhySys::Attach(CObject* Parent, CObject* Child)
{
    // the save list (an EH-guarded local, freed at 06284df1).
    FCollisionSaveList SaveList;
    PhySys_DisableCollisionsAndDetachSubtree(Child->EngineInstanceId, Child, SaveList);

    // the routine's body, inlined.
    PhySys_LinkBodies(Parent, Child);


    long const ChildId = Child->EngineInstanceId;
    if (ChildId != -1)
    {
        for (long BelowId = PhySys_RelatedInstance(ChildId, 1, -1); BelowId != -1;
             BelowId = PhySys_RelatedInstance(ChildId, 1, BelowId))
        {
            CObject* const BelowObject = PhySys_InstanceObject(BelowId);
            if (BelowObject != nullptr && BelowObject != Child)
            {
                PhySys_LinkBodies(Parent, BelowObject);
                PhySys_ReparentSubtree(BelowId, Parent, BelowObject);
            }
            else
            {
                PhySys_ReparentSubtree(BelowId, Parent, Child);
            }
        }
    }


    PhySys_RestoreCollisionStates(SaveList);
}

// . Mode 2 (welded): pull the subtree apart, detach the body with
// Parms, re-link everything below Object onto Object, restore the collision states. Mode 1
// (golem): delete the controller. Every mode ends at 0.
void __cdecl PhySys::Detach(CObject* Object, DetachParms const& Parms)
{

    if (Object->PhysicsControlMode == 2)
    {

        FCollisionSaveList SaveList;
        PhySys_DisableCollisionsAndDetachSubtree(Object->EngineInstanceId, Object, SaveList);


        PhySys_DetachBody(Object, Parms);


        long const ObjId = Object->EngineInstanceId;
        if (ObjId != -1)
        {
            for (long BelowId = PhySys_RelatedInstance(ObjId, 1, -1); BelowId != -1;
                 BelowId = PhySys_RelatedInstance(ObjId, 1, BelowId))
            {
                CObject* const BelowObject = PhySys_InstanceObject(BelowId);
                if (BelowObject != nullptr && BelowObject != Object)
                {
                    PhySys_LinkBodies(Object, BelowObject);
                    PhySys_ReparentSubtree(BelowId, Object, BelowObject);
                }
                else
                {
                    PhySys_ReparentSubtree(BelowId, Object, Object);
                }
            }
        }


        PhySys_RestoreCollisionStates(SaveList);
        Object->PhysicsControlMode = 0;
        return;
    }

    // the controller's virtual deleting destructor (slot +0x18,
    // flag 1), then +0x60 = 0.
    if (Object->PhysicsControlMode == 1)
    {
        IVP_Controller* const Controller = Object->GolemController;
        if (Controller != nullptr)
            delete Controller;
        Object->GolemController = nullptr;
    }

    // / 06284a78
    Object->PhysicsControlMode = 0;
}

// (BodyPtr): thiscall helper baseline calls with ECX = an object's *old* +0x54
// body pointer (read before the field is zeroed) — `if (BodyPtr) BodyPtr->vtbl[0](1)`, i.e.
// a virtual scalar-deleting-destructor call that frees the body. BuildIVP_Real/Sphere allocate
// bodies and cores with `new IVP_Real_Object()` / `new IVP_Core()`, so "releasing" one means
// `delete`-ing it and its owned sub-structures — the same steps DeactivatePhysics used to
// inline for its own +0x54 slot. Shared here so PhySys::UnMakePhysicalR (which, per raw
// analysis of 0x2a7d0, calls exactly this and nothing else for its own object — no RC
// controller / PhyCollisionStateManager / anim controller release) can reuse it without
// duplicating baseline's DeactivatePhysics (0x25350), a distinct function that does the fuller
// cleanup.
// UnMakePhysicalR's head (module storage..0628a7e7): both fields are cleared first, then the
// old body goes through the routine.
void CmnPhySys::PhySys_DeleteBody(IVP_Real_Object* Body)
{
    // `if (Body) Body->vtbl[0](1)`, the scalar deleting destructor. ~IVP_Object is
    // virtual (ivp_real_object.h), so this delete dispatches to the body's own destructor
    // (IVP_Polygon / IVP_Ball) exactly as the vtable call does.
    if (Body != nullptr)
    {
        delete Body;
    }
}

static void PhySys_ReleaseBody(CObject* Object)
{
    IVP_Real_Object* const Body = Object->BodyPtr;
    Object->BodyPtr = nullptr;
    Object->PhysicsSystemId = 0;
    PhySys_DeleteBody(Body);
}

void __cdecl PhySys::DeactivatePhysics(CObject* Object)
{
    // . No null test on Object.

    // the golem / remote controller through its virtual deleting
    // destructor (IVP_Controller slot +0x18, flag 1); +0x60 is cleared either way.
    IVP_Controller* const Controller = Object->GolemController;
    if (Controller != nullptr)
        delete Controller;
    Object->GolemController = nullptr;

    // the collision state manager (destructor, then operator delete).
    PhyCollisionStateManager* const Manager = Object->CollisionStateMgr;
    if (Manager != nullptr)
        delete Manager;
    Object->CollisionStateMgr = nullptr;

    // only with a body are +0x54 / +0x50 cleared before
    // releases it.
    IVP_Real_Object* const Body = Object->BodyPtr;
    if (Body != nullptr)
    {
        Object->BodyPtr = nullptr;
        Object->PhysicsSystemId = 0;
        PhySys_DeleteBody(Body);
    }

    // the surface manager through its virtual deleting destructor
    // (slot +0x24, flag 1); +0x5c is cleared either way.
    IVP_SurfaceManager* const SurfaceManager = Object->SurfaceManager;
    if (SurfaceManager != nullptr)
        delete SurfaceManager;
    Object->SurfaceManager = nullptr;
}

// (Object, ParentId): the recursive "self-match" companion PhySys::DeactivatePhysicsR
// calls instead of recursing into itself whenever a traversal step's Child equals Object (raw
// analysis 72116-72125: `CMP EAX,EDI` / `JZ` to the the routine call instead of the plain
// DeactivatePhysicsR(Child) call). analysis's analysis of this function (Common)
// is corrupted -- it misreads the always-zero EBP register as a phantom global "piVar4" and
// gets the self-compare operand wrong (analysis shows `piVar2 != piVar4`; raw analysis at listing
// 72182 actually compares the child against [esp+0x14], which resolves to this function's own
// param_1/Object, not the Engine pointer). Reconstructed from raw analysis (listing 72150-72243).

// Semantics: walks Engine's children under ParentId (passed in explicitly, unlike
// DeactivatePhysicsR which always recomputes Object+4) via the identical GetNextChild/GetObj
// pattern. For each child distinct from Object: cleans it up exactly like DeactivatePhysics
// (verified field-for-field identical -- RC controller/PhyCollisionStateManager/body/anim
// controller at +0x60/+0x84/+0x54,+0x50/+0x5c, same order, same vtable slots) and recurses
// using the child's OWN engine id (Child+4) as the next ParentId. For a child equal to Object
// (a nested self-loop), skips the cleanup and recurses using the raw ChildIdx as ParentId
// instead -- matching baseline's the branch/the branch exactly.
// (cdecl Object, Instance). Every child instance of Instance (IEngine
// +0xd8, flags 1, -1 ends): a child object other than Object gets the inlined
// DeactivatePhysics body (module storage..0628552d, the same instructions as 06285350) and is
// recursed into with its own instance (+0x04); a child that is Object itself is recursed into
// with the raw child instance. Neither the engine nor the child object is tested.
static void PhySys_DeactivatePhysicsSelfLoop(CObject* Object, long Instance)
{
    for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
         Child = PhySys_RelatedInstance(Instance, 1, Child))
    {
        CObject* const ChildObject = PhySys_InstanceObject(Child);
        if (ChildObject != Object)
        {
            PhySys::DeactivatePhysics(ChildObject);
            PhySys_DeactivatePhysicsSelfLoop(ChildObject, ChildObject->EngineInstanceId);
        }
        else
        {
            PhySys_DeactivatePhysicsSelfLoop(ChildObject, Child);
        }
    }
}

// . The inlined DeactivatePhysics body on Object (module storage..
// ), then every child instance of Object+0x04: a child object other than Object goes
// through DeactivatePhysicsR, Object itself through the routine with the child instance.
// Nothing is tested.
void __cdecl PhySys::DeactivatePhysicsR(CObject* Object)
{
    DeactivatePhysics(Object);

    long const Instance = Object->EngineInstanceId;
    for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
         Child = PhySys_RelatedInstance(Instance, 1, Child))
    {
        CObject* const ChildObject = PhySys_InstanceObject(Child);
        if (ChildObject == Object)
            PhySys_DeactivatePhysicsSelfLoop(ChildObject, Child);
        else
            DeactivatePhysicsR(ChildObject);
    }
}

// ============================================================================
// ReMakePhysicalR / UnMakePhysicalR
// ============================================================================

namespace
{
    void PhySys_ReMakeInstance(CObject* Owner, long Instance, PhySys::CreateParms const& Parms);

    // (module storage..0628a6fe, cdecl): a new body for an object that keeps its
    // surface manager (or a ball of Radius without one), in Parms' system, then the same for
    // every other object hanging off its instance tree.
    void PhySys_ReMakeBody(CObject* Object, PhySys::CreateParms const& Parms, float Radius)
    {

        IVP_RealObjectConstructionTemplate Template;
        Template.set_short_name(Parms.Name);
        // FlagsHi is 0 here (BuildIVP_Sphere / BuildPartIVP_Real
        // copy CreateParms+0x1c instead).
        PhySys_FillTemplateDamping(Template, Parms);
        Template.FlagsHi = 0;

        if (Parms.bHasInertia != 0)
            PhySys_FillTemplateInertia(Template, Parms.Inertia);

        // lower_bound the routine plus the key test, no end() test:
        // a miss uses the map head node's unconstructed storage as the system
        // (PhySys_FindSystem: very strange, reproduced on purpose).
        PhysicalSystem& System = PhySys_FindSystem(Parms.CollisionGroupId);

        if (Object->SurfaceManager != nullptr)
        {
            // the routine stores the body, the system id and the
            // back link itself.
            System.CreatePolygonBody(Object, &Template);
        }
        else
        {

            if (Template.Material == nullptr)
                Template.Material = G_PhySysMaterial;
            IVP_Ball* const Body = System.CreateBallBody(Object->WorldPosition, Object->Orientation, Radius, &Template);
            Object->BodyPtr = Body;
            Object->PhysicsSystemId = System.SystemId;
            Body->client_data = Object;
        }


        long const Instance = Object->EngineInstanceId;
        for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
             Child = PhySys_RelatedInstance(Instance, 1, Child))
        {
            PhySys_ReMakeInstance(Object, Child, Parms);
        }
        // the template's destructor.
    }

    // (module storage..0628a7cb, cdecl): an instance that belongs to another
    // object rebuilds that object (as a ball of radius 0 if it has no surface manager) and
    // re-attaches it to Owner's root; an instance of Owner (or of nobody) is descended into.
    void PhySys_ReMakeInstance(CObject* Owner, long Instance, PhySys::CreateParms const& Parms)
    {

        CObject* const Child = PhySys_InstanceObject(Instance);
        if (Child != nullptr && Child != Owner)
        {

            PhySys_ReMakeBody(Child, Parms, 0.0f);
            CObject* const Root = PhySys_InstanceObject(GetRoot(Owner->EngineInstanceId));
            PhySys::Attach(Root, Child);
            // no test on the manager.
            if (Parms.bEnableCollisions != 0)
                Child->CollisionStateMgr->enable_collisions(true, true);
            return;
        }


        for (long Below = PhySys_RelatedInstance(Instance, 1, -1); Below != -1;
             Below = PhySys_RelatedInstance(Instance, 1, Below))
        {
            PhySys_ReMakeInstance(Owner, Below, Parms);
        }
    }
}

// .
void __cdecl PhySys::ReMakePhysicalR(CObject* Object, CreateParms const& Parms, float Radius)
{
    PhySys_ReMakeBody(Object, Parms, Radius);
    if (Parms.bEnableCollisions != 0)                        // +0x1d
        Object->enable_collisions_r(true);
}

// (Owner, InstanceId): baseline walks descendant engine instances of InstanceId
// that still resolve to Owner (or to no CObject at all — attachments with no separate
// CObject), recursing deeper through them unchanged. The moment an instance resolves to a
// genuinely different, non-null CObject, that object's own body is released in place and the
// walk continues rooted at the newly found object instead. Mirrors PhySys::UnMakePhysicalR's
// own child loop below one level down; kept separate because baseline keeps it as its own
// function (mutually structured with UnMakePhysicalR, not a plain sub-loop).
// (cdecl Owner, Instance). Nothing is tested but the user data.
static void PhySys_UnMakePhysicalWalk(CObject* Owner, long Instance)
{

    CObject* const Owning = PhySys_InstanceObject(Instance);
    if (Owning == nullptr || Owning == Owner)
    {
        // children of Instance, still under Owner.
        for (long Child = PhySys_RelatedInstance(Instance, 1, -1); Child != -1;
             Child = PhySys_RelatedInstance(Instance, 1, Child))
        {
            PhySys_UnMakePhysicalWalk(Owner, Child);
        }
        return;
    }

    // a different object -- +0x54 / +0x50 cleared, the old body
    // through the routine.
    PhySys_ReleaseBody(Owning);

    // its own instance (+0x04, re-read every step); -1 ends.
    if (Owning->EngineInstanceId == -1)
        return;
    for (long Child = PhySys_RelatedInstance(Owning->EngineInstanceId, 1, -1); Child != -1;
         Child = PhySys_RelatedInstance(Owning->EngineInstanceId, 1, Child))
    {
        PhySys_UnMakePhysicalWalk(Owning, Child);
    }
}

// (the routine is an instruction-identical copy, called for a child
// owned by another object; the port recurses into this function for it). Object is not tested.
void __cdecl PhySys::UnMakePhysicalR(CObject* Object)
{
    // +0x54 / +0x50 cleared, the old body through the routine.
    PhySys_ReleaseBody(Object);

    // / 0628a887..0628a8a4: the children of Object+0x04 (re-read
    // every step); -1 ends.
    if (Object->EngineInstanceId == -1)
        return;
    for (long Child = PhySys_RelatedInstance(Object->EngineInstanceId, 1, -1); Child != -1;
         Child = PhySys_RelatedInstance(Object->EngineInstanceId, 1, Child))
    {
        // a child owned by another object -> the routine on it.
        CObject* const Owning = PhySys_InstanceObject(Child);
        if (Owning != nullptr && Owning != Object)
        {
            PhySys::UnMakePhysicalR(Owning);
            continue;
        }

        // otherwise each of the child's own children through
        // (Object, grandchild).
        for (long Grandchild = PhySys_RelatedInstance(Child, 1, -1); Grandchild != -1;
             Grandchild = PhySys_RelatedInstance(Child, 1, Grandchild))
        {
            PhySys_UnMakePhysicalWalk(Object, Grandchild);
        }
    }
}

// (module storage..0628a407, cdecl): DetachInstance's sub-part branch. Instance
// becomes a new object of Owner's archetype with a polygon body of its own part, takes along
// every part-map entry and instance below it, and inherits Owner's velocity.
static CObject* PhySys_SplitInstance(CObject* Owner, long Instance, PhySys::DetachParms const& Parms)
{
    IEngine* const Engine = NeuronRuntime::Engine;


    CObject* const New = CObject::Alloc(CObject::ClassObject);   // Alloc(1)
    Engine->SetInstanceTransform(PhySys_InstanceHandle(Instance), New); // IEngine +0x70
    New->open(Owner->Archetype);                                  // vtbl +0x4c

    // the part of Instance, or the default part.
    FEngineName Name;
    Engine->GetInstanceName(&Name, PhySys_InstanceHandle(Instance));
    PhyArch::Part const* Part = Owner->Archetype->get_phy_part(Name.Crc);
    if (Part == nullptr)
        Part = PhyArch::GetDefaultPart();

    // part, surface manager (inlined constructor), collision state
    // manager. No surface test here (BuildPartIVP_Real has one).
    New->PhyPart = Part;
    New->SurfaceManager = new PhySys::PhySurfaceManager(Part->Surface, New);
    New->CollisionStateMgr = new PhySys::PhyCollisionStateManager(New);

    // the template from DetachParms. baseline stores into +0x48 (and,
    // with an inertia, into +0x30) a never-written stack float (UB); the port keeps the
    // constructor's values there.
    IVP_RealObjectConstructionTemplate Template;
    Template.FlagsHi = 0;                                          // +0x0c
    Template.MassInput = Parms.Mass;                               // +0x1c <- +0x18
    Template.SpeedDampInput = Parms.LinearDamping;                 // +0x38 <- +0x00
    Template.RotSpeedDampInputX = Parms.AngularDamping.x;          // +0x3c <- +0x04
    Template.RotSpeedDampInputY = Parms.AngularDamping.y;          // +0x40 <- +0x08
    Template.RotSpeedDampInputZ = Parms.AngularDamping.z;          // +0x44 <- +0x0c
    Template.set_short_name(Parms.Name);                           // +0x10
    if (Parms.bHasInertia != 0)
        PhySys_FillTemplateInertia(Template, Parms.Inertia);       // +0x20..+0x34 <- +0x20..+0x28

    // Owner's system (lower_bound the routine plus the key test, no
    // end() test: a miss builds into the map head node's unconstructed storage --
    // PhySys_FindSystem, very strange, reproduced on purpose).
    PhySys_FindSystem(Owner->PhysicsSystemId).CreatePolygonBody(New, &Template);


    PhySys_SetInstanceObject(Instance, New);

    // every instance below Instance (relation 0) that Owner owned
    // now belongs to New.
    for (long Below = PhySys_RelatedInstance(Instance, 0, -1); Below != -1;
         Below = PhySys_RelatedInstance(Instance, 0, Below))
    {
        if (PhySys_InstanceObject(Below) == Owner)
            PhySys_SetInstanceObject(Below, New);
    }

    // the part-map entries under Instance move to New (appended in
    // order through the routine, erased from Owner by shifting the tail down; a match
    // re-tests the same slot).
    std::vector<FPartMapEntry>::iterator Entry = Owner->PartMap.begin();
    while (Entry != Owner->PartMap.end())
    {
        if (IsDescendant(Instance, Entry->EngineInstanceId))
        {
            New->PartMap.push_back(*Entry);
            Entry = Owner->PartMap.erase(Entry);
        }
        else
        {
            ++Entry;
        }
    }

    // the routine(New, New).
    PhySys_LinkAllDirectChildren(New, New);


    if (PhySys::GetCollisionState(Owner) && New->BodyPtr != nullptr)
        New->CollisionStateMgr->enable_collisions(true, true);


    PhySys::AddToVelocity(New, PhySys::GetVelocity(Owner));
    return New;
}

// ---- Instance detach helpers (CMN-EQUIP lane, claude-code:cmn-equip, 2026-10-08) ----
// and the routine. Callers: CNonPhysAttachment::Disconnect (module storage),
// CPhysAttachment::Disconnect (module storage / 06294067), CArchGroup::Destroy (module storage /
// ). baseline has no null guards in either function.

// Listing 0x0628a410..0x0628a4d3.
void __cdecl DetachInstanceInPlace(long InstanceId)
{
    long const ParentId = PhySys_InstanceParent(InstanceId);     // +0xd4
    NeuronRuntime::Engine->DestroyJoint(PhySys_InstanceHandle(ParentId), PhySys_InstanceHandle(InstanceId)); // +0xc0
    CObject* const Owner = PhySys_InstanceObject(InstanceId);    // +0x80

    if (Owner->EngineInstanceId == InstanceId)
    {
        Owner->Release();   // tail-jmp
        return;
    }

    // InstanceId is one of Owner's sub-parts: erase every PartMap entry whose instance (word 1)
    // descends from it, then destroy the instance (+0x60).
    // Erasing shifts the tail down one slot, so a match re-tests the same position.
    std::vector<FPartMapEntry>::iterator Entry = Owner->PartMap.begin();
    while (Entry != Owner->PartMap.end())
    {
        if (IsDescendant(InstanceId, Entry->EngineInstanceId))
            Entry = Owner->PartMap.erase(Entry);
        else
            ++Entry;
    }
    NeuronRuntime::Engine->DestroyInstance(PhySys_InstanceHandle(InstanceId));
}

// Listing 0x0628a050..0x0628a0b6.
CObject* __cdecl DetachInstance(long InstanceId, PhySys::DetachParms const& Parms)
{
    long const ParentId = PhySys_InstanceParent(InstanceId);     // +0xd4
    NeuronRuntime::Engine->DestroyJoint(PhySys_InstanceHandle(ParentId), PhySys_InstanceHandle(InstanceId)); // +0xc0
    CObject* const Owner = PhySys_InstanceObject(InstanceId);    // +0x80
    if (Owner->EngineInstanceId == InstanceId)
    {
        PhySys::Detach(Owner, Parms);
        return Owner;
    }
    // the routine(Owner, InstanceId, &Parms) -- the sub-part split into a new
    // object.
    return PhySys_SplitInstance(Owner, InstanceId, Parms);
}
