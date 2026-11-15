// Author: Giperion.

#pragma once

// SceneScript_Scene (RTTI .?AUSceneScript_Scene@@) -- the object a SCENE entity is bound to
// (operator new(0x78) in the routine, ctor the routine, identity vtable 0x005cdfec).

// A scene carries the ambient colour of the room (+0x6c, the first three dwords of the entity's
// 0x3c-byte tail) and answers SceneScript's fog and reverb facets. Neither facet looks at the object:
// fog reads and writes the room fog state the field (G_GfFog, gf_script.h), reverb goes
// straight to NeuronRuntime::Sound's EAX preview. Its identity table is SceneScript_Component's defaults plus
// two stdcall slots of its own, 23 / 24 (the ambient colour set / get).

// The spawn (the routine, entity type 9) remembers the last scene it made and hands it to every
// spawned object through identity slot 19; RoomObject's ambient override (the routine) reads the
// colour at +0x6c of the scene it was given.

// NAME. The C++ name is the RTTI name, SceneScript_Scene, with the F prefix. It is an exe-only class
// (its RTTI and vtable live in OutpostFrontier.exe) and must not be confused with scenescript.dll's own
// scene, FSceneScriptScene (src/SceneScript/SceneScriptScene.h) -- nothing crosses between the two.

#include "gf_scenescript_component.h"

struct FSceneScript_Scene : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScript_Scene(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // IFogControl (0x005cdec0; none of them reads `this`)
    GENRESULT NEURONCOMPONENTS_API SetOn(unsigned char On) override;                         // 3  the branch
    GENRESULT NEURONCOMPONENTS_API GetOn(unsigned char* OutOn) override;                     // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetColor(int const* Rgb3) override;                       // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetColor(int* OutRgb3) override;                          // 6  the branch
    GENRESULT NEURONCOMPONENTS_API SetRange(float Start, float End, float Density) override; // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetRange(float* OutStart, float* OutEnd,
                                 float* OutDensity) override;                     // 8  the branch
    GENRESULT NEURONCOMPONENTS_API SetTableMode(unsigned char Table, unsigned int Mode) override; // 9 the branch
    GENRESULT NEURONCOMPONENTS_API GetTableMode(unsigned char* OutTable,
                                     unsigned int* OutMode) override;             // 10 the branch
    GENRESULT NEURONCOMPONENTS_API SetPacked(unsigned char const* Packed32) override;        // 11 the routine
    GENRESULT NEURONCOMPONENTS_API GetPacked(unsigned char* Out32) override;                 // 12 the routine
    GENRESULT NEURONCOMPONENTS_API SetRangeValue(const float* Start, const float* End,
                                      const float* Density) override; // modern 13 +0x34

    // IReverbControl (0x005cdeac)
    GENRESULT NEURONCOMPONENTS_API SetReverb(unsigned int Environment, float Volume, float Decay,
                                  float Damping) override;                        // 3  the branch
    GENRESULT NEURONCOMPONENTS_API GetReverb(float* OutEnvironment, float* OutVolume, float* OutDecay,
                                  float* OutDamping) override;                    // 4  the branch

    // identity tail
    void* GetEngineInstance() override;                                           // 17 the routine

    // The scene's own identity slots (stdcall, `this` on the stack). No caller of either is
    // known in OutpostFrontier.exe; the RoomObject reads +0x6c directly.
    virtual GENRESULT NEURONCOMPONENTS_API SetAmbient(float const* Rgb3);                    // 23 the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetAmbient(float* OutRgb3);                       // 24 the branch

    float Ambient[3]; // +0x6c
};
