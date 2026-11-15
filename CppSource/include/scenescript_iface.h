// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// 8-slot IMotionPath. Slot 4 = per-frame position the branch (stdcall RET 0xC).
struct IMotionPath : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3Transform(float T, float* OutXform) = 0;     // 3  RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot4Evaluate(float T, float* OutVec3) = 0;       // 4  RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot5(float T, float* Out) = 0;                    // 5  +0x14 quat
    virtual GENRESULT NEURONCOMPONENTS_API Slot6LookAt(float T, float* OutVec3) = 0;        // 6  +0x18 LOOK_AT
    // 7  RET 0x8 the branch: *OutLength = inner+0x4 (the routine), the
    // un-normalised total arc length accumulated by the routine; -1 when the
    // inner spline core has not been created yet.
    virtual GENRESULT NEURONCOMPONENTS_API Slot7Length(float* OutLength) = 0;
};


// scenescript.dll public ABI: IScriptEngine, the THN playback engine other modules drive.
// Source: scenescript.dll, implementation notes. The rest of the port's
// SceneScript internals stay in src/SceneScript/SceneScript.h.

// The file is scenescript_iface.h and not scenescript.h on purpose: NTFS is case-insensitive, so a
// public "scenescript.h" would be picked up instead of src/SceneScript/SceneScript.h by every include inside
// the module itself.

// Common (common_roomdata.cpp) and OutpostFrontier's Gf script layer (gf_script / gf_script_object /
// gf_script_spawn, the SceneScript_* component classes in gf_scenescript_*.cpp, gf_character_object,
// space_camera_script) both drive this interface; until 2026-09-16 both did it by raw slot
// number, and Common's locals were named `Engine`, which reads exactly like an IEngine and
// is not one (implementation notes).

// ---------------------------------------------------------------------------------------
// Entity and event records the slots below hand out and take in. Declared here once, for
// scenescript.dll (src/SceneScript/SceneScriptScene.h) and OutpostFrontier's Gf script layer (gf_script_object.h,
// gf_scenescript_*.h); no module may keep a private copy -- earlier port-invented copies in
// OutpostFrontier disagreed on the size.
// ---------------------------------------------------------------------------------------

// Entity types (snapshot +0x00). baseline: the per-type entity constructors of the scene
// loader (scenescript.dll FUN_06f25xxx switch), scenescript_thn_format.md.
static const int kSceneScriptEntityCompound = 1;
static const int kSceneScriptEntityDeformable = 2;
static const int kSceneScriptEntityCamera = 3;
static const int kSceneScriptEntityMonitor = 4;
static const int kSceneScriptEntityLight = 5;
static const int kSceneScriptEntitySound = 6;
static const int kSceneScriptEntityMarker = 7;
static const int kSceneScriptEntityHardpoint = 8;
static const int kSceneScriptEntityScene = 9;
static const int kSceneScriptEntitySubScene = 10;
static const int kSceneScriptEntityMotionPath = 11;
static const int kSceneScriptEntityPsys = 13;

// Event types (event descriptor +0x00), the switch at scenescript.dll 0x06f25953.
static const int kSceneScriptEventTypeMin = 1;
static const int kSceneScriptEventTypeMax = 19;
static const int kSceneScriptTypeUserEvent = 1;
static const int kSceneScriptTypeSetCamera = 2;
static const int kSceneScriptTypeStartSound = 3;
static const int kSceneScriptTypeLightPropAnim = 4;
static const int kSceneScriptTypeCameraPropAnim = 5;
static const int kSceneScriptTypePathAnim = 6;
static const int kSceneScriptTypeSpatialPropAnim = 7;
static const int kSceneScriptTypeAttachEntity = 8;
static const int kSceneScriptTypeConnectHardpoints = 9;
static const int kSceneScriptTypeStartMotion = 10;
static const int kSceneScriptTypeStartIk = 11;
static const int kSceneScriptTypeStartSubScene = 12;
static const int kSceneScriptTypeStartPsys = 13;
static const int kSceneScriptTypePsysPropAnim = 14;
static const int kSceneScriptTypeAudioPropAnim = 15;
static const int kSceneScriptTypeFogPropAnim = 16;
static const int kSceneScriptTypeReverbPropAnim = 17;
static const int kSceneScriptTypeFloorHeightAnim = 18;
static const int kSceneScriptTypeSubtitle = 19;

