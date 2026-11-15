// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// SceneScript entity control interfaces.

// A SceneScript scene drives the world through small NEURONCOMPONENTS facets QI'd off whatever object the
// script is attached to: one for the transform, one for the camera, one for lights, and so on.
// scenescript.dll QIs them by IID (Iids::GeoTransformControl and friends, all in neuroncomponents_iids.h) and
// then calls slots on them. OutpostFrontier.exe implements them, as several facets of one attach
// object -- Client/Gf/ScriptBase.cpp in baseline; here SceneScript_Component
// (src/OutpostFrontier/Client/gf_scenescript_component.cpp) and its per-entity-type subclasses in
// gf_scenescript_*.cpp. A second, much smaller implementation of IGeoTransformControl alone lives in
// space_camera_script.cpp.

// Until 2026-09-16 NONE of these were declared anywhere. SceneScript reached them through generic
// dispatchers that took the slot number as a runtime argument --
// `CallCtrlFloat(Ctrl, 6, Value)` -- and OutpostFrontier answered with `static void* [16]` arrays
// filled out with SlotOk / SlotOk1 / SlotGeneric placeholder stubs. Neither side could be
// checked against the other, and they disagreed in sixteen places. The comment that used to
// sit on the padding stub says it best: short tables had already made a call run off the end
// of the array into adjacent .data, and the fix at the time was to pad with more stubs.

// WHERE THE SIGNATURES COME FROM. scenescript.dll's side of this contract is ported and verified
// (see the scenescript.dll marathon), so a SceneScript call site is a trace of what baseline pushes. The
// OutpostFrontier placeholders were not traces of anything. Where the two disagreed, the caller
// wins, and each such slot says so below. Slots nobody calls keep the shape the placeholder
// had, marked TODO -- their widths are assumptions, not recoveries, and they must not be
// called until someone reads the baseline body.

// SLOT COUNTS (2026-09-26). They now follow baseline's tables, not the placeholder arrays they
// replaced. Every SceneScript object in OutpostFrontier.exe (RTTI SceneScript_Component and its thirteen
// derived classes, Client\Gf\ScriptBase.cpp) is these nine facets in one fixed order, and the
// shared QueryInterface the branch answers them from the table at 0x005cca20:
//   +0x00 ICompoundControl      +0x04 ISoundControl        +0x08 IIKControl
//   +0x0c IGeoTransformControl  +0x10 ICameraControl       +0x14 ID3DLightControl
//   +0x18 IPSysControl          +0x1c IFogControl          +0x20 IReverbControl
// SceneScript_Component's tables (0x005cc590 .. 0x005cc6d4, each followed by the next table's RTTI
// locator) fix the length of every facet and, through each default body's RET n, the width of
// every slot. The slots the placeholders appended past those lengths never existed; no caller
// in the tree used them. ILightControl, IScriptControl, ISubtitleControl and IUserEventControl
// are not in that table -- a SceneScript object answers E_NOINTERFACE (-3) for them.

// "1.11_IGeoTransformControl" -- position and orientation of the attached entity.
// baseline: the only control every entity type answers to.
struct IGeoTransformControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) = 0;     // 3
    virtual GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) = 0;   // 4
    virtual GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) = 0;  // 5
    virtual GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) = 0; // 6
    // The interface stops here. The two baseline sinks in OutpostFrontier.exe's script camera
    // (.rdata tables behind 0x0051e830) are seven entries long and nothing in the tree reads
    // past slot 6, so the nine further slots the port used to pad this table with were
    // invented, not recovered.
};

