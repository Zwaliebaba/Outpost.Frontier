// Author: Giperion.

#pragma once

// OutpostFrontier.exe ship.cpp -- baseline Client\space\ship.cpp,
// Action_Land, the IAction a client Ship's action list runs after the
// Ship asked to land on a dock (FClientShipGameobj::RequestLand, the routine). It also rides
// the ship's physics as a PhySys::Controller and flies the ship into the dock for three seconds.

//   struct Action_Land : IAction, PhySys::Controller   sizeof 0x50 (IAction::operator new(0x50)
//                                                          at 0x00543b7f), RTTI .?AUAction_Land@@
//   IAction face    module storage = {the branch get_id "Land", the branch update,
// deleting dtor}
//   Controller face module storage = {Controller thunks 005b734a / 005b7344 / 005b733e,
//                                      IVP defaults 0043d930 (1.0f) / 0053aaa0 (RET 8),
// do_simulation_controller,
// (SUB ECX,8 -> the routine)}

#include "common_cequip.h" // IAction
#include "common_csys.h"   // Matrix
#include "common_physys.h" // PhySys::Controller, IVP_Event_Sim
#include "x86math.h"       // FQuaternion

class FClientGameobj;
class FClientShipGameobj;
struct CEqObj;

struct Action_Land : public IAction, public PhySys::Controller
{
public:
    // (thiscall, RET 8). Ship is the landing Ship gameobj, Dock the dock's object
    // narrowed to a CEqObj ((class & 0x303) == 0x303) by the caller.
    Action_Land(FClientShipGameobj* InShip, CEqObj* Dock);
    // (body) / the routine (deleting): PhySys::Controller's destructor, then
    // IAction's; IAction::operator delete when bit 0 is set.
    ~Action_Land() override;

    // "Land".
    char const* get_id() const override;
    // (RET 4). Always true: the action list never drops it.
    bool update(float Dt) override;

protected:
    // (RET 8) -- the Controller face's slot 4.
    void do_simulation_controller(IVP_Event_Sim* EventSim,
                                  IVP_U_Vector<IVP_Core>* CoreList) override;

private:
    FClientGameobj* Gameobj;   // +0x18  the landing Ship gameobj (the ctor's first argument)
    unsigned BaseId;           // +0x1c  the dock's CEqObj::DockingPointBaseId (+0x164)
    float Elapsed;             // +0x20  0 at construction, + Dt every update
    float Speed;               // +0x24  |velocity| at construction
    bool bNoDockingCamera;     // +0x28  !dock->eqobjarch()->bDockingCamera (+0x81)
    Matrix Orient;             // +0x2c  the dock's launch orientation, columns 0 and 2 negated
};

// OutpostFrontier.exe ship.cpp module storage-00543316: Action_JumpOut, the IAction the Ship's jump
// out (FClientShipGameobj::BeginJumpOut, the routine) puts on its action list. As a
// PhySys::Controller it swings the ship from where it was to the gate's jump-out hardpoint over
// the gate's JumpOutTime, easing in (t squared), then marks itself done; its update keeps the
// controller awake until then and drops the action after.

//   struct Action_JumpOut : IAction, PhySys::Controller   sizeof 0x64 (IAction::operator
//                                                            new(0x64) at 0x005435d2)
//   IAction face    module storage = {the branch get_id "JumpOut", the routine update,
// deleting dtor}
//   Controller face module storage = {Controller thunks 005b734a / 005b7344 / 005b733e,
//                                      IVP defaults 0043d930 / 0053aaa0,
// do_simulation_controller,
// (SUB ECX,8 -> the routine)}
struct Action_JumpOut : public IAction, public PhySys::Controller
{
public:
    // (thiscall, RET 0xc). Duration is the ship effect's JumpOutTime (+4) and
    // GateType the gate solar's get_type (0 without a gate).
    Action_JumpOut(FClientShipGameobj* InShip, float InDuration, unsigned InGateType);
    // (body) / the routine (deleting).
    ~Action_JumpOut() override;

    // "JumpOut".
    char const* get_id() const override;
    // (RET 4). Dt is not read.
    bool update(float Dt) override;

protected:
    // (RET 8) -- the Controller face's slot 4.
    void do_simulation_controller(IVP_Event_Sim* EventSim,
                                  IVP_U_Vector<IVP_Core>* CoreList) override;

private:
    unsigned GateType;              // +0x18  bit 0x800: keep the start orientation (no slerp)
    float Elapsed;                  // +0x1c  0 at construction, + delta_time every sim step
    float Duration;                 // +0x20
    FClientShipGameobj* Ship;       // +0x24
    FQuaternion StartRotation;      // +0x28  the ship's orientation at construction
    Vector StartPosition;           // +0x38  the ship's position at construction
    FQuaternion EndRotation;        // +0x44  the jump effect's +0xa8 orientation (else the ship's)
    Vector EndPosition;             // +0x54  the jump effect's +0xa8 position (else the ship's)
    bool bDone;                     // +0x60  set once Elapsed passes Duration
};

// FClientShipGameobj::RequestLand is declared in client_gameobj.h (the routine).

// ship.cpp's 3DMathEngine, acquired (the routine) on first use; shared by the
// actions above and the Ship draw the branch (0x00546167).
I3DMathEngine* GetShipMathEngine();