// param_2: 0x24 dwords. Color = _ftol ints; cutoff/theta stored in radians.
// The LIGHT entity's descriptor tail (+0x44) and the light event's target.
struct FSceneScriptLightProps
{
    int Color[3];
    float Direction[3];
    float RangeTemp;
    float CutoffTemp;
    int On;
    int Infinite;
    int Type;
    float Diffuse[4];
    float Specular[4];
    float Ambient[4];
    int Unused17[6];
    float Range;
    int Unused1e;
    float Atten[3];
    float Theta;
    float Cutoff;
};

static_assert(sizeof(FSceneScriptLightProps) == 0x90, "module state lightprops is 0x24 dwords");

// IPersistString facet retained by MOTION_PATH descriptors. RET includes the interface receiver.
struct IPersistString : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3Load(const char* Text) = 0;                  // +0x0c RET 8
    virtual GENRESULT NEURONCOMPONENTS_API Slot4Save(char* Buffer, unsigned long Size) = 0;   // +0x10 RET 12
    virtual unsigned long NEURONCOMPONENTS_API Slot5EstimateSize() = 0;                    // +0x14 RET 4
};

// The MOTION_PATH entity's descriptor tail (+0x44). baseline stores a VC6 string plus a
// path-control interface there; the port keeps the observed semantic payload.
struct FSceneScriptMotionPathDesc
{
    const char* PathType;
    IPersistString* MotionPath;
};

// An entity descriptor: IScriptEngine slots 8 / 9 hand out the one the entity holds, slots
// 35 / 37 take one in. baseline's per-type entity keeps it contiguous (the LIGHT entity's
// slot-7 accessor the branch is `LEA EAX,[ECX+0x84]`, and its tail is copied in whole by
// the type's ApplyDesc), so the tail that follows the common 0x44 bytes is the one the
// descriptor was loaded with. The tail is read by type:
//   CAMERA / MONITOR  +0x44 fovh, near, far, aspect
//   COMPOUND / DEFORMABLE +0x44 floor height
//   LIGHT             +0x44 FSceneScriptLightProps (0x90)
//   SOUND             +0x44 eight audio floats
//   PSYS              +0x44 the sparam
//   SCENE             +0x44 the 15-dword fog/reverb blob (scenescript the routine)
//   MOTION_PATH       +0x44 FSceneScriptMotionPathDesc
struct FSceneScriptEntitySnap
{
    int Type;             // +0x00 kSceneScriptEntity*
    int Flags;            // +0x04 bit 0: a reference entity (resolved, not created)
    float Pos[3];         // +0x08
    float Orient[9];      // +0x14
    char* Name;           // +0x38 the entity's name in the script
    char* TemplateName;   // +0x3c the template (archetype nickname, "PlayerShip", ...)
    unsigned char LtGrp;  // +0x40
    unsigned char SrtGrp; // +0x41
    unsigned char UsrFlg; // +0x42
    unsigned char Pad43;
    union
    {
        struct
        {
            float Fovh;      // +0x44
            float NearPlane; // +0x48
            float FarPlane;  // +0x4c
            float Aspect;    // +0x50
        };
        float FloorHeight;
        FSceneScriptLightProps Light;
        float Audio[8];
        float PsysSparam;
        unsigned int SceneBlob[15];
        FSceneScriptMotionPathDesc MotionPath;
        float Extra[36]; // the raw tail
    };
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptEntitySnap) == 0xd4,
              "the common 0x44 bytes plus the largest tail, the LIGHT props");

// Modern internal output transport: an x87 value, not a baseline object layout.
struct FSceneScriptExtendedProgress
{
    unsigned char Bytes[10];
};
static_assert(sizeof(FSceneScriptExtendedProgress) == 10, "x87 extended value storage");

