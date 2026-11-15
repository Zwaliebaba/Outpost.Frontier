// Author: Giperion.

#pragma once

// Content.dll TU53 (06f5c760-06f6b840), voice-line durations: [Voice] / [Sound] sections of
// audio\voices_space_male.ini, voices_space_female.ini and voices_recognizable.ini.

#include <vector>

class INI_Reader;

// One [Sound] line, 12 bytes (vector the field..the field), sorted by
// (VoiceId, MsgId) unsigned.
struct FVoiceLine
{
    unsigned int VoiceId;                                    // +0x00 pub::GetVoiceID of [Voice] nickname
    unsigned int MsgId;                                      // +0x04 pub::GetNicknameId of msg
    float Duration;                                          // +0x08
};
static_assert(sizeof(FVoiceLine) == 0xc, "baseline voice line is 12 bytes");

// constructed by the branch, freed by the atexit the routine.
extern std::vector<FVoiceLine> GVoiceLines;

// append this file's lines, re-sort the table, return its size.
int __cdecl LoadVoiceLines(INI_Reader* Reader);
// the duration of (VoiceId, MsgId), or -1.0f.
float __cdecl FindVoiceLineDuration(unsigned int const* VoiceId, unsigned int const* MsgId);
