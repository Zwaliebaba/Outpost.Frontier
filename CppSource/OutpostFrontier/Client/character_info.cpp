// Author: Giperion.

#include "character_info.h"

#include "NeuronRuntime.h"              // NeuronRuntime::Anim
#include "common_id.h"          // CreateID
#include "common_reputation.h"  // CostumeDescriptions, GetCostumeDescriptions
#include "common_misc.h"        // DAEngine_create_filesystem / DAEngine_release_filesystem
#include "neuroncomponents.h"              // FDUMP
#include "engine/sceneruntime.h"     // IAnimation2
#include "readfile.h"           // IFileSystem

#include <cstdint>
#include <string.h>

namespace
{
    char const kCharacterInfoCpp[] = "Client\\gf\\CharacterInfo.cpp";
    char const kFailedToOpen[] = "%s(%d) : *** WARNING: Failed to open %s\n";

    // The "no script" value of +0x2c (the ctor's MOV [EDI+0x2c],-1 and every CMP ...,-1).
    void* NoAnimScript()
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    // 's stack record for the enumeration callback: 0x00435261..0x00435274 stores
    // the database at +0 and the remembered file name at +4.
    struct FScriptEnumContext
    {
        FCharacterInfoDb* Db;
        std::string const* File;
    };

    // (cdecl): one Animation\Script directory -> AddScript(Name, Ctx->File).
    void __cdecl AddEnumeratedScript(char const* Name, void* Context)
    {
        FScriptEnumContext* const Ctx = static_cast<FScriptEnumContext*>(Context);
        Ctx->Db->AddScript(Name, Ctx->File);
    }

    // .
    FCharacterInfoDb G_CharacterInfoDb;
}

FCharacterInfoDb::FCharacterInfoDb()
    : AnimScript(NoAnimScript())
{
    // the list sentinel (0x18-byte node), the map head (0x30-byte node, colour
    // black, isnil), the second list sentinel (0xc-byte node), +0x2c = -1.
}

FCharacterInfoDb::~FCharacterInfoDb()
{
    // 0x00434bb2..0x00434bc8 ScriptFiles erased, 0x00434bcd..0x00434bea the map,
    // the queue; then +0x2c through IAnimation2 slot 4 (no test of
    // NeuronRuntime::Anim, as baseline) and back to -1. The containers' own destructors follow.
    ScriptFiles.clear();
    Scripts.clear();
    Queued.clear();
    if (AnimScript != NoAnimScript())
    {
        NeuronRuntime::Anim->CloseScript(AnimScript);
        AnimScript = NoAnimScript();
    }
}

void* FCharacterInfoDb::AcquireScript(unsigned Id)
{
    // . 0x00434d2c..0x00434d49: the routine lookup; no entry -> -1.
    std::map<unsigned, FCharacterScript>::iterator const It = Scripts.find(Id);
    if (It == Scripts.end())
    {
        return NoAnimScript();
    }
    FCharacterScript& Script = It->second;
    if (Script.RefCount == 0)
    {
        // DAEngine_create_filesystem(file, &fs, 0).
        char const* const Path = Script.File->c_str();
        IFileSystem* File = nullptr;
        DAEngine_create_filesystem(Path, &File, nullptr);
        if (File != nullptr)
        {
            // IAnimation2 slot 30 (+0x78)(AnimScript, fs, &name, 1),
            // then the file system released.
            char const* Names[1];
            Names[0] = Script.Name.c_str();
            NeuronRuntime::Anim->AddScriptNames(AnimScript, File, Names, 1);
            DAEngine_release_filesystem(&File);
        }
        else
        {
            // .
            FDUMP(0x100002, kFailedToOpen, kCharacterInfoCpp, 0x92, Path);
        }
    }
    // / 0x00434e00..0x00434e05: counted on both paths.
    ++Script.RefCount;
    return AnimScript;
}

