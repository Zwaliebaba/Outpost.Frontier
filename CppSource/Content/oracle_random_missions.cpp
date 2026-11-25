// Oracle instrumentation (P3.7a, decision record 0008 points 8 and 10, Gate G3): the
// random-mission tables and vignette tree the generator object loaded with Content.dll, and the
// offers the generator builds from a seed, for the Rust comparison
// (crates/game-logic/tests/random_missions.rs). A new export, not part of the reference DLL's
// interface: only Oracle.exe sim calls it, for the session file
// CppSource/Oracle/random_missions.txt (`dump random_missions`), right after the multiplayer
// start-up, so a game run never reaches it. P3.7c adds two exports for the [RandomMission] save
// sections at the end of the file (OracleDumpRandomMissionSaves, OracleLoadRandomMissionSaves).
//
// The tables are written first, as the generator's constructor left them. Then, for every base
// of OracleRmBases that Content knows, every [BaseFaction] of it with a mission_type and with
// faction properties and names, every difficulty of OracleRmDifficulties and every seed of
// OracleRmSeeds, in that order (one "case" each): the objective zones are emptied
// (GRmObjectiveZones), the synthetic player 7 (ReplaceContentPlayerState) is put in the base's
// system, the CRT generator is seeded with the seed, and one random-mission loader's
// CreateMission runs for {player 7, the base, the faction, the difficulty}. CreateMission
// releases the loader's previous mission first (its offers' target names go back to the faction
// name pools), draws the mission's seed, picks the objective zone and the enemy group and, when
// it has both, calls FinalizeMission, which seeds the generator again with the mission's seed and
// builds the offer. Its draws since that seeding are counted. The mission's values, its result
// and the counters outside the seed are written, then its offer. The loader is released at the
// end and player 7 removed.
//
// Every file is `<Dir>\rm_<table>.txt`, one record per line, fields `key=value` separated by one
// space: ids and unsigned values decimal, signed values decimal with their sign, flags as 0 or 1,
// floats as the eight lower-case hex digits of their bit pattern, strings and flattened FmtStr
// values as the hex of their bytes; lists joined by `,` (sub-fields by `/`, `:` or `+`, records
// by `;`).
//
//   rm_npc_rank_to_difficulty.txt  GRmNpcRankToDifficulty, by rank;
//   rm_diff2money.txt              GRmDiff2Money, by difficulty;
//   rm_bonus_loot.txt              the drop chance, then GRmBonusLoot by faction;
//   rm_solar_formations.txt        GetSolarFormations(), by nickname;
//   rm_killable_solars.txt         GRmKillableSolars, by nickname;
//   rm_critical_loot.txt           GRmCriticalLoot, in order;
//   rm_generator.txt               the generator's root, leaves and node count;
//   rm_nodes.txt                   one line per vignette node, in key order;
//   rm_offer_texts.txt             one line per offer_text of each node;
//   rm_comm_sequences.txt          one line per comm_sequence of each node;
//   rm_missions.txt                one line per case;
//   rm_offers.txt                  one line per offer the mission kept;
//   rm_offer_ship_groups.txt       one line per ship group of its target ship and ship components;
//   rm_offer_ships.txt             one line per ship of each group;
//   rm_offer_solars.txt            one line per solar of its solar component;
//   rm_offer_objectives.txt        one line per objective of its four components;
//   rm_offer_comms.txt             one line per conversation of its general component.

#include "content.h"
#include "BaseDB.h"               // FindContentBaseInfo, FContentBase, FBaseFaction
#include "BaseSupport.h"          // FBaseVisit (P3.7c)
#include "ContentPlayerState.h"   // ReplaceContentPlayerState, RemoveContentPlayerState
#include "FactionProps.h"         // GetFactionProps, GetFactionNames
#include "LootProps.h"            // CreateBarMissionOffer, BuildMissionVendorOffers (P3.7c)
#include "RandomEncounters.h"     // GRmKillableSolars, GRmCriticalLoot
#include "RandomMission.h"        // FRandomMissionData, FRandomMissionLoader, GRmObjectiveZones
#include "RandomMissionDB.h"      // GRmBonusLoot, GetSolarFormations
#include "RandomMissionGen.h"     // GRandomMissionGenerator, GRmNpcRankToDifficulty, GRmDiff2Money
#include "RandomMissionOffer.h"   // FRmVignetteOffer and its components
#include "RandomMissionRuntime.h" // FRmParamText, FRmObjectiveType2, GRmShipGroupSerial
#include "SystemInfo.h"           // FindSystemInfo
#include "common_goods.h"         // MissionVendorOfferDescription (P3.7c)
#include "common_id.h"            // CreateID
#include "common_ini.h"           // INI_Reader (P3.7c)
#include "common_server.h"        // pub::GetBaseID, pub::GetSystem
#include "common_text.h"          // FmtStr

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <map>
#include <string>
#include <vector>

namespace
{
    // The synthetic player.
    unsigned int const OracleRmPlayerId = 7u;

    // The bases, spread over the houses, the border worlds and the edge systems.
    char const* const OracleRmBases[] = {
        "he01_01_Base", "he01_02_Base", "he01_10_Base", "he02_01_Base", "he03_01_Base",
        "he04_01_Base", "ve01_01_Base", "ve01_08_Base", "ve02_01_Base", "ve04_06_Base",
        "ve05_01_Base", "ve06_01_Base", "or01_03_Base", "or02_04_Base", "or05_02_Base",
        "dm01_04_Base", "dm02_01_Base", "dm03_01_Base", "dm05_01_Base", "fc03_01_Base",
        "fc04_02_Base", "fr01_04_Base", "fr03_01_Base", "fr07_01_Base", "oe01_01_Base",
        "pe01_01_Base",
    };

    // The difficulties: below the first windows, low, middle and high.
    float const OracleRmDifficulties[] = {0.2f, 1.5f, 8.0f, 30.0f, 85.0f};

    // The seeds.
    unsigned int const OracleRmSeeds[] = {1u, 0x4f524143u};

    unsigned int OracleBits(float Value)
    {
        unsigned int Bits = 0;
        std::memcpy(&Bits, &Value, sizeof(Bits));
        return Bits;
    }

    void OracleHex(FILE* Out, char const* Data)
    {
        if (Data == nullptr)
            return;
        for (size_t Index = 0; Data[Index] != '\0'; ++Index)
            std::fprintf(Out, "%02x", static_cast<unsigned char>(Data[Index]));
    }