// Canonical modern NEURONCOMPONENTS interface: 17 original indices plus 2 output methods.
// NEURONCOMPONENTS_API applies to every interface method; RET comments below describe baseline
// reference arities only, not the modern stdcall ABI (which also passes this).
// Two appended methods are modern internal transport contracts, not baseline slots.
struct IParamCurve : INeuronComponent
{
    virtual float NEURONCOMPONENTS_API Evaluate(float T) = 0;                               // 3  RET 4, ST(0)
    virtual unsigned char NEURONCOMPONENTS_API Slot4Replace(int Id, float F0, float F1, float F2,
                                                  float F3) = 0;                 // 4  RET 0x14
    virtual unsigned char NEURONCOMPONENTS_API Slot5ReadKey(int Id, float* Out0, float* Out1, float* Out2,
                                                  float* Out3) = 0;              // 5  RET 0x14
    virtual int NEURONCOMPONENTS_API AddKey(float F0, float F1, float F2, float F3) = 0;     // 6  RET 0x10
    virtual unsigned char NEURONCOMPONENTS_API Slot7DeleteKey(int Id) = 0;                 // 7  RET 4
    virtual unsigned int NEURONCOMPONENTS_API GetKeyCount() = 0;                          // 8 RET 0, the branch
    virtual int NEURONCOMPONENTS_API Slot9IdByIndex(int Index) = 0;                         // 9  RET 4
    virtual const char* NEURONCOMPONENTS_API Slot10Clsid() = 0;                             // 10 RET 0
    virtual const char* NEURONCOMPONENTS_API Slot11Display() = 0;                           // 11 RET 0
    virtual float NEURONCOMPONENTS_API Slot12WrapEvaluate(int A, int B) = 0;                // 12 RET 8
    virtual void NEURONCOMPONENTS_API Slot13Rebuild() = 0;                                  // 13 RET 0
    virtual void* NEURONCOMPONENTS_API Slot14Dtor(unsigned char DeleteFlag) = 0;            // 14
    virtual int NEURONCOMPONENTS_API Slot15PersistStub(void* Unused) = 0;                   // 15 RET 4 → 0
    virtual int NEURONCOMPONENTS_API Slot16PersistStub(void* Unused) = 0;                   // 16 RET 4 → 0
    virtual void NEURONCOMPONENTS_API EvaluateToExtended(const float* T, FSceneScriptExtendedProgress* Output) = 0;
    virtual void NEURONCOMPONENTS_API WrapEvaluateToExtended(int A, int B, FSceneScriptExtendedProgress* Output) = 0;
};

// The common eleven-word event descriptor (baseline: event object +0x28, copied by
// ). IScriptEngine slots 15 / 16 hand out the event's, slot 38 takes one in.
struct FSceneScriptEventDesc
{
    int Type;           // +0x00 kSceneScriptType*
    int StartMs;        // +0x04
    int Flags;          // +0x08 bit 1 "loop" for START_MOTION
    int EventFlags;     // +0x0c
    int Field10;        // +0x10 TODO: meaning not recovered
    int UniqueId;       // +0x14
    int DurationMs;     // +0x18
    int PrimaryIndex;   // +0x1c the entity the event drives
    int SecondaryIndex; // +0x20
    IParamCurve* ParamCurve; // +0x24 curve, or null
    int Field28;        // +0x28 TODO: meaning not recovered
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptEventDesc) == 0x2c,
              "eleven dwords (module state copies 0xb)");

// PATH_MOTION: 450a0 copies eight payload words; 4a4b0 exposes their live owner.
struct FSceneScriptPathEventDesc
{
    FSceneScriptEventDesc Common;
    float StartPercent;
    float StopPercent;
    float PercentDelta;
    float Offset[3];
    int LookAtUpAxis;
    int LookAtFrontAxis;
};
static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptPathEventDesc) == 0x4c,
              "PATH_MOTION common and eight payload words");

// CAMERA_PROP: 46060 copies mask+0x2c and fovh/aspect/near/far at +0x30.
// 4a510 returns this same live sixteen-word descriptor.
struct FSceneScriptCameraEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    float Target[4];
};
static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptCameraEventDesc) == 0x40,
              "CAMERA_PROP common, mask and four target words");

// USER_EVENT: the routine copies descriptor+0x2c as an opaque dword.
// Lua user_event_string is passed separately to the load-time COM hook;
// it does not populate this word (06f25f49 initializes it to zero).
struct FSceneScriptUserEventDesc
{
    FSceneScriptEventDesc Common;
    // Opaque external representation: SceneScript copies these bits without interpreting them.
    // SceneScript does not own any pointer represented by this word.
    unsigned int ExternalPayloadWord;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptUserEventDesc) == 0x30,
              "USER_EVENT common descriptor plus opaque payload");

