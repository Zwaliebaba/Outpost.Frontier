// Author: Giperion.

#pragma once

#include "beam_effect.h"

#include <windows.h>

#include <string>
#include <vector>

// Client EffectArch / VisEffect / BeamSpear-Bolt / EffectType / EffectLOD — the routine.

namespace Archetype
{
    struct FCliMaterialEntry;
}

// baseline VisEffect (Client\cmn\VisEffectArch.cpp), 0x38 bytes, held by value in the map
// . Default state from the map's node build (the routine, 0x0042eadf..0x0042eb07):
// both vectors empty, has_alpha 0, RefCount 0, AleHandle -1. baseline offsets in the comments;
// the port's layout is its own (no VC6 ABI), and the ale name is held in a buffer rather than
// as a StringAlloc'd CacheString.
struct FVisEffectArch
{
    // +0x00  CacheString: the StringAlloc'd nickname the routine stores (0x0042dca4); the
    // node build clears it first (CacheString::clear, the port's "").
    char const* Nickname = "";
    char ParticleFxPath[MAX_PATH];      // +0x08  "particlefx", StringAlloc'd
    unsigned long EffectCrc = 0;     // +0x04  "effect_crc" bit pattern (may be negative in source)
    std::vector<std::string> Textures; // +0x0c..+0x18  "textures", one per key
    bool bHasAlpha = false;          // +0x1c  bit 0, "has_alpha"
    int RefCount = 0;                // +0x20
    int AleHandle = -1;              // +0x24  the routine handle, -1 = not loaded
    std::vector<Archetype::FCliMaterialEntry*> TextureSlots; // +0x28..+0x34  the routine entries

    FVisEffectArch() = default;
    // (the map node pair's destructor, VisEffect at pair + 4): a VisEffect still
    // referenced gives back its texture slots and its .ale use first. Runs from the map's
    // teardown (atexit the branch in baseline), so the material cache (client_cliobj.cpp) and
    // the ALE registry must still be alive then -- see the note on G_VisEffects.
    ~FVisEffectArch();
    // Only ever built in place in G_VisEffects; a copy would release the same slots twice.
    FVisEffectArch(FVisEffectArch const&) = delete;
    FVisEffectArch& operator=(FVisEffectArch const&) = delete;
};

// (thiscall) -- VisEffectArch AddRef. On the first reference (RefCount <= 0) the
// texture libraries are (re)resolved when their count differs from the texture names, and the
// .ale is loaded through the routine. The count goes up on every path. True only when this call
// loaded the .ale successfully.
bool VisEffectArchAddRef(FVisEffectArch* Vis);
// (thiscall) -- VisEffectArch Release. At zero: release every texture slot
// (the routine) and the .ale (the routine), handle back to -1; true only then.
bool VisEffectArchRelease(FVisEffectArch* Vis);

// Client\cmn\ALELib.cpp: .ale files by name (map the field, compared with stricmp), each
// with a use count and the handle handed out from the field (starts at 1).
// empty name -> -1; known name -> ++count, its handle; otherwise, with
// NeuronRuntime::ParticleFx present, open the file through NeuronRuntime::Data, ParticleFx LoadNodeLibrary +
// FxEffectLibrary LoadAle, and register it with count 1 and a fresh handle. -1 on failure.
int AleLibLoad(char const* AleName);
// -1 for handle -1 or an unknown handle; otherwise --count (an ERROR FDUMP
// when it drops below zero) and the new count. Nothing is unloaded at zero.
int AleLibRelease(int Handle);
// (ALELib::Shutdown, client teardown 0x005b22fb) -- a WARNING FDUMP for every
// name whose count is still positive, then the registry emptied. Nothing is unloaded from
// ParticleFx. Returns how many names were still in use.
int AleLibShutdown();

// baseline Effect record (EffectArch.cpp) — operator_new(0x58) in the routine.
struct FEffectArch
{
    int EffectType;                      // +0x00  EFT_* index
    unsigned NicknameId;                 // +0x04  CreateID
    FVisEffectArch* VisEffect;           // +0x08
    char const* VisEffectName;           // +0x0C  CacheString: the found VisEffect's first field
    FVisEffectArch* VisGeneric;          // +0x10
    FBeamEffectArch* VisBeam;            // +0x14
    void* SndEffect;                     // +0x18  SoundArch* (LookupClientSoundArch / the routine)
    unsigned LgtEffectLen;               // +0x1C
    char LgtEffect[0x20];                // +0x20
    float LgtRangeScale;                 // +0x40
    float LgtTimeScale;                  // +0x44
    float LgtRadius;                     // +0x48
    unsigned char LgtColorR;             // +0x4C
    unsigned char LgtColorG;             // +0x4D
    unsigned char LgtColorB;             // +0x4E
    unsigned char LgtColorA;             // +0x4F
    unsigned char Flags50;               // +0x50  bit0 from EffectType.RunTime<0
    unsigned char Pad51[3];
    int RefCount;                        // +0x54
};
static_assert(sizeof(FEffectArch) == 0x58, "EffectArch 0x58");

// (cdecl Path, bReload; caller the [Data] effects = key, 0x005b0dd6 with 0) --
// binds the FxRuntime once (the routine), then reads [Effect] (new EffectArch into the
// registry; a repeated nickname is merged through the routine when bReload,
// else warned about and dropped), [EffectLOD] (the routine), [EffectType] (the routine),
// [VisEffect] (the routine) and [BeamSpear] / [BeamBolt] (the routine). Returns how many
// EffectArchs the registry gained.
int EffectArchLoadIni(char const* Path, bool bReload);

// (thiscall) -- EffectArch's constructor (also inlined in the routine): only
// the fields below are written; +0x21..+0x3f stay as they were.
void EffectArchConstruct(FEffectArch* Arch);
// (thiscall) -- EffectArch's destructor (inlined in the routine and
// ): with references still out, a warning, then every one of them dropped.
void EffectArchDestruct(FEffectArch* Arch);
// (client teardown 0x005b22aa) -- every registered EffectArch destroyed and
// freed, the registry emptied, then the routine unbinds the FxRuntime.
void EffectArchShutdown();

FVisEffectArch* FindVisEffectByNickname(char const* Nickname);
FBeamEffectArch* FindBeamEffectArchByNickname(char const* Nickname);
// (cdecl) -- the EffectArch registered under *NicknameId (baseline std::map
// lower_bound + key compare), null when there is none. baseline takes the key by
// pointer; every caller passes the address of a CreateID value.
FEffectArch* FindEffectArch(unsigned const* NicknameId);
FEffectArch* FindEffectArchByNickname(char const* Nickname);

// (thiscall, RET 4; the stack argument is never read) -- EffectArch::AddRef:
// VisEffect +0x08 through the routine, SoundArch +0x18 through its slot 3 (+0x0c), then
// ++RefCount (+0x54).
void EffectArchAddRef(FEffectArch* Arch, unsigned UnusedArg);
// (thiscall) -- EffectArch::Release: VisEffect through the routine, SoundArch
// through slot 4 (+0x10), then --RefCount.
void EffectArchRelease(FEffectArch* Arch);