    void OracleFmtStr(FILE* Out, char const* Key, FmtStr const& Str)
    {
        unsigned char Buffer[0x2000];
        unsigned int const Size = Str.flatten(Buffer, sizeof(Buffer));
        std::fprintf(Out, " %s=", Key);
        for (unsigned int Index = 0; Index < Size; ++Index)
            std::fprintf(Out, "%02x", Buffer[Index]);
    }

    void OracleUints(FILE* Out, char const* Key, std::vector<unsigned int> const& Values)
    {
        std::fprintf(Out, " %s=", Key);
        for (size_t Index = 0; Index < Values.size(); ++Index)
            std::fprintf(Out, "%s%u", Index == 0 ? "" : ",", Values[Index]);
    }

    void OracleInts(FILE* Out, std::vector<int> const& Values, char const* Separator)
    {
        for (size_t Index = 0; Index < Values.size(); ++Index)
            std::fprintf(Out, "%s%d", Index == 0 ? "" : Separator, Values[Index]);
    }

    void OracleVector(FILE* Out, char const* Key, float X, float Y, float Z)
    {
        std::fprintf(Out, " %s=%08x,%08x,%08x", Key, OracleBits(X), OracleBits(Y), OracleBits(Z));
    }

    void OracleMatrix(FILE* Out, char const* Key, FMatrix const& M)
    {
        std::fprintf(Out, " %s=", Key);
        for (int Row = 0; Row < 3; ++Row)
        {
            for (int Column = 0; Column < 3; ++Column)
                std::fprintf(Out, "%s%08x", Row == 0 && Column == 0 ? "" : ",",
                             OracleBits(M.D[Row][Column]));
        }
    }

    // " key=<str_id>:<param>+<param>".
    void OracleParamText(FILE* Out, char const* Key, FRmParamText const& Text)
    {
        std::fprintf(Out, " %s=%d:", Key, Text.StrId);
        OracleInts(Out, Text.Params, "+");
    }

    FILE* OracleOpen(std::string const& Dir, char const* Table)
    {
        std::string const Path = Dir + "\\rm_" + Table + ".txt";
        return std::fopen(Path.c_str(), "wb");
    }

    // The draws since std::srand(Seed): the next four values are matched against the
    // generator's sequence from the seed (at most 0x10000 draws; -1 beyond).
    int CountDraws(unsigned int Seed)
    {
        int Probe[4];
        for (int Index = 0; Index < 4; ++Index)
            Probe[Index] = std::rand();
        std::srand(Seed);
        std::vector<int> Sequence;
        Sequence.reserve(0x10004);
        for (int Index = 0; Index < 0x10004; ++Index)
            Sequence.push_back(std::rand());
        for (int Draws = 0; Draws <= 0x10000; ++Draws)
        {
            if (Sequence[Draws] == Probe[0] && Sequence[Draws + 1] == Probe[1] &&
                Sequence[Draws + 2] == Probe[2] && Sequence[Draws + 3] == Probe[3])
                return Draws;
        }
        return -1;
    }

    struct FOracleRmFiles
    {
        FILE* Missions;
        FILE* Offers;
        FILE* ShipGroups;
        FILE* Ships;
        FILE* Solars;
        FILE* Objectives;
        FILE* Comms;
    };

    int DumpTables(FILE* Ranks, FILE* Money, FILE* Bonus, FILE* Formations, FILE* Killable,
                   FILE* Critical)
    {
        int Lines = 0;
        for (std::map<int, std::vector<float>>::const_iterator It = GRmNpcRankToDifficulty.begin();
             It != GRmNpcRankToDifficulty.end(); ++It)
        {
            std::fprintf(Ranks, "rank=%d difficulties=", It->first);
            for (size_t Index = 0; Index < It->second.size(); ++Index)
                std::fprintf(Ranks, "%s%08x", Index == 0 ? "" : ",", OracleBits(It->second[Index]));
            std::fprintf(Ranks, "\n");
            ++Lines;
        }
        for (std::map<float, int>::const_iterator It = GRmDiff2Money.begin(); It != GRmDiff2Money.end();
             ++It)
        {
            std::fprintf(Money, "difficulty=%08x money=%d\n", OracleBits(It->first), It->second);
            ++Lines;
        }
        std::fprintf(Bonus, "drop_chance=%08x\n", OracleBits(GRmBonusLootDropChance));
        ++Lines;
        for (std::map<unsigned int, std::vector<FRmBonusLootEntry>>::const_iterator It =
                 GRmBonusLoot.begin();
             It != GRmBonusLoot.end(); ++It)
        {
            for (size_t Index = 0; Index < It->second.size(); ++Index)
            {
                FRmBonusLootEntry const& E = It->second[Index];
                std::fprintf(Bonus,
                             "faction=%u index=%u archetype=%u num_to_drop=%d,%d difficulty=%08x,%08x "
                             "weight=%08x\n",
                             It->first, static_cast<unsigned int>(Index), E.Archetype, E.NumToDropMin,
                             E.NumToDropMax, OracleBits(E.DifficultyMin), OracleBits(E.DifficultyMax),
                             OracleBits(E.Weight));
                ++Lines;
            }
        }
        std::map<unsigned int, FRmSolarFormation>* const SolarFormations = GetSolarFormations();
        for (std::map<unsigned int, FRmSolarFormation>::const_iterator It = SolarFormations->begin();
             It != SolarFormations->end(); ++It)
        {
            FRmSolarFormation const& F = It->second;
            std::fprintf(Formations, "formation=%u nickname=%u type=%d slots=", It->first, F.Nickname,
                         F.Type);
            for (size_t Index = 0; Index < F.Slots.size(); ++Index)
            {
                FRmSolarFormationSlot const& S = F.Slots[Index];
                std::fprintf(Formations, "%s%u/%d/%08x,%08x,%08x/", Index == 0 ? "" : ";",
                             static_cast<unsigned int>(S.bMandatory), S.SolarType,
                             OracleBits(S.Position.X), OracleBits(S.Position.Y),
                             OracleBits(S.Position.Z));
                for (int Row = 0; Row < 3; ++Row)
                {
                    for (int Column = 0; Column < 3; ++Column)
                        std::fprintf(Formations, "%s%08x", Row == 0 && Column == 0 ? "" : ",",
                                     OracleBits(S.Orientation.D[Row][Column]));
                }
            }
            std::fprintf(Formations, "\n");
            ++Lines;
        }
        for (std::map<unsigned int, FRmKillableSolar>::const_iterator It = GRmKillableSolars.begin();
             It != GRmKillableSolars.end(); ++It)
        {
            FRmKillableSolar const& K = It->second;
            std::fprintf(Killable, "solar=%u nickname=%u type=%d string_id=%d any=%u", It->first,
                         K.Nickname.Id, K.SolarType, K.StringId,
                         static_cast<unsigned int>(K.bAnyFaction));
            OracleUints(Killable, "factions", K.Factions);
            std::fprintf(Killable,
                         " archetype=%u loadout=%u hitpoints=%08x,%08x difficulty=%08x,%08x zone_mask=%u "
                         "pilot_choices=%d\n",
                         K.ArchetypeId, K.LoadoutId, OracleBits(K.HitpointsMin), OracleBits(K.HitpointsMax),
                         OracleBits(K.DifficultyMin), OracleBits(K.DifficultyMax), K.ZoneTypeMask,
                         K.PilotChoices);
            ++Lines;
        }
        for (size_t Index = 0; Index < GRmCriticalLoot.size(); ++Index)
        {
            FRmCriticalLoot const& C = GRmCriticalLoot[Index];
            std::fprintf(Critical, "loot=%u why=%d,%d name=%d archetype=%u",
                         static_cast<unsigned int>(Index), C.WhyDesired[0], C.WhyDesired[1], C.Name,
                         C.ArchetypeId);
            OracleUints(Critical, "types", C.Types);
            std::fprintf(Critical, " any=%u", static_cast<unsigned int>(C.bAnyFaction));
            OracleUints(Critical, "factions", C.Factions);
            std::fprintf(Critical, "\n");
            ++Lines;
        }
        return Lines;
    }

