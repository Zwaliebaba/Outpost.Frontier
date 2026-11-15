// Author: Giperion.

#include "common_physys.h"
#include "physics_tick_profile.h" // Port-only owner-requested diagnostics

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
// PhySys::PhyPerfCounter -- the IVP performance counter every PhysicalSystem hands to
// its environment (IVP_Environment_Create_Parms +0x18, the routine module storage..
// ). RTTI: .?AVPhyPerfCounter@PhySys@@ : IVP_PerformanceCounter (COL 063a6b68).
// vtable module storage, 0xc bytes.
// ============================================================================
namespace PhySys
{
    class PhyPerfCounter : public IVP_PerformanceCounter
    {
    public:
        // (module storage..06285e00): +0x04 = 0, byte +0x08 = 0, vtable 0639baac.
        PhyPerfCounter()
            : Unread04(0)
            , Unread08(0)
        {
        }

        virtual void Slot00() override {}                // `RET` (ICF-shared)
        virtual void Slot01(int32_t /*Arg*/) override {} // `RET 4` (ICF-shared)
        virtual void Slot02() override {}                // `RET` (ICF-shared)

        // `if (this) this->vtbl[5](1)` -- the deleting destructor.
        virtual void environment_is_going_to_be_deleted(IVP_Environment* /*Env*/) override
        {
            delete this;
        }

        virtual void Slot04(float /*Arg0*/, float /*Arg1*/) override {} // `RET 8`

        // (ICF-folded): the routine stamps IVP_PerformanceCounter's vtable
        // , then operator delete when bit 0 of the flag is set.
        virtual ~PhyPerfCounter() {}

        // Never read: every slot of this class's vtable 0639baac is one of the no-ops above,
        // the deleting call or the destructor, so nothing looks at these after the ctor.
        int32_t Unread04;   // +0x04 (formerly Field_0x04), zeroed by the ctor
        uint8_t Unread08;   // +0x08 (formerly Field_0x08), zeroed by the ctor (byte store)
    };
}

namespace
{
    // .data:the field / 063fc0cc: the multiplayer time accumulator the routine keeps
    // (one double for all systems).
    double G_PhySysSimAccumulator = 0.0;

    // The IVP_Compact_Edge part id: the fourth dword of the edge's start point
    // (module storage..0628c482 for edge A, 0628c489..0628c4af for edge B):
    // triangle = edge & ~0xf; ledge = triangle - ((tri_index & 0xfff) + 1) * 16;
    // point = ledge + ledge->c_point_offset + start_point_index * 16; part id = point+0xc.
    unsigned PhySys_PartIdOfEdge(IVP_Compact_Edge const* Edge)
    {
        IVP_U_Float_Point const* Point = Edge->GetStartPoint();
        unsigned PartId;
        memcpy(&PartId, &Point->HesseVal, sizeof(PartId));
        return PartId;
    }
}

PhysicalSystem::PhysicalSystem(unsigned InSystemId)
    : IVP_Listener_Collision(IVP_LISTENER_COLLISION_CALLBACK_PRE_COLLISION) // +0x08 = 1
    , IVP_Collision_Filter_Coll_Group_Ident(0)                              // the routine(0)
    , SystemId(InSystemId)
    , Environment(nullptr)
    , RefCount(0)
{
}

// (module storage..0628c406). The object's client data (+0xa4) is the CObject;
// a null one is skipped, otherwise every set entry equal to it is erased (equal_range +
// erase, with the whole-tree clear fast path when the range is [begin, end)).
void PhysicalSystem::event_object_deleted(IVP_Environment_Object_Notify_Event* Event)
{
    CObject* const Object = (CObject*)Event->Object->client_data;
    if (Object == nullptr)
        return;
    MovingObjects.erase(Object);
}

// (module storage..0628c65b): a null client is skipped, otherwise a unique
// insert (lower-bound walk, the routine when the key is absent).
void PhysicalSystem::event_object_revived(IVP_Environment_Object_Notify_Event* Event)
{
    CObject* const Object = (CObject*)Event->Object->client_data;
    if (Object == nullptr)
        return;
    MovingObjects.insert(Object);
}

// (module storage..0628c766): same erase as event_object_deleted but WITHOUT the
// null test -- a null client erases a null key (normally absent, so nothing happens).
void PhysicalSystem::event_object_frozen(IVP_Environment_Object_Notify_Event* Event)
{
    CObject* const Object = (CObject*)Event->Object->client_data;
    MovingObjects.erase(Object);
}