// "1.11_ICameraControl" -- camera and monitor entities.
// baseline sink: the SceneScript_Camera camera facet, .rdata 0x005ccb18, thirteen entries (the dword
// at 0x005ccb4c is the next table's RTTI locator). Slots 4, 5, 9 and 10 are all the branch
// (MOV EAX,0xfffffff9; RET 8): one dword argument, "not implemented". SceneScript does call 5 and
// 10 (the aspect pair) and gets that result back. The seven slots the port used to declare
// past 12 do not exist.
struct ICameraControl : INeuronComponent
{
    // Slots 4 and 9 carry the facet prefix (see ICompoundControl).
    // Modern typed pointer transport preserves the raw F32 word until the callee
    // consumes it. baseline forwards DWORDs without evaluating floating arguments.
    // Slot count/order is unchanged; every port caller and implementation agrees.
    virtual GENRESULT NEURONCOMPONENTS_API SetFov(const float* Fov) = 0;          // 3  the branch
    virtual GENRESULT NEURONCOMPONENTS_API CameraSlot4(void* Arg) = 0;     // 4  the branch
    virtual GENRESULT NEURONCOMPONENTS_API SetAspect(const float* Aspect) = 0;    // 5  the branch
    virtual GENRESULT NEURONCOMPONENTS_API SetNear(const float* Near) = 0;        // 6  the branch
    virtual GENRESULT NEURONCOMPONENTS_API SetFar(const float* Far) = 0;          // 7  the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetFov(float* OutFov) = 0;      // 8  the branch
    virtual GENRESULT NEURONCOMPONENTS_API CameraSlot9(void* Arg) = 0;     // 9  the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetAspect(float* OutAspect) = 0; // 10 the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetNear(float* OutNear) = 0;    // 11 the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetFar(float* OutFar) = 0;      // 12 the branch
};

// "1.11_IScriptControl" -- a sub-scene entity's control: scenescript.dll's own script engine
// answers it (FScriptEngine facet +8, offset 8 in the {IID, offset} table module storage),
// and the parent scene drives the sub-scene through it (SceneScriptScene.cpp). Seven slots: the
// engine's abstract-base table module storage and its final table module storage both end
// after slot 6 (the next dword is the following table). Slots 3..6 are adjustor thunks
// (`this -= 8; JMP [vtbl+n]`) into IScriptEngine slots 22 / 26 / 23 / 17, which name them.
// Until 2026-10-16 this declared slots 7..15 as well; they never existed.
struct IScriptControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3Start(int Arg) = 0;      // 3  the branch -> engine slot 22
    virtual GENRESULT NEURONCOMPONENTS_API Slot4Tick(int DtMs) = 0;      // 4  the branch -> engine slot 26
    virtual GENRESULT NEURONCOMPONENTS_API Slot5Stop() = 0;              // 5  the branch -> engine slot 23
    virtual int NEURONCOMPONENTS_API Slot6GetDuration() = 0;             // 6  the branch -> engine slot 17
};

// "1.11_ICompoundControl" -- compound and deformable entities: motions, hardpoints, floor
// height. Ten slots (SceneScript_Component 0x005cc6e0..0x005cc704); the identity table continues
// with the SceneScript object's own thiscall virtuals (gf_scenescript_component.h), not with this interface.
// SceneScript_Compound 0x005ccf78.., SceneScript_Deformable 0x005cd1f8.. implement it.
struct ICompoundControl : INeuronComponent
{
    // NAMES. A SceneScript object inherits all nine facets, so two facets' slots with one name and one
    // signature would be a single override in C++ -- SceneScript_Compound's motion stop would answer
    // ISoundControl slot 4 and IIKControl slot 4 too. Every slot a sibling facet also declares
    // therefore has a name of its own: recovered from the baseline bodies where they say what the
    // slot does, the facet's prefix plus the slot number where they do not.
    
    // 3 / 4: START_MOTION / its stop, with the motion descriptor (scenescript_iface.h
    // FSceneScriptMotionEventDesc). Compound the routine / the routine, Deformable the branch /
    // .
    virtual GENRESULT NEURONCOMPONENTS_API StartMotion(void* Desc) = 0;  // 3
    virtual GENRESULT NEURONCOMPONENTS_API StopMotion(void* Desc) = 0;   // 4
    // 5: SceneScriptScene sends a float -- the floor height (Deformable the branch stores it at
    // +0x74 and hands the character the height above the script frame).
    virtual GENRESULT NEURONCOMPONENTS_API SetFloorHeight(float Height) = 0;
    // 6: elapsed milliseconds (Deformable the branch turns them into the character's update).
    virtual GENRESULT NEURONCOMPONENTS_API AdvanceTime(int DeltaMs) = 0;
    // 7 / 8: CONNECT_HARDPOINTS / its disconnect, with the event descriptor (+0x1c / +0x20 the
    // two entity indices, +0x2c / +0x30 the two hardpoint names).
    virtual GENRESULT NEURONCOMPONENTS_API ConnectHardpoints(void* Desc) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API DisconnectHardpoints(void* Desc) = 0;
    // 9: SceneScriptScene sends (int TargetType, const char* Name, float* OutXform12). The out is a
    // Common Transform: the 3x3 orientation, then the position at +0x24.
    virtual GENRESULT NEURONCOMPONENTS_API GetHardpointXform(int TargetType, char const* Name,
                                                  float* OutXform12) = 0;
    // 10: GET, not set. baseline's scenescript at 0x06f497fa does `LEA EAX,[ESI+0x70]; PUSH EAX;
    // PUSH ECX; CALL [EDX+0x28]` -- slot 10, and the argument is an address.
    virtual GENRESULT NEURONCOMPONENTS_API GetFloorHeight(float* OutHeight) = 0;
    // 11 / 12: the whole frame as a Common Transform (orientation, then position at +0x24):
    // Compound the branch / the branch, Deformable the branch / the branch. Both RET 8.
    virtual GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) = 0;
};