    int DumpTree(FILE* Generator, FILE* Nodes, FILE* OfferTexts, FILE* CommSequences)
    {
        FRandomMissionGenerator const* const G = GRandomMissionGenerator;
        if (G == nullptr)
            return -1;
        int Lines = 0;
        std::fprintf(Generator, "loaded=%u linked=%u root=%u nodes=%u", static_cast<unsigned int>(G->bLoaded),
                     static_cast<unsigned int>(G->bLinked), G->RootNodeId,
                     static_cast<unsigned int>(G->Nodes.size()));
        OracleUints(Generator, "leaves", G->LeafNodeIds);
        std::fprintf(Generator, "\n");
        ++Lines;
        for (std::map<unsigned int, FRmVignetteNode>::const_iterator It = G->Nodes.begin(); It != G->Nodes.end();
             ++It)
        {
            FRmVignetteNode const& N = It->second;
            FRmVignetteParams const& P = N.Params;
            std::fprintf(Nodes, "node=%u node_id=%u nickname=%u", It->first, N.NodeId, N.Nickname.Id);
            OracleUints(Nodes, "parents", N.Parents);
            OracleUints(Nodes, "children", N.Children);
            std::fprintf(Nodes, " implemented=%u first_child=%08x weight=%08x difficulty=%08x,%08x windows=",
                         static_cast<unsigned int>(N.bImplemented), OracleBits(N.FirstChildChance),
                         OracleBits(P.Weight), OracleBits(P.Difficulty.Min), OracleBits(P.Difficulty.Max));
            bool bFirst = true;
            for (std::list<FRmDifficultyWindow>::const_iterator W = N.Windows.Windows.begin();
                 W != N.Windows.Windows.end(); ++W)
            {
                std::fprintf(Nodes, "%s%08x:%08x", bFirst ? "" : ",", OracleBits(W->Min), OracleBits(W->Max));
                bFirst = false;
            }
            OracleUints(Nodes, "offer_groups", P.OfferGroups);
            OracleUints(Nodes, "hostile_groups", P.HostileGroups);
            std::fprintf(Nodes, " zone_types=%u", P.AllowableZoneTypes);
            OracleParamText(Nodes, "failure", P.FailureText);
            OracleParamText(Nodes, "reward", P.RewardText);
            std::fprintf(Nodes, " objectives=");
            bFirst = true;
            for (std::map<int, FRmParamText>::const_iterator O = P.ObjectiveText.Texts.begin();
                 O != P.ObjectiveText.Texts.end(); ++O)
            {
                std::fprintf(Nodes, "%s%d/%d:", bFirst ? "" : ";", O->first, O->second.StrId);
                OracleInts(Nodes, O->second.Params, "+");
                bFirst = false;
            }
            std::fprintf(Nodes, " offer_texts=%u comm_sequences=%u\n",
                         static_cast<unsigned int>(P.OfferTexts.size()),
                         static_cast<unsigned int>(P.CommSequences.size()));
            ++Lines;
            for (size_t Index = 0; Index < P.OfferTexts.size(); ++Index)
            {
                FRmOfferText const& T = P.OfferTexts[Index];
                std::fprintf(OfferTexts, "node=%u index=%u append=%u entries=", It->first,
                             static_cast<unsigned int>(Index), static_cast<unsigned int>(T.bAppend));
                for (size_t E = 0; E < T.Entries.size(); ++E)
                {
                    std::fprintf(OfferTexts, "%s%d/%d/", E == 0 ? "" : ";", T.Entries[E].StrId,
                                 T.Entries[E].Form);
                    OracleInts(OfferTexts, T.Entries[E].Params, "+");
                }
                std::fprintf(OfferTexts, "\n");
                ++Lines;
            }
            for (size_t Index = 0; Index < P.CommSequences.size(); ++Index)
            {
                FRmCommSequence const& S = P.CommSequences[Index];
                std::fprintf(CommSequences, "node=%u index=%u trigger=%d target=%d f08=%08x delay=%08x,%08x lines=",
                             It->first, static_cast<unsigned int>(Index), S.Trigger, S.Target,
                             OracleBits(S.Field_0x08), OracleBits(S.MinDelay), OracleBits(S.MaxDelay));
                for (size_t L = 0; L < S.Lines.size(); ++L)
                {
                    std::fprintf(CommSequences, "%s%d/", L == 0 ? "" : ";", S.Lines[L].Speaker);
                    OracleHex(CommSequences, S.Lines[L].Text);
                }
                std::fprintf(CommSequences, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    void DumpLoot(FILE* Out, std::vector<FRmLootSpawn> const& Loot)
    {
        std::fprintf(Out, " loot=");
        for (size_t Index = 0; Index < Loot.size(); ++Index)
        {
            FRmLootSpawn const& L = Loot[Index];
            std::fprintf(Out, "%s%d/%d/%d/%u/%u/%u/%08x/%u/%u/%08x,%08x,%08x", Index == 0 ? "" : ";", L.Kind,
                         L.Field_0x100, L.CriticalLoot != nullptr ? L.CriticalLoot->Name : 0,
                         L.Info.EquipArchID, L.Info.Units, L.Info.SystemID, OracleBits(L.Info.HitPtsScale),
                         static_cast<unsigned int>(L.Info.bCanAiTractor), static_cast<unsigned int>(L.bRelPos),
                         OracleBits(L.RelPosOffset.x), OracleBits(L.RelPosOffset.y),
                         OracleBits(L.RelPosOffset.z));
        }
    }

    int DumpShipGroups(FOracleRmFiles const& F, unsigned int Case, char const* Component,
                       std::vector<FRmShipGroup> const& Groups)
    {
        int Lines = 0;
        for (size_t G = 0; G < Groups.size(); ++G)
        {
            FRmShipGroup const& Group = Groups[G];
            std::fprintf(F.ShipGroups, "case=%u component=%s group=%u f00=%u f01=%u f02=%u f04=%d f08=%d system=%u",
                         Case, Component, static_cast<unsigned int>(G),
                         static_cast<unsigned int>(Group.Field_0x00), static_cast<unsigned int>(Group.Field_0x01),
                         static_cast<unsigned int>(Group.Field_0x02), Group.Field_0x04, Group.Field_0x08,
                         Group.System != nullptr ? Group.System->SystemId : 0u);
            OracleVector(F.ShipGroups, "position", Group.Position.x, Group.Position.y, Group.Position.z);
            std::fprintf(F.ShipGroups, " f40=%08x formation=%u serial=%u ships=%u\n", OracleBits(Group.Field_0x40),
                         Group.Formation.Id, Group.Serial, static_cast<unsigned int>(Group.Ships.size()));
            ++Lines;
            for (size_t S = 0; S < Group.Ships.size(); ++S)
            {
                FRmShipSpawn const& Ship = Group.Ships[S];
                std::fprintf(F.Ships, "case=%u component=%s group=%u ship=%u nickname=", Case, Component,
                             static_cast<unsigned int>(G), static_cast<unsigned int>(S));
                OracleHex(F.Ships, Ship.Nickname.Data);
                std::fprintf(F.Ships, " random_name=%u affiliation=%u npc_ship_arch=%u target_name=%u d8=%d dc=%d",
                             static_cast<unsigned int>(Ship.bRandomName), Ship.Npc.Affiliation,
                             Ship.Npc.NpcShipArch.Id, Ship.TargetName != nullptr ? 1u : 0u, Ship.Field_0xd8,
                             Ship.Field_0xdc);
                DumpLoot(F.Ships, Ship.Loot);
                std::fprintf(F.Ships, "\n");
                ++Lines;
            }
        }
        return Lines;
    }

    int DumpObjectives(FOracleRmFiles const& F, unsigned int Case, char const* Component,
                       FRmObjectiveMap const& Map)
    {
        int Lines = 0;
        for (std::map<int, FRmParamText*>::const_iterator It = Map.Objectives.begin(); It != Map.Objectives.end();
             ++It)
        {
            // Every objective the generator makes is an FRmObjectiveType2 or FRmObjectiveType3, which
            // derives from it.
            FRmObjectiveType2 const* const O = static_cast<FRmObjectiveType2 const*>(It->second);
            std::fprintf(F.Objectives, "case=%u component=%s key=%d type=%d str_id=%d params=", Case, Component,
                         It->first, O->GetType(), O->StrId);
            OracleInts(F.Objectives, O->Params, ",");
            std::fprintf(F.Objectives, " marker=%u:%d:%08x,%08x,%08x\n", O->MarkerSystemId, O->MarkerValue,
                         OracleBits(O->MarkerPosition.x), OracleBits(O->MarkerPosition.y),
                         OracleBits(O->MarkerPosition.z));
            ++Lines;
        }
        return Lines;
    }

    int DumpOffer(FOracleRmFiles const& F, unsigned int Case, FRmVignetteOffer const* Offer)
    {
        int Lines = 0;
        std::fprintf(F.Offers,
                     "case=%u machine=%d type=%u system=%u offer_group=%d hostile_group=%u difficulty=%08x reward=%d "
                     "rep_hit=%08x rep_reward=%08x offer_base=%u mc_costume=%d voice=%u",
                     Case, Offer->MachineType, static_cast<unsigned int>(Offer->Type),
                     Offer->System != nullptr ? Offer->System->SystemId : 0u, Offer->OfferGroup, Offer->HostileGroup,
                     OracleBits(Offer->Difficulty), Offer->Reward, OracleBits(Offer->RepHit),
                     OracleBits(Offer->RepReward), Offer->OfferBase, Offer->McCostume, Offer->VoiceId);
        OracleFmtStr(F.Offers, "text", Offer->Text);
        OracleParamText(F.Offers, "reward_text", Offer->RewardText);
        OracleParamText(F.Offers, "failure_text", Offer->FailureText);
        OracleFmtStr(F.Offers, "target_first", Offer->TargetName.First);
        OracleFmtStr(F.Offers, "target_second", Offer->TargetName.Second);
        std::fprintf(F.Offers, " target_gender=%d target_affiliation=%u general=%u",
                     Offer->TargetName.Gender, Offer->TargetName.Affiliation,
                     static_cast<unsigned int>(Offer->General.bActive));
        std::fprintf(F.Offers, " target_ships=%u:%08x,%08x,%08x:%d:%d",
                     static_cast<unsigned int>(Offer->TargetShips.bActive), OracleBits(Offer->TargetShips.Position.x),
                     OracleBits(Offer->TargetShips.Position.y), OracleBits(Offer->TargetShips.Position.z),
                     Offer->TargetShips.Field_0x50, Offer->TargetShips.Field_0x54);
        std::fprintf(F.Offers, " solars=%u:%08x,%08x,%08x", static_cast<unsigned int>(Offer->Solars.bActive),
                     OracleBits(Offer->Solars.Position.x), OracleBits(Offer->Solars.Position.y),
                     OracleBits(Offer->Solars.Position.z));
        std::fprintf(F.Offers, " ships=%u:%08x,%08x,%08x\n", static_cast<unsigned int>(Offer->Ships.bActive),
                     OracleBits(Offer->Ships.Position.x), OracleBits(Offer->Ships.Position.y),
                     OracleBits(Offer->Ships.Position.z));
        ++Lines;
        Lines += DumpShipGroups(F, Case, "target_ships", Offer->TargetShips.Groups);
        Lines += DumpShipGroups(F, Case, "ships", Offer->Ships.Groups);

        unsigned int Index = 0;
        for (std::list<FRmSolarSpawn>::const_iterator It = Offer->Solars.Solars.begin();
             It != Offer->Solars.Solars.end(); ++It, ++Index)
        {
            FRmSolarSpawn const& S = *It;
            std::fprintf(F.Solars, "case=%u solar=%u nickname=", Case, Index);
            OracleHex(F.Solars, S.Nickname.Data);
            std::fprintf(F.Solars, " faction=%u", S.Faction);
            OracleMatrix(F.Solars, "orientation", S.Orientation);
            OracleVector(F.Solars, "position", S.Position.x, S.Position.y, S.Position.z);
            std::fprintf(F.Solars,
                         " archetype=%u loadout=%u pilot=%u f4c8=%08x arch=%u type=%d dest_sys=%u mode=%d "
                         "invincible=%08x f4e0=%u hitpoints=%d",
                         S.ArchetypeId, S.LoadoutId, static_cast<unsigned int>(S.bPilot), OracleBits(S.Field_0x4c8),
                         S.Arch != nullptr ? S.Arch->Nickname.Id : 0u, S.SolarType, S.DestSys, S.ModeFlag,
                         OracleBits(S.InvincibleDuration), static_cast<unsigned int>(S.Field_0x4e0), S.Hitpoints);
            DumpLoot(F.Solars, S.Loot);
            std::fprintf(F.Solars, "\n");
            ++Lines;
        }

        Lines += DumpObjectives(F, Case, "general", Offer->General.Objectives);
        Lines += DumpObjectives(F, Case, "target_ships", Offer->TargetShips.Objectives);
        Lines += DumpObjectives(F, Case, "solars", Offer->Solars.Objectives);
        Lines += DumpObjectives(F, Case, "ships", Offer->Ships.Objectives);

        Index = 0;
        for (std::list<FRmConversation>::const_iterator It = Offer->General.Conversations.begin();
             It != Offer->General.Conversations.end(); ++It, ++Index)
        {
            std::fprintf(F.Comms, "case=%u conversation=%u trigger=%d delay=%08x ready=%u lines=", Case, Index,
                         It->Trigger, OracleBits(It->Delay), static_cast<unsigned int>(It->bReady));
            for (size_t L = 0; L < It->Lines.size(); ++L)
            {
                FRmLine const* const Line = static_cast<FRmLine const*>(It->Lines[L]);
                std::fprintf(F.Comms, "%s%d/%d/", L == 0 ? "" : ";", Line->SpeakerKind, Line->TargetKind);
                OracleHex(F.Comms, Line->Text.Data);
            }
            std::fprintf(F.Comms, "\n");
            ++Lines;
        }
        return Lines;
    }

    // " enemy_uses=<pilot names>,<designations>" of a faction's name pools; -1,-1 without names.
    void DumpNameUses(FILE* Out, unsigned int Group)
    {
        FFactionNames const* const Names = GetFactionNames(&Group);
        if (Names == nullptr)
        {
            std::fprintf(Out, " enemy_uses=-1,-1");
            return;
        }
        std::fprintf(Out, " enemy_uses=%d,%d", static_cast<int>(Names->PilotNameUses.size()),
                     static_cast<int>(Names->FormationDesigUses.size()));
    }

    int DumpCases(FOracleRmFiles const& F)
    {
        FRandomMissionLoader* Loader = nullptr;
        CreateRandomMissionLoader("DestroyMission", &Loader);
        if (Loader == nullptr)
            return -1;
        FContentPlayerState* const State = ReplaceContentPlayerState(OracleRmPlayerId);
        int Lines = 0;
        unsigned int Case = 0;
        for (size_t BaseIndex = 0; BaseIndex < sizeof(OracleRmBases) / sizeof(OracleRmBases[0]); ++BaseIndex)
        {
            unsigned int BaseId = 0;
            pub::GetBaseID(BaseId, OracleRmBases[BaseIndex]);
            FContentBase const* const Base = FindContentBaseInfo(&BaseId);
            unsigned int SystemId = 0;
            if (Base == nullptr || pub::GetSystem(SystemId, BaseId) != 0)
                continue;
            FSystemInfo* const System = FindSystemInfo(&SystemId);
            if (System == nullptr)
                continue;
            for (size_t FactionIndex = 0; FactionIndex < Base->Factions.size(); ++FactionIndex)
            {
                FBaseFaction const& Faction = Base->Factions[FactionIndex];
                unsigned int Group = Faction.Group;
                if (Faction.MissionTypes.empty() || GetFactionProps(&Group) == nullptr ||
                    GetFactionNames(&Group) == nullptr)
                    continue;
                for (size_t D = 0; D < sizeof(OracleRmDifficulties) / sizeof(OracleRmDifficulties[0]); ++D)
                {
                    for (size_t S = 0; S < sizeof(OracleRmSeeds) / sizeof(OracleRmSeeds[0]); ++S)
                    {
                        GRmObjectiveZones.clear();
                        State->SystemRecord = System;
                        std::srand(OracleRmSeeds[S]);
                        FRmMissionRequest Request;
                        Request.PlayerId = OracleRmPlayerId;
                        Request.OfferBase = BaseId;
                        Request.OfferGroup = static_cast<int>(Group);
                        Request.Difficulty = OracleRmDifficulties[D];
                        FRandomMissionData* Mission = nullptr;
                        int const Result = Loader->CreateMission(&Request, &Mission);
                        int Draws = -1;
                        if (Mission != nullptr && (Result == 0 || Result == 2))
                            Draws = CountDraws(Mission->RandomSeed);
                        if (Mission == nullptr)
                            return -1;
                        FRandomMissionData const& M = *Mission;
                        std::fprintf(F.Missions,
                                     "case=%u base=%u system=%u group=%u difficulty=%08x seed=%u result=%d "
                                     "random_seed=%u player=%u offer_base=%u offer_group=%d enemy_group=%u "
                                     "dest_sys=%u zone=%u",
                                     Case, BaseId, SystemId, Group, OracleBits(Request.Difficulty), OracleRmSeeds[S],
                                     Result, M.RandomSeed, M.PlayerId, M.OfferBase, M.OfferGroup, M.EnemyGroup,
                                     M.DestSys, M.ObjectiveZone);
                        OracleVector(F.Missions, "pos", M.ObjectivePos.x, M.ObjectivePos.y, M.ObjectivePos.z);
                        std::fprintf(F.Missions, " reward=%d rep_reward=%08x rep_hit=%08x path=", M.Reward,
                                     OracleBits(M.RepReward), OracleBits(M.RepHit));
                        for (size_t Bit = 0; Bit < M.TreePath.Bits.size(); ++Bit)
                            std::fprintf(F.Missions, "%c", M.TreePath.Bits[Bit] == 0 ? 'F' : 'T');
                        std::fprintf(F.Missions, " offers=%u draws=%d serial=%u",
                                     static_cast<unsigned int>(M.Offers.size()), Draws, GRmShipGroupSerial);
                        DumpNameUses(F.Missions, M.EnemyGroup);
                        std::fprintf(F.Missions, "\n");
                        ++Lines;
                        if (!M.Offers.empty())
                            Lines += DumpOffer(F, Case, static_cast<FRmVignetteOffer const*>(M.Offers.front()));
                        ++Case;
                    }
                }
            }
        }
        // The last mission goes with the loader.
        Loader->Release();
        GRmObjectiveZones.clear();
        RemoveContentPlayerState(OracleRmPlayerId);
        return Lines;
    }
}

// The tables, the tree, then the cases. Returns the number of lines written, -1 when a file
// cannot be opened or the generator or a loader is missing.
extern "C" __declspec(dllexport) int __cdecl OracleDumpRandomMissions(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    char const* const Tables[] = {"npc_rank_to_difficulty", "diff2money", "bonus_loot", "solar_formations",
                                  "killable_solars", "critical_loot", "generator", "nodes", "offer_texts",
                                  "comm_sequences", "missions", "offers", "offer_ship_groups", "offer_ships",
                                  "offer_solars", "offer_objectives", "offer_comms"};
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
        Lines = DumpTables(Files[0], Files[1], Files[2], Files[3], Files[4], Files[5]);
        int const TreeLines = DumpTree(Files[6], Files[7], Files[8], Files[9]);
        if (TreeLines < 0)
        {
            Lines = -1;
        }
        else
        {
            Lines += TreeLines;
            FOracleRmFiles const F = {Files[10], Files[11], Files[12], Files[13], Files[14], Files[15], Files[16]};
            int const CaseLines = DumpCases(F);
            Lines = CaseLines < 0 ? -1 : Lines + CaseLines;
        }
    }
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Files[Index] != nullptr)
            std::fclose(Files[Index]);
    }
    return Lines;
}

// =======================================================================================
// P3.7c (decision record 0008 points 8 and 10, Gate G3): the [RandomMission] save sections and
// the restore from them. Two more exports, for the session files
// CppSource/Oracle/random_mission_saves.txt (`dump random_mission_saves`) and
// CppSource/Oracle/random_mission_save_loads.txt (`dump random_mission_save_loads`), right after
// the multiplayer start-up, so a game run never reaches them.
//
// OracleDumpRandomMissionSaves(Dir) makes random missions through the vendor and bar offers:
// for every base of OracleRmBases that Content knows, with a system record, the objective zones
// are emptied, the synthetic player 7 (ReplaceContentPlayerState) is put in the base's system,
// the base is loaded (FContentBase::Load, as BaseSupport loads the base a player enters, which
// reads its [MVendor] num_offers) and the CRT generator is seeded with OracleRmSeeds[1];
// BuildMissionVendorOffers runs for the base into a local visit, every vendor offer is written
// and saved, ClearMissionVendorOffers releases them, and the base is unloaded. Then, for every [BaseFaction] of the base with a
// mission_type and with faction properties and names, the generator is seeded again and
// CreateBarMissionOffer runs with that faction's first mission type as the NPC's mission; a
// created offer is saved, and the loader is released. Each saved mission is
// <Dir>\rmsave_<n>.ini: what a DestroyMission controller writes on PlayerSave
// (DestroyMission.cpp:409-411); rmsave_pair.ini is the first two saves one after the other.
// rms_offers.txt has one line per base (its vendor offer count) and one per offer made.
//
// Every save is then loaded in turn, n = 0 up, then the pair: one random-mission loader's Load
// for player 7 over the file (every [RandomMission] section, each followed by FinalizeMission,
// which seeds the generator with the saved seed and rebuilds the offer), its draws since that
// seeding counted, and the loader released. rmload_missions.txt has one line per load,
// rmload_<table>.txt the rebuilt offers in rm_<table>.txt's format, with the load's number as
// the case.
//
// OracleLoadRandomMissionSaves(Dir) loads <Dir>\rust\rmsave_<n>.ini, n = 0 up while the file
// opens, then <Dir>\rust\rmsave_pair.ini, the same way, into <Dir>\rmload_<table>.txt.
//
// Both return the number of lines written, -1 when a file cannot be opened.
// =======================================================================================

namespace
{
    char const* const OracleRmLoadTables[] = {"missions",     "offers",           "offer_ship_groups", "offer_ships",
                                              "offer_solars", "offer_objectives", "offer_comms"};
    int const OracleRmLoadTableCount = 7;

