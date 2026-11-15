// Author: Giperion.

#include "gf_scenescript_sound.h"
#include "msvcrt_compat.h" // MsvcrtAtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "common_id.h"   // CreateID
#include "common_misc.h" // malloc_strcpy / malloc_strfree
#include "neuroncomponents.h"
#include "gf_character_object.h" // FCharacterObject::ResolveBgcsContextToken (the routine)
#include "gf_object_ship.h"
#include "gf_script_object.h"
#include "gf_scenescript_light.h" // SceneScriptReference* forwarders shared by every reference class
#include "menu_sound_arch.h"
#include "sound_archetype.h"
#include "sound_config.h"
#include "soundmanager.h"
#include "voice_profiles.h"


#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    char const* const GScriptSoundFile = "Client\\Gf\\ScriptSound.cpp";

    // The play key the routine reads (menu_sound_arch.h): +0 start offset, +4 flags byte.
    // baseline builds it on the stack and leaves the flags dword's upper bytes uninitialised; only
    // bits 0..2 are read.
    struct FSoundPlayKey
    {
        unsigned Offset;   // +0x00
        unsigned Flags;    // +0x04 bit 0 loop, bit 1 spatial, bit 2 streamer
        unsigned Pad[4];   // +0x08..+0x14 zeroed (0x00457533..0x00457557)
    };

    void CopyDword(void* Dst, void const* Src)
    {
        memcpy(Dst, Src, 4);
    }
}

// ---------------------------------------------------------------------------------------
// SceneScript_Sound
// ---------------------------------------------------------------------------------------