// (module storage..0628c5a0), called with this = the IVP_Listener_Collision
// sub-object (World+4).
void PhysicalSystem::event_pre_collision(IVP_Event_Collision* Event)
{
    // FLD float d_time; FCOMP double MIN_TIME_BETWEEN_COLLISIONS;
    // TEST AH,0x41; JNZ -> return. C0 or C3 (less, equal, unordered) takes the return; the
    // fall-through is d_time > MIN_TIME_BETWEEN_COLLISIONS.
    if (!((double)(Event->d_time_since_last_collision) > PhySys::MIN_TIME_BETWEEN_COLLISIONS))
        return;

    IVP_Contact_Situation* const Situation = Event->contact_situation;
    CObject* const ObjectA = (CObject*)Situation->objects[0]->client_data;
    CObject* const ObjectB = (CObject*)Situation->objects[1]->client_data;

    if (ObjectA != nullptr)
    {
        if (ObjectB != nullptr)
        {
            // build the event and append it (the routine =
            // insert(_Last, 1, Event)).
            PhySys::CollisionEvent Collision;
            Collision.ObjectA = ObjectA;
            Collision.ObjectB = ObjectB;
            Collision.PartIdA = PhySys_PartIdOfEdge(Situation->compact_edges[0]);
            Collision.PartIdB = PhySys_PartIdOfEdge(Situation->compact_edges[1]);
            Collision.Point.x = Situation->contact_point_ws[0];      // R+0x20..0x28
            Collision.Point.y = Situation->contact_point_ws[1];
            Collision.Point.z = Situation->contact_point_ws[2];
            Collision.SurfaceNormal.x = Situation->surf_normal[0];      // R+0x00..0x08
            Collision.SurfaceNormal.y = Situation->surf_normal[1];
            Collision.SurfaceNormal.z = Situation->surf_normal[2];
            Collision.RelativeVelocity.x = Situation->speed[0];            // R+0x10..0x18
            Collision.RelativeVelocity.y = Situation->speed[1];
            Collision.RelativeVelocity.z = Situation->speed[2];
            Collisions.push_back(Collision);
        }

        // an object whose class has the three low bits set
        // (the projectile classes) is taken out of collision after its first contact.
        if (((unsigned)ObjectA->ObjectClass & 7) == 7)
        {
            // the routine on ObjectA->BodyPtr (not tested).
            ObjectA->BodyPtr->disable_simulation();
            ObjectA->enable_collisions_r(false);
        }
    }

    // the same for ObjectB.
    if (ObjectB != nullptr && ((unsigned)ObjectB->ObjectClass & 7) == 7)
    {
        ObjectB->BodyPtr->disable_simulation();
        ObjectB->enable_collisions_r(false);
    }
}

// (module storage..0628c799, this = the filter sub-object World+0xc). The base
// filter the routine is called directly; when it allows the pair (== 1) and a game filter
// is installed, the game filter's answer is returned instead.
int32_t PhysicalSystem::check_objects_for_collision_detection(IVP_Real_Object* A, IVP_Real_Object* B)
{
    int32_t const Result = IVP_Collision_Filter_Coll_Group_Ident::check_objects_for_collision_detection(A, B);
    if (PhySys::m_pCollisionFilter != nullptr && Result == 1)
        return (int32_t)PhySys::m_pCollisionFilter(A, B);
    return Result;
}

// (module storage..0628c7a7): `MOV [ECX+0xc],0` with this = World+0xc, i.e.
// World+0x18 = 0. The base's self-delete is not called.
void PhysicalSystem::environment_will_be_deleted(IVP_Environment* /*Env*/)
{
    Environment = nullptr;
}