    FILE* OracleOpenPath(std::string const& Path)
    {
        return std::fopen(Path.c_str(), "wb");
    }

    std::string OracleRmSaveName(int Index)
    {
        char Name[0x40];
        std::snprintf(Name, sizeof(Name), "rmsave_%d.ini", Index);
        return Name;
    }

    // The section a DestroyMission controller writes on PlayerSave.
    void WriteOracleRmSave(FILE* File, FRandomMissionData* Mission)
    {
        std::fprintf(File, "[RandomMission]\n");
        std::fprintf(File, "type = %s\n", "DestroyMission");
        Mission->Save(File);
    }

    void OracleRmPath(FILE* Out, FRmTreePath const& Path)
    {
        std::fprintf(Out, " path=");
        for (size_t Bit = 0; Bit < Path.Bits.size(); ++Bit)
            std::fprintf(Out, "%c", Path.Bits[Bit] == 0 ? 'F' : 'T');
    }

    // The mission's vendor description (the loader's slot 6); " class=-" without an offer, whose
    // first offer GetOfferDescription would read.
    void DumpOracleRmDescription(FILE* Out, FRandomMissionLoader* Loader, FRandomMissionData* Mission)
    {
        if (Mission->Offers.empty())
        {
            std::fprintf(Out, " class=-");
            return;
        }
        MissionVendorOfferDescription Desc;
        Desc.BaseId = 0;
        Desc.TargetId = 0;
        Desc.SystemId = 0;
        Desc.MissionClass = 0;
        Desc.IconTextureId = 0;
        Desc.Filter = 0;
        Loader->GetOfferDescription(Mission, &Desc);
        std::fprintf(Out, " class=%u", Desc.MissionClass);
        OracleFmtStr(Out, "title", Desc.Title);
        OracleFmtStr(Out, "description", Desc.Description);
        OracleFmtStr(Out, "briefing", Desc.Briefing);
        std::fprintf(Out, " base_id=%u icon=%u filter=%u", Desc.BaseId, Desc.IconTextureId, Desc.Filter);
    }