// "1.11_IIKControl" -- inverse kinematics on deformable entities. Three slots
// (SceneScript_Component 0x005cc690..0x005cc698; SceneScript_Deformable 0x005cd1a8.. the branch /
// / the branch).
struct IIKControl : INeuronComponent
{
    // 3 and 5: SceneScriptScene sends (void* Desc, float* Position, float* Orientation) to both.
    // Deformable: 3 starts an aim on a free slot, 4 stops the slot whose event id matches, 5
    // moves a running aim's target.
    virtual GENRESULT NEURONCOMPONENTS_API StartIk(void* Desc, float* Position, float* Orientation) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API StopIk(void* Desc) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API UpdateIk(void* Desc, float* Position, float* Orientation) = 0;
};

// "1.11_ILightControl" -- light and scene entities.

// Slots 6 and 7 were transposed. SceneScriptScene sends the cone cutoff to slot 6 and the on/off
// flag to slot 7; OutpostFrontier had a no-argument placeholder in 6 and the cutoff setter in 7,
// so the cutoff was dropped on the floor and the setter received a boolean instead.
struct ILightControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetColor(int* Rgb) = 0;        // 3
    virtual GENRESULT NEURONCOMPONENTS_API SetDirection(float* Dir3) = 0; // 4
    virtual GENRESULT NEURONCOMPONENTS_API SetRange(float Range) = 0;     // 5
    virtual GENRESULT NEURONCOMPONENTS_API SetCutoff(float Radians) = 0;  // 6
    virtual GENRESULT NEURONCOMPONENTS_API SetOn(int On) = 0;             // 7
    // 8: placeholder took nothing; SceneScriptScene sends `(Type == 3) ? 1 : 0`, i.e. a spot flag.
    virtual GENRESULT NEURONCOMPONENTS_API SetIsSpot(int IsSpot) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetColor(float* OutRgb) = 0;      // 9
    virtual GENRESULT NEURONCOMPONENTS_API GetDirection(float* OutDir3) = 0; // 10
    virtual GENRESULT NEURONCOMPONENTS_API GetRange(float* OutRange) = 0;    // 11
    virtual GENRESULT NEURONCOMPONENTS_API GetCutoff(float* OutRadians) = 0; // 12
    // 13 and 14: placeholders took nothing; SceneScriptScene reads both with a float*. Which
    // property each returns is not recovered -- the port's callers only copy them into a
    // light blob -- so they keep numeric names.
    virtual GENRESULT NEURONCOMPONENTS_API GetSlot13(float* Out) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetSlot14(float* Out) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot15() = 0;                  // 15 TODO
    virtual GENRESULT NEURONCOMPONENTS_API Slot16(void* Arg) = 0;         // 16..19 TODO
    virtual GENRESULT NEURONCOMPONENTS_API Slot17(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot18(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot19(void* Arg) = 0;
};

// "1.11_ID3DLightControl" -- the Direct3D-facing half of a light entity.

// SceneScriptScene sends (on-flag, &type, direction) to slots 3, 4 and 5. OutpostFrontier's table read
// those as (float, direction, colour), so the direction went into the colour setter and the
// light type into the direction setter.

// Slot semantics read off the one implementation, SceneScript_Light (OutpostFrontier.exe table 0x005cd458,
// 2026-09-26): 3 the branch writes the registry enable byte, 4 the branch hands its
// argument to the engine light's D3DLIGHT8 setter the routine, 5 the branch stores a
// direction, 6 the branch reads the enable byte into an int, 7 the branch copies 0x68 bytes
// (a D3DLIGHT8) out of the engine light, 8 the branch reads the direction back. SceneScriptScene
// passes the light props' +0x20 (On), +0x28 (the embedded D3DLIGHT8) and +0x0c (Direction) to
// exactly these. The D3DLIGHT8 travels as void*: OutpostFrontier keeps its own FD3dLight8 layout
// (engine_light.h) and SceneScript only forwards the blob.
struct ID3DLightControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetOn(int On) = 0;                   // 3
    virtual GENRESULT NEURONCOMPONENTS_API SetLight(void const* D3dLight8) = 0; // 4
    virtual GENRESULT NEURONCOMPONENTS_API SetDirection(float* Dir3) = 0;       // 5
    virtual GENRESULT NEURONCOMPONENTS_API GetOn(int* OutOn) = 0;               // 6
    virtual GENRESULT NEURONCOMPONENTS_API GetLight(void* OutD3dLight8) = 0;    // 7
    virtual GENRESULT NEURONCOMPONENTS_API GetDirection(float* OutDir3) = 0;    // 8
    // Six slots (SceneScript_Component 0x005cc610..0x005cc624, all the branch: one argument).
};

