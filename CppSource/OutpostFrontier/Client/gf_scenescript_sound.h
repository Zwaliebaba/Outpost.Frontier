// Author: Giperion.

#pragma once

// SceneScript_Sound, SceneScript_PlayerEngineSound and SceneScript_SoundReference (RTTI .?AUSceneScript_Sound@@ /
// .?AUSceneScript_PlayerEngineSound@@ / .?AUSceneScript_SoundReference@@, Client\Gf\
// ScriptSound.cpp) -- the objects SOUND entities are bound to.

// A SceneScript_Sound (operator new(0xd8) in the routine, ctor the routine, identity vtable
// ) plays one sound archetype, a voice line of a speaking character
// ("VoiceProfile..." templates) or the room's background music / ambience
// ("BackgroundMusic" / "BackgroundAmbient"). Streamed archetypes are started by the owning
// script's event list (FGfScript::PlayStreamedSoundEvent); the others straight through the
// client sound heap (the routine). Identity slot 21 re-applies attenuation, pan and position to
// the playing handle; the Gf fade (the field +0x10, sound_config.h) is added only for a
// non-background sound whose script asked for it (+0x84).

// A SceneScript_PlayerEngineSound (operator new(0x70) in the routine, ctor the routine) drives the
// Gf ObjectShip's engine sound; a SceneScript_SoundReference (operator new(0x70) in the routine,
// ctor the routine) forwards to a SceneScript_Sound another script created.

#include "common_csys.h"
#include "gf_scenescript_component.h"

struct ISoundSource;

struct FSceneScriptSound : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptSound(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc);
    // (the routine is the scalar deleting destructor).
    ~FSceneScriptSound() override;

    // (thiscall, RET 8) -- the template: background music / ambience take the
    // global source, anything else (not a voice line) its sound archetype. Caller: the spawn
    // (0x0044e0f6).
    void BindTemplate(char const* InTemplate, unsigned char bInVoice);
    // (thiscall, RET 4) -- malloc_strcpy of the speaker unless it is null.
    void SetSpeaker(char const* InSpeaker);
    // (thiscall, RET 4) -- +0x75.
    void SetSpatial(unsigned char bInSpatial);
    // (thiscall) -- true for a playing voice line with +0x78 set whose handle's
    // slot 29 answers true. Caller: FGfScript::GetSoundTailMs (the routine, 0x0044f528).
    bool IsHoldingScript() const;

    // ICompoundControl (0x005ce290 / 0x005ce294)
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override;   // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;      // 12 the branch

    // ISoundControl (0x005ce234)
    GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int TimeMs) override;        // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) override;                     // 4  the routine
    GENRESULT NEURONCOMPONENTS_API SetAttenuation(float Attenuation) override;         // 5  the branch
    GENRESULT NEURONCOMPONENTS_API SetPan(float Pan) override;                         // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* OutAttenuation) override;     // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetPan(float* OutPan) override;                     // 8  the branch
    GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props8, int Mask) override; // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props8, int Mask) override;    // 10 the branch

    // IGeoTransformControl (0x005ce1f8)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;             // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;           // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;          // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;        // 6  the branch

    // identity tail
    void SetSpawnTable(FSceneScriptComponent** Table, int Count) override;       // 15 the branch
    void* GetEngineInstance() override;                                    // 17 the routine
    void RefreshSound() override;                                          // 21 the branch
    bool IsBackgroundSound() override;                                     // 22 the branch

    char* Template;                // +0x6c malloc'd template (the "VoiceProfile" prefix cut off)
    char* Speaker;                 // +0x70 malloc'd "speaker" user property of a voice line
    unsigned char bVoice;          // +0x74
    unsigned char bSpatial;        // +0x75 the entity's Flags bit 1
    unsigned char bAmbient;        // +0x76 "BackgroundAmbient"
    unsigned char bMusic;          // +0x77 "BackgroundMusic"
    unsigned char Field_0x78;      // +0x78 TODO: meaning not recovered; the routine writes 0 for
                                   //       the base / faction tokens and "$BGCS_Rellan_", 1 for
                                   //       "$BGCS_Faction_Rebuff_*"; the routine reads it
    unsigned char Pad79[3];
    void* Arch;                    // +0x7c Archetype::ISound (or VoiceMsg) to play, or null
    ISoundSource* Handle;          // +0x80 the playing source, or null
    unsigned char bUseScriptFade;  // +0x84 the owning script's +0x60, set by the spawn
    unsigned char Pad85[3];
    float Attenuation;             // +0x88 audio prop 0 (dB)
    float Pan;                     // +0x8c audio prop 1
    float Field_0x90;              // +0x90 audio prop 2, 1e9 -- TODO: meaning not recovered
    float Field_0x94;              // +0x94 audio prop 3, 1e9 -- TODO: meaning not recovered
    unsigned int Field_0x98;       // +0x98 audio prop 4, integer 360 -- TODO: meaning not recovered
    unsigned int Field_0x9c;       // +0x9c audio prop 5, integer 360 -- TODO: meaning not recovered
    float Field_0xa0;              // +0xa0 audio prop 6, 0 -- TODO: meaning not recovered
    float Field_0xa4;              // +0xa4 audio prop 7, 0 -- TODO: meaning not recovered
    Csys Local;                    // +0xa8 position 0, orientation identity; the handle takes
                                   //       the position (ISoundSource slot 35)