    // One line of rms_offers.txt.
    void DumpOracleRmCreated(FILE* Out, int Save, char const* Source, unsigned int BaseId, unsigned int Group,
                             bool bCreated, unsigned int OfferId, char const* Type, FRandomMissionLoader* Loader,
                             FRandomMissionData* Mission)
    {
        std::fprintf(Out, "save=%d source=%s base=%u group=%u created=%u offer_id=%u type=", Save, Source, BaseId,
                     Group, bCreated ? 1u : 0u, OfferId);
        OracleHex(Out, Type);
        if (Mission == nullptr)
        {
            std::fprintf(Out, " mission=-\n");
            return;
        }
        FRandomMissionData const& M = *Mission;
        std::fprintf(Out, " random_seed=%u player=%u offer_base=%u offer_group=%d enemy_group=%u dest_sys=%u zone=%u",
                     M.RandomSeed, M.PlayerId, M.OfferBase, M.OfferGroup, M.EnemyGroup, M.DestSys, M.ObjectiveZone);
        OracleVector(Out, "pos", M.ObjectivePos.x, M.ObjectivePos.y, M.ObjectivePos.z);
        std::fprintf(Out, " reward=%d rep_reward=%08x rep_hit=%08x difficulty=%08x", M.Reward,
                     OracleBits(M.RepReward), OracleBits(M.RepHit), OracleBits(M.Difficulty));
        OracleRmPath(Out, M.TreePath);
        std::fprintf(Out, " offers=%u serial=%u zones=%u", static_cast<unsigned int>(M.Offers.size()),
                     GRmShipGroupSerial, static_cast<unsigned int>(GRmObjectiveZones.size()));
        DumpOracleRmDescription(Out, Loader, Mission);
        std::fprintf(Out, "\n");
    }

