// Author: Giperion.

#pragma once

#include "client_environment.h"

// baseline in-space client state singleton the routine / the field -- SpaceApp (space_app.h),
// an IEnvironment. Its callers (the state machine the routine, InitializeEngine 0x005b1e9c,
// the teardown 0x005b21ea) only send it IEnvironment slots.
struct SpaceApp;

IEnvironment* GetSpaceState();
// slot 12 (+0x30), thiscall RET 4. True when the space init finished with no
// next state pending (the field == 0).
bool InitSpaceState(SpaceApp* State, int Param);
int UpdateSpaceState(SpaceApp* State, float DeltaSeconds);
void DestroySpaceState(SpaceApp* State);
// slot 9 (+0x24) of the space state vftable .rdata 0x005e0554: the routine,
// this->slot 13 (DestroySpaceState), CObject::ReleaseAll, JMP the routine. Caller: the client
// atexit teardown (0x005b21ea..0x005b21f3, on the the routine singleton).
void ShutdownSpaceState(SpaceApp* State);

// store the base a landing ship asked for (the field). Callers: the Ship's
// land request (0x00543d12) and the Land action's update (0x00542ddd).
void SetPendingLandBase(unsigned const* BaseId);

// store the system a jumping ship asked for (the field) and the byte argument
// (the field). Callers: the Ship's jump state 3 (0x005439d6) and the IClient
// system-switch-out handler's helper the routine (0x0054378a); both pass false. The space frame
// consumes it (the routine).
void SetPendingJumpSystem(unsigned const* SystemId, bool bFlag);

// (cdecl) -- the player's ship landed on a dock the ObjDB knows: the base recorded
// (the routine), the routine(0), and the space frame's next state set to 4. Caller: the
// IClient land handler (0x0053d782) with &dock CEqObj::DockingPointBaseId (+0x164).
void EnterBaseFromDock(unsigned const* BaseId);

