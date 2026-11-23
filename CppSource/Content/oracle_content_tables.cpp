// Oracle instrumentation (P3.4b, decision record 0008 point 5 and its Verification table):
// Content.dll's static tables, as LoadContentGameTables (Encounters.cpp) left them, written
// as one text file per table for the Rust comparison (crates/game-logic/tests/
// content_tables.rs). P3.4c (record 0008 point 10) adds the other loaders' tables after
// them (crates/game-logic/tests/content_loaders.rs): the mBases.ini records (BaseDB.cpp's
// OracleContentDumpBases), the specific NPCs, the three system path tables, and for every
// system its encounter parameters, zones and patrol paths, read as FSystemInfo::
// LoadFromReader reads them, with the factions and archetypes of every ship line (Cargo.cpp).
// A new export, not part of the reference DLL's interface: only
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
#include "Cargo.h"              // GetShipLineFactions, GetFactionShipLineArchs (P3.4c)
#include "EncounterParams.h"    // FEncounterParameters (P3.4c)
#include "SystemConnections.h"  // GSystemPathsAll, GSystemPathsGate, GSystemPathsHole (P3.4c)
#include "SystemInfo.h"         // FSystemInfo, FSystemZoneEnumerator (P3.4c)
#include "common_id.h"          // CreateID
#include "common_ini.h"
#include "common_server.h"      // pub::NextSystemID, BuildSystemReader, System::EnumerateZones

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

// The tables whose globals are file-static: PilotPersonalities.cpp, LootProps.cpp and
// Support.cpp write their own lines. Each returns the number of lines it wrote.
int __cdecl OracleContentDumpPilots(FILE* Out);
int __cdecl OracleContentDumpLoot(FILE* Loot, FILE* Phantom);
int __cdecl OracleContentDumpToughness(FILE* Out);
// P3.4c: GContentBases is file-static in BaseDB.cpp.
int __cdecl OracleContentDumpBases(FILE* Bases, FILE* Npcs, FILE* Rooms);

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

// P3.4c: the writers BaseDB.cpp shares (outside the anonymous namespace).
void __cdecl OracleContentWriteHex(FILE* Out, void const* Data, size_t Size)
{
    OracleHex(Out, Data, Size);
}

// " costume=<head,body,left,right,count,accessories> voice= name= affiliation= nickname=".
void __cdecl OracleContentWriteNpcDesc(FILE* Out, FNpcCharacterDesc const& Desc)
{
    fprintf(Out, " costume=");
    OracleCostume(Out, Desc.Appearance);
    fprintf(Out, " voice=%u name=%d affiliation=%u nickname=%u", Desc.Voice, Desc.IndividualName,
            Desc.Affiliation, Desc.Nickname.Id);
}

namespace
{
    int DumpSpecificNpcs(FILE* Out)
    {
        // StoryLauncher's constructor (StoryLauncher.cpp:38-52) runs only in single player;
        // its loop runs here into a local table, as it fills GSpecificNpcs.
        FNpcCharacterDescMap Npcs;
        INI_Reader Reader;
        OpenContentDataIni("missions\\specific_npc.ini", &Reader);
        while (Reader.read_header())
        {
            if (!Reader.is_header("NPC"))
                continue;
            FNpcCharacterDesc Desc;
            ID_String Nickname;
            Nickname.clear();
            LoadNpcCharacterDesc(&Nickname, &Desc, &Reader);
            Npcs.insert(std::make_pair(Nickname.Id, Desc));
        }
        int Lines = 0;
        for (FNpcCharacterDescMap::const_iterator It = Npcs.begin(); It != Npcs.end(); ++It)
        {
            fprintf(Out, "npc=%u", It->first);
            OracleContentWriteNpcDesc(Out, It->second);
            fprintf(Out, "\n");
            ++Lines;
        }
        return Lines;
    }

    void OracleIdList(FILE* Out, std::vector<unsigned int> const& Ids, char const* Separator)
    {
        for (size_t Index = 0; Index < Ids.size(); ++Index)
            fprintf(Out, "%s%u", Index == 0 ? "" : Separator, Ids[Index]);
    }