    // Saves Mission as rmsave_<Index>.ini; false when the file cannot be opened.
    bool SaveOracleRm(std::string const& Root, int Index, FRandomMissionData* Mission)
    {
        FILE* const File = OracleOpenPath(Root + "\\" + OracleRmSaveName(Index));
        if (File == nullptr)
            return false;
        WriteOracleRmSave(File, Mission);
        std::fclose(File);
        return true;
    }

    // One load of the file at Path as case Case: the line of rmload_missions.txt and the
    // rebuilt offers. -1 when the file does not open.
    int LoadOracleRm(FOracleRmFiles const& F, std::string const& Path, unsigned int Case)
    {
        INI_Reader Reader;
        if (!Reader.open(Path.c_str(), false))
            return -1;
        FRandomMissionLoader* Loader = nullptr;
        CreateRandomMissionLoader("DestroyMission", &Loader);
        if (Loader == nullptr)
        {
            Reader.close();
            return -1;
        }
        FRandomMissionData* Mission = nullptr;
        bool const bLoaded = Loader->Load(&Reader, &Mission, OracleRmPlayerId);
        int Lines = 0;
        if (Mission != nullptr)
        {
            int const Draws = CountDraws(Mission->RandomSeed);
            FRandomMissionData const& M = *Mission;
            std::fprintf(F.Missions,
                         "save=%u loaded=%u player=%u offer_base=%u offer_group=%d enemy_group=%u dest_sys=%u zone=%u",
                         Case, bLoaded ? 1u : 0u, M.PlayerId, M.OfferBase, M.OfferGroup, M.EnemyGroup, M.DestSys,
                         M.ObjectiveZone);
            OracleVector(F.Missions, "pos", M.ObjectivePos.x, M.ObjectivePos.y, M.ObjectivePos.z);
            std::fprintf(F.Missions,
                         " reward=%d rep_reward=%08x rep_hit=%08x difficulty=%08x jump_pref=%d offer_ids=%d "
                         "random_seed=%u",
                         M.Reward, OracleBits(M.RepReward), OracleBits(M.RepHit), OracleBits(M.Difficulty),
                         M.JumpPref, M.OfferIds, M.RandomSeed);
            OracleRmPath(F.Missions, M.TreePath);
            std::fprintf(F.Missions, " offers=%u draws=%d serial=%u zones=%u",
                         static_cast<unsigned int>(M.Offers.size()), Draws, GRmShipGroupSerial,
                         static_cast<unsigned int>(GRmObjectiveZones.size()));
            OracleFmtStr(F.Missions, "description", M.Description);
            DumpNameUses(F.Missions, M.EnemyGroup);
            std::fprintf(F.Missions, "\n");
            ++Lines;
            for (std::list<FRmOffer*>::const_iterator It = M.Offers.begin(); It != M.Offers.end(); ++It)
                Lines += DumpOffer(F, Case, static_cast<FRmVignetteOffer const*>(*It));
        }
        Loader->Release();
        Reader.close();
        return Lines;
    }

