// Author: Giperion.

#pragma once

// SceneScript_Component (RTTI .?AVSceneScript_Component@@, Client\Gf\ScriptBase.cpp) --
// the base of every object a SceneScript script entity is bound to in OutpostFrontier.exe. Thirteen classes
// derive from it (Camera, CameraReference, Compound, Deformable, Light, LightReference, Marker,
// MarkerReference, Psys, PlayerEnginePsys, Scene, Sound, PlayerEngineSound, SoundReference),
// each in its own gf_scenescript_*.h.

// LAYOUT. Nine facets, one vptr each, in the order of the QueryInterface table at 0x005cca20
// (scenescript_controls.h lists it). The identity vtable (facet +0x00, ICompoundControl) carries the
// object's own thiscall virtuals after ICompoundControl's thirteen slots: 13 the scalar deleting
// destructor, 14..22 below. Every facet's INeuronComponent slots are the shared the branch /
// / the branch; the eight other facets reach them through the compiler's
// this-adjustor thunks 0x004501c0..0x00450330 (`SUB [ESP+4],N; JMP`), which C++ multiple
// inheritance produces for us.

// CONSTRUCTION. The Gf script spawn the routine and its factories the routine..the routine
// (gf_script_spawn.cpp) new the concrete class per entity type.

#include "common_csys.h"
#include "scenescript_controls.h"
#include "scenescript_iface.h"

struct FGfScript;
struct ICamera;

static_assert(sizeof(void*) == 4, "the facet offsets of the QI table (0x005cca20) are x86 offsets");

// The "not implemented" answer of every default slot (the branch, the branch..the branch:
// MOV EAX,0xfffffff9). neuroncomponents.h has no name for it.
static const GENRESULT kSceneScriptSlotNotImplemented = static_cast<GENRESULT>(0xFFFFFFF9);

