// Oracle instrumentation (P3.4b, decision record 0008 point 5 and its Verification table):
// Content.dll's static tables, as LoadContentGameTables (Encounters.cpp) left them, written
// as one text file per table for the Rust comparison (crates/game-logic/tests/
// content_tables.rs). A new export, not part of the reference DLL's interface: only
// Oracle.exe sim calls it, for the session file CppSource/Oracle/content_tables.txt, so a
// game run never reaches it.
//
// Every file is `<Dir>\content_<table>.txt`, one record per line, fields `key=value`
// separated by one space. Ids and unsigned values are decimal, signed values decimal with
// their sign, floats their bit pattern in eight lower-case hex digits, strings the hex of
// their bytes, lists comma-separated (empty when the list is). Maps are written in key
// order, vectors in their order.

// The headers in Encounters.cpp's order (LoadContentGameTables' own file).
#include "Encounters.h"         // GFormations
#include "BaseSupport.h"        // GNewsItems
#include "GenericScripts.h"     // GGcsExclusions
#include "Voices.h"             // GVoiceLines
#include "ContentInternal.h"
#include "FactionProps.h"       // GFactionProps, GFactionNames, GFactionEmpathy, GFactionGroups
#include "NpcShips.h"           // GNpcShipArchs
#include "RandomMissionDB.h"    // GRmShipClasses, GRmShipMissionProperties, GRmRankDifficulties
#include "RandomMissionGen.h"   // GVoiceProperties
#include "common_text.h"        // FmtStr

#include <cstdio>
#include <cstring>
#include <string>

// The tables whose globals are file-static: PilotPersonalities.cpp, LootProps.cpp and
// Support.cpp write their own lines. Each returns the number of lines it wrote.
int __cdecl OracleContentDumpPilots(FILE* Out);
int __cdecl OracleContentDumpLoot(FILE* Loot, FILE* Phantom);
int __cdecl OracleContentDumpToughness(FILE* Out);

