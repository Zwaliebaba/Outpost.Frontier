// Author: Giperion.

#pragma once

// Common-internal declarations shared by the three TUs of baseline's Reputation.cpp /
// CharDescrip.cpp cluster: common_costume.cpp, common_reputation.cpp and common_vibe.cpp.
// Nothing here is exported; everything is defined in common_reputation.cpp.

#include "common_reputation.h"

#include <map>
#include <vector>

namespace CmnReputation
{
    // A record of the group registry the field (std::map keyed by the 16-bit MakeId value;
    // record = node+0x10): +0x00 nickname TString<16>, +0x14 ids_name, +0x18 ids_info,
    // +0x1c ids_short_name, +0x20 feelings map (Reputation::Startup 0631eb11-0631eba1).
    struct FGroup
    {
        TString<16> Nickname;
        int IdsName;
        int IdsInfo;
        int IdsShortName;
        std::map<unsigned, float> Feelings;
    };

    // The story-faction ids, filled lazily by EnsureStoryFactionIds.
    extern std::vector<unsigned> GStoryFactionIds;

    FGroup const* FindGroupConst(unsigned GroupId);
    void EnsureStoryFactionIds();
}

// The NEURONCOMPONENTS.DLL stricmp import baseline calls (common_neuroncomponents_stricmp.cpp).
int CallNeuronComponentsStricmp(const char* Left, const char* Right);