    void CloseOracleRmFiles(FILE* (&Files)[7])
    {
        for (int Index = 0; Index < OracleRmLoadTableCount; ++Index)
        {
            if (Files[Index] != nullptr)
                std::fclose(Files[Index]);
            Files[Index] = nullptr;
        }
    }

    // Every rmsave_<n>.ini of SaveDir, n = 0 up while one opens (fewer than Count when Count >= 0),
    // then rmsave_pair.ini, into Root's rmload_<table>.txt. -1 when an output file cannot be opened.
    int LoadOracleRmSaves(std::string const& Root, std::string const& SaveDir, int Count)
    {
        FILE* Files[7] = {};
        bool bOpen = true;
        for (int Index = 0; Index < OracleRmLoadTableCount; ++Index)
        {
            Files[Index] = OracleOpenPath(Root + "\\rmload_" + OracleRmLoadTables[Index] + ".txt");
            bOpen = bOpen && Files[Index] != nullptr;
        }
        int Lines = -1;
        if (bOpen)
        {
            FOracleRmFiles const F = {Files[0], Files[1], Files[2], Files[3], Files[4], Files[5], Files[6]};
            Lines = 0;
            unsigned int Case = 0;
            for (;; ++Case)
            {
                if (Count >= 0 && static_cast<int>(Case) >= Count)
                    break;
                int const Loaded = LoadOracleRm(F, SaveDir + "\\" + OracleRmSaveName(static_cast<int>(Case)), Case);
                if (Loaded < 0)
                    break;
                Lines += Loaded;
            }
            int const Pair = LoadOracleRm(F, SaveDir + "\\rmsave_pair.ini", Case);
            if (Pair > 0)
                Lines += Pair;
        }
        CloseOracleRmFiles(Files);
        return Lines;
    }

