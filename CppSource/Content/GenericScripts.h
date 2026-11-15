// Author: Giperion.

#pragma once

// Content.dll TU34 (06f00670-06f03fa0): generic bar scripts (scripts\gcs\genericScripts.ini,
// [GenericScripts]) and the per-voice GCS exclusions (scripts\GCS\GCSExcl.ini,
// [GCS_Exclusions]).

#include "BaseDB.h"     // FBarCharacterList

#include <map>
#include <string>
#include <vector>

struct CharacterBehaviorDescription;   // common_iface.h

// 0x108 vectors of script names, index Posture + 2 * (Gender + 2 * Segment)
// (0x42 segments x 2 genders x 2 postures). Constructed by the branch (eh vector ctor,
// element ctor the branch), destroyed by the atexit the routine.
extern std::vector<std::string> GGenericScripts[0x108];

// voice id -> script names that voice must not play. Constructed by
// , destroyed by the atexit the routine.
extern std::map<unsigned int, std::vector<std::string>> GGcsExclusions;

// fill GGenericScripts from the given ini (BaseSupport ctor).
void __cdecl LoadGenericScripts(char const* Path);
// clear every GGenericScripts vector (BaseSupport dtor).
void __cdecl UnloadGenericScripts();
// fill GGcsExclusions from the given ini (LoadContentGameTables).
void __cdecl LoadGcsExclusions(char const* Path);

// a random script of the (Segment, Gender, Posture) list that VoiceId is not
// excluded from; null when there is none. Gender 1 selects the "male" lists, Posture 0 the
// "stand" lists.
char const* __cdecl PickGenericScript(int Segment, int Gender, int Posture, unsigned int VoiceId);
// PickGenericScript(0 /* FIDGET */, Gender, Posture, 0).
char const* __cdecl PickFidgetScript(int Gender, int Posture);

// (thiscall on the description, RET 8; callers the routine, inlined seven
// times in the bar scene creator the routine): Text is appended to the second string array
// with Value when it is non-null and non-empty. No bound check (32 slots), as in baseline.
void AppendBehaviorText(CharacterBehaviorDescription* Behavior, char const* Text,
                        unsigned int Value);

// (cdecl): a character on a placement gets the behavior built by the routine.
void __cdecl StartBarCharacterScript(FBarCharacterList::iterator Character);
