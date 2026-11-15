// Author: Giperion.

#pragma once

// RoomControl runtime — baseline the routine object, sizeof 0x13ac.
// Subobjects: +0x4C the routine (0x18), +0x504 the routine (0x10),
// +0x1020 the routine (0x18). Camera* at +0x500 = the routine.

// Base +0x00..+0x2C = the routine RoomObject, registered in the field.

#include <list>
#include <stdint.h>
#include <vector>

struct ICamera;
class RoomData;
struct IRenderPipeline;

struct FRoomRuntime;

namespace Archetype
{
    struct FCliMaterialEntry;
}

// The engine-instance handle a RoomObject answers with when it has none (the routine -1).
static void* const kRoomObjectNoInstance = reinterpret_cast<void*>(static_cast<intptr_t>(-1));

// RoomObject (RTTI .?AVRoomObject@@) -- the base of every object a room draws. The ctor
// (Owner) appends it to the field and the dtor the routine releases its
// material entries and takes it out again. the routine walks that list three times, once per
// draw slot, for the objects whose Owner is the room being drawn, layer by layer; the routine
// runs slot 5 on every object whose +0x04 byte is clear.
// Vtable 0x005e14a4 (13 slots). Derived: RoomControl (0x005cbea0), Gf::ObjectProp
// (0x005cbc0c), Character (0x005cb930), ObjectShip (0x005cbdf0), 0x005cb8d4.
struct FRoomObject
{
    explicit FRoomObject(FRoomRuntime* InOwner);           // (RET 4)
    virtual ~FRoomObject();                                // 0  the routine / the routine
    virtual bool Slot1();                                  // 1  +0x04 the routine: false
    virtual bool IsVisible();                              // 2  +0x08 the branch: +0x10
    virtual void SetFloat1C(float Value);                  // 3  +0x0c the branch
    virtual float GetFloat1C();                            // 4  +0x10 the branch
    virtual void Update(double DeltaSeconds) = 0;          // 5  +0x14 _purecall (RET 8)
    virtual void* GetEngineInstance();                     // 6  +0x18 the routine: -1
    virtual void DrawOpaque(ICamera* Camera);              // 7  +0x1c no-op (RET 4)
    virtual void DrawTranslucent(ICamera* Camera);         // 8  +0x20 no-op (RET 4)
    virtual void DrawEffects(ICamera* Camera);             // 9  +0x24 no-op (RET 4)
    // 10..12: the hotspot events the routine sends to a hotspot's owner (gf_hotspot.h) with
    // the hotspot's cookie (+0x08) and the cursor point: 1 enter, 2 leave, 3 click. Only
    // Character (0x005cb930: 0x00445a90 / 0x00445be0 / 0x00445c10) overrides them.
    virtual void Slot10(unsigned long Cookie, int const* Point); // 10 +0x28 no-op (RET 8)
    virtual void Slot11(unsigned long Cookie, int const* Point); // 11 +0x2c no-op (RET 8)
    virtual void Slot12(unsigned long Cookie, int const* Point); // 12 +0x30 no-op (RET 8)

    // AmbientSource's +0x6c (three ints, 0..255 each), or null.
    int const* GetAmbientOverride() const;

    unsigned char bNoUpdate;      // +0x04 the routine skips slot 5 while it is set
    FRoomRuntime* Owner;          // +0x08 the room whose the routine draws this object
    void* AmbientSource;          // +0x0c the routine hands back its +0x6c (three ints)
    unsigned char bVisible;       // +0x10 1 -- slot 2; clear means black ambient
    unsigned char bLit;           // +0x11 1 -- the routine picks lights only when set
    unsigned char bFog;           // +0x12 1 -- D3DRS_FOGENABLE when room fog is active
    unsigned char bEnvMap;        // +0x13 0 -- the routine render mode 0 "envmapbasic"
    unsigned char Flag14;         // +0x14 0
    unsigned char LightGroup;     // +0x15 0 -- the routine group filter
    unsigned char Layer;          // +0x16 0 -- the the routine sort key
    int RenderMode;               // +0x18 0 -- the routine: 0 renderer, 1 / 2 appearance
    float Float1C;                // +0x1c 0 -- slots 3 / 4
    // +0x20..+0x2f: a std::vector of MaterialLib entries (baseline's allocator byte at +0x20
    // takes the low byte of the ctor argument); the routine releases every entry through
    // . Filled by the routine when the object loads its mesh.
    std::vector<Archetype::FCliMaterialEntry*> MaterialEntries;
};

// -> the field: the list every RoomObject is appended to.
std::list<FRoomObject*>& GetRoomObjectList();