    int DumpSystemPaths(FILE* Out, FSystemPathTable const& Table)
    {
        int Lines = 0;
        for (FSystemPathTable::const_iterator From = Table.begin(); From != Table.end(); ++From)
        {
            for (FSystemPathsFrom::const_iterator To = From->second.begin(); To != From->second.end(); ++To)
            {
                fprintf(Out, "from=%u to=%u path=", From->first, To->first);
                std::vector<unsigned int> const Path(To->second.begin(), To->second.end());
                OracleIdList(Out, Path, ",");
                fprintf(Out, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    unsigned long long OracleBits64(double Value)
    {
        unsigned long long Bits = 0;
        memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    void OracleWeights(FILE* Out, weighted_vector<unsigned int> const& Weights)
    {
        for (size_t Index = 0; Index < Weights.Items.size(); ++Index)
            fprintf(Out, "%s%u:%08x", Index == 0 ? "" : "+", Weights.Items[Index].first,
                    OracleBits(Weights.Items[Index].second));
    }

    // One parameters object, its formations and their ship lines with their factions.
    int DumpEncounterParameters(FILE* Params, FILE* Formations, FILE* LineFactions, unsigned int SystemId,
                                unsigned int Id, FEncounterParameters const& P)
    {
        int Lines = 0;
        fprintf(Params,
                "system=%u params=%u nickname=%u system_id=%u zone=%u reroll=%08x last=%08x difficulty=%d "
                "formations=%u permutations=",
                SystemId, Id, P.Nickname.Id, P.SystemId, P.ZoneId, OracleBits(P.RerollInterval),
                OracleBits(P.LastRerollTime), P.Difficulty, static_cast<unsigned int>(P.Formations.size()));
        for (size_t Index = 0; Index < P.Permutations.size(); ++Index)
        {
            FEncounterPermutation const& Perm = P.Permutations[Index];
            fprintf(Params, "%s%08x:", Index == 0 ? "" : ";", OracleBits(Perm.Weight));
            for (size_t Entry = 0; Entry < Perm.Entries.size(); ++Entry)
                fprintf(Params, "%s%d%s", Entry == 0 ? "" : "+", Perm.Entries[Entry].FormationIndex,
                        Perm.Entries[Entry].bStar ? "*" : "");
        }
        fprintf(Params, "\n");
        ++Lines;
        for (size_t F = 0; F < P.Formations.size(); ++F)
        {
            FEncounterFormation const& E = P.Formations[F];
            fprintf(Formations,
                    "system=%u params=%u formation=%u feeling=%d reroll=%d mask=%u f0c=%d value=%d faction=%u "
                    "formation_id=%u by_class=%u behavior=%d arrival=",
                    SystemId, Id, static_cast<unsigned int>(F), E.bFeelingToFormation, E.RerollFrequency,
                    E.FeelingMask, E.Field_0x0c, E.FeelingValue, E.Faction, E.Formation.Id, E.FormationByClass.Id,
                    E.Behavior);
            for (size_t A = 0; A < E.Arrival.size(); ++A)
                fprintf(Formations, "%u", static_cast<unsigned int>(E.Arrival[A]));
            fprintf(Formations, " distance=%016llx times=%d simultaneous=%d f78=%d longevity=%d ships=",
                    OracleBits64(E.ZoneCreationDistance), E.TimesToCreate, E.bAllowSimultaneousCreation ? 1 : 0,
                    E.Field_0x78, E.Longevity);
            for (size_t S = 0; S < E.Ships.size(); ++S)
            {
                FEncounterShip const& Ship = E.Ships[S];
                fprintf(Formations, "%s", S == 0 ? "" : ";");
                OracleIdList(Formations, Ship.ShipClasses, "+");
                fprintf(Formations, "/%u/%u/%d/%d/", Ship.NpcArch.Id, Ship.PilotJob.Id, Ship.MinCount,
                        Ship.MaxCount);
                OracleIdList(Formations, Ship.MakeClasses, "+");
                fprintf(Formations, "/%d", Ship.DifficultyOffset);

                std::vector<unsigned int> Factions;
                GetShipLineFactions(&Ship, &Factions);
                fprintf(LineFactions, "system=%u params=%u formation=%u ship=%u factions=", SystemId, Id,
                        static_cast<unsigned int>(F), static_cast<unsigned int>(S));
                OracleIdList(LineFactions, Factions, ",");
                fprintf(LineFactions, " archs=");
                for (size_t G = 0; G < Factions.size(); ++G)
                {
                    std::list<FNpcShipArch*> Archs;
                    GetFactionShipLineArchs(Factions[G], &Ship, &Archs);
                    fprintf(LineFactions, "%s%u:", G == 0 ? "" : ";", Factions[G]);
                    bool bFirst = true;
                    for (std::list<FNpcShipArch*>::const_iterator It = Archs.begin(); It != Archs.end(); ++It)
                    {
                        fprintf(LineFactions, "%s%u", bFirst ? "" : "+", (*It)->Nickname.Id);
                        bFirst = false;
                    }
                }
                fprintf(LineFactions, "\n");
                ++Lines;
            }
            fprintf(Formations, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpZone(FILE* Out, unsigned int SystemId, FSystemInfo const& Info, FSystemZone const& Z)
    {
        int PathIndex = -1;
        for (size_t Index = 0; Index < Info.Paths.size(); ++Index)
        {
            if (Z.Path == &Info.Paths[Index])
                PathIndex = static_cast<int>(Index);
        }
        FZonePopulation const& P = Z.Population;
        fprintf(Out,
                "system=%u zone=%u nickname=%u path=%d unpopulated=%d tradelane=%d lawful=%d unlawful=%d "
                "vignette=%u density=%d repop=%08x keys=%d attack=%08x restrictions=",
                SystemId, Z.ZoneId, Z.Nickname.Id, PathIndex, Z.bUnpopulated ? 1 : 0, Z.bTradelane ? 1 : 0,
                Z.bLawful ? 1 : 0, Z.bUnlawful ? 1 : 0, Z.VignetteType, P.Density, OracleBits(P.RepopTime),
                P.bHasPopulationKeys ? 1 : 0, OracleBits(P.TradelaneAttack));
        bool bFirst = true;
        for (std::list<FDensityRestriction>::const_iterator It = P.DensityRestrictions.begin();
             It != P.DensityRestrictions.end(); ++It)
        {
            fprintf(Out, "%s%d:%u", bFirst ? "" : ",", It->MaxCount, It->Type.Id);
            bFirst = false;
        }
        fprintf(Out, " f1c=%08x f30=%08x additive=%d battle=%d relief=%08x encounters=", OracleBits(P.Field_0x1c),
                OracleBits(P.Field_0x30), P.bPopulationAdditive ? 1 : 0, P.MaxBattleSize, OracleBits(P.ReliefTime));
        for (size_t Index = 0; Index < P.Encounters.size(); ++Index)
        {
            FZoneEncounter const& E = P.Encounters[Index];
            fprintf(Out, "%s%u/%u/%d/%08x/", Index == 0 ? "" : ";", E.Nickname.Id, E.ZoneId, E.Difficulty,
                    OracleBits(E.Chance));
            OracleWeights(Out, E.Factions);
            fprintf(Out, "/");
            bool bFirstClass = true;
            for (size_t F = 0; F < E.Formations.size(); ++F)
            {
                for (size_t S = 0; S < E.Formations[F].Ships.size(); ++S)
                {
                    std::vector<unsigned int> const& Classes = E.Formations[F].Ships[S].ShipClasses;
                    for (size_t C = 0; C < Classes.size(); ++C)
                    {
                        fprintf(Out, "%s%u", bFirstClass ? "" : "+", Classes[C]);
                        bFirstClass = false;
                    }
                }
            }
        }
        fprintf(Out, "\n");
        return 1;
    }

    int DumpPatrolPath(FILE* Out, unsigned int SystemId, size_t Index, FPatrolPath const& P)
    {
        fprintf(Out, "system=%u path=%u label=%u usage=%u eligible=%d loop=%d points=", SystemId,
                static_cast<unsigned int>(Index), P.Label.Id, P.Usage, P.bMissionEligible ? 1 : 0, P.bLoop ? 1 : 0);
        for (size_t Point = 0; Point < P.Points.size(); ++Point)
            fprintf(Out, "%s%08x:%08x:%08x", Point == 0 ? "" : ",", OracleBits(P.Points[Point].x),
                    OracleBits(P.Points[Point].y), OracleBits(P.Points[Point].z));
        fprintf(Out, " zones=");
        for (std::map<unsigned int, std::vector<int>>::const_iterator It = P.ZoneSteps.begin();
             It != P.ZoneSteps.end(); ++It)
        {
            fprintf(Out, "%s%u:", It == P.ZoneSteps.begin() ? "" : ",", It->first);
            for (size_t Step = 0; Step < It->second.size(); ++Step)
                fprintf(Out, "%s%d", Step == 0 ? "" : "+", It->second[Step]);
        }
        fprintf(Out, " objects=");
        for (size_t Object = 0; Object < P.Objects.size(); ++Object)
        {
            fprintf(Out, "%s%u:", Object == 0 ? "" : ",", P.Objects[Object].ObjectId);
            for (size_t Point = 0; Point < P.Objects[Object].PointIndices.size(); ++Point)
                fprintf(Out, "%s%d", Point == 0 ? "" : "+", P.Objects[Object].PointIndices[Point]);
        }
        fprintf(Out, "\n");
        return 1;
    }

    // Every system in id order (GSystemInfos' order), read as FSystemInfo::LoadFromReader
    // (SystemInfo.cpp:48-301) reads its encounter parameters, zones and patrol paths, into a
    // local record so that the live one is not touched: the [EncounterParameters] block of
    // its header loop, the zone enumeration and BuildPatrolPaths. The [System] and [Object]
    // sections, the trade lanes and the zones' Validate (which draws rand()) are the system
    // record's (P3.4d) and are left out.
    int DumpSystems(FILE* Params, FILE* Formations, FILE* LineFactions, FILE* Zones, FILE* Paths)
    {
        std::vector<unsigned int> Systems;
        unsigned int Next = 0;
        while (pub::NextSystemID(Next))
            Systems.push_back(Next);
        std::sort(Systems.begin(), Systems.end());
        int Lines = 0;
        for (size_t SystemIndex = 0; SystemIndex < Systems.size(); ++SystemIndex)
        {
            unsigned int SystemId = Systems[SystemIndex];
            FSystemInfo Info(&SystemId);
            INI_Reader Reader;
            if (pub::BuildSystemReader(Reader, SystemId) != 0)
                continue;
            Info.EncounterParameters = new std::map<unsigned int, FEncounterParameters*>();
            while (Reader.read_header())
            {
                if (!Reader.is_header("EncounterParameters"))
                    continue;
                // SystemInfo.cpp:84-114; a repeated id takes the later object (the earlier
                // one is freed here, where the loader leaks it).
                Reader.read_value();
                unsigned int EncounterId = CreateID(Reader.get_value_string());
                Reader.read_value();
                char FileName[0x104];
                char const* FileNameValue = Reader.get_value_string();
                if (FileNameValue == nullptr)
                    FileName[0] = '\0';
                else
                    strncpy_s(FileName, sizeof(FileName), FileNameValue, _TRUNCATE);
                INI_Reader EncounterReader;
                if (OpenContentDataIni(FileName, &EncounterReader))
                {
                    FEncounterParameters* Parameters =
                        new FEncounterParameters(&EncounterReader, &EncounterId, &SystemId);
                    std::pair<std::map<unsigned int, FEncounterParameters*>::iterator, bool> const Inserted =
                        Info.EncounterParameters->insert(
                            std::pair<unsigned int const, FEncounterParameters*>(EncounterId, nullptr));
                    if (Inserted.first->second != nullptr)
                        delete Inserted.first->second;
                    Inserted.first->second = Parameters;
                }
            }
            for (std::map<unsigned int, FEncounterParameters*>::const_iterator It = Info.EncounterParameters->begin();
                 It != Info.EncounterParameters->end(); ++It)
                Lines += DumpEncounterParameters(Params, Formations, LineFactions, SystemId, It->first, *It->second);

            std::vector<FZonePathLabel> Labels;
            FSystemZoneEnumerator Enumerator;
            Enumerator.System = &Info;
            Enumerator.bLoadEncounters = true;
            Enumerator.Zones = &Info.Zones;
            Enumerator.PathLabels = &Labels;
            pub::System::EnumerateZones(SystemId, Enumerator);
            for (std::map<unsigned int, FEncounterParameters*>::iterator It = Info.EncounterParameters->begin();
                 It != Info.EncounterParameters->end(); ++It)
                delete It->second;
            delete Info.EncounterParameters;
            Info.EncounterParameters = nullptr;

            Info.ZoneById.clear();
            for (std::vector<FSystemZone>::iterator It = Info.Zones.begin(); It != Info.Zones.end(); ++It)
                Info.ZoneById[It->ZoneId] = &*It;
            Info.BuildPatrolPaths(&Labels);
            for (size_t Index = 0; Index < Info.Zones.size(); ++Index)
                Lines += DumpZone(Zones, SystemId, Info, Info.Zones[Index]);
            for (size_t Index = 0; Index < Info.Paths.size(); ++Index)
                Lines += DumpPatrolPath(Paths, SystemId, Index, Info.Paths[Index]);
        }
        return Lines;
    }
}

// The tables in LoadContentGameTables' order, then P3.4c's. Returns the number of lines
// written, -1 when a file cannot be opened.
extern "C" __declspec(dllexport) int __cdecl OracleDumpContentTables(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const Tables[] = {"voice_lines", "voice_properties", "empathy", "formations", "pilots",
                                  "npc_ships", "factions", "faction_names", "faction_groups", "ship_classes",
                                  "ship_mission_properties", "loot", "phantom_loot", "gcs_exclusions",
                                  "toughness", "rank_difficulty", "news",
                                  // P3.4c.
                                  "bases", "base_npcs", "base_rooms", "specific_npcs", "system_paths_all",
                                  "system_paths_gate", "system_paths_hole", "encounter_params",
                                  "encounter_formations", "ship_line_factions", "zones", "patrol_paths"};
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
        // P3.4c: the other loaders' tables.
        Lines += OracleContentDumpBases(Files[17], Files[18], Files[19]);
        Lines += DumpSpecificNpcs(Files[20]);
        Lines += DumpSystemPaths(Files[21], GSystemPathsAll);
        Lines += DumpSystemPaths(Files[22], GSystemPathsGate);
        Lines += DumpSystemPaths(Files[23], GSystemPathsHole);
        Lines += DumpSystems(Files[24], Files[25], Files[26], Files[27], Files[28]);
    }
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Files[Index] != nullptr)
            fclose(Files[Index]);
    }
    return Lines;
}
