// Author: Giperion.

#pragma once

// baseline space-object mesh draw walk (camera +0x78 → Slot7).
// Listing anchors: the routine, the routine, the routine, the routine,
// plus same-pass drain the routine / the routine (required to close the queue).

// LOD: Gameobj vtable +0x17c = the routine (not data fields at those offsets).

struct FSpaceCamera;
struct IObjInspect;
struct I3DMathEngine;
class FClientGameobj;
class FClientEquippedObj;

// Gameobj.cpp's math engine, acquired (the routine) on first use.
I3DMathEngine* GameobjMathEngine();

struct FGameobjEffectList; // client_gameobj.h
// (thiscall on GameObject+0x28) -- sweep the effect list: dead companions are
// cleared and their nodes freed, live particles re-armed. Callers: the gameobj draw submit
// (0x005399d2, inside this file) and the dynamic asteroid draw the routine (0x005265ea).
void GameobjSweepEffectListbaseline(FGameobjEffectList* List);

// GameObject's vt+0x17c body (the LOD pick), called by FClientGameobj::Slot17c
// and, non-virtually, by EquippedObj's override the routine. *OutLod: low word = the camera's
// ClassifySphere class (vt+0x3c) of the object's Slot178 sphere, high word 0xffff below class 3,
// else 0 / 1 / 2 by the companion's +0x08 / +0x0c distances.
unsigned* GameobjPickLodbaseline(FClientGameobj* Self, unsigned* OutLod, void* Camera,
                               unsigned const* CachedLod);

// GameObject's vt+0x184 body (per-object lighting): own +0x178 radius,
// get_type & 0x807, 1 - perf against 0.999, then the routine(position, radius, Camera, +0x38,
// +0x3c, flag). For FClientGameobj::Slot184 to call.
void GameobjUpdateDrawLightingbaseline(FClientGameobj* Self, void* Camera);

// (cdecl) -- 1.0 - the generic effect perf (the field), left unrounded on the
// FPU. Callers include the routine and the first-person cockpit pass 0x0051852b.
double SpaceEffectPerfOneMinus();

// (cdecl, 6 args) -- the StarSys per-object lighting: ambient (with the containing
// nebula's lerp and fog), then the nearby lights. Callers 0x00518574 (cockpit), 0x005227c1
// (asteroid ambient), 0x00539bb1 (the routine).
void SpaceApplyObjectLighting(float const* Pos3, float Radius, void* Camera, unsigned DrawStateA,
                              unsigned DrawStateB, unsigned char Flag);

// (thiscall on the camera, RET 8) -- ZNear (+0xa4) and ZFar (+0xa8) written.
void SpaceCameraSetNearFar(FSpaceCamera* Cam, float ZNear, float ZFar);

// GameObject's vt+0x188 body (the draw submit), for FClientGameobj::Slot188:
// Slot180, the Slot17c LOD pick written back over CachedLod, the gate (low word < 3 or high
// word >= 2 returns the LOD), the effect-list sweep the routine (high word < 1), Slot184, the
// player flags, the IFLAppearance envmap arm, IRenderer2::Slot7(Camera, get_index, 1.0f, Flags,
// Matrix, Extra), FlushAll(1) on a result >= 3; *OutLod = {result, 0xffff}. The draw drain
// reaches it through the node slot the branch (vt+0x188(&scratch, Cam, node LOD, 0, 0)).
unsigned* GameobjSubmitDrawbaseline(FClientGameobj* Self, unsigned* OutLod, void* Camera,
                                  unsigned CachedLod, void* Matrix, void* Extra);

// EquippedObj's vt+0x188 body, for FClientEquippedObj::Slot188: the CEqObj's
// +0x140 fade above 0 pushes 1 - fade through IFLAppearance Slot3 (latch +0xe0 = 1) and, for a
// non-veilborn archetype, Slot8(3, &Extra); otherwise a set latch is cleared through Slot5. Then
// non-virtually. Also called directly by 0x005462a3 and 0x005482d3.
unsigned* EquippedObjSubmitDrawbaseline(FClientEquippedObj* Self, unsigned* OutLod, void* Camera,
                                      unsigned CachedLod, void* Matrix, void* Extra);

// / the routine (cdecl) -- NeuronRuntime::Engine QI for IFLAppearance, then Slot3
// (InstId, Value) / Slot8 (Index, Out); true when the call returned >= 0, false when the QI
// failed. Callers: the draw submits above and the Ship draw the branch (0x0054614a /
// ).
bool SetAppearanceInstanceValue(void* InstId, void* ArchPath);
bool GetAppearanceOverrideMaterial(unsigned long Index, void** Out);

// (thiscall on the camera, RET 8) -- GameCamera slot 0x78, also called directly by
// and the first-person the routine. The player's jump arm (Ship +0x158 == 2|3)
// draws only the ship, its effects and the jump tunnel; otherwise the queue the field is
// refilled (spatial Fill, large solars, ManagerKind walks the routine), sorted and drained
// (the routine, the routine), the effects drawn (layer bit 0x100) and the clear colour set.
void SpaceObjDrawPass(FSpaceCamera* Cam, unsigned LayerFlags, unsigned Param2);

// — rebuild the field light XZ quadtree from gamelit list.
void LightGridRebuild();
// (cdecl, callers the space teardown 0054afc5 and StarSysLeave 00554bbd): the grid
// root emptied with HalfSize = the field / 2 (the extent is left as is) and both node pools
// rethreaded as free lists.
void LightGridReset();

// — nearby light collect from the XZ light grid (max 4 typical).
int SpaceCollectNearbyLights(void** OutSlots, int MaxSlots, float const* Pos3, float Radius,
                             unsigned char Flag);

// (LayerFlags, Param2): watcher gate → SpaceObjDrawPass → depth clear → HUD.
// baseline in-space frame (0054b10a) passes LayerFlags=0xffff, Param2=0.
void SpaceCameraRenderSlot78(FSpaceCamera* Cam, unsigned LayerFlags, unsigned Param2);

// / the routine (thiscall on the camera) -- with options 0x25, 0x24 and 0x28 all
// on, walk the routine()'s environment-effect list around the first-person cockpit draw:
// before, slot 0x10 (camera, 1.0f) on each entry, or +0x34 = 1.0f when fully inside a field;
// after, slot 0x14 (camera, 1.0f). The first-person scene slot 0x00518420 calls both.
void SpaceEnvFxBeginCockpitPass(FSpaceCamera* Cam);
void SpaceEnvFxEndCockpitPass(FSpaceCamera* Cam);

// default draw range / far clip: the field = 20000, the field = 21000.
// Caller: the system INI load the routine (before the file opens).
void SpaceResetFarClip();

// [SystemInfo] space_farclip: the field = max(FarClip, 1000) (NaN kept),
// = that - 1000.
void SpaceSetFarClip(float FarClip);

// / the field / the field -- the field controller (a nebula) the routine
// picked for the current camera, how far the camera is inside it (0 none, 1 partial, 2 fully)
// and its radius squared. Read by the particle visibility test the routine.
void* GetSpaceFieldController();
int GetSpaceFieldEnclosure();
float GetSpaceFieldRadiusSq();

// the draw queue's teardown: Heap::Shrink on the two ZItem heaps
// (the field / the field), free() every queued ZItem and empty the five bucket vectors.
// Callers: the space-state exit 0x0054afa7 and the tail jump 0x0054ab36.
void SpaceDrawQueueRelease();