// . Base ctor; +0x90 / +0x94 = 0x4e6e6b28 (1e9f), +0x98 / +0x9c = integer 0x168;
// +0x6c / +0x70 / +0x74..+0x78 / +0x7c / +0x80 / +0x84 / +0x88 / +0x8c / +0xa0 / +0xa4 = 0;
// vptrs; +0xa8 position 0 and +0xb4 identity.
FSceneScriptSound::FSceneScriptSound(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Template(nullptr),
      Speaker(nullptr),
      bVoice(0),
      bSpatial(0),
      bAmbient(0),
      bMusic(0),
      Field_0x78(0),
      Arch(nullptr),
      Handle(nullptr),
      bUseScriptFade(0),
      Attenuation(0.0f),
      Pan(0.0f),
      Field_0x90(1e9f),
      Field_0x94(1e9f),
      Field_0x98(0x168),
      Field_0x9c(0x168),
      Field_0xa0(0.0f),
      Field_0xa4(0.0f)
{
    Pad79[0] = 0;
    Pad79[1] = 0;
    Pad79[2] = 0;
    Pad85[0] = 0;
    Pad85[1] = 0;
    Pad85[2] = 0;
    Local.Pos.x = 0.0f;
    Local.Pos.y = 0.0f;
    Local.Pos.z = 0.0f;
    MatrixSetIdentity(Local.Orient);
}

// . Both strings freed; a foreground sound leaves the script's event list
// (the routine with the handle, 0x00456f71) and its handle is stopped (slot 24) and released;
// a background one only releases the source it borrowed. Then the base dtor (the routine).
FSceneScriptSound::~FSceneScriptSound()
{
    malloc_strfree(&Template);
    malloc_strfree(&Speaker);
    if (bMusic == 0 && bAmbient == 0)
    {
        Owner->RemoveSoundEvent(Handle);
        if (Handle != nullptr)
        {
            Handle->Stop();
            Handle->Release();
            Handle = nullptr;
        }
    }
    if ((bMusic != 0 || bAmbient != 0) && Handle != nullptr)
    {
        Handle->Release();
        Handle = nullptr;
    }
}

// . 0x00456fff malloc_strcpy(+0x6c, Template); +0x74 = bVoice;
// "BackgroundAmbient" -> +0x76 = 1 and the routine(&+0x80); "BackgroundMusic" -> +0x77 = 1
// and the routine(&+0x80); otherwise, not a voice line and a non-empty template ->
// +0x7c = the routine(&CreateID(+0x6c)). The archetype is not Acquired here.
void FSceneScriptSound::BindTemplate(char const* InTemplate, unsigned char bInVoice)
{
    malloc_strcpy(&Template, InTemplate);
    bVoice = bInVoice;
    if (_stricmp(InTemplate, "BackgroundAmbient") == 0)
    {
        bAmbient = 1;
        BindClientBackgroundAmbient(reinterpret_cast<void**>(&Handle));
        return;
    }
    if (_stricmp(InTemplate, "BackgroundMusic") == 0)
    {
        bMusic = 1;
        BindClientBackgroundMusic(reinterpret_cast<void**>(&Handle));
        return;
    }
    if (bVoice == 0 && Template != nullptr && Template[0] != '\0')
    {
        ID_String TemplateId;
        TemplateId.Id = CreateID(Template);
        Arch = LookupClientSoundArch(&TemplateId);
    }
}

// .
void FSceneScriptSound::SetSpeaker(char const* InSpeaker)
{
    if (InSpeaker != nullptr)
        malloc_strcpy(&Speaker, InSpeaker);
}

// .
void FSceneScriptSound::SetSpatial(unsigned char bInSpatial)
{
    bSpatial = bInSpatial;
}

// . BL = 0; +0x80 and +0x78 both set -> the handle's slot 29 (+0x74); nonzero ->
// 1, else 0.
bool FSceneScriptSound::IsHoldingScript() const
{
    if (Handle == nullptr || Field_0x78 == 0)
        return false;
    return Handle->IsFinished() != 0;
}

// . Only for a voice line (+0x74 == 1) with a non-empty template. The message name
// starts as the template; the six "$BGCS_*" context tokens go through the speaking character
// (the routine, 0x00457129 / 0x00457162), and a "$BGCS_Rellan_<name>_<n>" template gets a
// random index 0..n-1 written over its last underscore and loses the leading "$B". The voice
// profile is the character's (VoiceId), the message CreateID(name) in it; either missing is an
// FDUMP and +0x7c keeps its value.

// The receiver of the routine is Actor as it came back from the spawn table's FindActor (only
// SceneScript_Deformable answers with one: its CharacterObject); with no actor baseline calls it on a
// null receiver, and so does the port -- no guard.
void FSceneScriptSound::ResolveVoice(void* VoiceId, void* Actor)
{
    if (bVoice != 1 || Template == nullptr || Template[0] == '\0')
        return;

    FCharacterObject const* const Talker = static_cast<FCharacterObject const*>(Actor);
    char const* MessageName = Template;
    if (_stricmp(Template, "$BGCS_Base_Name") == 0 || _stricmp(Template, "$BGCS_Base_Run_By") == 0 ||
        _stricmp(Template, "$BGCS_My_Faction_ML") == 0 ||
        _stricmp(Template, "$BGCS_My_Faction_FM") == 0)
    {
        MessageName = Talker->ResolveBgcsContextToken(Template); 
        Field_0x78 = 0;
    }
    if (_stricmp(Template, "$BGCS_Faction_Rebuff_ML") == 0 ||
        _stricmp(Template, "$BGCS_Faction_Rebuff_FM") == 0)
    {
        MessageName = Talker->ResolveBgcsContextToken(Template); 
        Field_0x78 = 1;
    }

    // . The local is 0x100 bytes (SUB ESP,0x100); the copy is unbounded,
    // as baseline's.
    char Scratch[0x100];
    if (_strnicmp(Template, "$BGCS_Rellan_", strlen("$BGCS_Rellan_")) == 0)
    {
        strcpy(Scratch, Template);
        char* const Underscore = strrchr(Scratch, '_');
        int const Count = MsvcrtAtoi(Underscore + 1);
        int const Product = rand() * Count;
        int const Pick = (Product + ((Product >> 31) & 0x7fff)) >> 15;
        _ltoa(Pick, Underscore, 10);
        MessageName = Scratch + 2;
        Field_0x78 = 0;
    }

    unsigned int const MessageId = CreateID(MessageName);
    unsigned int const Voice = static_cast<unsigned int>(reinterpret_cast<uintptr_t>(VoiceId));
    FVoiceProfile* const Profile = LookupClientVoice(Voice);
    if (Profile == nullptr)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Voice profile %u does not exist\n", GScriptSoundFile,
              0xe8, Voice);
        return;
    }
    FVoiceMsg* const Message = Profile->FindMessage(MessageId);
    if (Message != nullptr)
    {
        Arch = Message;
        return;
    }
    FDUMP(0x100001, "%s(%d) : *** ERROR: Voice profile %s (%x) does not support message %s",
          GScriptSoundFile, 0xe3, Profile->GetName(), Voice, MessageName);
}

// (identity, RET 8). +0xa8 = Transform+0x24, +0xb4 = the rotation, slot 21.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetTransform(float const* Transform12)
{
    Local.Pos.x = Transform12[9];
    Local.Pos.y = Transform12[10];
    Local.Pos.z = Transform12[11];
    memcpy(&Local.Orient, Transform12, sizeof(Local.Orient));
    RefreshSound();
    return GR_OK;
}

