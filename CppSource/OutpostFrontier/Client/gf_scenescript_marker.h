// Author: Giperion.

#pragma once

// SceneScript_Marker and SceneScript_MarkerReference (RTTI .?AUSceneScript_Marker@@ /
// .?AUSceneScript_MarkerReference@@) -- the objects MARKER entities are bound to.

// A SceneScript_Marker (operator new(0x9c) in the routine, ctor the routine, identity vtable
// ) is nothing but a frame in world space: SceneScript sets and reads it relative to the
// script frame. A SceneScript_MarkerReference (operator new(0x70), ctor the routine, identity vtable
// ) forwards to a created marker of the same name in a running script; the spawn
// finds it (the routine) and writes it into +0x6c.

#include "gf_scenescript_component.h"

struct FSceneScriptMarker : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, Pos 0, Orient identity.
    FSceneScriptMarker(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // ICompoundControl (0x005cd908 / 0x005cd90c)
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl (0x005cd87c..0x005cd888)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // identity tail
    void* GetEngineInstance() override;                                  // 17 the routine

    Csys World; // +0x6c the marker in world space (position +0x6c, orientation +0x78)
};

struct FSceneScriptMarkerReference : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, +0x6c = 0.
    FSceneScriptMarkerReference(Csys const& ScriptFrame, FGfScript* InOwner,
                          FSceneScriptEntitySnap const* Desc);

    // ICompoundControl
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the branch

    FSceneScriptComponent* Target; // +0x6c the created marker, or null
};
