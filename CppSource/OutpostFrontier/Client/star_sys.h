// Author: Giperion.

#pragma once

// baseline Client\space\StarSys.cpp and StarSphere.h -- OutpostFrontier.exe
// .text 0x00552b60..0x00555760. The per-system hub: entering a system (the routine) loads the
// system INI (the routine), keeps the preload lists that pin archetypes, sounds and voice
// lines for the stay, owns the nebula list, the [TexturePanels] shape DB and the background
// star sphere, and ticks nebulae, texture panels, asteroid fields, space dust and the music
// state every frame (the routine).

// baseline globals (all in .data 0x00679870..0x00679978):
// /74/78  std::list<Archetype::Root*> reloaded on leave     (static 005536a0)
// /80/84  std::list<{voice, msg}>     [Archetype] voice =   (static 005534f0)
// /8c/90  std::list<unsigned>         [Archetype] snd =     (static 00553450)
// /98/9c  std::list<Archetype::Root*> [Archetype] preloads  (static 005533b0)
// /a4/a8  std::list<Archetype::Root*> [Object] archetypes   (static 00553060)
// ShapeDB for [TexturePanels]                        (static 00552e40)
// /c8/cc  std::list<Nebula*>                                 (static 00552da0)
// ..f4    music: override block (d0..e4), battle/danger/space ids (ec/f0/f4)
// ..fa    [Ambient] color bytes
// StarSphere background object (0x44 bytes)          (static 00552b60)
// Universe::ISystem* of the current system
// ..70    music state machine
// double: player collisions suppressed until this time (the routine)

// The VC6 std::list globals are modern std::lists here (No baseline ABI); the static
// constructor / atexit destructor pairs baseline emits for them are the lists' own ctors/dtors.

#include "common_archetype.h" // CacheString
#include "client_mesh_load.h" // FClientMeshHolder

#include <list>

struct FNebulaEffect;
class Vector;
struct IObjRW;

namespace Universe
{
    struct ISystem;
}

// ------------------------------------------------------------------------ StarSphere.h

// One background layer: the routine's 0x10-byte ObjectHandle plus the CacheString name at
// +0x10 (baseline stride 0x14, vtable the routine whose only slot the routine resets the
// vptr). A bare ObjectHandle, not an Object3D: its destructor releases nothing.
struct FStarSphereLayer
{
    FMeshHolder Holder;
    CacheString Name;
};

// . Constructor the routine, destructor the routine.
//   Layers[0] (+0x00, name the field) = [Background] nebulae
//   Layers[1] (+0x14, name the field) = [Background] basic_stars
//   Layers[2] (+0x28, name the field) = [Background] complex_stars / file
struct FStarSphereBackground
{
    FStarSphereLayer Layers[3];
    int CurrentQuality; // +0x3c  the field
    int DesiredQuality; // +0x40  the field, written by the star_sphere perf option

    FStarSphereBackground();
    ~FStarSphereBackground();
};

extern FStarSphereBackground G_StarSphereBackground;

// draw the background when [BACKGROUND] and STARS are on (options 0x19/0x1a).
void StarSphereDraw(void* Camera);

// the `star_sphere` perf option (IDS 0x57b): pick the desired quality.
void StarSphereApplyDetail(float Value);

// ------------------------------------------------------------------------ StarSys.cpp

// [Ambient] color. the field -- the working copy the routine and
// compute from it.
extern unsigned char G_StarSysAmbientRgb[3];
extern unsigned char G_StarSysAmbientWorkRgb[3];

// the field.
Universe::ISystem const* StarSysGetSystem();

// the current system's id (ISystem slot 0), 0 when there is none.
unsigned StarSysGetActiveSystemId();

// &the field, the nebula list.
std::list<FNebulaEffect*>& StarSysNebulaList();

// the nebula whose slot +0x20 fog factor at Pos is > 0; Hint is tried first.
// OutFactor (may be null) receives the factor, 0 when none.
FNebulaEffect* StarSysFindNebula(Vector const* Pos, float* OutFactor, FNebulaEffect* Hint);

// the fog colour at Pos: the nebula's slot +0x34 colour * 255 when a nebula
// covers Pos with a factor >= 1, else the system's space_color.
void StarSysGetFogColor(Vector const* Pos, unsigned char* OutRgb);

// &the field, the [System] space_color bytes.
unsigned char const* StarSysSpaceColor();

// ambient bytes + the AMBIENT option (0x3b), clamped at 0xff; &the field.
unsigned char const* StarSysAmbientColor();

// enter the system: bind the field, PhySys, the system INI and the preload
// lists.
void StarSysEnter(unsigned SystemId);

// per-frame tick.
void StarSysUpdate(float Dt);

// is (Voice, Msg) on the [Archetype] voice preload list?
bool StarSysHasVoiceMessage(unsigned VoiceId, unsigned MsgId);
// Acquire the voice message and append it to that list (the body is the
// file-local PreloadVoiceLine). Also called by the NN voice queue the routine (0x00489ac8).
void StarSysPreloadVoiceMessage(unsigned VoiceId, unsigned MsgId);

// music override (the field..e4, fade the field, flag the field).
void StarSysSetMusicOverride(unsigned SpaceMusic, unsigned DangerMusic, unsigned BattleMusic,
                             unsigned OverrideMusic, float Fade, bool bNoFadeFlag);
// drop the music override.
void StarSysClearMusicOverride();

// a hit between A and the object the damage record names; flags battle music
// (the field) when one side is the player and the other is hostile to it.
void StarSysNoteDamage(IObjRW* Victim, void const* DamageRecord);
// flags battle music when the player's target (ship id) is hostile.
void StarSysNoteTargetChange(unsigned ShipId);

// suppress player collision reporting for Seconds from now (the field).
void StarSysSuppressPlayerCollisions(double Seconds);

// leave the current system (the routine passes a null player). Partial: the
// callees that have no port yet are named in the body. baseline callers 0054aa88 / 0054af90
// (the space teardown the routine = DestroySpaceState, space_state.cpp) -- 0054af90 is ported,
// is not.
void StarSysLeave(IObjRW* Player);
// StarSysLeave(nullptr). baseline caller 0054ab23: ShutdownSpaceState (space_state.cpp).
void StarSysLeaveWithoutPlayer();

// (param_1 = Target, param_2 = Viewer) -- Viewer's attitude towards Target
// (Viewer vt+0x68), bucketed: -1 at or below -0.6, 1 at or above 0.6, 0 otherwise and whenever
// either side is null or the query fails. The StarSys callers pass (player, other): the other
// object's attitude towards the player. Callers across the exe (HUD, scanner, StarSys music);
// defined in star_sys.cpp.
struct IObjInspect;
int GetAttitudeClass(IObjInspect const* Target, IObjInspect const* Viewer);