private:
    // (thiscall, RET 8) -- resolve the voice line for the speaking character:
    // VoiceId is the character's voice (SceneScript_Deformable FindActor's first out), Actor the
    // character itself. Caller: SetSpawnTable (0x0045736c).
    void ResolveVoice(void* VoiceId, void* Actor);
};

struct FSceneScriptPlayerEngineSound : FSceneScriptComponent
{
    // (thiscall, RET 0xc) -- base ctor, +0x6c = 0.
    FSceneScriptPlayerEngineSound(Csys const& ScriptFrame, FGfScript* InOwner,
                            FSceneScriptEntitySnap const* Desc);

    // ISoundControl (0x005ce508)
    GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int TimeMs) override;        // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) override;                     // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetAttenuation(float Attenuation) override;         // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* OutAttenuation) override;     // 7  the branch
    GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props8, int Mask) override; // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props8, int Mask) override;    // 10 the branch

    // identity tail
    void Stop() override;                                                  // 14 the branch
    void* GetEngineInstance() override;                                    // 17 the routine

    float Attenuation; // +0x6c
};

struct FSceneScriptSoundReference : FSceneScriptComponent
{
    // (thiscall, RET 0xc).
    FSceneScriptSoundReference(Csys const& ScriptFrame, FGfScript* InOwner,
                         FSceneScriptEntitySnap const* Desc);
    // (the routine is the scalar deleting destructor): +0x6c = 0, base dtor.
    ~FSceneScriptSoundReference() override;

    // ICompoundControl
    GENRESULT NEURONCOMPONENTS_API SetTransform(float const* Transform12) override;   // 11 the branch
    GENRESULT NEURONCOMPONENTS_API GetTransform(float* OutTransform12) override;      // 12 the branch

    // ISoundControl (0x005ce6e8)
    GENRESULT NEURONCOMPONENTS_API StartSound(void* Desc, int TimeMs) override;        // 3  the branch
    GENRESULT NEURONCOMPONENTS_API StopSound(void* Desc) override;                     // 4  the branch
    GENRESULT NEURONCOMPONENTS_API SetAttenuation(float Attenuation) override;         // 5  the branch
    GENRESULT NEURONCOMPONENTS_API SetPan(float Pan) override;                         // 6  the branch
    GENRESULT NEURONCOMPONENTS_API GetAttenuation(float* OutAttenuation) override;     // 7  the branch
    GENRESULT NEURONCOMPONENTS_API GetPan(float* OutPan) override;                     // 8  the branch
    GENRESULT NEURONCOMPONENTS_API SetAudioProps(float const* Props8, int Mask) override; // 9  the branch
    GENRESULT NEURONCOMPONENTS_API GetAudioProps(float* Props8, int Mask) override;    // 10 the branch

    // IGeoTransformControl (0x005ce6ac)
    GENRESULT NEURONCOMPONENTS_API SetPosition(float* Position) override;             // 3  the branch
    GENRESULT NEURONCOMPONENTS_API SetOrientation(float* Orient9) override;           // 4  the branch
    GENRESULT NEURONCOMPONENTS_API GetPosition(float* OutPosition) override;          // 5  the branch
    GENRESULT NEURONCOMPONENTS_API GetOrientation(float* OutOrient9) override;        // 6  the branch

    // identity tail
    void Stop() override;                                                  // 14 the routine
    void* GetEngineInstance() override;                                    // 17 the branch

    FSceneScriptComponent* Target; // +0x6c the created sound of that name, or null
};
