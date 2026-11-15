// Author: Giperion.

#pragma once

// SceneScript_Deformable (RTTI .?AVSceneScript_Deformable@@, Client\Gf\
// ScriptDeformable.cpp) -- the object a DEFORMABLE entity is bound to: a CharacterObject whose
// animations (CharacterInfo scripts through NeuronRuntime::Deform) and IK aims the script drives.
// operator new(0x9f8) in the routine (ctor call 0x0044df6c); identity vtable 0x005cd1ec.

// Built by FGfScript::SpawnDeformable (the routine -> the routine, gf_script_spawn.cpp) with
// the room's character (the routine / the routine), the sequence's, or a new one from the
// character factory the routine (gf_character_object.h).

#include "gf_scenescript_component.h"

struct FCharacterObject;

struct FSceneScriptDeformable : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptDeformable(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);
    // (the routine is the scalar deleting destructor).
    ~FSceneScriptDeformable() override;

    // ICompoundControl (0x005cd1f8..0x005cd21c)
    GENRESULT NEURONCOMPONENTS_API StartMotion(void* Desc) override;                // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopMotion(void* Desc) override;                 // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetFloorHeight(float Height) override;           // 5  the branch
    GENRESULT NEURONCOMPONENTS_API AdvanceTime(int DeltaMs) override;               // 6  the branch
    GENRESULT NEURONCOMPONENTS_API ConnectHardpoints(void* Desc) override;          // 7  the routine
    GENRESULT NEURONCOMPONENTS_API DisconnectHardpoints(void* Desc) override;       // 8  the routine
    GENRESULT NEURONCOMPONENTS_API GetHardpointXform(int TargetType, char const* Name,
                                          float* OutXform12) override;   // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetFloorHeight(float* OutHeight) override;       // 10 the branch
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override; // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;    // 12 the branch

    // IIKControl (0x005cd1a8..0x005cd1b0)
    GENRESULT NEURONCOMPONENTS_API StartIk(void* Desc, float* Position, float* Orientation) override;  
    GENRESULT NEURONCOMPONENTS_API StopIk(void* Desc) override;                                        
    GENRESULT NEURONCOMPONENTS_API UpdateIk(void* Desc, float* Position, float* Orientation) override; 

    // IGeoTransformControl (0x005cd188..0x005cd194)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;           
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;         
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;        
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;      

    // identity tail
    void Stop() override;                                                // 14 the routine
    bool FindActor(char const* InActorName, void** OutActorField,
                   void** OutActor) override;                            // 16 the branch
    void* GetEngineInstance() override;                                  // 17 the branch
    void SetScene(void* Scene) override;                                 // 19 the branch
    void SetFlags(unsigned char bVisible, unsigned char bLit, unsigned char bFog,
                  unsigned char bFlag14) override;                       // 20 the branch

    // (thiscall, RET 4) -- the character instance's frame (world). Caller: the
    // spawn the routine (0x0044dfd8).
    Csys GetCharacterCsys() const;
    // the character's deformable instance. Caller: the routine (0x0044dffb).
    void* GetDeformInstance() const;

    // One motion (baseline slot ctor the branch).
    struct FMotionSlot
    {
        FMotionSlot(); // {-1, 0, -1, 0}

        int Handle;           // +0x00 -1; IDeformable slot 7's out handle
        unsigned ScriptId;     // +0x04 0; CreateID of the animation name
        int EventId;           // +0x08 -1
        unsigned char bActive; // +0x0c 0
    };

    // One IK aim (baseline slot ctor the branch).
    struct FAimSlot
    {
        FAimSlot(); // zeros, Target identity

        void* Handle;          // +0x00 0; IDeformable slot 11's out handle
        void const* Key;        // +0x04 0; FSceneScriptStartIkDesc::Key
        Csys Target;            // +0x08 identity; the script-frame target the aim follows
        unsigned char bActive;  // +0x38 0
        unsigned char bStarted; // +0x39 0; start_aim succeeded
    };

    static const int kMotionSlots = 32;
    static const int kAimSlots = 32;

    FCharacterObject* Character;       // +0x6c 0 (written by the spawn)
    char* ActorName;                   // +0x70 0 (malloc'd by the spawn)
    float FloorHeight;                 // +0x74 0
    FMotionSlot Motions[kMotionSlots]; // +0x78
    FAimSlot Aims[kAimSlots];          // +0x278
};