// (module storage..0628ccea): copy an IVP body's pose into its CObject.
static void PhySys_SyncObjectFromBody(CObject* Object)
{
    // IVP_Real_Object::calc_at_time_AT (the routine).
    IVP_U_Quat BodyQuat;
    IVP_U_Point BodyPos;
    Object->BodyPtr->calc_at_time_AT(&BodyQuat, &BodyPos);

    // position as three floats; the quaternion rotated from IVP's
    // (x, y, z, w) into the math engine's (w, x, y, z).
    Vector Position;
    Position.x = BodyPos.x;
    Position.y = BodyPos.y;
    Position.z = BodyPos.z;
    FQuaternion Quat;
    Quat.W = BodyQuat.w;
    Quat.X = BodyQuat.x;
    Quat.Y = BodyQuat.y;
    Quat.Z = BodyQuat.z;

    // the field (lazily the routine) vtable+0x54.
    FMatrix Orientation;
    Acquire3DMathEngine()->QuaternionToMatrix(Orientation, Quat);

    // Object+0x2c = position, Object+0x08 = the 3x3 (REP MOVSD 9).
    static_assert(sizeof(Matrix) == sizeof(FMatrix), "EngineObject orientation is 9 floats");
    Object->WorldPosition = Position;
    memcpy(&Object->Orientation, &Orientation, sizeof(FMatrix));

    // with an engine instance, read the instance extent (flags 1)
    // and write the same values back, then the routine(instance, Object). The vector buffer
    // baseline hands both calls is the position copy above, overwritten by the read.
    if (Object->EngineInstanceId != -1)
    {
        float Extent;
        NeuronRuntime::Engine->GetInstanceExtent(PhySys_InstanceHandle(Object->EngineInstanceId), 1, &Extent, &Position.x);
        NeuronRuntime::Engine->SetInstanceExtent(PhySys_InstanceHandle(Object->EngineInstanceId), 1, Extent, &Position.x);
        // the routine(Object->EngineInstanceId, Object).
        PhySys_UpdateJointedChildren(Object->EngineInstanceId, Object);
    }

    // CSimple-derived classes ((class & 3) == 3) re-cache their
    // physical properties through vtable slot +0x84 (CSimple::cache_physical_props).
    if (((unsigned)Object->ObjectClass & 3) == 3)
        ((CSimple*)Object)->cache_physical_props();
}

// (module storage..0628ca70).
void PhysicalSystem::AddRef()
{
    // post-increment; the environment is built only on 0 -> 1.
    int const PreviousRefCount = RefCount;
    RefCount = PreviousRefCount + 1;
    if (PreviousRefCount != 0)
        return;

    // default create parms (the routine), the collision filter is
    // this object's filter sub-object (World+0xc), the performance counter a new PhyPerfCounter.
    IVP_Environment_Create_Parms Parms;
    Parms.CollisionFilter = this;                              // implicit upcast: this + 0x0c
    Parms.PerformanceCounter = new PhySys::PhyPerfCounter();

    // IVP_Environment_Manager::get_environment_manager() (the routine)
    // ->create_environment(&Parms, "digitalanvil", 0x11c2de34) (the routine).
    Environment = IVP_Environment_Manager::get_environment_manager()->create_environment(
        &Parms, "digitalanvil", 0x11c2de34);

    // PSI time step = 1.0f / PHYSICAL_SIM_RATE (float FDIV); IVP's
    // copy of MAX_SPAWNED_MINDIST_COUNT (the field = settings +0x12c) is written between
    // the division and the call.
    float const PsiTime = 1.0f / PhySys::PHYSICAL_SIM_RATE;
    G_IVP_MindistSettings.max_spawned_mindist_count = PhySys::MAX_SPAWNED_MINDIST_COUNT;
    IVP_Environment_SetDeltaPsiTime(Environment, PsiTime);

    // the range manager's (environment +0x1c) look-ahead values
    // (plain dword copies of the PhySys constants 063eb340..063eb35c / 063fc0f4 / 063fc0f8).
    IVP_Range_Manager* const RangeManager = Environment->range_manager;
    RangeManager->look_ahead_time_world = PhySys::RMGR_LOOK_AHEAD_TIME_WORLD;                 // +0x20
    RangeManager->look_ahead_max_radius_world = PhySys::RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD;     // +0x24
    RangeManager->look_ahead_min_distance_world = PhySys::RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD; // +0x28
    RangeManager->look_ahead_max_distance_world = PhySys::RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD; // +0x2c
    RangeManager->look_ahead_min_seconds_world = PhySys::RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD;   // +0x30
    RangeManager->look_ahead_time_intra = PhySys::RMGR_LOOK_AHEAD_TIME_INTRA;                 // +0x0c
    RangeManager->look_ahead_max_radius_intra = PhySys::RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA;     // +0x10
    RangeManager->look_ahead_min_distance_intra = PhySys::RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA; // +0x14
    RangeManager->look_ahead_max_distance_intra = PhySys::RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA; // +0x18
    RangeManager->look_ahead_min_seconds_intra = PhySys::RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA;   // +0x1c

    // zero gravity -- an IVP_U_Point on the stack with x, y, z = 0
    // (the fourth float is not written) handed to the routine (set_gravity).
    IVP_U_Point Gravity;
    Gravity.set(0.0f, 0.0f, 0.0f);
    IVP_Environment_SetGravity(Environment, &Gravity);

    // register as object listener (the routine) and as collision
    // listener (the routine, the IVP_Listener_Collision sub-object World+4).
    // The IVP helpers take void*, so the base sub-objects are formed first (implicit upcasts:
    // IVP_Listener_Object at +0x00, IVP_Listener_Collision at +0x04).
    IVP_Listener_Object* const ObjectListener = this;
    IVP_Listener_Collision* const CollisionListener = this;
    IVP_Environment_Append_Object_Listener(Environment, ObjectListener);
    IVP_Environment_Append_Listener_C8(Environment, CollisionListener);

    // anomaly limits (environment +0x24).
    Environment->anomaly_limits->MaxVelocity = PhySys::ANOM_LIMITS_MAX_VELOCITY;
    Environment->anomaly_limits->MaxAngularVelocityPerPsi = PhySys::ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI;
}