// (thiscall on the room, RET 4) -- draw one object: the routine lights, fog and
// ambient; the "shadow_glare" glare light into the instance's appearance; then by render mode
// the renderer (0, optionally "envmapbasic") or appearance slot 10 / 9 (1 / 2).
void RoomControlDrawObject(FRoomRuntime* Room, FRoomObject* Object);

// (thiscall on the room, RET 0xc) -- the SHADOWS option and "shadow_glare" with
// the room's shadow light on: appearance slot 12 (bFlag) or 11 with the camera and {A, B}.
void RoomControlDrawShadow(FRoomRuntime* Room, unsigned long A, unsigned long B,
                           unsigned char bFlag);

// + camera install from RoomData (the routine / the routine).
// Also the routine(0) — inserts Runtime into the field.
FRoomRuntime* CreateRoomRuntime(RoomData* Room);

// Release path (camera the routine + free).
// Unlinks from the field (the routine) before free.
void DestroyRoomRuntime(FRoomRuntime* Runtime);

// Runtime+0x500 — null until CreateRoomRuntime applied a camera.
ICamera* GetRoomRuntimeCamera(FRoomRuntime* Runtime);

// The same +0x500 as the concrete room camera (room_camera.h) -- what Script+0x30->+0x500
// hands the routine when a SceneScript camera takes a pool slot.
class FRoomCamera;
FRoomCamera* GetRoomRuntimeRoomCamera(FRoomRuntime* Runtime);

unsigned GetRoomRuntimeByteSize();

// Runtime+0x34 -- the base the routine loaded the room for (Gf::ObjectProp's "$..." template
// indirections, the routine, read it through the prop's owner).
unsigned GetRoomRuntimeBaseId(FRoomRuntime* Runtime);

// Runtime+0x40 -- the room's RoomData. The Gf script spawn reads it directly (0x0044d737,
// , 0x0044dad7, 0x0044dc98) for the find_camera / find_setpoint lookups.
RoomData* GetRoomRuntimeRoomData(FRoomRuntime* Runtime);

// Runtime+0x38 -- the room's id. CharacterObject reads it through its owner (0x00445ae8) and
// through its room argument (0x00435de2) for the room state's buckets.
unsigned GetRoomRuntimeRoomId(FRoomRuntime* Runtime);

// One element of the room's character placements (Runtime+0x504, a VC6 std::vector of 8-byte
// elements built by the routine from RoomData+0x174): the placement and the character
// standing on it (null while free).
class CharacterProperties;
struct CharPlaceInfo;
struct FCharacterObject;
struct FRoomCharPlacement
{
    CharPlaceInfo* Info;          // +0x00
    FCharacterObject* Character;  // +0x04
};

// (thiscall on the room, RET 4) -- fog enable, scene ambient and the lights for
// Object. CharacterObject's draw calls it directly (0x00445246).
void RoomControlLightObject(FRoomRuntime* Room, FRoomObject* Object);

// (thiscall, RET 4) -- the +0x518 character whose record key (+0x154) is Key, or
// null. Caller: the routine (0x00435f22).
FCharacterObject* RoomControlFindCharacterByKey(FRoomRuntime* Runtime, unsigned Key);
// (thiscall) -- the first +0x518 character on a placement of char type 1 (pc), or
// null. Callers: the routine, the Gf script spawn (0x0044d793, 0x0044e913).
FCharacterObject* RoomControlPlayerCharacter(FRoomRuntime* Runtime);
// (thiscall, RET 4) -- the first +0x518 character of Role (+0x300), or null.
// Caller: 0x0043ef4c.
FCharacterObject* RoomControlFindCharacterByRole(FRoomRuntime* Runtime, int Role);
// (thiscall, RET 4) -- the first +0x518 character whose placement's setpoint
// segment is Name (NEURONCOMPONENTS stricmp), or null. Caller: the Gf script spawn (0x0044e953).
FCharacterObject* RoomControlFindCharacterByName(FRoomRuntime* Runtime, char const* Name);
// (thiscall, RET 4) -- a character record of this room with a costume: the
// character of its key updated in place (record, hotspot, idle script), or a new one on a
// free placement. Caller: the LocationController's upsert the routine (0x0043accd).
void RoomControlUpdateCharacter(FRoomRuntime* Runtime, CharacterProperties const& Props);

class FObjectProp;
// (thiscall, RET 4) -- the first +0x618 prop named Name, or null.
FObjectProp* RoomControlFindProp(FRoomRuntime* Runtime, char const* Name);
// (thiscall, RET 4) -- append Prop to +0x618 (no bound check).
void RoomControlAddProp(FRoomRuntime* Runtime, FObjectProp* Prop);

// (thiscall, no arguments) -- the byte at Runtime+0x1338. Field_0x1338: meaning
// not recovered (TODO); written by the ctor, 0x004499e9, 0x0044bad5, 0x0044bd29, 0x0044c297 and
// . Callers: 0x0043eedb and the Gameflow tick the routine (0x00558bcd), which
// restores the software cursor only while it is clear.
unsigned char RoomControlGetField1338(FRoomRuntime* Runtime);