// (identity, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetTransform(float* OutTransform12)
{
    OutTransform12[9] = Local.Pos.x;
    OutTransform12[10] = Local.Pos.y;
    OutTransform12[11] = Local.Pos.z;
    memcpy(OutTransform12, &Local.Orient, sizeof(Local.Orient));
    return GR_OK;
}

// (sound facet, RET 0xc). Slot 4 first, through the facet's vtable. Nothing more
// while the owning script is held (+0x44). Offset = Desc+0x2c - StartMs + TimeMs, loop = Flags
// bit 3. With an archetype: a streamer is started by the script's event list
// (the routine(UniqueId, Offset) -> +0x80), anything else through the routine with the
// offset and {streamer 0, spatial +0x75, loop} -> +0x80. Then slot 21, with or without one.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::StartSound(void* Desc, int TimeMs)
{
    static_cast<ISoundControl*>(this)->StopSound(Desc);
    if (Owner->bPaused != 0)
        return GR_OK;

    FSceneScriptSoundEventDesc const* const Event = static_cast<FSceneScriptSoundEventDesc const*>(Desc);
    int const Offset = Event->SoundStartTime - Event->Common.StartMs + TimeMs;
    unsigned char const bLoop = (Event->Common.Flags & 8) != 0 ? 1 : 0;

    if (Arch != nullptr)
    {
        Archetype::ISound* const SoundArch = static_cast<Archetype::ISound*>(Arch);
        if (SoundArch->IsStreamer() != 0)
        {
            Handle = static_cast<ISoundSource*>(
                Owner->PlayStreamedSoundEvent(Event->Common.UniqueId, Offset));
        }
        else
        {
            FSoundPlayKey Key;
            memset(&Key, 0, sizeof(Key));
            unsigned char Flags = 0;
            unsigned char const bStreamer = SoundArch->IsStreamer();
            Flags = static_cast<unsigned char>(Flags ^ (((bStreamer << 2) ^ Flags) & 4));
            Flags = static_cast<unsigned char>(Flags ^ (((bSpatial << 1) ^ Flags) & 2));
            Flags = static_cast<unsigned char>(Flags ^ ((bLoop ^ Flags) & 1));
            Key.Flags = Flags;
            Key.Offset = static_cast<unsigned>(Offset);
            PlayClientSoundArch(Arch, reinterpret_cast<unsigned*>(&Key),
                                reinterpret_cast<void**>(&Handle), 0);
        }
    }
    RefreshSound();
    return GR_OK;
}

// (sound facet, RET 8). A foreground sound: out of the script's event list
// (the routine), and a handle stopped (slot 24), released and cleared. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::StopSound(void*)
{
    if (bMusic == 0 && bAmbient == 0)
    {
        Owner->RemoveSoundEvent(Handle);
        if (Handle != nullptr)
        {
            Handle->Stop();
            Handle->Release();
            Handle = nullptr;
        }
    }
    return GR_OK;
}

// (sound facet, RET 8). +0x88 = Value; the ambience also snaps the Gf fade to it
// (the routine on the field). Slot 21. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetAttenuation(float Value)
{
    Attenuation = Value;
    if (bAmbient != 0)
        SnapClientAudioFade(Value);
    RefreshSound();
    return GR_OK;
}

// (sound facet, RET 8). +0x8c = Value, slot 21. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetPan(float Value)
{
    Pan = Value;
    RefreshSound();
    return GR_OK;
}

// (sound facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetAttenuation(float* OutAttenuation)
{
    *OutAttenuation = Attenuation;
    return GR_OK;
}

// (sound facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetPan(float* OutPan)
{
    *OutPan = Pan;
    return GR_OK;
}

// (sound facet, RET 0xc). Mask bit i copies Props[i] into the i-th dword from
// +0x88; bit 0 (the attenuation) also snaps the Gf fade for the ambience. Slot 21. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetAudioProps(float const* Props8, int Mask)
{
    if ((Mask & 0x1) != 0)
    {
        float const Value = Props8[0];
        Attenuation = Value;
        if (bAmbient != 0)
            SnapClientAudioFade(Value);
    }
    if ((Mask & 0x2) != 0)
        CopyDword(&Pan, &Props8[1]);
    if ((Mask & 0x4) != 0)
        CopyDword(&Field_0x90, &Props8[2]);
    if ((Mask & 0x8) != 0)
        CopyDword(&Field_0x94, &Props8[3]);
    if ((Mask & 0x10) != 0)
        CopyDword(&Field_0x98, &Props8[4]);
    if ((Mask & 0x20) != 0)
        CopyDword(&Field_0x9c, &Props8[5]);
    if ((Mask & 0x40) != 0)
        CopyDword(&Field_0xa0, &Props8[6]);
    if ((Mask & 0x80) != 0)
        CopyDword(&Field_0xa4, &Props8[7]);
    RefreshSound();
    return GR_OK;
}