// "1.11_IFogControl" -- scene entities. Shares the attach object's D3D-light facet field:
// a scene entity puts the fog table there instead of the D3D light table.
// Ten slots; the widths are SceneScript_Component's defaults (0x005cc5b4..0x005cc5d8): 3..6 and
// 11..12 the branch (one argument), 7..8 the branch (three), 9..10 the branch (two).
// SceneScript_Scene's table 0x005cdec0 is the one implementation (FSceneScript_Scene, gf_scenescript_scene.cpp).
// Names and argument types read off that table (the branch..the routine): the colour is
// three ints whose low bytes are kept (the branch), and slots 11 / 12 exchange the 0x20-byte
// block {+0 on, +1 table, +4 int colour[3], +0x10 mode, +0x14 start, +0x18 end, +0x1c density}
// through the facet's own slots 3/5/7/9 and 4/6/8/10. scenescript.dll the routine passes the colour
// ints straight from that block (0x06f431bb ADD ESI,0x13c; PUSH ESI).
struct IFogControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetOn(unsigned char On) = 0;                          // 3
    virtual GENRESULT NEURONCOMPONENTS_API GetOn(unsigned char* OutOn) = 0;                      // 4
    virtual GENRESULT NEURONCOMPONENTS_API SetColor(int const* Rgb3) = 0;                        // 5
    virtual GENRESULT NEURONCOMPONENTS_API GetColor(int* OutRgb3) = 0;                           // 6
    virtual GENRESULT NEURONCOMPONENTS_API SetRange(float Start, float End, float Density) = 0;  // 7
    virtual GENRESULT NEURONCOMPONENTS_API GetRange(float* OutStart, float* OutEnd,
                                         float* OutDensity) = 0;                      // 8
    virtual GENRESULT NEURONCOMPONENTS_API SetTableMode(unsigned char Table, unsigned int Mode) = 0; // 9
    virtual GENRESULT NEURONCOMPONENTS_API GetTableMode(unsigned char* OutTable,
                                             unsigned int* OutMode) = 0;              // 10
    virtual GENRESULT NEURONCOMPONENTS_API SetPacked(unsigned char const* Packed32) = 0;         // 11
    virtual GENRESULT NEURONCOMPONENTS_API GetPacked(unsigned char* Out32) = 0;                  // 12
    // Modern raw-DWORD transport; original thirteen slots retain their indices.
    virtual GENRESULT NEURONCOMPONENTS_API SetRangeValue(const float* Start, const float* End,
                                              const float* Density) = 0;             // 13 +0x34
};

