// Author: Giperion.

#pragma once

// Gf\Script -- the two process-wide tables the SceneScript script components share.

//   * The SceneScript camera pool at .data 0x00668b00 (ScriptCamera.cpp unit, the routine ..
// ). Every non-reference SceneScript camera and monitor takes a slot
//     (the routine); the slot with the highest priority among those allowed to be active
//     is the one whose position, orientation, fov and clip planes are pushed into its room
//     camera (the routine .. the routine).
//   * The script light registry at .data 0x0066d130 / 0x0066d230 (ScriptLight.cpp unit): 64
//     engine-light wrappers and their enable bytes, filled by the SceneScript_Light ctor
//     (the routine), emptied by its release slot (the branch) and enumerated by the room's
//     per-frame light list (the routine through the routine / the routine).

class FRoomCamera;
struct FGfLightWrapper;

// One 0x40-byte pool slot.
struct FGfCameraSlot
{
    FRoomCamera* Camera;     // +0x00  the room camera (RoomObject+0x500) this slot drives
    float Pos[3];            // +0x04
    float Orient[9];         // +0x10
    float HalfFovXDeg;       // +0x34  35.0
    float ZNear;             // +0x38  0.25
    float ZFar;              // +0x3c  1e6
};

// The whole pool object, 0x4604 bytes at 0x00668b00.
struct FGfCameraPool
{
    FGfCameraSlot Slots[0x100];        // +0x0000  the field
    int Priority[0x100];               // +0x4000  the field, -1 when free
    unsigned char bCanBeActive[0x100]; // +0x4400  the field
    unsigned char bOccupied[0x100];    // +0x4500  the field
    int ActiveIndex;                   // +0x4600  the field, -1 when none

    // (static init) and the branch (its atexit twin): every slot back to the
    // defaults, every priority -1, both byte arrays 0, no active slot.
    FGfCameraPool();
    ~FGfCameraPool();
    void Reset();

    // (thiscall, RET 0xc). Takes the first free slot; returns -1 when all 256
    // are taken. The new slot becomes active when it may and outranks the active one. Nothing
    // is pushed into the camera here.
    int Allocate(int SlotPriority, FRoomCamera* Camera, unsigned char bSlotCanBeActive);

    // (thiscall, RET 4). Frees the slot; when it was the active one, the highest
    // priority occupied slot that may be active takes over and its whole state is pushed into
    // its camera.
    void Free(int Slot);
};

extern FGfCameraPool G_GfCameraPool;

// the registry's capacity, 0x40.
int GfLightRegistryCount();

// the wrapper in slot Index when its enable byte is exactly 1, else null.
// The wrapper is the routine's 8-byte { FEngineLight*, byte group }.
FGfLightWrapper* GfLightRegistryGet(int Index);

// tail (0x004530b4..0x004530d9): the first null slot takes the wrapper and the
// enable byte bOn != 0; returns that index, or -1 when all 64 are taken.
int GfLightRegistryAdd(FGfLightWrapper* Wrapper, bool bOn);

// The enable byte of slot Index, written by SceneScript_Light's D3D-light slot 3 (the branch) and
// read by its slot 6 (the branch). No range check, as baseline.
void GfLightRegistrySetEnabled(int Index, bool bOn);
bool GfLightRegistryIsEnabled(int Index);

// head: clear the slot and its enable byte. The caller then resets its index
// to -1 and destroys the wrapper (the routine).
void GfLightRegistryRemove(int Index);

struct FEngineLight;
struct FD3dLight8;

// 's 8-byte result: the engine light and the light group byte. Both the script
// lights (the routine) and the room's own lights (the routine, RoomControl+0xe4) are these.
struct FGfLightWrapper
{
    FEngineLight* Light;     // +0x00
    unsigned char Group;     // +0x04
    unsigned char Pad05[3];
};

// (Frame, Source, Direction, Group): operator new(8) zeroed, then the engine light
// (EngineLightCreate: Frame12 = position + 3x3 orientation, Direction3 = the local direction
// the orientation turns), then the group byte. Returns the wrapper.
FGfLightWrapper* GfLightWrapperCreate(float const* Frame12, FD3dLight8 const* Source,
                                      float const* Direction3, unsigned char Group);

// (FGfLightWrapper** Field): nothing when *Field is null; otherwise the light is
// released through its slot 0 when present, both wrapper fields cleared, the wrapper freed and
// *Field nulled.
void GfLightWrapperDestroy(FGfLightWrapper** Field);

