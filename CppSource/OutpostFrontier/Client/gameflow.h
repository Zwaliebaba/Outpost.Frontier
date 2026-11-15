// Author: Giperion.

#pragma once

// GF -- the client state for being docked at a base (RTTI .?AUGF@@, COL 0x005e8734), and the
// Support-file functions it runs.

// The state object is the field (the routine returns its address), struct GF :
// IEnvironment (client_environment.h), vftable 0x005cae24, source
// Client\gf\Gameflow.cpp (FDUMP at line 0xa7). Layout: the vptr, then
// three bytes -- +0x04 "first frame pending" (Init sets it, the first Update clears it), +0x05
// "base entered", +0x06 the Init argument. The main loop's state machine (the routine,
// client_state.cpp) enters it for code 3 (a base is set) and code 7 on a base, sends it slot 10
// for code 4, and slot 13 / slot 14 like every state; InitializeEngine sends it slot 8
// (0x005b1f47..0x005b1f50) and the atexit teardown slot 9 (0x005b21de..0x005b21e7).

// Its slot bodies call into Client\supt\Support.cpp (the viewport at
// names it, line 0x222), OutpostFrontier.exe 0x00558870..0x005592aa: entering and leaving
// a base (the routine / the routine), the base frame (the routine) and its tick
// (the routine), and the mouse tracking over room hotspots (gf_hotspot.h).

#include "client_environment.h"

class FHotspot;

// &the field, the GF state object. Its slots: PreInit (8, the branch)
// = the routine with the field, then +0x04 = +0x05 = 0; Shutdown (9, the branch) = JMP
// ; the rest gameflow.cpp.
IEnvironment* GetGameflowState();

// (cdecl, one ignored argument) -- IDPMsgHandler::OnDisconnect (a bare RET),
// (the LocationController maps), the routine (the CharacterInfo database), JMP
// (its preload scripts).
void InitGameflowData();

// (cdecl) -- the routine (CharacterInfo emptied), the routine on the field
// (the location maps), the routine (the RoomObject list), the routine on the field and
// JMP the routine (the hotspot leak check, whose answer is the return value).
bool ShutdownGameflow();

// (thiscall) on the field, the engine-instance table: 256 engine handles
// (+0x000, -1 = none), 256 material-cache entries (+0x400) and a count (+0x800,
// ). Each of the Count entries has its material slot released (the routine) and
// its handle given back (IEngine slot 15), then every slot is reset to -1 / null and Count to 0.
// Nothing in OutpostFrontier.exe ever adds an entry (raw PE scan of 0x00667e50 / 0x00668250 /
// only the static initializer the branch, its atexit the branch and the three
// callers), so in practice this only rewrites the initial image. Callers: 0x0054aeda (space
// exit), 0x00558884 (ShutdownGameflow), 0x00558a8f (LeaveGameflowBase).
void ResetEngineInstanceCache();

// (cdecl) -- enter a base. LocationId 0 goes through the routine (the base's own
// start location); otherwise the routine + the routine + the routine on it. bFromIntro is
// GF's +0x06: when clear the arrival caption is shown. Returns true.
bool EnterGameflowBase(unsigned BaseId, unsigned LocationId, bool bFromIntro);

// cut to black (the routine(1.0)) and make the base frame answer 8 (the
// MissionBriefing state, the routine). Callers: 0x00440fc0, 0x00447d62, 0x00448283,
// .
void RequestGameflowMissionBriefing();

// (cdecl) -- leave the base (GF slot 13).
void LeaveGameflowBase();

// the field = frame count + 2: the base frame passes a zero frame time until
// the frame counter has moved past it. Callers: 0x0044988c, 0x00455097, 0x00455548.
void HoldGameflowTime();

// the arrival caption's delay in milliseconds. The Gf script sequence's
// (0x00454f0e) stores each added script's duration there.
void SetGameflowArrivalDelayMs(unsigned DelayMs);

// make the next base frame answer 5 (once). Caller: 0x00447d58.
void RequestGameflowStateFive();

// (cdecl) -- the field = BaseId. Callers: the routine, the routine.
// is the MissionBriefing state's +0x10: its update the branch passes it to
// (0x0043d440, a LEA, not a MOV read).
void SetGameflowLocationBase(unsigned BaseId);
// The MissionBriefing state's read of the field (mission_briefing.cpp).
unsigned GetGameflowLocationBase();

// The MissionBriefing state object the field (the routine) keeps two bytes the nav map
// reads and writes through the accessors below: +0x0c (the field) is 1 while its slot 14
// (the branch, 0x0043d45c) builds the NeuroNetNavMap, +0x0d (the field) is the
// "continue" request. The state itself is mission_briefing.cpp; these three members of it
// (+0x0c, +0x0d, +0x10) stay globals here, where their accessors already were.
extern unsigned char G_bMissionBriefingNavMapUp;    // (state +0x0c)
extern unsigned char G_bMissionBriefingContinue;    // (state +0x0d)
// (cdecl) -- MOV AL,[the field]. Caller: NeuroNetNavMap::Create 0x0048ebac.
unsigned char IsMissionBriefingNavMapUp();
// (cdecl) -- the field = 1. Caller: NeuroNetNavMap::OnEvent 0x0049e1a4.
void RequestMissionBriefingContinue();

// (cdecl) -- with a room camera: the routine (forget the letterbox camera) and
// (camera, 0). Callers: GF slot 11, 0x004b1caf.
void ResetGameflowRoomLetterbox();

// the hotspot tracking reset: the field = 0, the field = 0,
// = 0.
void ResetHotspotTracking();
// the field = 0 (the hovered hotspot forgotten, no leave event). Caller:
// .
void ClearHoveredHotspot();
// (cdecl) -- the same, only when Hotspot is the hovered one. Callers: the Hotspot
// destructors the routine / the routine.
void ClearHoveredHotspotIf(FHotspot* Hotspot);
// stop tracking: the hovered hotspot (if any) gets a leave with point {0, 0}.
void ReleaseHoveredHotspot();
// / the routine -- the field = 1 / 0: while it is set the tracking ignores the
// mouse. Callers: 0x00455497 / 0x00449896, 0x004555b6.
void LockHotspotInput();
void UnlockHotspotInput();
