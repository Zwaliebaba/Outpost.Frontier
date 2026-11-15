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

namespace
{
    // (PhyCollisionStateManager::mindist_left_volume, module storage).
    const char kPhyCollisionStateManagerSource[] = "Common\\PhyCollisionStateManager.cpp";
}

// ============================================================================
// PhySys::PhyCollisionStateManager
// ============================================================================

PhySys::PhyCollisionStateManager::PhyCollisionStateManager(CObject* Object)
    : OwnerObject(Object)
    , IntruderCount(0)
    , IntruderTime(0.0f)
    , CollisionType(0)
{
    // Pad11 (+0x11..+0x13) is not written, as in baseline.
    // +0x04 = Object, +0x08 / +0x0c = 0, byte +0x10 = 0, vtable;
    // then CAsteroid::asteroidarch(Object) (the ICF-folded `MOV EAX,[this+0x88]`, Object not
    // tested) and, with an archetype, byte +0x10 = Root byte +0x39 (phantom_physics).
    Archetype::Root const* const Arch = Object->Archetype;
    if (Arch != nullptr)
        CollisionType = (unsigned char)Arch->bPhantomPhysics;
}

// vptr, the dwords +0x04 / +0x08 / +0x0c and the byte +0x10; the
// padding +0x11..+0x13 is not copied.
PhySys::PhyCollisionStateManager::PhyCollisionStateManager(PhyCollisionStateManager const& Other)
    : OwnerObject(Other.OwnerObject)
    , IntruderCount(Other.IntruderCount)
    , IntruderTime(Other.IntruderTime)
    , CollisionType(Other.CollisionType)
{
}

PhySys::PhyCollisionStateManager::~PhyCollisionStateManager()
{
    // Body = OwnerObject->BodyPtr (the owner is not tested); a body
    // with a phantom controller (+0x1c) has this manager removed from the phantom's listener
    // vector: the routine(ECX = phantom, this).
    IVP_Real_Object* const Body = OwnerObject->BodyPtr;
    if (Body == nullptr)
        return;
    IVP_Controller_Phantom* const Phantom = (IVP_Controller_Phantom*)Body->first_child;
    if (Phantom == nullptr)
        return;
    Phantom->remove_listener_phantom(this);
}

// (ICF-shared with CPhysControllerEquip::operator=): the dwords
// +0x04 / +0x08 / +0x0c and the byte +0x10, no self test, padding not copied.
PhySys::PhyCollisionStateManager& PhySys::PhyCollisionStateManager::operator=(
    PhyCollisionStateManager const& Other)
{
    OwnerObject = Other.OwnerObject;
    IntruderCount = Other.IntruderCount;
    IntruderTime = Other.IntruderTime;
    CollisionType = Other.CollisionType;
    return *this;
}

// (thiscall, RET 8).
void PhySys::PhyCollisionStateManager::enable_collisions(bool bEnable, bool bImmediate)
{
    // Body = this->OwnerObject->BodyPtr; none -> return.
    IVP_Real_Object* const Body = OwnerObject->BodyPtr;
    if (Body == nullptr)
        return;

    // AND EDX,0x300; CMP EDX,0x100; SETZ BL -- collision detection
    // is enabled exactly when the two location bits read 01.
    bool const bWasEnabled = (Body->flags & 0x300u) == 0x100u;

    if (!bEnable)
    {
        // only an enabled body is switched off.
        if (bWasEnabled)
        {
            Body->enable_collision_detection(false);   // the routine(0)
            IntruderCount = 0;
            IntruderTime = 0.0f;                       // (dword 0)
        }
        return;
    }

    // neither immediate nor a phantom-physics archetype -> the
    // deferred path: drop the collision phantom and enable detection.
    if (!bImmediate && CollisionType == 0)
    {
        // with a phantom (Body+0x1c), the routine turns the body
        // back into an object, then IntruderCount / IntruderTime = 0.
        if (Body->first_child != nullptr)
        {
            Body->convert_phantom_to_object();
            IntruderCount = 0;
            IntruderTime = 0.0f;
        }
        // an already enabled body is left as it is.
        if (bWasEnabled)
            return;
        // OwnerObject->BodyPtr re-read, the routine(1).
        OwnerObject->BodyPtr->enable_collision_detection(true);
        return;
    }

    // immediate (or phantom-physics) path; nothing to do when the
    // body is already enabled.
    if (bWasEnabled)
        return;

    // without a phantom one is built: an IVP_Template_Phantom on the
    // stack (the routine) whose client_data (+0x14, local_4) is 1 (module storage),
    // on the re-read body converts it to a phantom, and this manager becomes a
    // listener of the re-read body's phantom (the routine).
    if (Body->first_child == nullptr)
    {
        IVP_Template_Phantom Template;
        Template.client_data = (void*)(uintptr_t)1;
        OwnerObject->BodyPtr->convert_to_phantom(&Template);
        IVP_Real_Object* const Converted = OwnerObject->BodyPtr;
        ((IVP_Controller_Phantom*)Converted->first_child)->add_listener_phantom(this);
    }

    // OwnerObject->BodyPtr re-read, the routine(1).
    OwnerObject->BodyPtr->enable_collision_detection(true);

    // re-read the body; with a phantom, a non-phantom archetype,
    // detection enabled and nobody inside, the phantom is turned back into an object again
    // (the routine) and IntruderTime = 0 (IntruderCount is already 0).
    IVP_Real_Object* const BodyAfter = OwnerObject->BodyPtr;
    if (BodyAfter == nullptr || BodyAfter->first_child == nullptr)
        return;
    if (CollisionType != 0)
        return;
    if ((BodyAfter->flags & 0x300u) != 0x100u)
        return;
    if (IntruderCount != 0)
        return;
    BodyAfter->convert_phantom_to_object();
    IntruderTime = 0.0f;
}