// (thiscall, no arguments) -- +0x1338 = 0 and the field (G_bNnKeepPanelOpen,
// nn_main_button.h) = 0. Callers: NavBar 0x0043ff7a / 0x004419a9 (nav_bar.cpp).
void RoomControlClearField1338(FRoomRuntime* Runtime);

// Runtime+0x1340, a byte (Field_0x1340: meaning not recovered, TODO). baseline has no accessor:
// NavBar's the routine stores 1 (0x004417e1, 0x0044185c, 0x004419a2) or 0 (0x004419f4) into it
// right before a camera track starts.
void RoomControlSetField1340(FRoomRuntime* Runtime, unsigned char Value);

// (thiscall, RET 0xc) -- with C != -1 and B == 3, slot 1 (+0x04, no arguments) on
// every +0x1134 room controller whose +0x04 or +0x08 dword equals C (count +0x1334, re-read
// each pass). A is not read. Callers: NavBar 0x004407b9 / 0x00440c68 / 0x004415d2.
void RoomControlNotifyControllers(FRoomRuntime* Runtime, int A, int B, int C);

// The room camera move (Runtime +0x1338..+0x13a0 and the +0x500 camera):
// (RET 4)    -- snap the room camera to the named RoomData +0x138 camera
//                              (any move stopped first); false when no camera has that name.
// (RET 0x10) -- with no move running and a camera of that name, a 2-second
//                              cosine-eased move to it (+0x1338 = the field = 1), dropped at
//                              once when the camera is already there; answers +0x1338.
//                              Done(A, B, 0) when the move ends (the routine).
// (RET 4)    -- the per-frame step, from the room update (the branch).
// (RET 0x14) -- a step (Arg0's low byte: forward / backward) along the room's
//                              "Camera_Track_%d" cameras, +0x13a4 the current number; with
//                              bArg4 a camera already standing there repeats the step
//                              (the routine). Answers the routine's result.
typedef void(__cdecl* FRoomCameraDoneCallback)(void* A, void* B, unsigned char bArg);
bool RoomControlSnapCamera(FRoomRuntime* Runtime, char const* CameraName);
bool RoomControlTrackCamera(FRoomRuntime* Runtime, char const* CameraName,
                            FRoomCameraDoneCallback Done, void* A, void* B);
void RoomControlUpdateCameraMove(FRoomRuntime* Runtime, float Delta);
bool RoomControlTrackScriptCamera(FRoomRuntime* Runtime, int Arg0, FRoomCameraDoneCallback Done,
                                  void* A, void* B, unsigned char bArg4);

// Runtime+0x517, a byte (Field_0x517: meaning not recovered, TODO). the routine reads it
// directly (0x005589c7) and skips the arrival text while it is set.
unsigned char RoomControlGetField517(FRoomRuntime* Runtime);

// (thiscall) -- +0x516 = 1: the room update runs the room characters' start script
// again. Caller: the Gf script sequence stop the routine (0x004555cb).
void RoomControlRequestCharacterScripts(FRoomRuntime* Runtime);

// (thiscall) -- the room's scene scripts started (the routine) as Gf script
// sequences, then the character start script latch. Caller: the room load the routine
// (0x0043a055).
void RoomControlPlayScenes(FRoomRuntime* Runtime);

// (thiscall) -> the routine on the ship-pad table Runtime+0x1020: the room has a
// player ship pad (+0x1034 != -1) and its ShipPlaceInfo has a landing script (+0x38).
// Caller: the routine (0x005589d9).
bool RoomControlHasLandingScript(FRoomRuntime* Runtime);

// (cdecl) -- the field = Value: the player's ship lands in the next room that
// starts its scenes (read by the routine only). Callers: the routine (0x00558967) with
// !bFromIntro; the routine clears it (0x0044ab47).
void RoomControlSetLandingPending(unsigned char Value);

// (cdecl) -- free every node of the RoomObject list the field and zero its size;
// the objects themselves are left alone. Caller: the Gameflow shutdown the routine.
void ClearRoomObjectList();

// (thiscall, no arguments) -- the room draw the routine jumps to: viewport and
// camera, depth / cull / blend, the frame light list (the routine), the room fog
// (the routine), then the objects this room owns, layer by layer in ascending order: slot 7
// inside the material-batcher sink (the routine .. the routine), slot 8, the flashlight
// flares (the routine), full-white ambient, the lights unlinked, and slot 9 plus the cue list
// (the routine) inside ParticleFx BeginDraw / EndDraw with dithering off.
void RoomControlDraw(FRoomRuntime* Runtime);
