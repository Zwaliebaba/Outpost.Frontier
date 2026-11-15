// Author: Giperion.

// Content.dll TU53: voice-line durations. See Voices.h.

#include "Voices.h"

#include "ContentSort.h"   // ContentSort (the VC6 std::sort of LoadVoiceLines)
#include "common_ini.h"
#include "common_server.h"

#include <algorithm>

std::vector<FVoiceLine> GVoiceLines;

namespace
{
    // The ordering inlined in the routine's sort (06f67b3d) and the routine's search
    // (06f677e0): VoiceId first, then MsgId, both unsigned.
    bool VoiceLineLess(FVoiceLine const& Left, FVoiceLine const& Right)
    {
        if (Left.VoiceId < Right.VoiceId)
            return true;
        if (Left.VoiceId > Right.VoiceId)
            return false;
        return Left.MsgId < Right.MsgId;
    }
}

// (06f67840-06f67bbe).
int __cdecl LoadVoiceLines(INI_Reader* Reader)
{
    // [ESP+0x10]: zeroed once per call and kept across sections.
    unsigned int VoiceId = 0;

    while (Reader->read_header())
    {
        if (Reader->is_header("Voice"))
        {
            while (Reader->read_value())
            {
                if (Reader->is_value("nickname"))
                    pub::GetVoiceID(VoiceId, Reader->get_value_string());
            }
        }
        else if (Reader->is_header("Sound"))
        {
            unsigned int MsgId = 0;
            double Duration = -1.0;
            while (Reader->read_value())
            {
                if (Reader->is_value("msg"))
                    MsgId = pub::GetNicknameId(Reader->get_value_string());
                else if (Reader->is_value("duration"))
                    Duration = Reader->get_value_float(0);
            }

            // FCOMP against -1.0 (the field), TEST AH,0x44 + JNP skips on
            // equal; not-equal and unordered both insert.
            if (!(Duration == -1.0))
            {
                FVoiceLine Line;
                Line.VoiceId = VoiceId;
                Line.MsgId = MsgId;
                Line.Duration = static_cast<float>(Duration);
                GVoiceLines.insert(GVoiceLines.end(), 1, Line);
            }
        }
    }

    // VC6 std::sort of the whole table = ContentSort (median the routine,
    // partition the routine, _Sort the routine, insertion sort the routine and the
    // unguarded tail the routine), so equal keys keep baseline's order.
    ContentSort(GVoiceLines.data(), GVoiceLines.data() + GVoiceLines.size(), VoiceLineLess);
    return static_cast<int>(GVoiceLines.size());
}

// (06f67780-06f67830): lower_bound, then an exact-key check.
float __cdecl FindVoiceLineDuration(unsigned int const* VoiceId, unsigned int const* MsgId)
{
    FVoiceLine Key;
    Key.VoiceId = *VoiceId;
    Key.MsgId = *MsgId;
    Key.Duration = 0.0f;
    std::vector<FVoiceLine>::iterator It =
        std::lower_bound(GVoiceLines.begin(), GVoiceLines.end(), Key, VoiceLineLess);
    if (It != GVoiceLines.end() && Key.MsgId == It->MsgId && Key.VoiceId == It->VoiceId)
        return It->Duration;
    return -1.0f;
}