bool PhySys::PhyCollisionStateManager::get_desired_collision_state() const
{
    // Body = OwnerObject->BodyPtr (the owner is not tested); no body
    // -> false; else (Body->flags & 0x300) == 0x100 (SUB / NEG / SBB / INC).
    IVP_Real_Object const* const Body = OwnerObject->BodyPtr;
    if (Body == nullptr)
        return false;
    return (Body->flags & 0x300u) == 0x100u;
}

bool PhySys::PhyCollisionStateManager::get_actual_collision_state() const
{
    // Body = OwnerObject->BodyPtr; no body or detection off -> false;
    // else true exactly when the body has no phantom controller (+0x1c, SETZ).
    IVP_Real_Object const* const Body = OwnerObject->BodyPtr;
    if (Body == nullptr)
        return false;
    if ((Body->flags & 0x300u) != 0x100u)
        return false;
    return Body->first_child == nullptr;
}

void PhySys::PhyCollisionStateManager::update(float Dt)
{
    // (thiscall, RET 4).
    IVP_Real_Object* const Body = OwnerObject->BodyPtr;
    if (Body == nullptr)
        return;
    if (Body->first_child == nullptr)         // no phantom controller
        return;
    if (CollisionType != 0)
        return;
    if ((Body->flags & 0x300u) != 0x100u)
        return;
    if (IntruderCount == 0)
    {
        Body->convert_phantom_to_object();    // the routine
        IntruderTime = 0.0f;                  // (dword 0)
        return;
    }
    IntruderTime += Dt;                       // FLD / FADD / FSTP float
}

namespace
{
    // `MindistBase->vtbl[1](&Pair)` at the top of every PhyCollisionStateManager callback:
    // slot 1 of IVP_Mindist_Base (ivp_mindist.h) is get_objects (thiscall, one out-array
    // argument).
    void PhySys_GetMindistObjects(IVP_Mindist_Base* MindistBase, IVP_Real_Object* OutPair[2])
    {
        MindistBase->get_objects(OutPair);
    }

    // The shared selection: the pair member that is not the owner -- Pair[1] when
    // Pair[0]->client_data (+0xa4) is the owner, else Pair[0].
    IVP_Real_Object* PhySys_SelectOther(IVP_Real_Object* const Pair[2], CObject const* Owner)
    {
        return (Owner == (CObject*)Pair[0]->client_data) ? Pair[1] : Pair[0];
    }

    // The intruder filter of module storage..06281726 / 06281767..06281786: an object without
    // a client is ignored; when the owner's body location byte (+0x78) is 9, an intruder whose
    // own location byte (MOVSX) is 0x10 or 9 is ignored as well.
    bool PhySys_CountsAsIntruder(CObject const* Owner, IVP_Real_Object const* Other)
    {
        if (Other->client_data == nullptr)
            return false;
        IVP_Real_Object const* const OwnerBody = Owner->BodyPtr;
        if ((uint8_t)OwnerBody->flags != 9)
            return true;
        int8_t const OtherLocation = (int8_t)Other->flags;
        return OtherLocation != 0x10 && OtherLocation != 9;
    }
}