// (module storage..0628cbd2, thiscall RET 4).
void PhysicalSystem::Update(float Dt)
{
    // the result is never used (EAX is overwritten at 0628ca8a).
    timeGetTime();

    // erase(_First, _Last) (the routine).
    Collisions.clear();

    if (SinglePlayer())
    {
        // FCOMP double 0.0999; TEST AH,0x41; JNZ keeps Dt (less,
        // equal, unordered); the fall-through (Dt > 0.0999) stores (float)0.0999.
        if ((double)(Dt) > 0.0999)
            Dt = (float)(0.0999);
        // the routine (simulate_dtime).
        IVP_Environment_SimulateDTime(Environment, Dt);
    }
    else
    {
        // acc += Dt (double, FST); only acc > 0.0051 steps.
        G_PhySysSimAccumulator += (double)(Dt);
        if (G_PhySysSimAccumulator > 0.0051)
        {
            // whole 0.0999 steps; after each the moving objects are
            // synced from their bodies.
            while (G_PhySysSimAccumulator > 0.0999)
            {
                // the routine with (float)0.0999.
                IVP_Environment_SimulateDTime(Environment, (float)(0.0999));
                G_PhySysSimAccumulator -= 0.0999;
                FPhysicsTickScope SyncProfile(EPhysicsTickSection::SyncObjects);
                for (std::set<CObject*>::iterator It = MovingObjects.begin(); It != MovingObjects.end(); ++It)
                    PhySys_SyncObjectFromBody(*It);
            }
            // the remainder when above (double)1e-5f (the field).
            if (G_PhySysSimAccumulator > (double)(1.0e-5f))
            {
                // the routine with (float)acc.
                IVP_Environment_SimulateDTime(Environment, (float)(G_PhySysSimAccumulator));
            }

            G_PhySysSimAccumulator = 0.0;
        }
    }

    // sync every moving object.
    FPhysicsTickScope SyncProfile(EPhysicsTickSection::SyncObjects);
    for (std::set<CObject*>::iterator It = MovingObjects.begin(); It != MovingObjects.end(); ++It)
        PhySys_SyncObjectFromBody(*It);
}

