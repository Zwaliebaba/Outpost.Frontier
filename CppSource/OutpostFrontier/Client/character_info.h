// Author: Giperion.

#pragma once

// Client\gf\CharacterInfo.cpp (string 0x005caddc) -- the character animation script database.

// One object, the field (0x30 bytes, static init the branch -> the routine, atexit
// -> the routine). It knows every script named in the costume descriptions'
// "[Animations] anim =" files (bodyparts.ini, CostumeDescriptions +0x00) and loads a script into
// one shared IAnimation2 script (+0x2c) the first time somebody acquires it, reference-counted.
// Users hand the returned script to IDeformable::Play -- HUD_Comm's portrait (the routine) and
// the Gf character code.

// The class has no vtable and no RTTI, so its name is the port's.

#include <list>
#include <map>
#include <string>

class CostumeDescriptions;

// The map value (node +0x10), built by the routine.
struct FCharacterScript
{
    std::string Name;         // +0x00 the script (a directory under Animation\Script)
    unsigned Id;              // +0x10 CreateID(Name), the map key again
    int RefCount;             // +0x14 users; the script is loaded while this is non-zero
    std::string const* File;  // +0x18 the .utf the script lives in (an element of ScriptFiles)
};

class FCharacterInfoDb
{
public:
    // three empty containers, AnimScript -1.
    FCharacterInfoDb();
    // the three containers emptied, AnimScript closed (IAnimation2 slot 4) when
    // it is not -1, then the containers themselves.
    ~FCharacterInfoDb();

    // (thiscall, RET 4). Unknown id -> -1. Otherwise, on the first reference the
    // script is added to AnimScript (IAnimation2 slot 30) out of its .utf ("Failed to open"
    // FDUMP, line 0x92, when the file does not open -- the reference is counted anyway).
    // Returns AnimScript.
    void* AcquireScript(unsigned Id);
    // (thiscall, RET 4): a known id loses a reference; at zero the script leaves
    // AnimScript (IAnimation2 slot 31).
    void ReleaseScript(unsigned Id);
    // (thiscall, RET 4): AcquireScript(CreateID(Name)).
    void* AcquireScriptByName(char const* Name);
    // (thiscall, RET 4): ReleaseScript(CreateID(Name)), inlined.
    void ReleaseScriptByName(char const* Name);
    // (thiscall, RET 8): insert {CreateID(Name) -> {Name, id, 0, File}}; an id that
    // is already there keeps its first entry.
    void AddScript(char const* Name, std::string const* File);
    // (thiscall, RET 4): AnimScript = IAnimation2 slot 29 (a new empty script), then
    // every not-yet-seen file of Descs +0x00 is remembered in ScriptFiles and its
    // Animation\Script directories enumerated into AddScript (IAnimation2 slot 32 with the
    // callback the branch); "Failed to open" FDUMP (line 0x149) when the file does not open.
    void Load(CostumeDescriptions const* Descs);
    // (thiscall, RET 4): a known name is queued for FlushQueuedScripts.
    void QueueScript(char const* Name);
    // the queue in rounds -- per round every queued script of the first entry's
    // file (at most 0x100) takes a reference and leaves the queue, and the ones that were not
    // loaded yet are added to AnimScript in one IAnimation2 slot 30 call ("Failed to open"
    // FDUMP, line 0x1c2).
    void FlushQueuedScripts();

    std::list<std::string> ScriptFiles;           // +0x00 each "anim =" file once
    std::map<unsigned, FCharacterScript> Scripts; // +0x0c by CreateID(name)
    std::list<FCharacterScript*> Queued;          // +0x20 QueueScript's entries
    void* AnimScript;                             // +0x2c IAnimation2 script, -1 = none

private:
    FCharacterInfoDb(FCharacterInfoDb const&) = delete;
    FCharacterInfoDb& operator=(FCharacterInfoDb const&) = delete;
};

// &the field.
FCharacterInfoDb* GetCharacterInfoDb();
// Load(GetCostumeDescriptions()). Caller: the routine (0x0055885f).
void LoadCharacterInfo();
// the three containers emptied and AnimScript closed. Callers: 0x00558870 and
// WinMain's teardown (0x005b2202).
void ShutdownCharacterInfo();
// AcquireScript for every "preload =" name (CostumeDescriptions +0x0c). Caller:
// 's tail (0x00558864).
void PreloadCharacterScripts();
// every reference count back to 0 and, with an IAnimation2, AnimScript replaced by
// a new empty script. Callers: the routine (0x0054aee4) and the routine (0x00558a76).
void ResetCharacterScripts();