// (sound facet, RET 0xc). The same eight dwords out, by mask. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetAudioProps(float* Props8, int Mask)
{
    if ((Mask & 0x1) != 0)
        CopyDword(&Props8[0], &Attenuation);
    if ((Mask & 0x2) != 0)
        CopyDword(&Props8[1], &Pan);
    if ((Mask & 0x4) != 0)
        CopyDword(&Props8[2], &Field_0x90);
    if ((Mask & 0x8) != 0)
        CopyDword(&Props8[3], &Field_0x94);
    if ((Mask & 0x10) != 0)
        CopyDword(&Props8[4], &Field_0x98);
    if ((Mask & 0x20) != 0)
        CopyDword(&Props8[5], &Field_0x9c);
    if ((Mask & 0x40) != 0)
        CopyDword(&Props8[6], &Field_0xa0);
    if ((Mask & 0x80) != 0)
        CopyDword(&Props8[7], &Field_0xa4);
    return GR_OK;
}

// (geo facet, RET 8). +0xa8 = Position, slot 21. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetPosition(float* Position)
{
    Local.Pos.x = Position[0];
    Local.Pos.y = Position[1];
    Local.Pos.z = Position[2];
    RefreshSound();
    return GR_OK;
}

// (geo facet, RET 8). +0xb4 = the nine floats; no slot 21.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::SetOrientation(float* Orient9)
{
    memcpy(&Local.Orient, Orient9, sizeof(Local.Orient));
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetPosition(float* OutPosition)
{
    OutPosition[0] = Local.Pos.x;
    OutPosition[1] = Local.Pos.y;
    OutPosition[2] = Local.Pos.z;
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSound::GetOrientation(float* OutOrient9)
{
    memcpy(OutOrient9, &Local.Orient, sizeof(Local.Orient));
    return GR_OK;
}

// (identity, RET 8). A voice line asks every spawned entity in turn for the actor
// named by its speaker (slot 16) and stops at the first that answers; none is the "Speaker name"
// FDUMP. Either way the routine runs, with the two outs still null on a miss.
void FSceneScriptSound::SetSpawnTable(FSceneScriptComponent** Table, int Count)
{
    if (bVoice != 1)
        return;
    void* VoiceId = nullptr;
    void* Actor = nullptr;
    bool bFound = false;
    int Index = 0;
    for (Index = 0; Index < Count; ++Index)
    {
        FSceneScriptComponent* const Entity = Table[Index];
        if (Entity != nullptr && Entity->FindActor(Speaker, &VoiceId, &Actor))
        {
            bFound = true;
            break;
        }
    }
    if (!bFound)
    {
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: Speaker name %s for sound %s in script %s does not match any "
              "entity names, sound cannot play",
              GScriptSoundFile, 0x123, Speaker, Template, Owner->ScriptFile);
    }
    ResolveVoice(VoiceId, Actor);
}

// .
void* FSceneScriptSound::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// (identity). Fade = the routine (the field +0x10), zero for a background
// sound or one without +0x84. With a handle: slot 32 (+0x80) SetAttenuation(Fade + +0x88),
// slot 33 (+0x84) SetPan(+0x8c * 100.0 (the field, double), stored as a float), slot 35
// (+0x8c) with &+0xa8.
void FSceneScriptSound::RefreshSound()
{
    float Fade = GetClientAudioFade();
    if (bMusic != 0 || bAmbient != 0 || bUseScriptFade == 0)
        Fade = 0.0f;
    if (Handle == nullptr)
        return;
    Handle->SetAttenuation(Fade + Attenuation);
    Handle->SetPan(static_cast<float>(static_cast<double>(Pan) * 100.0));
    Handle->Slot35(&Local.Pos);
}

// (identity). +0x77 or +0x76.
bool FSceneScriptSound::IsBackgroundSound()
{
    return bMusic != 0 || bAmbient != 0;
}

// ---------------------------------------------------------------------------------------
// SceneScript_PlayerEngineSound
// ---------------------------------------------------------------------------------------

// .
FSceneScriptPlayerEngineSound::FSceneScriptPlayerEngineSound(Csys const& ScriptFrame, FGfScript* InOwner,
                                                 FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Attenuation(0.0f)
{
}

// (sound facet, RET 0xc). the routine; non-null -> the routine. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::StartSound(void*, int)
{
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->StartEngineSound();
    return GR_OK;
}

// (sound facet, RET 8). the routine on the ship. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::StopSound(void*)
{
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->StopEngineSound();
    return GR_OK;
}

// (sound facet, RET 8). Facet +0x68 (= +0x6c) = Value, then the routine(Value)
// on the ship. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::SetAttenuation(float Value)
{
    Attenuation = Value;
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->SetEngineSoundAttenuation(Value);
    return GR_OK;
}

// (sound facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::GetAttenuation(float* OutAttenuation)
{
    *OutAttenuation = Attenuation;
    return GR_OK;
}

// (sound facet, RET 0xc). Mask bit 0 only: +0x6c = Props[0], then the ship's
// with +0x6c. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::SetAudioProps(float const* Props8, int Mask)
{
    if ((Mask & 1) == 0)
        return GR_OK;
    CopyDword(&Attenuation, &Props8[0]);
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->SetEngineSoundAttenuation(Attenuation);
    return GR_OK;
}

// (sound facet, RET 0xc). Mask bit 0 only.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEngineSound::GetAudioProps(float* Props8, int Mask)
{
    if ((Mask & 1) != 0)
        CopyDword(&Props8[0], &Attenuation);
    return GR_OK;
}

// (identity) -- the routine; non-null -> JMP the routine.
void FSceneScriptPlayerEngineSound::Stop()
{
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->StopEngineSound();
}

// .
void* FSceneScriptPlayerEngineSound::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ---------------------------------------------------------------------------------------
// SceneScript_SoundReference
// ---------------------------------------------------------------------------------------

// . +0x6c = the routine(the field, 6, Desc->Name); null is a WARNING FDUMP.
FSceneScriptSoundReference::FSceneScriptSoundReference(Csys const& ScriptFrame, FGfScript* InOwner,
                                           FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Target(nullptr)
{
    Target = G_GfScriptManager.FindCreatedEntity(kSceneScriptEntitySound, Desc->Name);
    if (Target == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: invalid sound reference to name %s",
              GScriptSoundFile, 0x2f2, Desc->Name);
    }
}

// .
FSceneScriptSoundReference::~FSceneScriptSoundReference()
{
    Target = nullptr;
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetTransform(float const* Transform12)
{
    return SceneScriptReferenceSetTransform(Target, Transform12);
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetTransform(float* OutTransform12)
{
    return SceneScriptReferenceGetTransform(Target, OutTransform12);
}

// (sound facet) -- XOR EAX,EAX; RET 0xc: a reference never starts its target.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::StartSound(void*, int)
{
    return GR_OK;
}

// 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::StopSound(void*)
{
    return GR_OK;
}

// the target's sound facet slot 5, or -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetAttenuation(float Value)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->SetAttenuation(Value);
}

// slot 6, or -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetPan(float Value)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->SetPan(Value);
}

// slot 7, or -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetAttenuation(float* OutAttenuation)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->GetAttenuation(OutAttenuation);
}

// slot 8, or -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetPan(float* OutPan)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->GetPan(OutPan);
}

// slot 9, or -1 (RET 0xc).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetAudioProps(float const* Props8, int Mask)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->SetAudioProps(Props8, Mask);
}

// slot 10, or -1 (RET 0xc).
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetAudioProps(float* Props8, int Mask)
{
    if (Target == nullptr)
        return static_cast<GENRESULT>(-1);
    return static_cast<ISoundControl*>(Target)->GetAudioProps(Props8, Mask);
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetPosition(float* Position)
{
    return SceneScriptReferenceSetPosition(Target, Position);
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::SetOrientation(float* Orient9)
{
    return SceneScriptReferenceSetOrientation(Target, Orient9);
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetPosition(float* OutPosition)
{
    return SceneScriptReferenceGetPosition(Target, OutPosition);
}

// .
GENRESULT NEURONCOMPONENTS_API FSceneScriptSoundReference::GetOrientation(float* OutOrient9)
{
    return SceneScriptReferenceGetOrientation(Target, OutOrient9);
}

// +0x6c = 0.
void FSceneScriptSoundReference::Stop()
{
    Target = nullptr;
}

// .
void* FSceneScriptSoundReference::GetEngineInstance()
{
    return SceneScriptReferenceGetEngineInstance(Target);
}