// (module storage..0628d19c, thiscall RET 0xc). The beam's ray through this
// system's environment: the nearest hit whose object is not in the beam's collision group and
// has a client object moves the beam to the hit point and becomes one CollisionEvent.
void PhysicalSystem::GenerateCollisions(CBeam* Beam, Vector const& Direction, float Length)
{
    // the start is LastPos (+0x110) when the beam moved (+0x11c),
    // else its WorldPosition (+0x2c) (dword copies).
    Vector const& Source = (Beam->bMoved != 0) ? Beam->LastPos : Beam->WorldPosition;
    Vector Start = Source;

    // when BULLET_LENGTH_FUDGE * 0.5f < Length (FCOMP / TEST AH,0x5 /
    // JP skips on greater, equal or unordered): Length += fudge (FSTP float), and the start is
    // pulled back by (Direction * fudge) * 0.5f (the routine twice, the routine).
    const float Fudge = PhySys::BULLET_LENGTH_FUDGE;
    if ((double)(Fudge) * 0.5f < Length)
    {
        Length = (float)((double)(Fudge) + Length);
        Vector Scaled;
        Scaled.x = (float)((double)(Fudge) * Direction.x);
        Scaled.y = (float)((double)(Fudge) * Direction.y);
        Scaled.z = (float)((double)(Fudge) * Direction.z);
        Vector Half;
        Half.x = (float)(0.5 * Scaled.x);
        Half.y = (float)(0.5 * Scaled.y);
        Half.z = (float)(0.5 * Scaled.z);
        Start.x = (float)((double)(Start.x) - Half.x);
        Start.y = (float)((double)(Start.y) - Half.y);
        Start.z = (float)((double)(Start.z) - Half.z);
    }

    // the template -- the start (FLD / FSTP float), the direction as
    // given (the routine, not normalised here), Length, flags 0. UB in baseline: both fourth
    // floats are a never-written stack slot; the port stores 0.
    IVP_Ray_Solver_Template Template;
    Template.ray_start_point.set(Start.x, Start.y, Start.z);
    Template.ray_start_point.hesse = 0.0f;
    Template.ray_normized_direction.set(Direction.x, Direction.y, Direction.z);
    Template.ray_normized_direction.hesse = 0.0f;
    Template.ray_length = Length;
    Template.ray_flags = 0;

    // the solver (the routine, the routine(8), the hit array's
    // `eh vector constructor iterator` with the no-op Csys::Csys, vptr 0639c2c4) against
    // everything in the simulation (the routine on +0x18).
    IVP_Ray_Solver_Min_Hash Solver(&Template);
    Solver.check_ray_against_all_objects_in_sim(Environment);


    if (Solver.min_hash.counter <= 0)
        return;

    for (;;)
    {

        IVP_Ray_Hit* const Hit = (IVP_Ray_Hit*)Solver.min_hash.find_min_elem();
        if (Hit == nullptr)
            return;
        IVP_Real_Object* const HitObject = Hit->hit_real_object;

        // a hit in the beam's own collision group (both names
        // non-empty and equal, the inlined strcmp), or one without a client object, is dropped
        // (the routine remove_min) and the next one is taken.
        bool bDrop = false;
        if (HitObject->name[0] != 0 && Beam->CollisionGroup[0] != 0 &&
            strcmp(HitObject->name, Beam->CollisionGroup) == 0)
            bDrop = true;
        else if (HitObject->client_data == nullptr)
            bDrop = true;
        if (bDrop)
        {
            Solver.min_hash.remove_min();
            continue;
        }

        // the event's two objects and part ids; the distance is the
        // min hash element's key (stadium[1]->value).
        CObject* const Client = (CObject*)HitObject->client_data;
        const float Distance = Solver.min_hash.stadium[1]->value;
        PhySys::CollisionEvent Event;
        Event.ObjectA = Client;
        Event.ObjectB = Beam;
        Event.PartIdA = (Hit->hit_compact_ledge != nullptr) ? (unsigned)Hit->hit_compact_ledge->ClientData
                                                            : 0;
        Event.PartIdB = 0;

        // the hit point Start + Direction * Distance (the routine,
        // ) becomes the beam's WorldPosition and the event's point.
        Vector Step;
        Step.x = (float)((double)(Distance) * Template.ray_normized_direction.x);
        Step.y = (float)((double)(Distance) * Template.ray_normized_direction.y);
        Step.z = (float)((double)(Distance) * Template.ray_normized_direction.z);
        Vector HitPoint;
        HitPoint.x = (float)((double)(Start.x) + Step.x);
        HitPoint.y = (float)((double)(Start.y) + Step.y);
        HitPoint.z = (float)((double)(Start.z) + Step.z);
        Beam->WorldPosition = HitPoint;
        Event.Point = HitPoint;

        // the hit object's cache slot (lazily claimed by
        // , referenced, refreshed by the routine when stale -- the inline
        // ResolveNarrowPhaseCacheSlot), the object-space hit normal rotated to world space
        // (the routine), the reference dropped.
        IVP_NarrowPhaseCacheSlot* const Slot = HitObject->ResolveNarrowPhaseCacheSlot();
        IVP_U_Point NormalWs;
        IVP_RotateSlotToWorldSpace(Slot, &Hit->hit_surface_direction_os, &NormalWs);
        HitObject->ReleaseNarrowPhaseCacheSlot(Slot);
        Event.SurfaceNormal.x = NormalWs.x;
        Event.SurfaceNormal.y = NormalWs.y;
        Event.SurfaceNormal.z = NormalWs.z;

        // the client body's surface speed at the hit point
        // (the routine on Client->Body->core). UB in baseline: the point's fourth float is never
        // written; the port stores 0.
        IVP_U_Point HitPointWs;
        HitPointWs.set(HitPoint.x, HitPoint.y, HitPoint.z);
        HitPointWs.hesse = 0.0f;
        IVP_U_Point SurfaceSpeed;
        Client->BodyPtr->core->get_surface_speed(&HitPointWs, &SurfaceSpeed);

        // the beam's velocity (vtable +0x54) minus that speed, per
        // component (FSUB float, FSTP float).
        const Vector BeamVelocity = Beam->get_velocity();
        Event.RelativeVelocity.x = (float)((double)(BeamVelocity.x) - SurfaceSpeed.x);
        Event.RelativeVelocity.y = (float)((double)(BeamVelocity.y) - SurfaceSpeed.y);
        Event.RelativeVelocity.z = (float)((double)(BeamVelocity.z) - SurfaceSpeed.z);

        // the routine(_Last, 1, &Event) -- appended to +0x1c. Only
        // the first accepted hit is used.
        Collisions.push_back(Event);
        return;
    }
}