// (thiscall, RET 8).
void PhySys::PhyCollisionStateManager::mindist_entered_volume(IVP_Controller_Phantom* /*PhantomCtrl*/, IVP_Mindist_Base* MindistBase)
{
    IVP_Real_Object* Pair[2];
    PhySys_GetMindistObjects(MindistBase, Pair);
    IVP_Real_Object* const Other = PhySys_SelectOther(Pair, OwnerObject);
    if (PhySys_CountsAsIntruder(OwnerObject, Other))
        ++IntruderCount;
}

// (thiscall, RET 8).
void PhySys::PhyCollisionStateManager::mindist_left_volume(IVP_Controller_Phantom* /*PhantomCtrl*/, IVP_Mindist_Base* MindistBase)
{
    IVP_Real_Object* Pair[2];
    PhySys_GetMindistObjects(MindistBase, Pair);
    IVP_Real_Object* const Other = PhySys_SelectOther(Pair, OwnerObject);
    if (!PhySys_CountsAsIntruder(OwnerObject, Other))
        return;
    // DEC dword +0x8; JNS -> done; a negative count is reported.
    --IntruderCount;
    if ((int)IntruderCount < 0)
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kPhyCollisionStateManagerSource, 0x11d,
              "Negative intruder count!");
}

void PhySys::PhyCollisionStateManager::phantom_is_going_to_be_deleted_event(IVP_Controller_Phantom* PhantomCtrl)
{
    // ICF-folded with CostumeDescriptions::warn @ 0x8D730.
    // Empty body — just returns.
    (void)PhantomCtrl;
}

IVP_Real_Object* PhySys::PhyCollisionStateManager::get_other_object(IVP_Mindist_Base* MindistBase)
{
    // (thiscall, RET 4): get_objects, then the non-owner member.
    IVP_Real_Object* Pair[2];
    PhySys_GetMindistObjects(MindistBase, Pair);
    return PhySys_SelectOther(Pair, OwnerObject);
}

CObject* PhySys::PhyCollisionStateManager::mindist_to_cobject(IVP_Mindist_Base* MindistBase)
{
    // (thiscall, RET 4): get_objects, the non-owner member, its
    // client (+0xa4).
    IVP_Real_Object* Pair[2];
    PhySys_GetMindistObjects(MindistBase, Pair);
    return (CObject*)PhySys_SelectOther(Pair, OwnerObject)->client_data;
}

// (thiscall, RET 8).
int PhySys::PhyCollisionStateManager::get_intruding_cobjs(bool bRoot, CheapSet<CObject*, std::less<CObject*>>& Out)
{
    // An IVP_VHash slot: {hash index, element} (the routine reads the element at +4 of an
    // 8-byte slot). TODO: IVP_VHash exposes no typed slot accessor yet; this view of its
    // Elems array is the only use outside ivp_vhash.h.
    struct FVHashSlot
    {
        unsigned HashIndex;
        IVP_Mindist_Base* Elem;
    };

    // no body or no phantom (Body+0x1c) -> nothing is added.
    IVP_Real_Object* const Body = OwnerObject->BodyPtr;
    if (Body != nullptr)
    {
        IVP_Controller_Phantom* const Phantom = (IVP_Controller_Phantom*)Body->first_child;
        if (Phantom != nullptr)
        {
            // every slot of the phantom's mindist set, SizeMm + 1 of
            // them (re-read on every pass).
            for (int Slot = 0; Slot < Phantom->mindist_set.SizeMm + 1; ++Slot)
            {
                IVP_Mindist_Base* const Mindist = ((FVHashSlot*)Phantom->mindist_set.Elems)[Slot].Elem;
                if (Mindist == nullptr)
                    continue;

                // the non-owner object of the pair, its client.
                IVP_Real_Object* Pair[2];
                PhySys_GetMindistObjects(Mindist, Pair);
                CObject* const Client = (CObject*)PhySys_SelectOther(Pair, OwnerObject)->client_data;
                if (Client == nullptr)
                    continue;

                // the root (GetRoot, 0630e660) or the client itself,
                // added unless present (the inlined find / insert-at-end / find; the client
                // path also builds the pair<iterator, bool> through the routine).
                if (bRoot)
                    Out.Insert(GetRoot(Client));
                else
                    Out.Insert(Client);
            }
        }
    }

    // (end - begin) >> 2, or 0 while begin is null.
    return Out.Num();
}

