// Author: Giperion.

#pragma once

// Client voice archetypes (Client\cmn\Voice.cpp).

// RTTI names from OutpostFrontier: Archetype::IVoice (pure base, vtable
// ) implemented by VoiceProfile (vtable 005cacec); Archetype::IVoiceMsg
// implemented by VoiceMsg (vtable 005cabfc).

// loads `voices =` files listed in OutpostFrontier.ini [Data];
// resolves a profile by CreateID(nickname); the routine clears
// the registry on engine shutdown. baseline keeps the registry in the VC6
// std::map at the field (head the field, size the field) — the port
// uses std::map, layout is deliberately not baseline-compatible.

#include <list>
#include <map>
#include <string>

#include "common_id.h"
#include "sound_archetype.h"

struct FVoiceProfile;

// VoiceMsg — baseline object 0x14 bytes, vtable 005cabfc (the 15 slots of Archetype::ISound, via
// Archetype::IVoiceMsg; RTTI in sound_archetype.h). It IS a sound archetype: HUD_Comm hands it
// to the sound heap the routine directly (0x004cb188 / 0x004cc01b).
// is the default ctor; the routine copies +4..+0x13.
struct FVoiceMsg : Archetype::IVoiceMsg
{
    ID_String Id;            // +0x04 CreateID(msg)
    int SmArchId;            // +0x08 ISoundManager archetype, -1 when not created
    FVoiceProfile* Owner;    // +0x0C
    signed char Priority;    // +0x10 `Priority =`
    signed char Attenuation; // +0x11 `attenuation =`
    short RefCount;          // +0x12

    FVoiceMsg();

    // slot 0 the branch — MOV EAX,[ECX+8].
    int GetSmArchId() override { return SmArchId; }
    // slot 1 the branch — Owner->GetSoundFile(Id).
    char const* GetFileName() override;
    // slot 2 the branch — MOV EAX,2.
    unsigned GetSourceType() override { return 2; }
    // slot 3 the routine — create the wav archetype on first use, ++RefCount; SETNZ (id != -1).
    unsigned char Acquire() override;
    // slot 4 the routine — --RefCount, destroy archetype at 0; returns MOVSX of the count.
    int Release() override;
    // slot 5 the branch — MOVSX [+0x12].
    int GetUseCount() override { return RefCount; }
    // slot 6 the branch — MOVSX [+0x11].
    int GetAttenuation() override { return Attenuation; }
    // slots 7/8 the branch — FLD -1.0f (the field).
    float GetRangeMin() override { return -1.f; }
    float GetRangeMax() override { return -1.f; }
    // slot 9 the routine — XOR AL,AL (not streamed).
    unsigned char IsStreamer() override { return 0; }
    // slot 10 the routine — MOV AL,1 (2D).
    unsigned char Is2d() override { return 1; }
    // slot 11 the branch — FLD 0.0f (the field).
    float GetRandomPitch() override { return 0.f; }
    // slot 12 the routine — MOV EAX,ECX.
    Archetype::ISound* GetLinked() override { return this; }
    // slot 13 the routine — XOR AL,AL.
    unsigned char IsPitchBendable() override { return 0; }
    // slot 14 the branch — MOVSX [+0x10].
    int GetPriority() override { return Priority; }
};

// VoiceProfile — baseline object 0x50 bytes, vtable 005cacec (the 8 slots of Archetype::IVoice,
// sound_archetype.h). baseline callers reach every method through that table (e.g. VoiceMsg
// slot 1 the branch calls the owner's slot 1, slot 3 the routine its slot 2).
struct FVoiceProfile : Archetype::IVoice
{
    ID_String Id;                                  // +0x04 CreateID(nickname)
    char* Name;                                    // +0x08 StringAlloc(nickname)
    std::map<unsigned int, FVoiceMsg> Messages;    // +0x0C
    std::list<std::string> Scripts;                // +0x20 `script =`
    std::string UtfPath;                           // +0x2C audio\<nickname>.utf, relative to the data root (O31)
    std::map<unsigned int, std::string> SoundFiles; // +0x3C `file =` per msg

    FVoiceProfile();

    // slot 0 the branch — ignored while UtfPath is set.
    void SetSoundFile(unsigned int MsgId, char const* File) override;
    // slot 1 the branch — "0x%X" of the msg id when the profile has a .utf.
    char const* GetSoundFile(unsigned int MsgId) override;
    // slot 2 the branch — NeuronRuntime::Data CreateInstance("FileSystem"/"UTF", UtfPath).
    void* OpenScriptFileSystem() override;
    // slot 3 the branch.
    unsigned int GetId() const override { return Id.Id; }
    // slot 4 the branch — ID_String::get_string, i.e. the hex id, not Name.
    char const* GetName() const override { return Id.get_string(); }
    // slot 5 the branch.
    FVoiceMsg* FindMessage(unsigned int MsgId) override;
    // slot 6 the branch — &Scripts.
    std::list<std::string>& GetScripts() override { return Scripts; }
    // slot 7 the branch — always ""; RET 4, the one argument is not read.
    char const* GetUnusedString(void* /*Unused*/) const override { return ""; }

    // — resolve DATA\AUDIO\<Name>.utf into UtfPath when it exists.
    void ResolveScriptFile();
};

// — parse one `voices =` file. Returns the number of profiles added.
int LoadClientVoicesIni(char const* FullPath);

// — profile by CreateID(nickname), null when unknown.
FVoiceProfile* LookupClientVoice(unsigned int VoiceId);

// — drop every profile (engine shutdown, the routine).
void ClearClientVoices();

// Registry size (baseline the field).
unsigned int GetClientVoiceCount();

// (the six `$BGCS_*` context tokens) is FCharacterObject::ResolveBgcsContextToken
// (gf_character_object.h), called on the speaking character.