// ATTACH: the routine; writer2b6f1..2b74b and tick47e13 prove the tail.
struct FSceneScriptAttachEventDesc
{
    FSceneScriptEventDesc Common;
    float Offset[3];
    int TargetType;
    int UpAxis;
    int FrontAxis;
    const char* TargetPart;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptAttachEventDesc) == 0x48,
              "ATTACH descriptor common eleven words and seven payload words");

// START_SOUND: the routine copies input+0x2c to event+0x54.
// /GetDesc hand this to the client; the branch computes
// playback offset = SoundStartTime - Common.StartMs + TimeMs.
struct FSceneScriptSoundEventDesc
{
    FSceneScriptEventDesc Common;
    int SoundStartTime;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptSoundEventDesc) == 0x30,
              "START_SOUND common descriptor plus start_time");

// START_SUB_SCENE: input+0x2c -> event+0x54; getter exposes twelve words.
struct FSceneScriptSubSceneEventDesc
{
    FSceneScriptEventDesc Common;
    float StartTime;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptSubSceneEventDesc) == 0x30,
              "START_SUB_SCENE common descriptor plus start_time");

// SUBTITLE: the routine stores word 11 at event +0x54; getter
// exposes the complete twelve-word descriptor.
struct FSceneScriptSubtitleEventDesc
{
    FSceneScriptEventDesc Common;
    unsigned int Strid;
};

static_assert(sizeof(FSceneScriptSubtitleEventDesc) == 0x30,
              "SUBTITLE common descriptor plus string resource id");

// START_MOTION's descriptor (baseline object 0x9c, the routine; the descriptor at +0x28 is
// filled by the routine and by the loader's Lua reads 0x06f25b5c..0x06f25e5f). This is what
// ICompoundControl slots 3 / 4 receive for a motion event and what slots 15 / 16 hand out.
struct FSceneScriptMotionEventDesc
{
    FSceneScriptEventDesc Common;       // +0x00 Type == kSceneScriptTypeStartMotion
    float StartTime;              // +0x2c "start_time"
    float TransTime;              // +0x30 "trans_time"
    float TimeScale;              // +0x34 "time_scale"
    float Weight;                 // +0x38 "weight"
    char const* Animation;        // +0x3c "animation" (the script name CharacterInfo knows)
    unsigned char Field40;        // +0x40 TODO: meaning not recovered (the routine copies it)
    unsigned char Pad41[3];
    float Heading;                // +0x44 "heading"
    char const* LockedBone;       // +0x48 "locked_bone"
    float TransScale;             // +0x4c "trans_scale"
    int Field50;                  // +0x50 TODO: meaning not recovered (the routine copies it)
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptMotionEventDesc) == 0x54,
              "module state reads the descriptor up to +0x50");

// START_IK's descriptor as the event hands it out (baseline: event +0x28, filled by the routine
// from the loader's input, whose Lua reads are 0x06f2a5ca..0x06f2a8d6). This is what IIKControl
// slots 3 / 4 / 5 receive and what IScriptEngine slots 15 / 16 hand out for an IK event. It is
// NOT the loader's input record (SceneScriptScene.h FSceneScriptIkEventDesc): the routine drops the input's
// "ik_id" (+0x2c) and puts the event object's own address there, 0x06f43f73
// `MOV [EBP+0x54],EBP` -- the key OutpostFrontier's SceneScript_Deformable matches a stop or an update
// against (the branch is the motion twin; the routine / the branch read +0x2c).
// Defaults when Type is not START_IK (0x06f440e4..0x06f44108): count 0, up 1, front 5,
// damping 1.0, both bytes 0, transition 1.0, target type 0, both names null.
struct FSceneScriptStartIkDesc
{
    FSceneScriptEventDesc Common;       // +0x00 Type == kSceneScriptTypeStartIk
    void const* Key;              // +0x2c the event object itself
    int CountToRoot;              // +0x30 "count_to_root"
    int LookAtUpAxis;             // +0x34 "up"
    int LookAtFrontAxis;          // +0x38 "front"
    float Damping;                // +0x3c "damping"
    unsigned char bPointAt;       // +0x40 "point_at"
    unsigned char bMoveTo;        // +0x41 "move_to"
    unsigned char Pad42[2];
    float TransitionDuration;     // +0x44 "transition_duration"
    char const* EndEffector;      // +0x48 "end_effector" (c_str of the event's copy)
    char const* TargetPart;       // +0x4c "target_part"
    int TargetType;               // +0x50 "target_type"
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptStartIkDesc) == 0x54,
              "module state writes the descriptor up to +0x50 (event +0x78)");