namespace CmnPhySys
{
    // The pose conversion the routine / the routine share: the math engine (the field,
    // lazily the routine) slot +0x50 MatrixToQuaternion, then the (W, X, Y, Z) result is
    // stored as IVP's (x, y, z, w) (module storage..0628c817 / 0628c8a1..0628c8c6).
    void PhySys_OrientationToIvpQuat(Matrix const& Orientation, IVP_U_Quat& Out)
    {
        static_assert(sizeof(Matrix) == sizeof(FMatrix), "EngineObject orientation is 9 floats");
        FQuaternion Quat;
        Acquire3DMathEngine()->MatrixToQuaternion(Quat, (FMatrix const&)Orientation);
        Out.x = Quat.X;
        Out.y = Quat.Y;
        Out.z = Quat.Z;
        Out.w = Quat.W;
    }
}

// (module storage..0628c851).
void PhysicalSystem::CreatePolygonBody(CObject* Object, IVP_RealObjectConstructionTemplate* Template)
{
    // the position (three float copies; the fourth float of the
    // IVP_U_Point is not written).
    IVP_U_Point Position;
    Position.set(Object->WorldPosition.x, Object->WorldPosition.y, Object->WorldPosition.z);

    // Object->Orientation (+0x08) as an IVP quaternion.
    IVP_U_Quat Rotation;
    PhySys_OrientationToIvpQuat(Object->Orientation, Rotation);

    // a template without a material gets the PhySys material.
    if (Template->Material == nullptr)
        Template->Material = G_PhySysMaterial;

    // the routine(this->Environment, Object+0x5c, Template,
    // &Rotation, &Position). Object+0x5c is the surface manager BuildPartIVP_Real
    // (the routine) stores there.
    IVP_Polygon* const Body = IVP_Environment_CreatePolygon(Environment, Object->SurfaceManager, Template,
                                                            &Rotation, &Position);


    Object->BodyPtr = Body;
    Object->PhysicsSystemId = SystemId;
    Body->client_data = Object;
}

// (module storage..0628c8f9).
IVP_Ball* PhysicalSystem::CreateBallBody(Vector const& Position, Matrix const& Orientation, float Radius,
                                         IVP_RealObjectConstructionTemplate* Template)
{
    // the position (three float copies).
    IVP_U_Point BallPosition;
    BallPosition.set(Position.x, Position.y, Position.z);


    IVP_U_Quat Rotation;
    PhySys_OrientationToIvpQuat(Orientation, Rotation);


    if (Template->Material == nullptr)
        Template->Material = G_PhySysMaterial;

    // the radius is copied into the first argument slot and its
    // address passed: the routine(this->Environment, &Radius, Template, &Rotation, &Position).
    float BallRadius = Radius;
    return IVP_Environment_CreateBall(Environment, &BallRadius, Template, &Rotation, &BallPosition);
}

// ============================================================================
// PhySys::Startup / Shutdown
// ============================================================================

