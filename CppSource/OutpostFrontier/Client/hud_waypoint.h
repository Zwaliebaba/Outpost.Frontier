// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// HUD_Waypoint / [Waypoint] -- the waypoint the player flies to: either one of the player's own
// waypoints (the list the field, player_spawn.h) or a point of the current best path
// (the field, best_path.h). The point is a client-side Solar ("waypoint" archetype, nickname
// "waypoint_target<n>") that the element spawns, targets through command 0x1c, spins and
// scales, and replaces with the next point once the ship gets within 150 m.

// RTTI HUD_Waypoint : HUDItem @0, WinControl @0x38 (IDataViewOwner @0xb4). Size 0x390
// (HUDFACTORY Create the routine = operator new(0x390) + the routine).
// vftables:
//   HUDItem face 0x005d9784 -- overrides 0 the routine (scalar deleting dtor around
// ) and 6 the branch Render.
//   WinControl face 0x005d96cc -- overrides 0 the branch (thunk, ECX -= 0x38), 9 the branch
//     Create (shared with HUD_DamageIndicator), 11 the branch Update, 12 the routine
//     UpdateInactive (ICF with HUD_CruiseProgress::UpdateInactive).

// Factory: the static HUDFACTORY<HUD_Waypoint> the field ("Waypoint"; XC entry
// , ctor the routine, atexit the branch -> the routine), vftable 0x005d97e0.

// is the live instance (ctor 0x004f3b91, dtor 0x004f3d16). It is read by
// , HUD_Comm's the routine (0x004cefc0) and the other getters' callers.

// Named gaps (see the .cpp):
//   * best-path waypoints: the list is read faithfully, but its writers are not ported, and
//     the record resolvers the routine / the routine are holes (best_path.h).
// -------------------------------------------------------------------------------------------

#include "hud_element_list.h"
#include "wincontrol_base.h"

#include "common_goods.h" // BaseWatcher
#include "common_cobject.h" // CEqObj::CreateParms (EqObjCreateParmsDefaults)

class INI_Reader;
class Vector;
struct IObjRW;

class HUD_Waypoint : public HUDItem, public WinControl
{
public:
    // (thiscall, RET 4) -- the reader is not read.
    explicit HUD_Waypoint(INI_Reader& Reader);
    // (scalar deleting the routine; WinControl-face thunk the branch)
    ~HUD_Waypoint() override;

    // HUDItem slot 6 the branch (RET 8) -- keep, advance or drop the waypoint, spin it.
    void Render(FSpaceCamera* Camera, float DeltaSeconds) override;

    // WinControl slot 9 the branch (RET 8) -- SetOffset(0, 0, -1), SetPosition(Position),
    // Win3D::Create.
    bool Create(Win3D* NewParent, void* Data) override;
    // WinControl slot 11 the branch -- shown only with the cursor drawn, the HUD options on
    // and a player ship; the child walk only then.
    void Update() override;
    // WinControl slot 12 the routine -- the show half of Update, then Win3D's inactive walk.
    void UpdateInactive() override;

    // (thiscall, RET 0x10) -- make Pos the waypoint: bUser / Index / Id say which
    // list entry it is. The same entry at (within 2 m) the same place only re-targets it;
    // otherwise the old waypoint Solar is released and a new one is spawned and targeted.
    // Callers: Render 0x004f4116, HUD_Comm 0x004cefc6 (the routine), 0x004cef1d,
    // , 0x004d010a.
    void SetWaypoint(Vector const* Pos, bool bUser, int Index, int Id);
    // (thiscall, RET 8) -- with a live waypoint: its list and index, true.
    // Caller 0x00475a2e.
    bool GetWaypointEntry(unsigned char* OutUser, int* OutIndex);
    // (thiscall, RET 4) -- a live waypoint within 2 m of Pos on every axis.
    // Callers: HUD_Comm 0x004d4171, 0x004d41dd, 0x004d4245, 0x004d429e.
    bool IsWaypointAt(Vector const* Pos);
    // FLD [this+0x37c]. Caller 0x00548347.
    float GetScale() const;
    // the waypoint Solar's IObjRW, or null. Callers 0x004c83c6, 0x004c83ee.
    IObjRW* GetWaypointObject() const;
    // MOV AL,[this+0x38c]. Caller 0x0054833a.
    bool IsActive() const;