// CONNECT_HARDPOINTS' descriptor, as ICompoundControl slots 7 / 8 receive it (SceneScriptScene
// PackConnectDesc). OutpostFrontier reads exactly these four fields: SceneScript_Compound the routine /
// hand (PrimaryIndex, Hardpoint, SecondaryIndex, ParentHardpoint) to
// / the routine; SceneScript_Deformable the branch / the branch swap the pairs.
struct FSceneScriptConnectEventDesc
{
    FSceneScriptEventDesc Common;       // +0x00 Type == kSceneScriptTypeConnectHardpoints
    char const* Hardpoint;        // +0x2c on the primary entity
    char const* ParentHardpoint;  // +0x30 on the secondary entity
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptConnectEventDesc) == 0x34,
              "module state reads the descriptor up to +0x30");

// Event+0x58 / capture+0x78 / entity+0x138: 8 dword fog record (the routine / the routine).
struct FSceneScriptFog8
{
    unsigned char On;
    unsigned char Table;
    unsigned char Pad[2];
    int Color[3];
    int Mode;
    float Start;
    float End;
    float Density;
};

static_assert(sizeof(FSceneScriptFog8) == 32, "fog 8-dword record");

// SPATIAL_PROP_ANIM: 465f0 stores orientation/position, quaternion and
// axis/target data; table 5b2cc getter 4a570 exposes all thirty-two words.
struct FSceneScriptSpatialEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    float Orient[9];
    float Position[3];
    float Quaternion[4];
    float AxisRotAngle;
    int AxisRotAxis;
    const char* TargetPart;
    int TargetType;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptSpatialEventDesc) == 0x80,
              "SPATIAL descriptor thirty-two words");

// LIGHT_PROP_ANIM: 45660 copies mask + 36 raw target words;
// 4a4e0 returns the live 48-word descriptor at event +0x28.
struct FSceneScriptLightEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    FSceneScriptLightProps Target;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptLightEventDesc) == 0xc0,
              "LIGHT descriptor forty-eight words");

// REVERB_PROP_ANIM: 49150 copies mask plus four raw words; 4a6d0
// in table 5b3cc exposes environment integer bits and three floats.
struct FSceneScriptReverb4
{
    unsigned int Environment;
    float Volume;
    float Decay;
    float Damping;
};

static_assert(sizeof(FSceneScriptReverb4) == 0x10, "REVERB four raw words");

struct FSceneScriptReverbEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    FSceneScriptReverb4 Target;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptReverbEventDesc) == 0x40,
              "REVERB descriptor sixteen words");

// AUDIO_PROP_ANIM: 47280 copies mask and eight raw target words;
// 4a540 returns the complete twenty-word descriptor. ain/aout retain integer bits.
struct FSceneScriptAudio8
{
    float Attenuation;
    float Pan;
    float Dmin;
    float Dmax;
    int Ain;
    int Aout;
    float Atout;
    float Rmix;
};

static_assert(sizeof(FSceneScriptAudio8) == 0x20, "AUDIO six floats and two integer cones");

struct FSceneScriptAudioPropEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    FSceneScriptAudio8 Target;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptAudioPropEventDesc) == 0x50,
              "AUDIO_PROP descriptor common + mask + eight raw words");

// PSYS_PROP_ANIM: 48920 stores mask and sparam; the 4a670 getter
// in table 5b38c exposes both after the common eleven words.
struct FSceneScriptPsysPropEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    float Sparam;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptPsysPropEventDesc) == 0x34,
              "PSYS_PROP descriptor common + mask + sparam");

// FOG_PROP_ANIM: 48cc0 copies mask and eight payload words; table 5b3ac
// slot 2 (4a6a0) exposes the contiguous twenty-word descriptor.
struct FSceneScriptFogEventDesc
{
    FSceneScriptEventDesc Common;
    int Mask;
    FSceneScriptFog8 Target;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptFogEventDesc) == 0x50,
              "FOG descriptor common + mask + eight payload words");