void __cdecl PhySys::Startup(unsigned SystemId)
{
    // Common 0x29510: material singleton once, std::map lookup by system id,
    // create world on miss, bump first-use counter.
    // `if (the field == 0) the field = new(0x1c)
    // IVP_Material_Simple((float)MATERIAL_FRICTION, (float)MATERIAL_ELASTICITY)` -- both
    // constants are doubles narrowed by FSTP float.
    if (G_PhySysMaterial == nullptr)
    {
        G_PhySysMaterial = new IVP_Material_Simple((float)(MATERIAL_FRICTION),
                                                   (float)(MATERIAL_ELASTICITY));
    }

    // the field lower_bound + `key < node key` test = find.
    std::map<unsigned, PhysicalSystem>::iterator It = G_PhysicalSystems.find(SystemId);
    if (It == G_PhysicalSystems.end())
    {
        // build PhysicalSystem(SystemId) (the routine) on the
        // stack, copy it into a pair (the routine), insert the pair (the routine), destroy
        // both temporaries. The copies are of a freshly built, empty object; the port builds
        // the mapped value in place (it is registered with IVP by address later, in AddRef).
        It = G_PhysicalSystems.emplace(std::piecewise_construct, std::forward_as_tuple(SystemId),
                                       std::forward_as_tuple(SystemId)).first;
    }

    // the routine on the mapped value (node + 0x10).
    It->second.AddRef();
}

void __cdecl PhySys::Shutdown(unsigned SystemId)
{
    // find.
    std::map<unsigned, PhysicalSystem>::iterator It = G_PhysicalSystems.find(SystemId);
    if (It != G_PhysicalSystems.end())
    {
        PhysicalSystem& System = It->second;
        // `if (--RefCount == 0)` delete the environment
        // (the routine then operator delete) and clear the pointer.
        if (--System.RefCount == 0)
        {
            IVP_Environment* const Env = System.Environment;
            if (Env != nullptr)
            {
                IVP_Environment_Destructor(Env);
                ::operator delete(Env);
            }
            System.Environment = nullptr;
        }
        // erase the node (the routine) once RefCount is 0.
        if (System.RefCount == 0)
            G_PhysicalSystems.erase(It);
    }

    // the material goes when the map (size the field) is empty --
    // tested whether or not SystemId was found.
    if (G_PhysicalSystems.empty())
    {
        // `if (the field) the field->vtbl[5](1)` (the
        // deleting destructor), `the field = 0`, then tail-JMP the routine.
        if (G_PhySysMaterial != nullptr)
            delete G_PhySysMaterial;
        G_PhySysMaterial = nullptr;
        // tail JMP the routine -- free IVP's cached point-soup compact
        // surface (the field) and clear it.
        IVP_SurfaceBuilder_Pointsoup::Cleanup();
    }
}

// ============================================================================
// PhySys::Update
// ============================================================================

void __cdecl PhySys::Update(unsigned SystemId, float Dt)
{
    // lower_bound + key test, then the routine on node + 0x10 in
    // both branches -- a missing SystemId updates the map head node's unconstructed storage
    // (PhySys_FindSystem: very strange, reproduced on purpose).
    PhySys_FindSystem(SystemId).Update(Dt);
}

// ============================================================================
// Queries: ray / sphere queries against the system's IVP environment
// (the routine, the routine, the routine, the routine).
// ============================================================================

