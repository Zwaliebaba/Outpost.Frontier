// Author: Giperion.

#pragma once

// SceneScript_Psys and SceneScript_PlayerEnginePsys (RTTI .?AUSceneScript_Psys@@ / .?AUSceneScript_PlayerEnginePsys@@,
// Client\GF\ScriptPsys.cpp) -- the objects PSYS entities are bound to.

// A SceneScript_Psys (operator new(0xac) in the routine, ctor the routine, identity vtable
// ) plays one [VisEffect] through a LightsCamera particle holder (lights_camera.h):
// SceneScript starts it (IPSysControl slot 3), ticks it with the frame the entity sits at, and ends it;
// the room draw hands it the camera (identity slot 18, through the routine).
// A SceneScript_PlayerEnginePsys (operator new(0x70) in the routine, ctor the routine, identity
// vtable 0x005cde3c) drives the Gf ObjectShip's engine effects instead ("PlayerShipEngines").

#include "common_csys.h"
#include "gf_scenescript_component.h"
#include "lights_camera.h"

struct FVisEffectArch;

struct FSceneScriptPsys : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptPsys(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // (thiscall, RET 4) -- the [VisEffect] named EffectName into +0x74, AddRef'd,
    // or an FDUMP. Caller: the spawn the routine (0x0044e3a6) with the entity's template.
    void BindVisEffect(char const* EffectName);

    // ICompoundControl (0x005cdc58 / 0x005cdc5c)
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl (0x005cdbcc..0x005cdbd8)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // IPSysControl (0x005cdb48..0x005cdb58)
    GENRESULT NEURONCOMPONENTS_API Start() override;                                // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopEffect() override;                           // 4  the branch
    GENRESULT NEURONCOMPONENTS_API Tick(int DeltaMs) override;                      // 5  the branch
    GENRESULT NEURONCOMPONENTS_API SetSparam(float Value) override;                 // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetSparam(float* OutValue) override;             // 7  the branch

    // identity tail
    void Stop() override;                                                // 14 the routine
    void* GetEngineInstance() override;                                  // 17 the routine
    void DrawEffects(ICamera* Camera) override;                          // 18 the branch

    FLightsCameraParticles Particles; // +0x6c the live effect and its VisEffect reference
    FVisEffectArch* Vis;              // +0x74 the bound [VisEffect], own reference
    Csys Local;                       // +0x78 the entity frame SceneScript set (no script-frame
                                      //       transform: position +0x78, orientation +0x84)
    float Sparam;                     // +0xa8
};

struct FSceneScriptPlayerEnginePsys : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, +0x6c = 0.
    FSceneScriptPlayerEnginePsys(Csys const& ScriptFrame, FGfScript* InOwner,
                           FSceneScriptEntitySnap const* Desc);

    // IPSysControl (0x005cdd58..0x005cdd68; slot 5 stays the branch)
    GENRESULT NEURONCOMPONENTS_API Start() override;                                // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopEffect() override;                           // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetSparam(float Value) override;                 // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetSparam(float* OutValue) override;             // 7  the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the branch

    float Sparam; // +0x6c
};
