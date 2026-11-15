// Author: Giperion.

#pragma once

// Common-internal declarations shared by the translation units baseline's physys.cpp /
// PhyObj.cpp code was split into (common_physys.cpp, common_physicalsystem.cpp,
// common_controllergolem.cpp, common_physys_build.cpp, common_physys_query.cpp,
// common_phantom.cpp, common_remotephysics.cpp, ...). Not a public header: nothing outside
// Common.dll includes it. Shared helpers live in namespace CmnPhySys; the two types keep the
// global names their RTTI gives them (.?AUPhysicalSystem@@) or that other headers forward-declare
// (FLControllerGolemParms, src/IVP/ivp_physics/ivp_controller.h).

#include "common_physys.h"
#include "common_cobject.h"
#include "x86math.h"  // FQuaternion, FMatrix
#include "NeuronRuntime.h"    // NeuronRuntime::Engine (the instance-id helpers below)
#include "engine/sceneruntime.h"
#include "../IVP/ivp_physics/ivp_core.h"
#include "../IVP/ivp_physics/ivp_real_object.h"
#include "../IVP/ivp_physics/ivp_environment.h"
#include "../IVP/ivp_physics/ivp_controller.h"
#include "../IVP/ivp_physics/ivp_listener.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

class IVP_Material;

// ============================================================================
// PhysicalSystem -- one per star system; the mapped value of the world map the field
// (std::map<unsigned, PhysicalSystem>, key at node+0xc, value at node+0x10).
// RTTI: .?AUPhysicalSystem@@ (COL 063a7344 / 063a73e0 / 063a7330) with bases
// IVP_Listener_Object (+0x00, vtable 0639c3bc), IVP_Listener_Collision (+0x04, vtable
// ) and IVP_Collision_Filter_Coll_Group_Ident (+0x0c, vtable 0639c3d4).
// ============================================================================
struct PhysicalSystem : public IVP_Listener_Object,
                        public IVP_Listener_Collision,
                        public IVP_Collision_Filter_Coll_Group_Ident
{
    // (module storage..0628c2db, thiscall RET 4).
    explicit PhysicalSystem(unsigned InSystemId);
    // (deleting the routine; this-adjusting thunks the branch / the branch;
    // is the same body for the map's pair). Compiler-generated member teardown:
    // the set, the vector, then the bases. It does NOT delete the environment.
    virtual ~PhysicalSystem() {}

    PhysicalSystem(PhysicalSystem const&) = delete;
    PhysicalSystem& operator=(PhysicalSystem const&) = delete;

    // first-use environment creation + refcount.
    void AddRef();
    // one PhySys::Update tick.
    void Update(float Dt);
    // (thiscall RET 0xc): the beam ray cast of PhySys::GenerateCollisions.
    void GenerateCollisions(CBeam* Beam, Vector const& Direction, float Length);
    // (thiscall RET 0x10): the ray query of PhySys::FindRayCollisions.
    int FindRayCollisions(Vector const& From, Vector const& To, PhySys::RayHit* Hits, int MaxHits);
    // (thiscall RET 0x10): the sphere query of PhySys::FindSphereCollisions.
    int FindSphereCollisions(Vector const& Center, float Radius, CObject** OutObjects, int MaxObjects);
    // (thiscall RET 8): build Object's polygon body in this system.
    void CreatePolygonBody(CObject* Object, IVP_RealObjectConstructionTemplate* Template);
    // (thiscall RET 0x10): build a ball body in this system.
    IVP_Ball* CreateBallBody(Vector const& Position, Matrix const& Orientation, float Radius,
                             IVP_RealObjectConstructionTemplate* Template);

    // IVP_Listener_Object
    virtual void event_object_deleted(IVP_Environment_Object_Notify_Event* Event) override;
    // `RET 4` (ICF-shared no-op).
    virtual void event_object_created(IVP_Environment_Object_Notify_Event* /*Event*/) override {}
    virtual void event_object_revived(IVP_Environment_Object_Notify_Event* Event) override;
    virtual void event_object_frozen(IVP_Environment_Object_Notify_Event* Event) override;

    // IVP_Listener_Collision (slots 1..3 keep the base's no-op bodies, module storage)
    virtual void event_pre_collision(IVP_Event_Collision* Event) override;

    // IVP_Collision_Filter
    virtual int32_t check_objects_for_collision_detection(IVP_Real_Object* A, IVP_Real_Object* B) override;
    virtual void environment_will_be_deleted(IVP_Environment* Env) override;

    unsigned SystemId;                                // +0x14
    IVP_Environment* Environment;                     // +0x18
    std::vector<PhySys::CollisionEvent> Collisions;   // +0x1c (VC6: alloc, _First +0x20, _Last +0x24, _End +0x28)
    std::set<CObject*> MovingObjects;                 // +0x2c (VC6 _Tree: head +0x30, nil +0x34, _Multi +0x38, _Size +0x3c)
    int RefCount;                                     // +0x40
};