// (r, g, b) (cdecl, three floats by value): the scene ambient. With the LIGHTING
// game option off it is full white; otherwise each channel is floor(c * 255 + 0.5) truncated to
// a byte. Either way the three bytes go to the routine (D3DRS_AMBIENT). Callers are the
// RoomControl draw passes at 0x0044af11 and 0x0044b7c3 (room_control.cpp).
void GfSetSceneAmbient(float R, float G, float B);

// (cdecl) -- how strongly a light reaches Pos3, for the nearest-light pick.
// Directional: 3.0, taken. Range 0: 0, not taken. Point: 2 - dist^2/range^2. Spot: the same,
// unless Pos3 lies outside the half cone (cos(Phi/2)), when the cosine of the angle replaces
// the 2. Radius is passed by baseline and never read.
bool GfLightScore(FD3dLight8 const* D3d, float const* LightPos3, float const* Dir3,
                  float const* Pos3, float Radius, float* OutScore);

// (cdecl) -- every wrapper of List that holds a light of this Group and scores,
// inserted into OutLights / OutScores from the highest score down; *InOutCount comes in as the
// number of entries to clear and leaves as the count, cut to MaxCount at the end.
void GfLightsSelectNearest(FGfLightWrapper* const* List, int Count, float const* Pos3,
                           float Radius, unsigned char Group, int MaxCount,
                           FEngineLight** OutLights, float* OutScores, int* InOutCount);

// (cdecl) -- the up to four best lights of Group for the engine instance's
// position, applied through the routine with the camera and no position.
struct ICamera;
void GfLightsApplyNearest(FGfLightWrapper* const* List, int Count, void* Instance,
                          unsigned char Group, ICamera* Camera);

// the room fog state (ScriptFog unit). The SceneScript fog facet (vtable
// , the branch .. the branch) writes it, the routine applies it once per room
// draw (the routine), the routine tells the per-object draw whether fog is on, and the
// RoomControl ctor clears it (the routine at 0x00449324).
struct FGfFogState
{
    unsigned char bOn;       // +0x00 the field
    unsigned Color;          // +0x04 the field, 0x00RRGGBB
    float Start;             // +0x08 the field
    float End;               // +0x0c the field
    float Density;           // +0x10 the field
    unsigned TableMode;      // +0x14 the field
    unsigned VertexMode;     // +0x18 the field
};

extern FGfFogState G_GfFog;

void GfFogReset();                                           // / the branch
void GfFogSetOn(unsigned char bOn);
bool GfFogIsActive();                                        // / the branch
void GfFogSetColor(int const* Rgb3);
void GfFogGetColor(int* Rgb3);
void GfFogSetRange(float Start, float End, float Density);
void GfFogSetRangeValue(const float* Start, const float* End, const float* Density);
void GfFogGetRange(float* Start, float* End, float* Density);
void GfFogSetMode(unsigned char bTable, unsigned Mode);
void GfFogGetMode(unsigned char* bTable, unsigned* Mode);
void GfFogApply();                                           // -> the routine

// ---------------------------------------------------------------------------------------
// SceneScript script scans the Gf script sequence runs over a loaded script (callers: the
// ScriptSequence setup the routine at 0x00455322 / 0x00455332 / 0x0045533a, and 0x0045576c
// / 0x00455764 -- neither is ported yet).
// ---------------------------------------------------------------------------------------
struct IScriptEngine;

// (cdecl): every START_MOTION event's animation (scenescript_iface.h
// FSceneScriptMotionEventDesc +0x3c) is queued in the CharacterInfo database (the routine) when
// bAcquire is set, released (the routine) otherwise. A null script does nothing.
void GfScriptCharacterAnimations(IScriptEngine* Script, bool bAcquire);

// (cdecl): out-bytes cleared, then among the SOUND entities a template
// "BackgroundAmbient" sets *OutAmbient and "BackgroundMusic" sets *OutMusic (stricmp; either
// out may be null).
void GfScriptFindBackgroundAudio(IScriptEngine* Script, unsigned char* OutAmbient,
                                 unsigned char* OutMusic);

// (cdecl): IScriptEngine slot 9 on every entity, the results dropped. Both callers
// pass a second byte -- 1 from the setup (0x0045531f..0x0045533a), 0 from the teardown
// (0x00455761..0x00455764), the same flag they give the routine -- and the body never reads it
// (only [ESP+param_1+0xc] is loaded).
void GfScriptTouchEntities(IScriptEngine* Script, bool bAcquire);
