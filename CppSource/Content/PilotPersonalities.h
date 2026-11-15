// Author: Giperion.

#pragma once

// Content.dll TU45 (06f36f10-06f46940): pilot personalities from
// missions\pilots_population.ini / missions\pilots_story.ini.

// baseline keeps seventeen file-static std::map objects (the pilot map
// and one map per *Block section, the field..the field) and
// exposes the functions below to the rest of Content.dll.

#include "common_id.h"
#include "common_pub.h"

class INI_Reader;

// walk every header of an already opened pilots ini and file
// each *Block section / [Pilot] into its map. Returns true (baseline AL = 1).
bool __cdecl LoadPilotPersonalities(INI_Reader* Reader);

// erase every entry of the seventeen maps.
void __cdecl ClearPilotPersonalities();

// copy the JobBlock named by JobId into Personality->JobParams.
// false when JobId is empty or no such JobBlock was loaded.
bool __cdecl ApplyPilotJob(ID_String* JobId, pub::AI::Personality* OutPersonality);

// fill a Personality from the [Pilot] named by PilotId.
// false when the pilot is unknown (Personality untouched).
bool __cdecl GetPilotPersonality(unsigned int* PilotId, pub::AI::Personality* OutPersonality);

// GetPilotPersonality, then ApplyPilotJob(JobId) when JobId is
// not empty. false when the pilot, or a non-empty job, is unknown.
bool __cdecl GetPilotPersonalityWithJob(unsigned int* PilotId, ID_String* JobId,
                                        pub::AI::Personality* OutPersonality);
