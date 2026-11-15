// Author: Giperion.

#pragma once

// SceneScript_Camera and SceneScript_CameraReference (RTTI .?AUSceneScript_Camera@@ / .?AUSceneScript_CameraReference@@,
// Client\Gf\ScriptCamera.cpp) -- the objects CAMERA and MONITOR entities
// are bound to.

// A SceneScript_Camera (operator new(0x70), ctor the routine, identity vtable 0x005ccbbc) owns one
// slot of the camera pool the field (G_GfCameraPool, gf_script.h): its geo and camera facets
// write the slot, and push into the slot's room camera only while the slot is the active one.
// A SceneScript_CameraReference (operator new(0x70), ctor the routine, identity vtable 0x005ccd5c)
// holds another script's camera at +0x6c and forwards every facet call to it.

#include "gf_scenescript_component.h"

class FRoomCamera;

struct FSceneScriptCamera : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, then +0x6c = -1 (no pool slot yet).
    FSceneScriptCamera(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);

    // (thiscall, RET 0xc) -- G_GfCameraPool.Allocate(Priority, Camera,
    // bCanBeActive) into +0x6c. Callers: the spawn factories the routine (0x0044da1f, 1),
    // (0x0044dc02, 0), the routine (0x0044dced, 0).
    void Register(FRoomCamera* Camera, int Priority, unsigned char bCanBeActive);

    // ICompoundControl (0x005ccbe8 / 0x005ccbec)
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl (0x005ccb5c..0x005ccb68)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // ICameraControl (0x005ccb24..0x005ccb48; slots 4, 5, 9, 10 stay the branch)
    GENRESULT NEURONCOMPONENTS_API SetFov(const float* Fov) override;                      // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetNear(const float* Near) override;                    // 6  the branch
    GENRESULT NEURONCOMPONENTS_API SetFar(const float* Far) override;                      // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetFov(float* OutFov) override;                  // 8  the branch
    GENRESULT NEURONCOMPONENTS_API GetNear(float* OutNear) override;                // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetFar(float* OutFar) override;                  // 12 the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the routine

    int PoolSlot; // +0x6c index into G_GfCameraPool, -1 until Register
};

struct FSceneScriptCameraReference : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, then +0x6c = 0. The spawn the routine
    // writes the target afterwards (0x0044dc84 / 0x0044dd62).
    FSceneScriptCameraReference(Csys const& ScriptFrame, FGfScript* InOwner,
                          FSceneScriptEntitySnap const* Desc);

    // ICompoundControl
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // ICameraControl
    GENRESULT NEURONCOMPONENTS_API SetFov(const float* Fov) override;                      // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetNear(const float* Near) override;                    // 6  the branch
    GENRESULT NEURONCOMPONENTS_API SetFar(const float* Far) override;                      // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetFov(float* OutFov) override;                  // 8  the branch
    GENRESULT NEURONCOMPONENTS_API GetNear(float* OutNear) override;                // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetFar(float* OutFar) override;                  // 12 the branch

    // identity tail
    void Stop() override;                                                // 14 the branch
    void* GetEngineInstance() override;                                  // 17 the branch

    FSceneScriptComponent* Target; // +0x6c the referenced camera, or null
};