    // rmsave_pair.ini: the bytes of rmsave_0.ini, then those of rmsave_1.ini.
    bool WriteOracleRmPair(std::string const& Root)
    {
        FILE* const Out = OracleOpenPath(Root + "\\rmsave_pair.ini");
        if (Out == nullptr)
            return false;
        for (int Index = 0; Index < 2; ++Index)
        {
            FILE* const In = std::fopen((Root + "\\" + OracleRmSaveName(Index)).c_str(), "rb");
            if (In == nullptr)
                continue;
            char Buffer[0x400];
            size_t Read = 0;
            while ((Read = std::fread(Buffer, 1, sizeof(Buffer), In)) != 0)
                std::fwrite(Buffer, 1, Read, Out);
            std::fclose(In);
        }
        std::fclose(Out);
        return true;
    }
}

// The vendor and bar offers made and saved, then every save loaded. Returns the number of lines
// written, -1 when a file cannot be opened.
extern "C" __declspec(dllexport) int __cdecl OracleDumpRandomMissionSaves(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    FILE* const Offers = OracleOpenPath(Root + "\\rms_offers.txt");
    if (Offers == nullptr)
        return -1;
    FContentPlayerState* const State = ReplaceContentPlayerState(OracleRmPlayerId);
    int Lines = 0;
    int Saves = 0;
    bool bFailed = false;
    for (size_t BaseIndex = 0; BaseIndex < sizeof(OracleRmBases) / sizeof(OracleRmBases[0]) && !bFailed;
         ++BaseIndex)
    {
        unsigned int BaseId = 0;
        pub::GetBaseID(BaseId, OracleRmBases[BaseIndex]);
        FContentBase* const Base = FindContentBaseInfo(&BaseId);
        unsigned int SystemId = 0;
        if (Base == nullptr || pub::GetSystem(SystemId, BaseId) != 0)
            continue;
        FSystemInfo* const System = FindSystemInfo(&SystemId);
        if (System == nullptr)
            continue;

        // The vendor offers, of the base loaded as BaseSupport loads it for a player entering it.
        GRmObjectiveZones.clear();
        State->SystemRecord = System;
        Base->Load(nullptr);
        std::srand(OracleRmSeeds[1]);
        FBaseVisit Visit;
        BuildMissionVendorOffers(Base, &Visit, State);
        std::fprintf(Offers, "source=vendor_offers base=%u count=%u zones=%u\n", BaseId,
                     static_cast<unsigned int>(Visit.VendorOffers.size()),
                     static_cast<unsigned int>(GRmObjectiveZones.size()));
        ++Lines;
        for (std::list<FBaseVendorOffer>::iterator It = Visit.VendorOffers.begin(); It != Visit.VendorOffers.end();
             ++It)
        {
            FRandomMissionData* const Mission = It->Data.Mission;
            int const Save = Mission != nullptr ? Saves : -1;
            DumpOracleRmCreated(Offers, Save, "vendor", BaseId,
                                Mission != nullptr ? static_cast<unsigned int>(Mission->OfferGroup) : 0u, true,
                                It->OfferId, It->Data.Type.Data, It->Data.Loader, Mission);
            ++Lines;
            if (Mission != nullptr)
            {
                if (!SaveOracleRm(Root, Saves, Mission))
                    bFailed = true;
                ++Saves;
            }
        }
        ClearMissionVendorOffers(&Visit);
        Base->Unload();

        // The bar offers.
        for (size_t FactionIndex = 0; FactionIndex < Base->Factions.size() && !bFailed; ++FactionIndex)
        {
            FBaseFaction const& Faction = Base->Factions[FactionIndex];
            unsigned int Group = Faction.Group;
            if (Faction.MissionTypes.empty() || GetFactionProps(&Group) == nullptr ||
                GetFactionNames(&Group) == nullptr)
                continue;
            FBaseNpcMission NpcMission;
            NpcMission.MinDifficulty = Faction.MissionTypes[0].MinDifficulty;
            NpcMission.MaxDifficulty = Faction.MissionTypes[0].MaxDifficulty;
            NpcMission.Type = Faction.MissionTypes[0].Type.Data;
            std::srand(OracleRmSeeds[1]);
            FBarMissionOfferData Data;
            bool const bCreated = CreateBarMissionOffer(&NpcMission, State, BaseId, Group, &Data);
            int const Save = bCreated && Data.Mission != nullptr ? Saves : -1;
            DumpOracleRmCreated(Offers, Save, "bar", BaseId, Group, bCreated, 0u, Data.Type.Data, Data.Loader,
                                Data.Mission);
            ++Lines;
            if (Save >= 0)
            {
                if (!SaveOracleRm(Root, Saves, Data.Mission))
                    bFailed = true;
                ++Saves;
            }
            if (Data.Loader != nullptr)
            {
                Data.Loader->Slot05(Data.Mission);
                Data.Loader->Release();
            }
        }
    }
    std::fclose(Offers);
    if (bFailed || (Saves >= 2 && !WriteOracleRmPair(Root)))
    {
        RemoveContentPlayerState(OracleRmPlayerId);
        return -1;
    }
    int const Loads = LoadOracleRmSaves(Root, Root, Saves);
    GRmObjectiveZones.clear();
    RemoveContentPlayerState(OracleRmPlayerId);
    return Loads < 0 ? -1 : Lines + Loads;
}

// The C++ restore of the port's saves in <Dir>\rust\.
extern "C" __declspec(dllexport) int __cdecl OracleLoadRandomMissionSaves(char const* Dir)
{
    if (Dir == nullptr)
        return -1;
    std::string const Root(Dir);
    ReplaceContentPlayerState(OracleRmPlayerId);
    int const Lines = LoadOracleRmSaves(Root, Root + "\\rust", -1);
    GRmObjectiveZones.clear();
    RemoveContentPlayerState(OracleRmPlayerId);
    return Lines;
}