// FLOOR_HEIGHT_ANIM: 495b7 copies a float, 495bd/49671 copy name/type;
// table 5b3ec slot 2 (4a700) exposes all fourteen words.
struct FSceneScriptFloorEventDesc
{
    FSceneScriptEventDesc Common;
    float FloorHeight;
    const char* TargetPart;
    int TargetType;
};

static_assert(sizeof(void*) != 4 || sizeof(FSceneScriptFloorEventDesc) == 0x38,
              "FLOOR descriptor common + float + name + type");

// Supplied ScriptEngine context facet, stored directly by 06f3a380 (no QI).
// No recovered IID/provider identity. This is distinct from IUserEventControl.
// Descriptor is the complete event packet, including type-specific trailing words.
struct ISceneScriptComContext : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(FSceneScriptUserEventDesc* Descriptor, const char* String) = 0; // +0x0c, 25fe4; return ignored
    virtual GENRESULT NEURONCOMPONENTS_API Slot4(FSceneScriptUserEventDesc* Descriptor, char* Buffer, unsigned int Size) = 0; // +0x10, 2b509
    virtual unsigned int NEURONCOMPONENTS_API Slot5(FSceneScriptUserEventDesc* Descriptor) = 0; // +0x14, 2b4e1
};

// Six-slot borrowed load parser, baseline vtable 06f5a1ec. The explicit
// destroy slot is used only for the temporary default parser.
struct IScriptEngine;
struct IFileSystem;
struct ISceneScriptLoadParser
{
    virtual void* NEURONCOMPONENTS_API Destroy(unsigned int Flags) = 0; // 0, baseline 06f250a0
    virtual unsigned int NEURONCOMPONENTS_API GetEntityCount() = 0;     // 1, baseline 06f2d4d0
    virtual unsigned int NEURONCOMPONENTS_API GetEventCount() = 0;      // 2, baseline 06f2d4e0
    virtual unsigned int NEURONCOMPONENTS_API GetDuration() = 0;        // 3, baseline 06f2d4f0
    virtual GENRESULT NEURONCOMPONENTS_API Load(IFileSystem* FileSystem) = 0; // 4, 06f250e0
    virtual GENRESULT NEURONCOMPONENTS_API Build(IFileSystem* FileSystem, IScriptEngine* Engine,
                                      ISceneScriptComContext* ComContext) = 0; // 5, 06f2a910
};

// Four-slot borrowed save parser. baseline vtable 06f5aef4; all methods
// use the modern NEURONCOMPONENTS calling convention across our DLL boundary.
struct IScriptEngine;
struct IFileSystem;
struct ISceneScriptSaveParser
{
    virtual void NEURONCOMPONENTS_API SetEntityCount(unsigned int Count) = 0; // 0, baseline 06f2a9e0
    virtual void NEURONCOMPONENTS_API SetEventCount(unsigned int Count) = 0;  // 1, baseline 06f2a9f0
    virtual void NEURONCOMPONENTS_API SetDuration(unsigned int Ms) = 0;       // 2, baseline 06f2aa00
    virtual GENRESULT NEURONCOMPONENTS_API Save(IFileSystem* FileSystem, IScriptEngine* Engine,
                                     ISceneScriptComContext* ComContext) = 0; // 3, baseline 06f2aa10
};

