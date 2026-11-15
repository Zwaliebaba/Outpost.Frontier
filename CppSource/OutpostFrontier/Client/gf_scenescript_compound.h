// Author: Giperion.

#pragma once

// SceneScript_Compound (RTTI .?AVSceneScript_Compound@@, Client\Gf\ScriptCompound.cpp)
// -- the object a COMPOUND entity is bound to: one engine instance (a room prop's, or the
// player's ship), motions played on it through NeuronRuntime::Anim, hardpoint connections, and its
// transform relative to the script frame with a fixed offset. operator new(0x190) at 0x0044de3b
// (the routine); identity vtable 0x005ccf6c.

// Built by FGfScript::SpawnCompound (the routine -> the routine, gf_script_spawn.cpp).

#include "common_goods.h" // HardpointSummary
#include "gf_scenescript_component.h"

struct FRoomObject;

struct FSceneScriptCompound : FSceneScriptComponent
{
    // (thiscall, RET 0x10).
    FSceneScriptCompound(Csys const& ScriptFrame, Csys const& InOffset, FGfScript* InOwner,
                   FSceneScriptEntitySnap const* Desc);
    // (the routine is the scalar deleting destructor).
    ~FSceneScriptCompound() override;

    // ICompoundControl
    GENRESULT NEURONCOMPONENTS_API StartMotion(void* Desc) override;                // 3  the routine
    GENRESULT NEURONCOMPONENTS_API StopMotion(void* Desc) override;                 // 4  the routine
    GENRESULT NEURONCOMPONENTS_API ConnectHardpoints(void* Desc) override;          // 7  the routine
    GENRESULT NEURONCOMPONENTS_API DisconnectHardpoints(void* Desc) override;       // 8  the routine
    GENRESULT NEURONCOMPONENTS_API GetHardpointXform(int TargetType, char const* Name,
                                          float* OutXform12) override;   // 9  the branch
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IGeoTransformControl (0x005ccf0c..0x005ccf18)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      // 6  the branch

    // identity tail
    void Stop() override;                                                // 14 the routine
    void* GetEngineInstance() override;                                  // 17 the branch
    void SetScene(void* Scene) override;                                 // 19 the branch
    void SetFlags(unsigned char bVisible, unsigned char bLit, unsigned char bFog,
                  unsigned char bFlag14) override;                       // 20 the branch

    // (thiscall, RET 4) -- the instance, and its hardpoints re-analyzed. The spawn
    // calls it (0x0044de67).
    void SetInstance(long InInstance);

    // every motion slot back to {-1, -1, 0}.
    void ResetMotionSlots();

    static const int kMotionSlots = 16;

    FRoomObject* Object;               // +0x6c the room object owning Instance, or null
    long Instance;                     // +0x70 -1
    void* AnimScript;                  // +0x74 IAnimation2 script, -1 = none
    int Category;                      // +0x78 -1 (written by the spawn; TODO meaning)
    unsigned char bUseScene;           // +0x7c not written by the ctor (the spawn sets it)
    Csys Offset;                       // +0x80 the entity's offset from its instance
    Csys Local;                        // +0xb0 identity; the last frame the script set
    unsigned long Playback[kMotionSlots]; // +0xe0 IAnimation2 playback ids, -1
    int EventId[kMotionSlots];         // +0x120 the START_MOTION's unique id, -1
    unsigned char bPlaying[kMotionSlots]; // +0x160
    HardpointSummary Hardpoints;       // +0x170 of Instance
};
