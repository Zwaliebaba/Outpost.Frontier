// Oracle instrumentation (P3.4d, decision record 0008 point 10 and its Verification table):
// Content.dll's system records as SpaceSupport's SystemLoaded left them, its gate locks as
// GameSupport's LoadGame left them, and a player state's [mPlayer] as the C++ writer writes
// it, for the Rust comparison (crates/game-logic/tests/content_systems.rs). A new export, not
// part of the reference DLL's interface: only Oracle.exe sim calls it, for the session file
// CppSource/Oracle/system_loads.txt (`dump content_systems`) after every system has loaded
// (Server.dll's OracleDumpSystemLoads), so a game run never reaches it.
//
// Every file is `<Dir>\content_<table>.txt`, written as content_tables.txt's files are: one
// record per line, fields `key=value` separated by one space; ids and unsigned values
// decimal, signed values decimal with their sign, floats their bit pattern in eight lower-case
// hex digits, strings the hex of their bytes, lists as the format of each table says.
//
//   content_systems.txt       one line per record of GSystemInfos, in key order;
//   content_system_jumps.txt  the jump gates and holes of each record;
//   content_system_lanes.txt  the trade lanes of each record, with their rings and the rings'
//                             attack zones;
//   content_solar_npcs.txt    the solar NPCs of each record, with the vibe of the reputation
//                             handle and the affiliation the vibe has now (fact F17);
//   content_gate_locks.txt    SaveContentGateLocks' own text (the [locked_gates] of a save);
//   content_mplayer.txt       FContentPlayerState::Save of a synthetic state of player 7, then
//                             the same text read back by LoadContentPlayerState into a fresh
//                             state and written again (the first save also stays in
//                             content_mplayer_first.txt, from which it is read back).
//
// Nothing it writes reaches a player: the synthetic state is a local; reading it back registers
// player 7 with the gates the text locks, after the gate locks were written.

