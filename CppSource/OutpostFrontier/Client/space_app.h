// Author: Giperion.

#pragma once

// baseline SpaceApp -- the in-flight client state, one static object at .data 0x00678b98 (handed
// out by the routine). RTTI COL at .rdata 0x005e0550 -> .?AVSpaceApp@@, bases IEnvironment,
// IInputEvent (both on the primary vptr, .rdata 0x005e0554, 15 slots) and Event::Handler at +4
// (vptr .rdata 0x005e0548, one slot: the branch).

// Size 0xbac: the docking camera at +0xa9c is 0x104 bytes and the controller's own words sit at
// +0xba0..+0xbab. Built by the static initializer the branch (the routine on the field,
// then atexit(the branch) -> the routine). Every offset below is a store in the ctor
// or in the listed slot.

// The eight cameras are the baseline classes at their baseline offsets; the routine picks among
// them, the routine installs one into +0xba0.

#include "client_environment.h"
#include "event_handler.h"
#include "space_camera_first.h"
#include "space_camera_third.h"
#include "space_camera_turret.h"
#include "space_camera_rear.h"
#include "space_camera_death.h"
#include "space_camera_script.h"
#include "space_camera_static.h"
#include "space_camera_docking.h"

#include <stddef.h>

// The Event::Handler face at +4. Its one slot is the branch (thiscall, RET 4) with `this` = the
// face, so every controller field it touches is read at face-relative offsets (+0xba0 there is
// the controller's +0xba4, and so on).
class FSpaceAppEventFace : public EventHandler
{
public:
    FSpaceAppEventFace()
    {
    }

    // on +4 -- the last step of the routine.
    ~FSpaceAppEventFace()
    {
    }

    // .
    bool OnEvent(unsigned MsgId) override;
};

// The primary vftable .rdata 0x005e0554 (COL 0x005ef678), fifteen slots -- raw PE dwords; the
// dword after slot 14 is the next vtable's COL 0x005ef664:
//   0 the branch  1 0x004b50e0 (XOR AL,AL / RET 0xc)  2 the branch  3 the branch
//   4 the branch  5 0x00401e90 (RET)  6 0x0053e4a0 (RET 4)  7 0x004b5010 (XOR EAX,EAX / RET)
//   8 0x00401e90  9 the routine  10 0x00401e90  11 the branch  12 the routine
//   13 the routine  14 the branch.
struct SpaceApp : public IEnvironment
{
    // / the routine.
    SpaceApp();
    ~SpaceApp();

    bool Slot00(unsigned A, unsigned Message, unsigned B, unsigned C) override;
    bool Slot01(unsigned A, unsigned B, unsigned C) override;
    bool Slot02(unsigned A, unsigned B, unsigned long Packed) override;
    bool Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed) override;
    bool Slot04(unsigned A, unsigned B, unsigned C) override;
    void Slot05() override;
    void Slot06(unsigned A) override;
    int GetEnvironmentType() override;
    void PreInit() override;
    void Shutdown() override;          // ShutdownSpaceState (space_state.cpp)
    void Slot10() override;
    void Resize(int Width, int Height) override;
    bool Init(int Param) override;     // InitSpaceState
    void Destroy() override;           // DestroySpaceState
    int Update(float DeltaSeconds) override;     // UpdateSpaceState

    //                                      +0x000  vptr .rdata 0x005e0554 (IEnvironment)
    FSpaceAppEventFace Events;              // +0x004  .rdata 0x005e0548
    unsigned char      bCameraReady;        // +0x008  the field -- 1 at 0x0054ab8e, 0 at 0x0054af8d
    unsigned char      bPlayerLostHandled;  // +0x009  the routine already run for a lost ship
    unsigned char      bLeaveRequested;     // +0x00a  the frame returns 6 while set
    unsigned char      bInitInProgress;     // +0x00b  the field (the routine)
    FFirstPersonCamera FirstPerson;         // +0x00c  ctor the routine
    FThirdPersonCamera ThirdPerson;         // +0x1b0  ctor the routine
    FTurretCamera      Turret;              // +0x3c8  ctor the routine
    FRearViewCamera    RearView;            // +0x500  ctor the routine
    FDeathCamera       Death;               // +0x60c  GameCamera ctor + four floats, vt 0x005e061c
    FScriptCamera      Script;              // +0x760  ctor the routine
    FStaticCamera      Static;              // +0x9a4  GameCamera ctor, vt 0x005e0594
    FDockingCamera     Docking;             // +0xa9c  ctor the routine
    FSpaceCamera*      ActiveCamera;        // +0xba0  the field
    int                PlayerPreference;    // +0xba4  0 first person, 1 third (ctor 1)
    // +0xba8 = the field: raised by the routine (a script camera was set up, 0x0054bb43) and
    // read by the routine; the frame drops it when the view returns to "player" (0x0054b1fa),
    // and the routine keeps the camera where it is while it is up.
    unsigned char      bScriptCameraLatch;  // +0xba8
    unsigned char      bSpaceReady;         // +0xba9  1 at 0x0054adf3, dropped once the launch ends
    unsigned char      bDockingPending;     // +0xbaa
    unsigned char      bDockingActive;      // +0xbab

private:
    SpaceApp(SpaceApp const&) = delete;
    SpaceApp& operator=(SpaceApp const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(SpaceApp) == 0xbac, "SpaceApp is 0xbac");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Events) == 0x004, "SpaceApp +0x004");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, bCameraReady) == 0x008, "SpaceApp +0x008");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, bInitInProgress) == 0x00b, "SpaceApp +0x00b");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, FirstPerson) == 0x00c, "SpaceApp +0x00c");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, ThirdPerson) == 0x1b0, "SpaceApp +0x1b0");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Turret) == 0x3c8, "SpaceApp +0x3c8");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, RearView) == 0x500, "SpaceApp +0x500");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Death) == 0x60c, "SpaceApp +0x60c");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Script) == 0x760, "SpaceApp +0x760");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Static) == 0x9a4, "SpaceApp +0x9a4");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, Docking) == 0xa9c, "SpaceApp +0xa9c");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, ActiveCamera) == 0xba0, "SpaceApp +0xba0");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, PlayerPreference) == 0xba4, "SpaceApp +0xba4");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, bScriptCameraLatch) == 0xba8, "SpaceApp +0xba8");
static_assert(sizeof(void*) != 4 || offsetof(SpaceApp, bDockingActive) == 0xbab, "SpaceApp +0xbab");

// (space_state.cpp).
extern SpaceApp G_SpaceApp;

// (thiscall on the controller, no arguments): read cameras.ini and hand every
// entry of [WinCamera] (the Win3D camera the field), [CockpitCamera], [ThirdPersonCamera],
// [TurretCamera], [RearViewCamera] and [DeathCamera] to that camera's slot 0x4c. Caller: the
// space init (0x0054ad86).
void SpaceAppLoadCamerasIni(SpaceApp* App);

// (cdecl, no arguments): the player's ship was destroyed -- arm the death camera
// (+0x60c) from the listener camera's transform and switch to it. Caller: the IClient
// DESTROYOBJECT handler (0x0053bcbc).
void SpaceAppArmDeathCamera();

// (cdecl, one argument): the static camera's Csys (+0x9a4 +0x04, twelve floats)
// and view-dirty byte, then "static" when the controller is up. Caller: IClient 0x0053e154.
void SetStaticCameraTransform(const float* Csys12);
