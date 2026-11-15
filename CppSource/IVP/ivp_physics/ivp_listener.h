// Author: Giperion.

#pragma once

// ============================================================================
// IVP listener / collision-filter / performance-counter interfaces
// (2026-10-08, claude-code:cmn-physys).

// baseline compiles these IVP SDK interface classes into Common.dll. Their names
// are RTTI-confirmed through the Complete Object Locators at vtable[-4] of the
// game classes that derive from them:
//   PhysicalSystem (COL module storage / 063a73e0 / 063a7330) lists
//     IVP_Listener_Object at +0, IVP_Listener_Collision at +4,
//     IVP_Collision_Filter_Coll_Group_Ident and IVP_Collision_Filter at +0xc;
//   PhySys::PhyPerfCounter (COL module storage) lists IVP_PerformanceCounter.
// Slot order and arity are read off each class's own vtable in .rdata and off
// the overriding bodies. Slots whose meaning no traced caller fixes keep a
// SlotNN name.
// ============================================================================

#include <cstdint>

class IVP_Environment;
class IVP_Real_Object;
class IVP_Compact_Edge;

// The event record IVP passes to IVP_Listener_Object slots: {environment,
// real object}. Declared in ivp_environment.h.
struct IVP_Environment_Object_Notify_Event;

// What an IVP_Event_Collision points at. Only the members a traced consumer
// reads are named; the object is larger and not modelled further here.
// Consumer: PhysicalSystem::event_pre_collision (module storage..0628c547).
struct IVP_Contact_Situation
{
    float surf_normal[4];                     // +0x00 (x, y, z read)
    float speed[4];                           // +0x10 (x, y, z read; written at module storage..06358aa4)
    float contact_point_ws[4];                // +0x20 (x, y, z read)
    IVP_Real_Object* objects[2];              // +0x30 / +0x34
    const IVP_Compact_Edge* compact_edges[2]; // +0x38 / +0x3c
};

// Built on the stack by IVP right before it fires the collision listeners
// (module storage..06358aa7 -> the routine).
struct IVP_Event_Collision
{
    float d_time_since_last_collision;        // +0x00
    IVP_Environment* environment;             // +0x04
    IVP_Contact_Situation* contact_situation; // +0x08
};

// vtable module storage: four _purecall slots + deleting dtor the branch.
// IVP fires the slots through env->object_listeners (+0x124):
// slot 0, the routine slot 1, the routine slot 2, the routine slot 3.
class IVP_Listener_Object
{
public:
    virtual void event_object_deleted(IVP_Environment_Object_Notify_Event* Event) = 0; // slot 0
    virtual void event_object_created(IVP_Environment_Object_Notify_Event* Event) = 0; // slot 1
    virtual void event_object_revived(IVP_Environment_Object_Notify_Event* Event) = 0; // slot 2
    virtual void event_object_frozen(IVP_Environment_Object_Notify_Event* Event) = 0;  // slot 3
    virtual ~IVP_Listener_Object() {}                                                  // slot 4
};

// IVP_Listener_Collision::enabled_callbacks bit gated by the routine
// (`TEST byte ptr [ECX+4],1` before it calls slot 0).
enum : int32_t
{
    IVP_LISTENER_COLLISION_CALLBACK_PRE_COLLISION = 0x01,
};

// vtable module storage: four `RET 4` no-op slots (module storage) + deleting
// dtor the branch; one data member at +4. IVP keeps these in
// env->vector_listeners_c8 (the routine appends, the routine fires slot 0).
class IVP_Listener_Collision
{
public:
    explicit IVP_Listener_Collision(int32_t InEnabledCallbacks)
        : enabled_callbacks(InEnabledCallbacks)
    {
    }

    virtual void event_pre_collision(IVP_Event_Collision* /*Event*/) {}  // slot 0
    // slot 1: fired by IVP_Cluster_Manager 0634b090 (enabled_callbacks & 2)
    // from ~IVP_Real_Object (module storage-06345381) with the object being
    // deleted. cmn-phy7 2026-10-08 (was event_post_collision(IVP_Event_Collision*)).
    virtual void event_collision_object_deleted(IVP_Real_Object* /*Object*/) {}
    virtual void Slot02(void* /*Arg*/) {}                                 // slot 2 (enabled_callbacks & 4): 0634b0e0, 0634cfa0
    virtual void Slot03(void* /*Arg*/) {}                                 // slot 3 (enabled_callbacks & 4): 0634b140, 0634cfe0
    virtual ~IVP_Listener_Collision() {}                              // slot 4

    int32_t enabled_callbacks; // +0x04
};