void FCharacterInfoDb::ReleaseScript(unsigned Id)
{
    // .
    std::map<unsigned, FCharacterScript>::iterator const It = Scripts.find(Id);
    if (It == Scripts.end())
    {
        return;
    }
    FCharacterScript& Script = It->second;
    // DEC then JNZ -- only the reference that reaches zero unloads.
    --Script.RefCount;
    if (Script.RefCount != 0)
    {
        return;
    }
    // IAnimation2 slot 31 (+0x7c)(AnimScript, &name, 1).
    char const* Names[1];
    Names[0] = Script.Name.c_str();
    NeuronRuntime::Anim->RemoveScriptNames(AnimScript, Names, 1);
}

void* FCharacterInfoDb::AcquireScriptByName(char const* Name)
{
    // .
    return AcquireScript(CreateID(Name));
}

void FCharacterInfoDb::ReleaseScriptByName(char const* Name)
{
    // the routine's body with CreateID in front.
    ReleaseScript(CreateID(Name));
}

void FCharacterInfoDb::AddScript(char const* Name, std::string const* File)
{
    // 0x00434f66 CreateID(Name); 0x00434f7d..0x00434fd3 a string of Name;
    // the routine builds {id, {name, id, 0, File}}; 0x00435050
    // the unique insert, whose result is not looked at.
    unsigned const Id = CreateID(Name);
    FCharacterScript Script;
    Script.Name = Name;
    Script.Id = Id;
    Script.RefCount = 0;
    Script.File = File;
    Scripts.insert(std::map<unsigned, FCharacterScript>::value_type(Id, Script));
}

void FCharacterInfoDb::Load(CostumeDescriptions const* Descs)
{
    // . 0x00435118..0x00435130: +0x2c = IAnimation2 slot 29 (+0x74), no test of
    // NeuronRuntime::Anim and no release of a script already there.
    AnimScript = NeuronRuntime::Anim->CreateEmptyScript();

    // every "anim =" file of CostumeDescriptions +0x00.
    for (std::list<std::string>::const_iterator FileIt = Descs->AnimationNames.begin();
         FileIt != Descs->AnimationNames.end(); ++FileIt)
    {
        char const* const FileName = FileIt->c_str();

        // a file already in ScriptFiles (inline strcmp) is skipped.
        bool bKnown = false;
        for (std::list<std::string>::const_iterator KnownIt = ScriptFiles.begin();
             KnownIt != ScriptFiles.end(); ++KnownIt)
        {
            if (strcmp(FileName, KnownIt->c_str()) == 0)
            {
                bKnown = true;
                break;
            }
        }
        if (bKnown)
        {
            continue;
        }

        // remembered at the end of ScriptFiles; the scripts keep a
        // pointer to that element (list nodes never move).
        ScriptFiles.push_back(std::string(FileName));
        std::string const* const Stored = &ScriptFiles.back();

        // DAEngine_create_filesystem(file, &fs, 0).
        IFileSystem* File = nullptr;
        DAEngine_create_filesystem(FileName, &File, nullptr);
        if (File != nullptr)
        {
            // IAnimation2 slot 32 (+0x80)(the branch, fs, &{this,
            // file}), then the file system released.
            FScriptEnumContext Context;
            Context.Db = this;
            Context.File = Stored;
            NeuronRuntime::Anim->EnumFsScripts(reinterpret_cast<void*>(&AddEnumeratedScript), File,
                                       &Context);
            DAEngine_release_filesystem(&File);
        }
        else
        {
            // .
            FDUMP(0x100002, kFailedToOpen, kCharacterInfoCpp, 0x149, FileName);
        }
    }
}

void FCharacterInfoDb::QueueScript(char const* Name)
{
    // a known script's map value goes to the end of the queue (+0x20), count +1.
    std::map<unsigned, FCharacterScript>::iterator const It = Scripts.find(CreateID(Name));
    if (It == Scripts.end())
    {
        return;
    }
    Queued.push_back(&It->second);
}