// 45-slot IScriptEngine (the branch). Names only where ABI callers exist.
// Slot = offset/4. RET evidence is in implementation tracker. Do not invent DA method names.
struct IScriptEngine : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* A, void* B) = 0;                      // 3  RET 0xC unused
    virtual GENRESULT NEURONCOMPONENTS_API Slot4Load(void* FileSystem) = 0;                   // 4  RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot5Save(void* FileSystem) = 0;                   // 5  RET 0x8 save the routine
    virtual unsigned long NEURONCOMPONENTS_API Slot6EntityCount() = 0;                       // 6  RET 0x4
    virtual void* NEURONCOMPONENTS_API Slot7EntityHandle(int Index) = 0;                     // 7  RET 0x8
    virtual void* NEURONCOMPONENTS_API Slot8(int Index) = 0;                                 // 8  RET 0x8 inorder snapshot
    virtual void* NEURONCOMPONENTS_API Slot9Entity(int Index) = 0;                           // 9  RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot10GetPos(int Index, float* OutVec3) = 0;      // 10 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot11GetOrient(int Index, float* OutMat9) = 0;   // 11 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot12Attach(int Index, void* EngineObj) = 0;     // 12 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot13Detach(int Index) = 0;                      // 13 RET 0x8
    virtual unsigned long NEURONCOMPONENTS_API Slot14EventCount() = 0;                       // 14 RET 0x4
    virtual void* NEURONCOMPONENTS_API Slot15EventByIndex(int Index) = 0;                    // 15 RET 0x8
    virtual void* NEURONCOMPONENTS_API Slot16EventById(int Id) = 0;                          // 16 RET 0x8
    virtual int NEURONCOMPONENTS_API Slot17GetDuration() = 0;                                // 17 RET 0x4
    virtual GENRESULT NEURONCOMPONENTS_API Slot18SetDuration(int Ms) = 0;                    // 18 RET 0x8 this+0x20
    virtual int NEURONCOMPONENTS_API Slot19GetClock() = 0;                                   // 19 RET 0x4
    // this in ECX, byte on stack → this+0x30, XOR EAX,EAX, RET 4.
    virtual GENRESULT __thiscall Slot20SetLoop(unsigned char Value) = 0;          // 20 thiscall
    virtual GENRESULT NEURONCOMPONENTS_API Slot21GetState() = 0;                             // 21 RET 0x4 get this+0x2c
    virtual GENRESULT NEURONCOMPONENTS_API Slot22Start(int Arg) = 0;                         // 22 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot23Stop() = 0;                                 // 23 RET 0x4
    virtual GENRESULT NEURONCOMPONENTS_API Slot24Pause() = 0;                                // 24 RET 0x4 the branch 6|7→5
    virtual GENRESULT NEURONCOMPONENTS_API Slot25Start(int Arg) = 0;                         // 25 JMP vtbl+0x58 = 22
    virtual GENRESULT NEURONCOMPONENTS_API Slot26Tick(int DtMs) = 0;                         // 26 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot27(void* A) = 0;                              // 27 RET 0x8 stub 0
    virtual GENRESULT NEURONCOMPONENTS_API Slot28(void* A, void* B) = 0;                     // 28 RET 0xC stub 0
    virtual GENRESULT NEURONCOMPONENTS_API Slot29(void* A, void* B, void* C) = 0;            // 29 RET 0x10 stub −1
    virtual GENRESULT NEURONCOMPONENTS_API Slot30(void* A, void* B, void* C) = 0;            // 30 RET 0x10 stub −1
    virtual unsigned long NEURONCOMPONENTS_API Slot31(int Index) = 0;                        // 31 RET 0x8 userprop count
    virtual const char* NEURONCOMPONENTS_API Slot32(int Index, int N) = 0;                   // 32 RET 0xC N-th userprop key
    virtual GENRESULT NEURONCOMPONENTS_API Slot33UserProp(void* Handle, const char* Key,
                                               char** Out) = 0;                  // 33 RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API Slot34SetUserProp(int Index, const char* Key,
                                                  const char* Value) = 0;        // 34 RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API Slot35AddEntity(void* Desc, int* OutIndex) = 0;  // 35 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot36RemoveEntity(int Index) = 0;                // 36 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot37UpdateEntity(int Index, void* Desc) = 0;    // 37 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot38AddEvent(const void* Desc, int* OutId) = 0;  // 38 RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API Slot39RemoveEvent(int Id) = 0;                    // 39 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot40SetCom(ISceneScriptComContext* Com) = 0;              // 40 RET 0x8 this+0x1c
    virtual GENRESULT NEURONCOMPONENTS_API Slot41SetLoadParser(ISceneScriptLoadParser* Parser) = 0;            // 41 RET 0x8 this+0x100
    virtual GENRESULT NEURONCOMPONENTS_API Slot42SetSaveParser(ISceneScriptSaveParser* Parser) = 0;            // 42 RET 0x8 this+0x104
    virtual GENRESULT NEURONCOMPONENTS_API Slot43LoadPath(const char* Path) = 0;             // 43 RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API Slot44SavePath(const char* Path) = 0;             // 44 RET 0x8 → slot 5
};