#include "ContentPlayerState.h"
#include "BaseDB.h"             // FContentBase::BaseId
#include "SystemInfo.h"         // GSystemInfos
#include "common_id.h"          // CreateID
#include "common_ini.h"
#include "common_server.h"      // pub::GetBaseID, pub::GetSystemID, pub::Reputation

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    unsigned int OracleBits(float Value)
    {
        unsigned int Bits = 0;
        memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    void OracleHex(FILE* Out, char const* Data, size_t Size)
    {
        for (size_t Index = 0; Index < Size; ++Index)
            fprintf(Out, "%02x", static_cast<unsigned char>(Data[Index]));
    }

    void OracleVector(FILE* Out, Vector const& V)
    {
        fprintf(Out, "%08x,%08x,%08x", OracleBits(V.x), OracleBits(V.y), OracleBits(V.z));
    }

    void OracleRotation(FILE* Out, FMatrix const& M, char const* Separator)
    {
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Column = 0; Column < 3; ++Column)
                fprintf(Out, "%s%08x", Row == 0 && Column == 0 ? "" : Separator, OracleBits(M.D[Row][Column]));
        }
    }

    FILE* OracleOpen(std::string const& Dir, char const* Table)
    {
        std::string const Path = Dir + "\\content_" + Table + ".txt";
        return fopen(Path.c_str(), "wb");
    }

    // "system= loads= local_faction= rpop= zones= paths= jumps= lanes= lane_rings= solar_npcs=
    // players= mission_objects=".
    int DumpSystems(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, FSystemInfo>::const_iterator It = GSystemInfos.begin();
             It != GSystemInfos.end(); ++It)
        {
            FSystemInfo const& S = It->second;
            fprintf(Out,
                    "system=%u loads=%d local_faction=%u rpop=%u zones=%u paths=%u jumps=%u lanes=%u "
                    "lane_rings=%u solar_npcs=%u players=%u mission_objects=%u\n",
                    It->first, S.LoadCount, S.LocalFaction, S.bRpopSolarDetection ? 1u : 0u,
                    static_cast<unsigned int>(S.Zones.size()), static_cast<unsigned int>(S.Paths.size()),
                    static_cast<unsigned int>(S.Jumps.size()), static_cast<unsigned int>(S.TradeLanes.size()),
                    static_cast<unsigned int>(S.TradeLaneByRing.size()),
                    static_cast<unsigned int>(S.SolarNpcs.size()), static_cast<unsigned int>(S.Players.size()),
                    static_cast<unsigned int>(S.MissionObjects.size()));
            ++Lines;
        }
        return Lines;
    }

    // "system= object= type= pos=x,y,z goto_system= goto_object=".
    int DumpJumps(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, FSystemInfo>::const_iterator It = GSystemInfos.begin();
             It != GSystemInfos.end(); ++It)
        {
            for (size_t Index = 0; Index < It->second.Jumps.size(); ++Index)
            {
                FSystemJump const& J = It->second.Jumps[Index];
                fprintf(Out, "system=%u object=%u type=%d pos=", It->first, J.ObjectId, J.Type);
                OracleVector(Out, J.Position);
                fprintf(Out, " goto_system=%u goto_object=%u\n", J.GotoSystem, J.GotoObject);
                ++Lines;
            }
        }
        return Lines;
    }

    // "system= lane= start=x,y,z end=x,y,z rotation=<9 bits, comma-separated> max_weight=
    // rings=<id:x:y:z:reputation:<9 bits, colon-separated>, '+'-separated>
    // candidates=<ring index:max weight:<zone id@weight, '/'-separated>, '+'-separated>".
    int DumpLanes(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, FSystemInfo>::const_iterator It = GSystemInfos.begin();
             It != GSystemInfos.end(); ++It)
        {
            for (size_t Index = 0; Index < It->second.TradeLanes.size(); ++Index)
            {
                FTradeLane const& L = It->second.TradeLanes[Index];
                fprintf(Out, "system=%u lane=%u start=", It->first, static_cast<unsigned int>(Index));
                OracleVector(Out, L.StartPosition);
                fprintf(Out, " end=");
                OracleVector(Out, L.EndPosition);
                fprintf(Out, " rotation=");
                OracleRotation(Out, L.Placement.Rotation, ",");
                fprintf(Out, " max_weight=%08x rings=", OracleBits(L.MaxWeight));
                for (size_t Ring = 0; Ring < L.Rings.size(); ++Ring)
                {
                    FTradeLaneRing const& R = L.Rings[Ring];
                    fprintf(Out, "%s%u:%08x:%08x:%08x:%u:", Ring == 0 ? "" : "+", R.ObjectId,
                            OracleBits(R.Position.x), OracleBits(R.Position.y), OracleBits(R.Position.z),
                            R.Reputation);
                    OracleRotation(Out, R.Placement.Rotation, ":");
                }
                fprintf(Out, " candidates=");
                for (size_t Ring = 0; Ring < L.RingCandidates.size(); ++Ring)
                {
                    FTradeLaneRingCandidates const& C = L.RingCandidates[Ring];
                    fprintf(Out, "%s%d:%08x:", Ring == 0 ? "" : "+", C.RingIndex, OracleBits(C.MaxWeight));
                    for (size_t Zone = 0; Zone < C.Candidates.size(); ++Zone)
                    {
                        fprintf(Out, "%s%u@%08x", Zone == 0 ? "" : "/", C.Candidates[Zone].first->ZoneId,
                                OracleBits(C.Candidates[Zone].second));
                    }
                }
                fprintf(Out, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    // "system= index= object= nickname=<hex> base= msg_id_prefix=<hex> state_graph= pilot=
    // reputation_group= vibe=<the handle's, `none` without one> affiliation=<the vibe's now,
    // `none` without a handle>".
    int DumpSolarNpcs(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, FSystemInfo>::const_iterator It = GSystemInfos.begin();
             It != GSystemInfos.end(); ++It)
        {
            for (size_t Index = 0; Index < It->second.SolarNpcs.size(); ++Index)
            {
                FSolarNpc const& N = It->second.SolarNpcs[Index];
                fprintf(Out, "system=%u index=%u object=%u nickname=", It->first,
                        static_cast<unsigned int>(Index), N.SpaceObjId);
                OracleHex(Out, N.Nickname.Data, N.Nickname.Length);
                fprintf(Out, " base=%u msg_id_prefix=", N.Base != nullptr ? N.Base->BaseId : 0u);
                OracleHex(Out, N.MsgIdPrefix.Data, N.MsgIdPrefix.Length);
                fprintf(Out, " state_graph=%d pilot=%u", N.StateGraph, N.bHasPersonality ? 1u : 0u);
                // The parameters' group is not kept by the NPC; the handle's affiliation is.
                if (N.ReputationHandle != nullptr)
                {
                    int const Vibe = N.ReputationHandle->Reputation;
                    unsigned int Affiliation = 0;
                    pub::Reputation::GetAffiliation(Vibe, Affiliation);
                    fprintf(Out, " vibe=%d affiliation=%u\n", Vibe, Affiliation);
                }
                else
                {
                    fprintf(Out, " vibe=none affiliation=none\n");
                }
                ++Lines;
            }
        }
        return Lines;
    }

    // The synthetic player state: every record kind and statistic once or twice.
    void FillPlayerState(FContentPlayerState* State)
    {
        unsigned int Base = 0;
        pub::GetBaseID(Base, "he01_01_Base");
        unsigned int SystemA = 0;
        unsigned int SystemB = 0;
        pub::GetSystemID(SystemA, "He02");
        pub::GetSystemID(SystemB, "He01");
        FVnpcRecord Vnpc = {{Base, CreateID("he0101_fix_bartender"), 3u, 1u}};
        State->Vnpcs.push_back(Vnpc);
        FVnpcRecord Second = {{Base, 0x80000001u, 0u, 2u}};
        State->Vnpcs.push_back(Second);
        FRumorRecord Rumor = {{5u, 2u}};
        State->Rumors.push_back(Rumor);
        FRumorRecord Other = {{6u, 1u}};
        State->Rumors.push_back(Other);
        State->bCanDock = 0;
        State->DockExceptions.push_back(11u);
        State->DockExceptions.push_back(12u);
        State->bCanTradelane = 1;
        FTlrException Pair = {{21u, 22u}};
        State->TradelaneExceptions.push_back(Pair);
        State->Stats.ShipTypesKilled[200u] = 1;
        State->Stats.ShipTypesKilled[100u] = 3;
        State->Stats.RandomMissionsCompleted[1] = 2;
        State->Stats.RandomMissionsAborted[0] = 1;
        State->Stats.RandomMissionsFailed[5] = 4;
        State->Stats.TotalCashEarned = 12345.678f;
        State->Stats.TotalTimePlayed = 3600.25f;
        State->Stats.SystemsVisited.push_back(SystemA);
        State->Stats.SystemsVisited.push_back(SystemB);
        State->Stats.BasesVisited.push_back(Base);
        State->Stats.HolesVisited.push_back(77u);
    }

    // The first save, then the save of the state read back from it. The first save goes
    // through <Dir>\content_mplayer_first.txt, which is read back from the disk.
    int DumpPlayerState(std::string const& Dir, FILE* Out)
    {
        FContentPlayerState First(7u);
        FillPlayerState(&First);
        std::string Text;
        {
            std::string const Path = Dir + "\\content_mplayer_first.txt";
            FILE* Scratch = fopen(Path.c_str(), "w+b");
            if (Scratch == nullptr)
                return -1;
            First.Save(Scratch);
            long const Size = ftell(Scratch);
            rewind(Scratch);
            Text.resize(Size > 0 ? static_cast<size_t>(Size) : 0u);
            size_t const Read = Text.empty() ? 0u : fread(&Text[0], 1, Text.size(), Scratch);
            Text.resize(Read);
            fclose(Scratch);
        }
        fwrite(Text.data(), 1, Text.size(), Out);
        INI_Reader Reader;
        Reader.open_memory(Text.data(), static_cast<unsigned int>(Text.size()));
        FContentPlayerState Second(7u);
        LoadContentPlayerState(&Second, &Reader);
        Second.Save(Out);
        int Lines = 0;
        for (size_t Index = 0; Index < Text.size(); ++Index)
            Lines += Text[Index] == '\n' ? 2 : 0;
        return Lines;
    }
}

// The tables in this order. Returns the number of lines written, -1 when a file cannot be
// opened.
extern "C" __declspec(dllexport) int __cdecl OracleDumpContentSystems(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const Tables[] = {"systems", "system_jumps", "system_lanes", "solar_npcs", "gate_locks",
                                  "mplayer"};
    int const Count = static_cast<int>(sizeof(Tables) / sizeof(Tables[0]));
    FILE* Files[sizeof(Tables) / sizeof(Tables[0])] = {};
    bool bOpen = true;
    for (int Index = 0; Index < Count; ++Index)
    {
        Files[Index] = OracleOpen(Root, Tables[Index]);
        bOpen = bOpen && Files[Index] != nullptr;
    }
    int Lines = -1;
    if (bOpen)
    {
        Lines = 0;
        Lines += DumpSystems(Files[0]);
        Lines += DumpJumps(Files[1]);
        Lines += DumpLanes(Files[2]);
        Lines += DumpSolarNpcs(Files[3]);
        SaveContentGateLocks(Files[4]);
        int const PlayerLines = DumpPlayerState(Root, Files[5]);
        Lines = PlayerLines < 0 ? -1 : Lines + PlayerLines;
    }
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Files[Index] != nullptr)
            fclose(Files[Index]);
    }
    return Lines;
}