struct FSceneScriptComponent : ICompoundControl,
                         ISoundControl,
                         IIKControl,
                         IGeoTransformControl,
                         ICameraControl,
                         ID3DLightControl,
                         IPSysControl,
                         IFogControl,
                         IReverbControl
{
    // (thiscall, RET 0xc). EntityIndex -1, RefCount 1, the script's frame copied,
    // Owner, the entity's type, a malloc_strcpy of its name (+0x38), the reference bit (Flags & 1).
    FSceneScriptComponent(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // 13: the routine (the routine is the scalar deleting variant) -- malloc_strfree(Name).
    virtual ~FSceneScriptComponent();

    // --- INeuronComponent, one body for all nine facets ---------------------------------------
    // (RET 0xc) -- the table at 0x005cca20, exact strcmp. A hit AddRefs the facet
    // (slot 1) and hands it out, 0; a miss writes null and answers -3.
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    // ++RefCount.
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    // see the .cpp: the last release frees the memory WITHOUT the destructor.
    unsigned long NEURONCOMPONENTS_API Release() override;

    // --- ICompoundControl defaults (SceneScript_Component 0x005cc6e0..0x005cc704) ------------------
    GENRESULT NEURONCOMPONENTS_API StartMotion(void* Desc) override;                 // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopMotion(void* Desc) override;                  // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetFloorHeight(float Height) override;            // 5  the branch: 0
    GENRESULT NEURONCOMPONENTS_API AdvanceTime(int DeltaMs) override;                // 6  the branch: 0
    GENRESULT NEURONCOMPONENTS_API ConnectHardpoints(void* Desc) override;           // 7  the branch
    GENRESULT NEURONCOMPONENTS_API DisconnectHardpoints(void* Desc) override;        // 8  the branch
    GENRESULT NEURONCOMPONENTS_API GetHardpointXform(int TargetType, char const* Name,
                                          float* OutXform12) override;    // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetFloorHeight(float* OutHeight) override;        // 10 the branch
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override;  // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;     // 12 the branch

    // --- ISoundControl defaults (0x005cc6ac..0x005cc6c8) --------------------------------------
    GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int TimeMs) override;      // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) override;                   // 4..8 the branch
    GENRESULT NEURONCOMPONENTS_API SetAttenuation(float Attenuation) override;
    GENRESULT NEURONCOMPONENTS_API SetPan(float Pan) override;
    GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* OutAttenuation) override;
    GENRESULT NEURONCOMPONENTS_API GetPan(float* OutPan) override;
    GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props8, int Mask) override; // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props8, int Mask) override;       // 10 the branch

    // --- IIKControl defaults (0x005cc690..0x005cc698) ----------------------------------------
    GENRESULT NEURONCOMPONENTS_API StartIk(void* Desc, float* Position, float* Orientation) override;  
    GENRESULT NEURONCOMPONENTS_API StopIk(void* Desc) override;                                        
    GENRESULT NEURONCOMPONENTS_API UpdateIk(void* Desc, float* Position, float* Orientation) override; 

    // --- IGeoTransformControl defaults (all the branch) ------------------------------------
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;

    // --- ICameraControl defaults (all the branch) ------------------------------------------
    GENRESULT NEURONCOMPONENTS_API SetFov(const float* Fov) override;
    GENRESULT NEURONCOMPONENTS_API CameraSlot4(void* Arg) override;
    GENRESULT NEURONCOMPONENTS_API SetAspect(const float* Aspect) override;
    GENRESULT NEURONCOMPONENTS_API SetNear(const float* Near) override;
    GENRESULT NEURONCOMPONENTS_API SetFar(const float* Far) override;
    GENRESULT NEURONCOMPONENTS_API GetFov(float* OutFov) override;
    GENRESULT NEURONCOMPONENTS_API CameraSlot9(void* Arg) override;
    GENRESULT NEURONCOMPONENTS_API GetAspect(float* OutAspect) override;
    GENRESULT NEURONCOMPONENTS_API GetNear(float* OutNear) override;
    GENRESULT NEURONCOMPONENTS_API GetFar(float* OutFar) override;

    // --- ID3DLightControl defaults (0x005cc610..0x005cc624, all the branch) -----------------
    GENRESULT NEURONCOMPONENTS_API SetOn(int On) override;
    GENRESULT NEURONCOMPONENTS_API SetLight(void const* D3dLight8) override;
    GENRESULT NEURONCOMPONENTS_API SetDirection(float* Dir3) override;
    GENRESULT NEURONCOMPONENTS_API GetOn(int* OutOn) override;
    GENRESULT NEURONCOMPONENTS_API GetLight(void* OutD3dLight8) override;
    GENRESULT NEURONCOMPONENTS_API GetDirection(float* OutDir3) override;

    // --- IPSysControl defaults (0x005cc5ec..0x005cc5fc) --------------------------------------
    GENRESULT NEURONCOMPONENTS_API Start() override;                                 // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopEffect() override;                            // 4  the branch
    GENRESULT NEURONCOMPONENTS_API Tick(int DeltaMs) override;                       // 5..7 the branch
    GENRESULT NEURONCOMPONENTS_API SetSparam(float Value) override;
    GENRESULT NEURONCOMPONENTS_API GetSparam(float* OutValue) override;

    // --- IFogControl defaults (0x005cc5b4..0x005cc5d8) ---------------------------------------
    GENRESULT NEURONCOMPONENTS_API SetOn(unsigned char On) override;                 // 3..6 the branch
    GENRESULT NEURONCOMPONENTS_API GetOn(unsigned char* OutOn) override;
    GENRESULT NEURONCOMPONENTS_API SetColor(int const* Rgb3) override;
    GENRESULT NEURONCOMPONENTS_API GetColor(int* OutRgb3) override;
    GENRESULT NEURONCOMPONENTS_API SetRange(float Start, float End, float Density) override; // 7 the branch
    GENRESULT NEURONCOMPONENTS_API GetRange(float* OutStart, float* OutEnd,
                                 float* OutDensity) override;                     // 8 the branch
    GENRESULT NEURONCOMPONENTS_API SetTableMode(unsigned char Table, unsigned int Mode) override; // 9 the branch
    GENRESULT NEURONCOMPONENTS_API GetTableMode(unsigned char* OutTable,
                                     unsigned int* OutMode) override;             // 10 the branch
    GENRESULT NEURONCOMPONENTS_API SetPacked(unsigned char const* Packed32) override; // 11, 12 the branch
    GENRESULT NEURONCOMPONENTS_API GetPacked(unsigned char* Out32) override;
    GENRESULT NEURONCOMPONENTS_API SetRangeValue(const float* Start, const float* End,
                                      const float* Density) override; // modern 13 +0x34

    // --- IReverbControl defaults (0x005cc59c / 0x005cc5a0, the branch) ----------------------
    GENRESULT NEURONCOMPONENTS_API SetReverb(unsigned int Environment, float Volume, float Decay,
                                  float Damping) override;
    GENRESULT NEURONCOMPONENTS_API GetReverb(float* OutEnvironment, float* OutVolume, float* OutDecay,
                                  float* OutDamping) override;

    // --- the identity table's own thiscall virtuals (SceneScript_Component 0x005cc70c..0x005cc72c) --
    // 14 (+0x38) -- the script stops: the entity lets go of what it holds. Base: RET.
    virtual void Stop();
    // 15 (+0x3c) -- the script's spawned table. Base: RET 8.
    virtual void SetSpawnTable(FSceneScriptComponent** Table, int Count);
    // 16 (+0x40) -- the actor named Name, if this entity is it. Base: XOR AL,AL; RET 0xc.
    virtual bool FindActor(char const* ActorName, void** OutActorField, void** OutActor);
    // 17 (+0x44) -- the engine instance the entity moves. Base: _purecall.
    virtual void* GetEngineInstance() = 0;
    // 18 (+0x48) -- the effects pass (the routine 0x0044f98f, per spawned entity). Base: RET 4.
    virtual void DrawEffects(ICamera* Camera);
    // 19 (+0x4c) -- the scene entity whose ambient override the room object takes (RoomObject
    // +0x0c, the routine). Base: RET 4.
    virtual void SetScene(void* Scene);
    // 20 (+0x50) -- RoomObject +0x10 / +0x11 / +0x12 / +0x14. Base: RET 0x10.
    virtual void SetFlags(unsigned char bVisible, unsigned char bLit, unsigned char bFog,
                          unsigned char bFlag14);
    // 21 (+0x54) -- re-apply the entity's sound state to what plays it. Only SceneScript_Sound has a
    // body (the branch); the Gf fade tick (the routine 0x0045671c) sends it to every spawned
    // entity of the playing scripts. Base: RET.
    virtual void RefreshSound();
    // 22 (+0x58) -- a background (music / ambience) sound. Only SceneScript_Sound has a body
    // (the branch); the spawn skips the initial property writes for one (0x0044f0d5).
    // Base: the routine (XOR AL,AL; RET).
    virtual bool IsBackgroundSound();

    // (thiscall, RET 8) -- a created (not reference) entity of this type and name.
    bool MatchesEntity(int InType, char const* InName) const;

    // (thiscall, RET 0x10) -- NeuronRuntime::Hardpoint slot 6 from the Owner's entity
    // IndexB's hardpoint HpB to entity IndexA's HpA; "Cannot connect" FDUMP on failure.
    void ConnectEntityHardpoints(int IndexA, char const* HpA, int IndexB, char const* HpB);
    // (thiscall, RET 0x10) -- NeuronRuntime::Engine slot 48 on the same pair; "Cannot
    // disconnect" FDUMP unless it answers 1.
    void DisconnectEntityHardpoints(int IndexA, char const* HpA, int IndexB, char const* HpB);

    int EntityIndex;          // +0x24 -1 (written by the spawn)
    unsigned long RefCount;   // +0x28 1
    Csys Frame;               // +0x2c the owning script's frame (FGfScript +0x00)
    FGfScript* Owner;         // +0x5c
    int Type;                 // +0x60 kSceneScriptEntity* (FSceneScriptEntitySnap +0x00)
    char* Name;               // +0x64 malloc'd copy of FSceneScriptEntitySnap +0x38
    unsigned char bReference; // +0x68 FSceneScriptEntitySnap +0x04 bit 0
};