    BaseWatcher Waypoint;          // +0x364 the waypoint Solar (its Watchable, IObjRW + 8)
    int SpawnCounter;              // +0x36c -1; "waypoint_target%d" uses it, then ++
    float SpinAngle;               // +0x370 degrees, += 250 * Dt in Render
    unsigned char bUser;           // +0x374 1: a player waypoint, 0: a best-path point
    unsigned char Pad375[3];
    int Index;                     // +0x378 the entry's index in its list
    float Scale;                   // +0x37c 1.0; 0 inside 15 m, Dist / 100 inside 100 m
    unsigned char bReached;        // +0x380 the "objective_reached" latch
    unsigned char Pad381[3];
    int Id;                        // +0x384 -1; the player waypoint's AutoId
    int Field388;                  // +0x388 0; not read in this file
    unsigned char bActive;         // +0x38c the waypoint is targeted
    unsigned char Pad38d[3];

private:
    HUD_Waypoint(HUD_Waypoint const&) = delete;
    HUD_Waypoint& operator=(HUD_Waypoint const&) = delete;
};

// the live HUD_Waypoint, or null.
HUD_Waypoint* GetHudWaypoint();

// (cdecl) -- the player's waypoint list was emptied: with a live, active waypoint
// and a player ship, clear the ship's target (IObjAffect::set_target(0, SUBOBJ_ID_NONE,
// player connection)). Callers: the routine (tail JMP, player_spawn.cpp) and 0x004c4aaa.
void HudWaypointOnPlayerWaypointsCleared();

// (cdecl) -- 1 when the waypoint has a Solar, else 0 (NEG / SBB / NEG on
// ; the field is not tested). Caller: the space draw 0x0051c4f1.
int HudWaypointGetObjectCount();
// (cdecl) -- object Index of that list: the waypoint Solar for 0, null otherwise.
// Caller: the space draw 0x0051c501.
IObjRW* HudWaypointGetObject(int ObjectIndex);

// (thiscall, returns this) -- OutpostFrontier.exe's own compiled default constructor of
// CEqObj::CreateParms (0xb8 bytes in baseline): the CSimple part (+0x0..+0x8 = 0, +0xc = -1.0f,
// +0x10..+0x18 = 0, the routine identity at +0x1c, +0x40..+0x54 = 0), +0x58 / +0x5c = 0,
// +0x60 = -1, +0x64..+0x70 = 0, +0x94 / +0x98 = 0, the +0x9c equipment vector empty and the
// +0xac collision-group list with a fresh self-linked head. +0x74..+0x90 are not written.
// The port runs it on a freshly constructed Parms: the two containers are the port's std
// containers, which their own constructors leave empty, as baseline's inline construction does.
// Callers: HUD_Waypoint::SetWaypoint 0x004f44d3 and the multiplayer CREATESOLAR build
// (the routine).
void EqObjCreateParmsDefaults(CEqObj::CreateParms& Parms);

// (cdecl) -- make best-path point Index the waypoint: target the object its
// record names, else put the waypoint at the record's position. baseline keeps it with HUD_Comm
// (0x004cef30); Render calls it with 0 when a best-path waypoint is lost.
void HudWaypointSelectBestPath(int Index);

// (cdecl) -- make the player's waypoint Index the waypoint: a waypoint on an object
// sends "select target" (0x1c) for it; otherwise HUD_Waypoint::SetWaypoint(&Pos, true, Index,
// AutoId). baseline keeps it with HUD_Comm (0x004ceeb0). Callers: 0x00494017,
// HUD_Targetable_Objects::PickUnderCursor 0x004edbf7 / 0x004ede8a.
void HudWaypointSelectPlayerWaypoint(int Index);
