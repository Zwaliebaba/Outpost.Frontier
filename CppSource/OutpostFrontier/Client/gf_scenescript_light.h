// Author: Giperion.

#pragma once

// SceneScript_Light and SceneScript_LightReference (RTTI .?AUSceneScript_Light@@ / .?AUSceneScript_LightReference@@,
// Client\GF\ScriptLight.cpp) -- the objects LIGHT entities are bound to.

// A SceneScript_Light (operator new(0xa4) in the routine, ctor the routine, identity vtable
// ) owns an engine light wrapper (the routine, gf_script.h) and a slot of the script
// light registry the field / the field, which the room draw enumerates (the routine).
// A SceneScript_LightReference (operator new(0x70) in the routine, ctor the routine, identity vtable
// ) looks up a created light of the same name in the running scripts (the routine)
// and forwards to it.

#include "common_csys.h"
#include "gf_scenescript_component.h"

struct FGfLightWrapper;

struct FSceneScriptLight : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptLight(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // ICompoundControl (0x005cd550 / 0x005cd554)
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl (0x005cd4c4..0x005cd4d0)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // ID3DLightControl (0x005cd464..0x005cd478)
    GENRESULT NEURONCOMPONENTS_API SetOn(int On) override;                          // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetLight(void const* D3dLight8) override;        // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetDirection(float* Dir3) override;              // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOn(int* OutOn) override;                      // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetLight(void* OutD3dLight8) override;           // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetDirection(float* OutDir3) override;           // 8  the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the routine

    FGfLightWrapper* Wrapper; // +0x6c the routine's wrapper
    int RegistryIndex;        // +0x70 slot in the field / the field, -1 when none
    Matrix Orient;            // +0x74 the last orientation SceneScript set (script frame)
    Vector Direction;         // +0x98 the light direction in Orient's frame (props +0x0c)
};

struct FSceneScriptLightReference : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptLightReference(Csys const& ScriptFrame, FGfScript* InOwner,
                         FSceneScriptEntitySnap const* Desc);

    // ICompoundControl
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // ID3DLightControl
    GENRESULT NEURONCOMPONENTS_API SetOn(int On) override;                          // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetLight(void const* D3dLight8) override;        // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetDirection(float* Dir3) override;              // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOn(int* OutOn) override;                      // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetLight(void* OutD3dLight8) override;           // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetDirection(float* OutDir3) override;           // 8  the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the branch

    FSceneScriptComponent* Target; // +0x6c the created light of that name, or null
};

// The reference forwarders shared (ICF) by SceneScript_LightReference, SceneScript_MarkerReference and
// SceneScript_SoundReference: a null target answers -1 without a message.
GENRESULT SceneScriptReferenceSetPosition(FSceneScriptComponent* Target, float* Position);       
GENRESULT SceneScriptReferenceSetOrientation(FSceneScriptComponent* Target, float* Orient9);     
GENRESULT SceneScriptReferenceGetPosition(FSceneScriptComponent* Target, float* OutPosition);    
GENRESULT SceneScriptReferenceGetOrientation(FSceneScriptComponent* Target, float* OutOrient9);  
GENRESULT SceneScriptReferenceSetTransform(FSceneScriptComponent* Target, float const* Xform12); 
GENRESULT SceneScriptReferenceGetTransform(FSceneScriptComponent* Target, float* OutXform12);    
void* SceneScriptReferenceGetEngineInstance(FSceneScriptComponent* Target);                      