// ============================================================================
// IVP_Listener_Hull -- what an IVP_Hull_Manager's priority pool holds as entry
// payloads (cmn-phy5, 2026-10-08). Library name (Ipion). Abstract base vtable
// , 4 slots: three _purecall + 0628f050 (`RET 8`), installed by
// IVP_OV_Element's ctor (:0635c024) before its own table. The hull manager
// dispatches slot 1 when the hull value passes an entry's key
// (the routine / the routine, IVP_Calc_Next_PSI_Solver), slot 2 when it is
// destroyed (the routine) and slot 3 when it rebases (the routine, args
// (-outer, -inner)). Implementations: IVP_OV_Element (vtable 063a5680) and the
// mindist synapses (vtable 063a55cc). No virtual destructor at this level.
// +0x04 is the entry's pool slot index, the full dword the insert returned.
// ============================================================================
class IVP_Hull_Manager;
class IVP_Listener_Hull
{
public:
    virtual int32_t get_type() = 0;                                                        // slot 0
    virtual void hull_limit_exceeded_event(IVP_Hull_Manager* HullManager, float HullIntrusionValue) = 0; // slot 1 (RET 8)
    virtual void hull_manager_is_going_to_be_deleted_event(IVP_Hull_Manager* HullManager) = 0;  // slot 2 (RET 4)
    virtual void hull_manager_is_reset(float /*DeltaTime*/, float /*CenterDeltaTime*/) {}   // slot 3, 0628f050 `RET 8`

    uint32_t HullPoolSlot; // +0x04

protected:
    IVP_Listener_Hull() {}
    ~IVP_Listener_Hull() {}
};

// The record IVP_Environment's PSI broadcast passes (the routine): a one-dword
// stack local holding the environment (:0634d02e `MOV [ESP+8],EDI`, its
// address pushed at :0634d040-:0634d044). cmn-phy5 2026-10-08.
struct IVP_Event_PSI
{
    IVP_Environment* environment; // +0x00
};

// Elements of IVP_Environment::vector_listeners_d0 (+0xd0). The only call
// traced is slot 0 with an IVP_Event_PSI* (the routine :0634d03e-:0634d045,
// `MOV EDX,[ECX]; PUSH EAX; CALL [EDX]`); no implementation was traced, so no
// further slot is declared. Library name IVP_Listener_PSI. cmn-phy5 2026-10-08.
class IVP_Listener_PSI
{
public:
    virtual void event_PSI(IVP_Event_PSI* Event) = 0; // slot 0
    // slot 1: ~IVP_Environment (module storage-0634c746) calls it on every
    // element, last to first, with the environment (cmn-phy7 2026-10-08).
    virtual void environment_will_be_deleted(IVP_Environment* Environment) = 0;
};

// vtable module storage: two _purecall slots + deleting dtor the routine
// (stamps 0639c40c through the routine).
class IVP_Collision_Filter
{
public:
    // slot 0: nonzero (IVP_BOOL) = the pair may be collision-tested.
    virtual int32_t check_objects_for_collision_detection(IVP_Real_Object* A, IVP_Real_Object* B) = 0;
    // slot 1
    virtual void environment_will_be_deleted(IVP_Environment* Env) = 0;
    // slot 2
    virtual ~IVP_Collision_Filter() {}
};

// vtable module storage, ctor the routine, 8 bytes.
class IVP_Collision_Filter_Coll_Group_Ident : public IVP_Collision_Filter
{
public:
    // (thiscall, RET 4): vtable 0639c3fc, +0x04 = the argument.
    explicit IVP_Collision_Filter_Coll_Group_Ident(int32_t bInDeleteOnEnvDelete);

    // (RET 8)
    virtual int32_t check_objects_for_collision_detection(IVP_Real_Object* A, IVP_Real_Object* B) override;
    // (RET 4)
    virtual void environment_will_be_deleted(IVP_Environment* Env) override;
    // (stamps 0639c3fc through the routine, then the base's).
    virtual ~IVP_Collision_Filter_Coll_Group_Ident() {}

    int32_t bDeleteOnEnvDelete; // +0x04
};

// vtable module storage: five _purecall slots + deleting dtor the branch.
// Arities from PhySys::PhyPerfCounter's overrides (vtable 0639baac):
// RET / RET 4 / RET / RET 4 / RET 8.
class IVP_PerformanceCounter
{
public:
    virtual void Slot00() = 0;                                                  // slot 0
    virtual void Slot01(int32_t Arg) = 0;                                       // slot 1
    virtual void Slot02() = 0;                                                  // slot 2
    // slot 3: PhyPerfCounter's body (the branch) deletes the counter.
    virtual void environment_is_going_to_be_deleted(IVP_Environment* Env) = 0;
    // slot 4: 8 bytes of stack arguments; their type is not derived.
    virtual void Slot04(float Arg0, float Arg1) = 0;
    virtual ~IVP_PerformanceCounter() {}                                        // slot 5
};