// (module storage..0628d444, thiscall RET 0x10). The ray From -> To against every
// object of this system's environment, nearest hit first, at most MaxHits; hits without a
// client object are skipped. Returns the number written.
int PhysicalSystem::FindRayCollisions(Vector const& From, Vector const& To, PhySys::RayHit* Hits, int MaxHits)
{
    // Delta = To - From (the routine, FSUB float -> FSTP float).
    const float Dx = (float)((double)(To.x) - From.x);
    const float Dy = (float)((double)(To.y) - From.y);
    const float Dz = (float)((double)(To.z) - From.z);

    // Length = sqrt((dy*dy + dz*dz) + dx*dx) on the stack; the
    // direction is Delta * (1.0f / Length), each FSTP float; Length is stored as a float.
    const double Length = sqrt(((double)(Dy) * Dy + (double)(Dz) * Dz) +
                               (double)(Dx) * Dx);
    const double InvLength = 1.0 / Length;
    const float DirX = (float)(Dx * InvLength);
    const float DirY = (float)(Dy * InvLength);
    const float DirZ = (float)(Dz * InvLength);

    // the ray template. UB in baseline: the fourth float of the start
    // point and of the direction is a never-written stack slot; the port stores 0.
    IVP_Ray_Solver_Template Template;
    Template.ray_start_point.set(From.x, From.y, From.z);
    Template.ray_start_point.hesse = 0.0f;
    Template.ray_normized_direction.set(DirX, DirY, DirZ);
    Template.ray_normized_direction.hesse = 0.0f;
    Template.ray_length = (float)(Length);
    Template.ray_flags = 0;

    // the solver (the routine, min hash the routine(8), vptr
    // ) against everything in the simulation (the routine on this+0x18).
    IVP_Ray_Solver_Min_Hash Solver(&Template);
    Solver.check_ray_against_all_objects_in_sim(Environment);


    int Count = 0;
    PhySys::RayHit* Out = Hits;
    for (;;)
    {
        IVP_Ray_Hit* const Hit = (IVP_Ray_Hit*)Solver.min_hash.find_min_elem();
        if (Hit == nullptr || Count >= MaxHits)
            break;

        CObject* const Client = (CObject*)Hit->hit_real_object->client_data;
        if (Client != nullptr)
        {
            Out->Object = Client;

            // V = distance * direction (FMUL, FSTP float each,
            // ), Point = From + V (the routine).
            const double Distance = Hit->hit_distance;
            const float Vx = (float)(DirX * Distance);
            const float Vy = (float)(DirY * Distance);
            const float Vz = (float)(DirZ * Distance);
            Out->Point.x = (float)((double)(From.x) + Vx);
            Out->Point.y = (float)((double)(From.y) + Vy);
            Out->Point.z = (float)((double)(From.z) + Vz);

            // the hit normal (object space, FLD / FSTP float copies)
            // through the client's orientation (+0x08): TransformMatrix (+0x2c).
            FVector NormalOs;
            NormalOs.X = Hit->hit_surface_direction_os.x;
            NormalOs.Y = Hit->hit_surface_direction_os.y;
            NormalOs.Z = Hit->hit_surface_direction_os.z;
            FVector NormalWs;
            Acquire3DMathEngine()->TransformMatrix(NormalWs, (FMatrix const&)Client->Orientation, NormalOs);
            Out->Normal.x = NormalWs.X;
            Out->Normal.y = NormalWs.Y;
            Out->Normal.z = NormalWs.Z;

            // the hit ledge's client data (its part id), or 0.
            Out->PartId = (Hit->hit_compact_ledge != nullptr)
                              ? (unsigned)Hit->hit_compact_ledge->ClientData
                              : 0;
            ++Count;
            ++Out;
        }


        Solver.min_hash.remove(Hit);
    }

    // ~IVP_Ray_Solver_Min_Hash.
    return Count;
}

// (module storage..0628d55e, thiscall RET 0x10).
// the six-dword record at local_248: Center (float copies), an
//                       uninitialised dword (local_24c, never read by the callee), Radius, 4
// the query object at local_230: Found = {memsize 0x80, n_elems 0,
//                       elems = the inline buffer local_228}
// Query.collect_objects(&Record, this->Environment (+0x18)) -- EH state 0
//                       from 0628d4dd, so the query is a RAII local
// for i < n_elems while Count < MaxObjects (JGE -> stop): client =
//                       Found[i]->client_data (+0xa4); a non-null client goes to
//                       OutObjects[Count++]
// the vector's own free of a grown buffer (not the inline one, not null)
// Returns Count. Nothing else is tested.
int PhysicalSystem::FindSphereCollisions(Vector const& Center, float Radius, CObject** OutObjects, int MaxObjects)
{
    FIVP_Sphere_Query_Record Record;
    Record.Center[0] = Center.x;
    Record.Center[1] = Center.y;
    Record.Center[2] = Center.z;
    Record.Unwritten_0x0c = 0.0f;  // UB in baseline: an uninitialised stack dword; never read
    Record.Radius = Radius;
    Record.MaxLedgeTreeDepth = 4;

    IVP_Sphere_Object_Query Query;
    Query.collect_objects(&Record, Environment);

    int Count = 0;
    for (int i = 0; i < (int)Query.Found.n_elems; ++i)
    {
        if (Count >= MaxObjects)
        {
            break;
        }
        CObject* const Client = (CObject*)Query.Found.elems[i]->client_data;
        if (Client != nullptr)
        {
            OutObjects[Count] = Client;
            ++Count;
        }
    }
    return Count;
}