// "1.11_IReverbControl" -- scene entities.
struct IReverbControl : INeuronComponent
{
    // 3 and 4 are the widest mismatch in this family. SceneScriptScene sends four dwords to each;
    // OutpostFrontier had a ONE-argument setter in slot 3 and a NO-argument placeholder in slot 4,
    // so a reverb set leaked twelve bytes of stack and a reverb get leaked sixteen.
    virtual GENRESULT NEURONCOMPONENTS_API SetReverb(unsigned int Environment, float Volume, float Decay,
                                          float Damping) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetReverb(float* OutEnvironment, float* OutVolume,
                                          float* OutDecay, float* OutDamping) = 0;
    // Two slots (SceneScript_Component 0x005cc59c / 0x005cc5a0, the branch: four arguments).
};

// "1.11_IPSysControl" -- particle-system entities. Five slots (SceneScript_Component
// 3 and 4 the branch with no argument, 5..7 the branch with one).
// Slot 4 ends the effect: SceneScript_Psys the branch destroys it (the routine), and SceneScriptScene
// sends it when a START_PSYS event ends. It cannot be called Stop: the SceneScript objects carry a
// thiscall Stop() of their own (identity slot 14) that it would collide with.
struct IPSysControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Start() = 0;                   // 3
    virtual GENRESULT NEURONCOMPONENTS_API StopEffect() = 0;              // 4
    virtual GENRESULT NEURONCOMPONENTS_API Tick(int DeltaMs) = 0;         // 5
    virtual GENRESULT NEURONCOMPONENTS_API SetSparam(float Value) = 0;    // 6
    virtual GENRESULT NEURONCOMPONENTS_API GetSparam(float* OutValue) = 0; // 7
};

// "1.11_ISoundControl" -- sound entities. Eight slots (SceneScript_Component 0x005cc6ac..0x005cc6c8:
// 3, 9 and 10 the branch with two arguments, 4..8 the branch with one).
struct ISoundControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int TimeMs) = 0; // 3
    // 4..8 read off SceneScript_Sound's table 0x005ce234: slot 4 (the branch) stops the playing
    // handle and SceneScriptScene sends it when a START_SOUND event ends; 5 / 7 (the branch /
    // ) keep the attenuation at +0x88 that identity slot 21 adds to the fade before
    // the handle's SetAttenuation (ISoundSource slot 32); 6 / 8 (the branch / the branch)
    // keep the pan at +0x8c that goes, times 100, to the handle's SetPan (slot 33).
    virtual GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) = 0;               // 4
    virtual GENRESULT NEURONCOMPONENTS_API SetAttenuation(float Attenuation) = 0;   // 5
    virtual GENRESULT NEURONCOMPONENTS_API SetPan(float Pan) = 0;                   // 6
    virtual GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* OutAttenuation) = 0; // 7
    virtual GENRESULT NEURONCOMPONENTS_API GetPan(float* OutPan) = 0;               // 8
    // 9: SceneScriptScene sends (float* Props8, int Mask).
    virtual GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props8, int Mask) = 0;
    // 10: the capture half of the same pair.
    virtual GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props8, int Mask) = 0;
};

// SceneScript binds SubtitleControl separately from UserEventControl. Subtitle calls
// pass a string ID; user-event calls pass a descriptor. A common foreign facet
// has not been established by baseline producer/consumer evidence.
struct ISubtitleControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API ShowSubtitle(int StringId) = 0;   // 3
    virtual GENRESULT NEURONCOMPONENTS_API HideSubtitle(int StringId) = 0;   // 4
    virtual GENRESULT NEURONCOMPONENTS_API Slot5() = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot6() = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot7(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot9(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot10(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot11(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot12(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot13(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot14(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot15(void* Arg) = 0;
};

// SceneScript requests this IID into its own UserEventControl field. The baseline
// client QI table does not establish a shared SubtitleControl/UserEventControl
// facet; foreign providers remain unresolved.
struct IUserEventControl : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* Desc) = 0;         // 3
    // 4: placeholder took nothing, SceneScriptScene passes a descriptor.
    virtual GENRESULT NEURONCOMPONENTS_API Slot4(void* Desc) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot5() = 0;                   // 5, 6 TODO
    virtual GENRESULT NEURONCOMPONENTS_API Slot6() = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot7(void* Arg) = 0;          // 7..15 TODO
    virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot9(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot10(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot11(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot12(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot13(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot14(void* Arg) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API Slot15(void* Arg) = 0;
};