namespace
{
    unsigned int OracleBits(float Value)
    {
        unsigned int Bits = 0;
        memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    void OracleHex(FILE* Out, void const* Data, size_t Size)
    {
        unsigned char const* Bytes = static_cast<unsigned char const*>(Data);
        for (size_t Index = 0; Index < Size; ++Index)
            fprintf(Out, "%02x", Bytes[Index]);
    }

    // A StringCache string as lower-case hex: the cache interns without regard to case, so
    // the spelling it hands back is the first one interned (D785).
    void OracleLowerHex(FILE* Out, char const* Text)
    {
        if (Text == nullptr)
            return;
        for (; *Text != '\0'; ++Text)
        {
            unsigned char Ch = static_cast<unsigned char>(*Text);
            if (Ch >= 'A' && Ch <= 'Z')
                Ch = static_cast<unsigned char>(Ch + ('a' - 'A'));
            fprintf(Out, "%02x", Ch);
        }
    }

    void OracleIds(FILE* Out, char const* Key, std::vector<unsigned int> const& Ids)
    {
        fprintf(Out, " %s=", Key);
        for (size_t Index = 0; Index < Ids.size(); ++Index)
            fprintf(Out, "%s%u", Index == 0 ? "" : ",", Ids[Index]);
    }

    // head, body, left hand, right hand, the accessory count, then the accessories
    // (at most the eight slots).
    void OracleCostume(FILE* Out, Costume const& C)
    {
        fprintf(Out, "%u,%u,%u,%u,%u", C.PartIds[0], C.PartIds[1], C.PartIds[2], C.PartIds[3],
                C.AccessoryCount);
        unsigned int const Count = C.AccessoryCount < 8u ? C.AccessoryCount : 8u;
        for (unsigned int Index = 0; Index < Count; ++Index)
            fprintf(Out, ",%u", C.AccessoryIds[Index]);
    }

    void OracleNameList(FILE* Out, char const* Key, FFactionNameList const& List)
    {
        fprintf(Out, " %s=%d:", Key, List.Count);
        for (size_t Index = 0; Index < List.Ranges.size(); ++Index)
            fprintf(Out, "%s%d-%d", Index == 0 ? "" : ",", List.Ranges[Index].First,
                    List.Ranges[Index].Last);
    }

    void OracleFmtStr(FILE* Out, char const* Key, FmtStr const& Str)
    {
        unsigned char Buffer[0x1000];
        int const Size = Str.flatten(Buffer, sizeof(Buffer));
        fprintf(Out, " %s=", Key);
        OracleHex(Out, Buffer, Size > 0 ? static_cast<size_t>(Size) : 0u);
    }

    FILE* OracleOpen(std::string const& Dir, char const* Table)
    {
        std::string const Path = Dir + "\\content_" + Table + ".txt";
        return fopen(Path.c_str(), "wb");
    }

    int DumpVoiceLines(FILE* Out)
    {
        for (size_t Index = 0; Index < GVoiceLines.size(); ++Index)
        {
            FVoiceLine const& L = GVoiceLines[Index];
            fprintf(Out, "voice=%u msg=%u duration=%08x\n", L.VoiceId, L.MsgId, OracleBits(L.Duration));
        }
        return static_cast<int>(GVoiceLines.size());
    }

    int DumpVoiceProperties(FILE* Out)
    {
        for (size_t Index = 0; Index < GVoiceProperties.size(); ++Index)
        {
            FVoiceProperties const& P = GVoiceProperties[Index];
            fprintf(Out, "voice=%u flags=%u permutations=", P.VoiceId, P.Flags);
            for (std::map<unsigned int, int>::const_iterator It = P.PermutationCounts.begin();
                 It != P.PermutationCounts.end(); ++It)
            {
                fprintf(Out, "%s%u:%d", It == P.PermutationCounts.begin() ? "" : ",", It->first, It->second);
            }
            fprintf(Out, "\n");
        }
        return static_cast<int>(GVoiceProperties.size());
    }

    int DumpEmpathy(FILE* Out)
    {
        int Lines = 0;
        for (FFactionEmpathyMap::const_iterator It = GFactionEmpathy.begin(); It != GFactionEmpathy.end(); ++It)
        {
            FFactionEmpathy const& E = It->second;
            fprintf(Out, "group=%u events=%08x,%08x,%08x,%08x rates=", It->first, OracleBits(E.EventRates[0]),
                    OracleBits(E.EventRates[1]), OracleBits(E.EventRates[2]), OracleBits(E.EventRates[3]));
            for (size_t Index = 0; Index < E.EmpathyRates.size(); ++Index)
            {
                fprintf(Out, "%s%u:%08x", Index == 0 ? "" : ",", E.EmpathyRates[Index].Group,
                        OracleBits(E.EmpathyRates[Index].Rate));
            }
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpFormations(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, FFormation>::const_iterator It = GFormations.begin(); It != GFormations.end();
             ++It)
        {
            FFormation const& F = It->second;
            fprintf(Out, "formation=%u nickname=%u slots=", It->first, F.Nickname.Id);
            for (size_t Index = 0; Index < F.Slots.size(); ++Index)
            {
                FFormationSlot const& S = F.Slots[Index];
                fprintf(Out, "%s%08x:%08x:%08x:%d", Index == 0 ? "" : ",", OracleBits(S.Position.X),
                        OracleBits(S.Position.Y), OracleBits(S.Position.Z), S.bPlayerSlot ? 1 : 0);
            }
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpNpcShips(FILE* Out)
    {
        int Lines = 0;
        for (FNpcShipArchMap::const_iterator It = GNpcShipArchs.begin(); It != GNpcShipArchs.end(); ++It)
        {
            FNpcShipArch const& A = It->second;
            fprintf(Out, "npc_ship=%u nickname=%u ship=%u loadout=%u pilot=%u state_graph=%d level=%d", It->first,
                    A.Nickname.Id, A.ShipArchetype, A.Loadout, A.Pilot.Id, A.StateGraph, A.Level);
            OracleIds(Out, "classes", A.NpcClasses);
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpFactions(FILE* Out)
    {
        int Lines = 0;
        for (FFactionPropsMap::const_iterator It = GFactionProps.begin(); It != GFactionProps.end(); ++It)
        {
            FFactionProps const& P = It->second;
            fprintf(Out, "faction=%u", It->first);
            OracleIds(Out, "npc_ships", P.NpcShips);
            fprintf(Out, " jump=%d scan_cargo=", P.JumpPreference);
            for (size_t Index = 0; Index < P.ScanForCargo.size(); ++Index)
                fprintf(Out, "%s%u:%d", Index == 0 ? "" : ",", P.ScanForCargo[Index].Archetype,
                        P.ScanForCargo[Index].Level);
            fprintf(Out, " scan_chance=%08x scan_announce=%d", OracleBits(P.ScanChance), P.bScanAnnounce ? 1 : 0);
            OracleIds(Out, "market_goods", P.MarketGoods);
            OracleIds(Out, "assassinate", P.AssassinateTargets);
            fprintf(Out, " legality=%d singular=%d formations=", P.Legality, P.bSingularNickname ? 1 : 0);
            for (std::map<unsigned int, unsigned int>::const_iterator F = P.Formations.begin();
                 F != P.Formations.end(); ++F)
            {
                fprintf(Out, "%s%u:%u", F == P.Formations.begin() ? "" : ",", F->first, F->second);
            }
            fprintf(Out, " costumes=");
            for (size_t Index = 0; Index < P.SpaceCostumes.size(); ++Index)
            {
                if (Index != 0)
                    fprintf(Out, ";");
                OracleCostume(Out, P.SpaceCostumes[Index]);
            }
            OracleIds(Out, "voices", P.Voices);
            fprintf(Out, " mc_costume=%d msg_id_prefix=", P.McCostume);
            OracleHex(Out, P.MsgIdPrefix.Data, P.MsgIdPrefix.Length < 64u ? P.MsgIdPrefix.Length : 63u);
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpFactionNames(FILE* Out)
    {
        int Lines = 0;
        for (FFactionNamesMap::const_iterator It = GFactionNames.begin(); It != GFactionNames.end(); ++It)
        {
            FFactionNames const& N = It->second;
            fprintf(Out, "faction=%u group=%u", It->first, N.Group);
            OracleNameList(Out, "female", N.FemaleFirstNames);
            OracleNameList(Out, "male", N.MaleFirstNames);
            OracleNameList(Out, "last", N.LastNames);
            OracleNameList(Out, "formation", N.FormationDesigs);
            OracleNameList(Out, "large_ship", N.LargeShipNames);
            fprintf(Out, " large_ship_desig=%d short_name=%u rank_desig=%u,%u,%u thresholds=%d,%d name_ids=",
                    N.LargeShipDesig, N.ShortGroupName, N.RankDesig[0], N.RankDesig[1], N.RankDesig[2],
                    N.RankThreshold[0], N.RankThreshold[1]);
            for (int Index = 0; Index < 8; ++Index)
                fprintf(Out, "%s%u", Index == 0 ? "" : ",", N.NameIds[Index]);
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpFactionGroups(FILE* Out)
    {
        for (size_t Index = 0; Index < GFactionGroups.size(); ++Index)
            fprintf(Out, "group=%u\n", GFactionGroups[Index]);
        return static_cast<int>(GFactionGroups.size());
    }

    int DumpIdLists(FILE* Out, char const* Key, std::map<unsigned int, std::vector<unsigned int>> const& Map)
    {
        int Lines = 0;
        for (std::map<unsigned int, std::vector<unsigned int>>::const_iterator It = Map.begin(); It != Map.end();
             ++It)
        {
            fprintf(Out, "%s=%u", Key, It->first);
            OracleIds(Out, "members", It->second);
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpGcsExclusions(FILE* Out)
    {
        int Lines = 0;
        for (std::map<unsigned int, std::vector<std::string>>::const_iterator It = GGcsExclusions.begin();
             It != GGcsExclusions.end(); ++It)
        {
            fprintf(Out, "voice=%u scripts=", It->first);
            for (size_t Index = 0; Index < It->second.size(); ++Index)
            {
                if (Index != 0)
                    fprintf(Out, ",");
                OracleHex(Out, It->second[Index].data(), It->second[Index].size());
            }
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpRankDifficulties(FILE* Out)
    {
        for (size_t Index = 0; Index < GRmRankDifficulties.size(); ++Index)
        {
            fprintf(Out, "stage=%d difficulty=%08x\n", GRmRankDifficulties[Index].Stage,
                    OracleBits(GRmRankDifficulties[Index].Difficulty));
        }
        return static_cast<int>(GRmRankDifficulties.size());
    }

    int DumpNews(FILE* Out)
    {
        for (size_t Index = 0; Index < GNewsItems.size(); ++Index)
        {
            FNewsItem const& N = GNewsItems[Index];
            fprintf(Out, "news=%u", static_cast<unsigned int>(Index));
            OracleIds(Out, "bases", N.Bases);
            fprintf(Out, " from=%d to=%d audio=", N.FromStage, N.ToStage);
            OracleLowerHex(Out, N.Audio.Value);
            fprintf(Out, " logo=");
            OracleLowerHex(Out, N.Logo.Value);
            fprintf(Out, " breaking=%d autoselect=%d icon=%u", N.Desc.bFlag1 ? 1 : 0, N.Desc.bFlag2 ? 1 : 0,
                    N.Desc.Category);
            OracleFmtStr(Out, "category", N.Desc.Headline);
            OracleFmtStr(Out, "headline", N.Desc.Text);
            OracleFmtStr(Out, "text", N.Desc.Audio);
            fprintf(Out, "\n");
        }
        return static_cast<int>(GNewsItems.size());
    }
}

// The tables in LoadContentGameTables' order. Returns the number of lines written, -1 when a
// file cannot be opened.
extern "C" __declspec(dllexport) int __cdecl OracleDumpContentTables(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const Tables[] = {"voice_lines", "voice_properties", "empathy", "formations", "pilots",
                                  "npc_ships", "factions", "faction_names", "faction_groups", "ship_classes",
                                  "ship_mission_properties", "loot", "phantom_loot", "gcs_exclusions",
                                  "toughness", "rank_difficulty", "news"};
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
        Lines += DumpVoiceLines(Files[0]);
        Lines += DumpVoiceProperties(Files[1]);
        Lines += DumpEmpathy(Files[2]);
        Lines += DumpFormations(Files[3]);
        Lines += OracleContentDumpPilots(Files[4]);
        Lines += DumpNpcShips(Files[5]);
        Lines += DumpFactions(Files[6]);
        Lines += DumpFactionNames(Files[7]);
        Lines += DumpFactionGroups(Files[8]);
        Lines += DumpIdLists(Files[9], "class", GRmShipClasses);
        Lines += DumpIdLists(Files[10], "ship", GRmShipMissionProperties);
        Lines += OracleContentDumpLoot(Files[11], Files[12]);
        Lines += DumpGcsExclusions(Files[13]);
        Lines += OracleContentDumpToughness(Files[14]);
        Lines += DumpRankDifficulties(Files[15]);
        Lines += DumpNews(Files[16]);
    }
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Files[Index] != nullptr)
            fclose(Files[Index]);
    }
    return Lines;
}