// The stack-built tuning struct the routine hands to the constructor. Default-constructed by
// (module storage-06342c7a) + the routine (module storage-0634304b), then
// selectively overwritten from the PhySys::GOLEM_* globals by the routine (PhySys::RCBegin
// builds one the same way with its RC_* values). Never escapes the stack in baseline; it is here
// only because both builders live in different translation units.
// +0x00..+0x23 is IVP_Controller_Motion_Parms (src/IVP/ivp_physics/ivp_controller.h): the
// share the routine initialises and IVP_Controller_Motion's constructor reads. Split out
// 2026-09-25 (claude-code:h-ivp) together with IVP_Controller_Motion itself.
struct FLControllerGolemParms : public IVP_Controller_Motion_Parms
{
    float MaxDeltaPosition;        // +0x24  default 10.0f (0x41200000)
    float DeltaOrientation;        // +0x28  default 1.5707964f (0x3fc90fdb, PI/2 as a float)
    float Field_0x2c;              // +0x2c  default 10.0f
    float Field_0x30;              // +0x30  default 1e20f (0x60ad78ec)
    float Field_0x34;              // +0x34  default 5.0f  (0x40a00000)
};

static_assert(sizeof(FLControllerGolemParms) == 0x38, "FLControllerGolemParms 0x38");
static_assert(offsetof(FLControllerGolemParms, MaxTorque) == 0x20, "parms+0x20 MaxTorque");
static_assert(offsetof(FLControllerGolemParms, MaxDeltaPosition) == 0x24, "parms+0x24");
static_assert(offsetof(FLControllerGolemParms, Field_0x34) == 0x34, "parms+0x34");

namespace CmnPhySys
{
    // .rdata source-file names baseline passes to FDUMP.
    const char kPhySysSource[] = "Common\\physys.cpp";
    const char kPhyObjSource[] = "Common\\PhyObj.cpp";

    // , the game's single IVP material (common_physys.cpp).
    extern IVP_Material* G_PhySysMaterial;
    // , the world map (common_physys.cpp).
    extern std::map<unsigned, PhysicalSystem> G_PhysicalSystems;

