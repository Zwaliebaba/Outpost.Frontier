// Author: Giperion.

#pragma once

// Content.dll TU18 (BasePop) / TU52 (SpacePop) entry points used by other Content
// translation units. The SpacePop flag/sphere accessors are declared in
// ContentInternal.h.

#include "NpcBase.h"          // FSpaceObjPlacement, Vector

#include <vector>

class FContentPlayerShip;     // ContentPlayerState.h (TU46)
struct FContentNpcShipParams; // NpcShip.h (TU43)
struct FSystemInfo;           // SystemInfo.h (TU58)
struct FSystemZone;           // SystemInfo.h (TU64)

// One exclusion sphere of SpacePop (16 bytes; the routine adds, the routine removes).
struct FSpacePopExclusionSphere
{
    float X;
    float Y;
    float Z;
    float Radius;
};

// (allocator byte; first/last/end at the field/5c/60): SpacePop's exclusion
// spheres; static init the branch, atexit the routine.
extern std::vector<FSpacePopExclusionSphere> GSpacePopExclusionSpheres;

// The population request SpacePop hands to the zone populations (TU30 the routine /
// ) and through them to the encounter spawner (TU29 the routine), baseline 0x5c
// bytes on the stack of its builders. Ctor TU53 the routine (RET 0x50; all five callers are
// SpacePop: 06f5b7fc, 06f5b8ed, 06f5ba54, 06f5c0c0, 06f5c2aa); the dtor (inlined, and the EH
// funclet the routine) frees the sphere vector.
struct FSpawnRequest
{
    // +0x00: 1 for the population around a ship that just launched / jumped in / left a trade
    // lane (the routine), 0 for the heartbeat population (the routine). With it set the
    // spawner takes every encounter, SuppressHails the created ships and, in single player,
    // sets the first block's +0x108 (TU29 06ef4f18).
    bool bArrival;
    // +0x04: which zone list of the routine the heartbeat request serves: 0 the zones at the
    // ship, 1 the additive zones within 2000, 2 the zones ahead (the routine); 0 for arrivals.
    // Read by the TU53 trade-lane spawn placement (SpawnPlacement.cpp): 0 searches the lanes
    // near the ship when its zone is a trade lane and avoids the end rings, non-zero always
    // takes a middle ring, 2 drops the 3750 ring offset.
    unsigned int ZoneList;
    unsigned int ControllerId;              // +0x08 GSpacePopControllerId
    FContentPlayerShip* PlayerShip;         // +0x0c the ship the population is for
    FSpaceObjPlacement Placement;           // +0x10 orientation, position at +0x34
    Vector Velocity;                        // +0x40 the ship's linear velocity (0 = none)
    // +0x4c: a copy of GSpacePopExclusionSpheres (the ctor's vector assignment).
    std::vector<FSpacePopExclusionSphere> ExclusionSpheres;

    // the identity placement is written first and then overwritten by the
    // by-value argument (06f63a9f-06f63b05).
    FSpawnRequest(bool bInArrival, unsigned int InZoneList, unsigned int InControllerId,
                  FContentPlayerShip* InPlayerShip, FSpaceObjPlacement const& InPlacement,
                  Vector const& InVelocity,
                  std::vector<FSpacePopExclusionSphere> const* InExclusionSpheres);
};

static_assert(sizeof(FSpaceObjPlacement) == 0x30, "FSpawnRequest +0x10 block");

// true when the BasePop instance (the field) tracks room NPCs for this
// player. baseline caller: BaseSupport 06eb4dc9.
bool __cdecl BasePopHasPlayer(unsigned int PlayerId);

// SpacePop's controller ID, written by the SpacePop ctor the routine.
// Readers: the routine (06f5a697) and TU43 the routine (an NPC ship owned by SpacePop:
// ship +0x170 == this id).
extern unsigned int GSpacePopControllerId;

// the random-population attack clamp. baseline has no accessor function: the
// byte is read and written in place -- set by the Mission_01a/01b ctors (06ea234f, 06ea2470),
// TU21 06eb8181 / 06eba17e / 06ebb161 and Act_RpopAttClamp (06ece7a3); read by TU21 06ebabde
// (save "att_clamp"), SpacePop the routine and TU56 06f704a1.
extern unsigned char GSpacePopAttackClamp;

// (1 in .data): the routine stores the byte (callers TU21 06eb81a2 /
// and the Act slot the branch of vtable 06fb5e20, next to the Act_HostileClamp
// ctor 06ecaf90), the routine returns it (06ebac4d, TU29 06ef5592). The name follows
// that Act; unconfirmed.
void __cdecl SpacePopSetHostileClamp(unsigned char EnabledByte);
unsigned char __cdecl SpacePopGetHostileClamp();
// (1 in .data), the system's rpop_solar_detection: the routine stores the
// byte in single player only (SystemInfo 06f759a7 / 06f75ca6); the routine returns it in
// single player and 1 otherwise (TU53 06f60e1a, 06f61c09, 06f62059).
void __cdecl SpacePopSetSolarDetection(unsigned char EnabledByte);
unsigned char __cdecl SpacePopGetSolarDetection();

// (cdecl; caller TU29 06ef51dc, once per created block): with the attack clamp
// off only when the player's flags have bit 2: the ship's block and every NPC
// ship / player ship of the system that belongs to the other side (SpacePop vs not) get their
// mutual attitudes raised to at least -0.59 (the field).
void __cdecl SpacePopClampBlockAttitudes(FContentPlayerShip* PlayerShip,
                                         FContentNpcShipParams const* Params);
// (cdecl; callers 06f5ada7, 06f5b5c8, TU58 06f77998): the populated zones around
// Placement. OutAtShip = the zones within 10 of the position that have a patrol path plus
// the smallest one without (inserted at a random place); *OutBlocked = one of them has
// density 0 (then the other two lists stay empty). OutNear = the population_additive zones
// within 2000; OutAhead = the population_additive zones of the 70x55 degree, 4000 deep view
// frustum whose surface is within 4000.
void __cdecl CollectSpacePopZones(FSystemInfo* System, FSpaceObjPlacement const* Placement,
                                  std::vector<FSystemZone*>* OutAtShip,
                                  std::vector<FSystemZone*>* OutNear,
                                  std::vector<FSystemZone*>* OutAhead, bool* OutBlocked);
