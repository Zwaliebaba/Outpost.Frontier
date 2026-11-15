// Author: Giperion.

#pragma once

#include <cstdint>

#include "ivp_collision.h"                  // IVP_Collision, IVP_Collision_Delegator
#include "../ivp_physics/ivp_listener.h"    // IVP_Listener_Hull
#include "../ivp_utility/ivp_u_vector.h"    // IVP_U_Vector

class IVP_Real_Object;
class IVP_Hull_Manager;
class IVP_OO_Watcher;

// ============================================================================
// IVP_OO_Watcher / IVP_Synapse_OO (cmn-ivp2b, 2026-10-08). Library names
// (Ipion). The collision record IVP_Collision_Delegator_Root_Mindist::
// delegate_collisions_for_object (the routine) creates for an object pair
// whose surface managers do not both hand out a single convex ledge
// (module storage-06360d7e: operator_new(0x40) + the routine(this, obj0,
// obj1)). The watcher keeps one hull listener on each object and, whenever
// either hull value passes its key, rebuilds the pair's exact mindists
// (the routine) into its own vector, acting as their delegator.

// baseline vtables (Common .rdata):
// IVP_OO_Watcher (IVP_Collision part, +0x00):
// (base trap) / 06369e20 get_objects / 06369e40 get_ledges
//             / 0634dc10 (base) / 06369cc0 deleting dtor
// IVP_OO_Watcher (IVP_Collision_Delegator part, +0x14):
// collision_is_going_to_be_deleted_event / 06369e60
//             (SUB ECX,0x14 thunk to 06369cc0) / 062ed730 (base, RET 4) /
// (base, -1)
// IVP_Synapse_OO (IVP_Listener_Hull):
// get_type (= 2) / 06369a00 hull_limit_exceeded_event /
// hull_manager_is_going_to_be_deleted_event /
// hull_manager_is_reset (base, RET 8) / 06369b50
//             deleting dtor
// ============================================================================

// 0x10 bytes. Built in place by the `eh vector constructor iterator`
// (the routine(this+0x18, 0x10, 2, the branch, the routine)): the
// constructor the branch stores only the vptr, the fields are filled by
// init_synapse_oo.
class IVP_Synapse_OO : public IVP_Listener_Hull
{
public:
    // +0x04 HullPoolSlot (IVP_Listener_Hull): the slot the object's hull pool
    // returned for this listener.
    IVP_Real_Object* object;   // +0x08
    IVP_OO_Watcher*  watcher;  // +0x0c

    // vptr only.
    IVP_Synapse_OO() {}

    // (+ deleting wrapper the routine).
    virtual ~IVP_Synapse_OO();

    // (RET 8).
    void init_synapse_oo(IVP_OO_Watcher* Watcher, IVP_Real_Object* Object);

    // (ICF-shared `MOV EAX,2; RET`).
    int32_t get_type() override { return 2; }

    // (RET 8): watcher->hull_limit_exceeded_event(); the two
    // arguments are not read.
    void hull_limit_exceeded_event(IVP_Hull_Manager* HullManager, float HullIntrusionValue) override;

    // (RET 4) -> the routine: `if (watcher) watcher->slot4(1)`.
    void hull_manager_is_going_to_be_deleted_event(IVP_Hull_Manager* HullManager) override;

    // Slot 3 is the base's 0628f050 (`RET 8`), not overridden.
};
static_assert(sizeof(IVP_Synapse_OO) == 0x10, "eh vector constructor iterator element size 0x10 (module storage)");

class IVP_OO_Watcher : public IVP_Collision, public IVP_Collision_Delegator
{
public:
    IVP_Synapse_OO synapses[2];             // +0x18 / +0x28
    IVP_U_Vector<IVP_Collision> mindists;   // +0x38: memsize 8, 0x20-byte heap buffer from the ctor

    // (module storage-06369ca6, RET 0xc).
    IVP_OO_Watcher(IVP_Collision_Delegator* Delegator, IVP_Real_Object* Object0, IVP_Real_Object* Object1);

    // (+ deleting wrapper the routine and the secondary-base
    // thunk the branch).
    ~IVP_OO_Watcher() override;

    // (plain RET).
    void hull_limit_exceeded_event();

    // IVP_Collision slot 1: the branch (RET 4).
    void get_objects(IVP_Real_Object* Out[2]) override;

    // IVP_Collision slot 2: the branch (RET 4).
    void get_ledges(const IVP_Compact_Ledge* Out[2]) override;

    // IVP_Collision_Delegator slot 0: the branch (RET 4).
    void collision_is_going_to_be_deleted_event(IVP_Collision* Collision) override;
};
static_assert(sizeof(IVP_OO_Watcher) == 0x40, "operator_new(0x40) at module storage");