    // The system a SystemId names, looked up the way baseline's world-map readers do it.
    // VERY STRANGE (очень странно), REPRODUCED ON PURPOSE. baseline runs find / lower_bound plus
    // the key test (the routine, the routine, the routine) and then uses node+0x10 WITHOUT
    // testing for end(): on a miss that node is the VC6 tree's head node (the field), whose
    // value storage was never constructed -- the head is allocated with links and colour only.
    // So a miss reads (or builds into) uninitialised heap that only looks like a
    // PhysicalSystem. The node's address is deterministic and is reproduced here: the found
    // node, or on a miss our own map's head node (end()), whose value storage is just as
    // unconstructed. Its CONTENT is not reproducible (uninitialised in both binaries; the
    // debug CRT fills it with 0xCD), which is the named gap every caller keeps in the tracker.
    // `_Ptr` / `_Myval` are public in the MSVC STL; reading the node pointer of the returned
    // iterator never dereferences it, so no checked-iterator assert fires in Debug.
    // Callers: PhySys::Update 062899c0, GetCollisions 06289a20, GetCurrentTime 06289a70,
    // MovingObjectCount 06289ad0, GenerateCollisions 06289b98, FindSphereCollisions 06289ff0,
    // BuildIVP_Sphere 062893f3, SplitInstance 0628a260, ReMakeBody 0628a5e6, the three
    // CreatePhantom (0628ab21 / 0628b0f0 / 0628b299). The callers that DO test (Startup,
    // Shutdown, BuildPartIVP_Real 0628929e, FindRayCollisions 06289c5b) keep using find().
    inline PhysicalSystem& PhySys_FindSystem(unsigned SystemId)
    {
        return G_PhysicalSystems.find(SystemId)._Ptr->_Myval.second;
    }

    // IEngine (engine/sceneruntime.h) types its instance parameters and results as void*, but
    // they carry baseline's engine instance id -- a long, -1 for "none". The physics code
    // keeps ids in a long and converts only at the IEngine call; each helper below is
    // exactly one IEngine slot call, nothing more.
    inline void* PhySys_InstanceHandle(long Instance)
    {
        return (void*)(intptr_t)Instance;
    }

    inline long PhySys_InstanceId(void* Handle)
    {
        return (long)(intptr_t)Handle;
    }

    // IEngine +0xd8 GetRelatedInstance(From, Flags, Cursor): Cursor -1 starts a walk, -1 ends it.
    inline long PhySys_RelatedInstance(long From, unsigned long Flags, long Cursor)
    {
        return PhySys_InstanceId(
            NeuronRuntime::Engine->GetRelatedInstance(PhySys_InstanceHandle(From), Flags, PhySys_InstanceHandle(Cursor)));
    }

    // IEngine +0x80 GetInstanceUserData: the CObject PhySys stored on the instance, or null.
    inline CObject* PhySys_InstanceObject(long Instance)
    {
        return (CObject*)NeuronRuntime::Engine->GetInstanceUserData(PhySys_InstanceHandle(Instance));
    }

    // IEngine +0x7c SetInstanceUserData.
    inline void PhySys_SetInstanceObject(long Instance, CObject* Object)
    {
        NeuronRuntime::Engine->SetInstanceUserData(PhySys_InstanceHandle(Instance), Object);
    }

    // IEngine +0xd4 GetInstanceParent.
    inline long PhySys_InstanceParent(long Instance)
    {
        return PhySys_InstanceId(NeuronRuntime::Engine->GetInstanceParent(PhySys_InstanceHandle(Instance)));
    }

    // re-pose Object's jointed child instances (common_physys.cpp).
    void PhySys_UpdateJointedChildren(long Instance, CObject* Object);
    // The MatrixToQuaternion pose conversion of the routine / the routine and the
    // CreatePhantom bodies (common_physicalsystem.cpp).
    void PhySys_OrientationToIvpQuat(Matrix const& Orientation, IVP_U_Quat& Out);
    // on an old body pointer (common_physys_build.cpp).
    void PhySys_DeleteBody(IVP_Real_Object* Body);
    // + the routine, the tuning struct's defaults (common_controllergolem.cpp).
    void FLControllerGolemParms_Init(FLControllerGolemParms& Parms);
    // Dst = A + B * Scale (common_phantom.cpp).
    void PhySys_Vec3AddMul(Vector& Dst, Vector const& A, Vector const& B, float Scale);
    // / the routine: a phantom handle's position (common_phantom.cpp).
    int PhySys_GetPhantomPosition(void* Handle, Vector* OutPosition);
    int PhySys_SetPhantomPosition(void* Handle, Vector const* NewPosition);
}