void FCharacterInfoDb::FlushQueuedScripts()
{
    // .
    for (;;)
    {
        // an empty queue ends it.
        if (Queued.empty())
        {
            return;
        }

        // one pass. The first entry picks the file (EDI); entries of
        // another file stay. Up to 0x100 matching entries leave the queue with a reference
        // each, and the names of the ones nobody held yet are collected.
        std::string const* File = nullptr;
        char const* Names[0x100];
        int Count = 0;
        std::list<FCharacterScript*>::iterator It = Queued.begin();
        while (It != Queued.end())
        {
            FCharacterScript* const Script = *It;
            if (File == nullptr)
            {
                File = Script->File;
            }
            if (File != Script->File)
            {
                ++It;
                continue;
            }
            if (Count >= 0x100)
            {
                break;
            }
            if (Script->RefCount == 0)
            {
                Names[Count] = Script->Name.c_str();
                ++Count;
            }
            ++Script->RefCount;
            It = Queued.erase(It);
        }

        // nothing new to load -> the next round.
        if (Count <= 0)
        {
            continue;
        }

        // the file opened, IAnimation2 slot 30 (+0x78)(AnimScript, fs,
        // names, count), released.
        char const* const Path = File->c_str();
        IFileSystem* Fs = nullptr;
        DAEngine_create_filesystem(Path, &Fs, nullptr);
        if (Fs != nullptr)
        {
            NeuronRuntime::Anim->AddScriptNames(AnimScript, Fs, Names, Count);
            DAEngine_release_filesystem(&Fs);
        }
        else
        {
            // .
            FDUMP(0x100002, kFailedToOpen, kCharacterInfoCpp, 0x1c2, Path);
        }
    }
}

FCharacterInfoDb* GetCharacterInfoDb()
{
    // MOV EAX,the field.
    return &G_CharacterInfoDb;
}

void LoadCharacterInfo()
{
    // .
    G_CharacterInfoDb.Load(GetCostumeDescriptions());
}

void ShutdownCharacterInfo()
{
    // 0x00435504..0x0043555d ScriptFiles node by node, 0x0043555f..0x00435572 the
    // map (the routine over begin..end), 0x00435577..0x004355ae the queue; then
    // the script through IAnimation2 slot 4 (no test of NeuronRuntime::Anim) and
    // +0x2c = -1.
    G_CharacterInfoDb.ScriptFiles.clear();
    G_CharacterInfoDb.Scripts.clear();
    G_CharacterInfoDb.Queued.clear();
    if (G_CharacterInfoDb.AnimScript != NoAnimScript())
    {
        NeuronRuntime::Anim->CloseScript(G_CharacterInfoDb.AnimScript);
        G_CharacterInfoDb.AnimScript = NoAnimScript();
    }
}

void PreloadCharacterScripts()
{
    // every "preload =" name (CostumeDescriptions +0x0c) acquired by CreateID.
    CostumeDescriptions const* const Descs = GetCostumeDescriptions();
    for (std::list<std::string>::const_iterator It = Descs->PreloadNames.begin();
         It != Descs->PreloadNames.end(); ++It)
    {
        G_CharacterInfoDb.AcquireScript(CreateID(It->c_str()));
    }
}

void ResetCharacterScripts()
{
    // . 0x00435630..0x0043567e: every map node's +0x24 (value +0x14, RefCount)
    // cleared, in tree order.
    for (std::map<unsigned, FCharacterScript>::iterator It = G_CharacterInfoDb.Scripts.begin();
         It != G_CharacterInfoDb.Scripts.end(); ++It)
    {
        It->second.RefCount = 0;
    }
    // only with an IAnimation2 -- the old script closed (slot 4) when
    // there is one, then a new empty script (slot 29).
    if (NeuronRuntime::Anim == nullptr)
    {
        return;
    }
    if (G_CharacterInfoDb.AnimScript != NoAnimScript())
    {
        NeuronRuntime::Anim->CloseScript(G_CharacterInfoDb.AnimScript);
        G_CharacterInfoDb.AnimScript = NoAnimScript();
    }
    G_CharacterInfoDb.AnimScript = NeuronRuntime::Anim->CreateEmptyScript();
}
