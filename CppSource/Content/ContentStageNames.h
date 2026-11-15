// Author: Giperion.

#pragma once

// baseline .data table PTR_s_base_0_rank_06fce5d0: 43 {name, value} pairs mapping a story
// stage name to the story mission number ("base_0_rank" = 0 ... "mission_end" = 0x2a),
// last entry {null, 0}. Read by name-lookup loops that walk all 43 entries
// (loop bound 0x158 bytes): TU19 06eb418e / 06eb41da, TU22 06ed4029 / 06ed4070,
// TU47 the routine (06f4aa5f). One object in baseline -> declared once here,
// defined in RandomMissionDB.cpp.
struct FContentStageName
{
    char const* Name;
    int Value;
};

extern FContentStageName const GContentStageNames[43];
