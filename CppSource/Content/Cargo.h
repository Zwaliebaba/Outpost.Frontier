// Author: Giperion.

#pragma once

// Content.dll TU44 (06f34ef0-06f36f10): faction / NPC ship arch / cargo helpers used by the
// encounter spawn path (TU29), the random missions (TU31, TU61, TU63) and SystemInfo (TU58).
// Lane CNT-POP (implementation notes, TU map). All free functions are cdecl in baseline.

#include "common_weighted_vector.h"     // weighted_vector<unsigned int>

#include <list>
#include <vector>

struct FBaseCommodity;          // BaseDB.h (TU17 market list element, 8 bytes)
struct FContentNpcShipParams;   // NpcShip.h (TU43)
struct FEncounterShip;          // EncounterParams.h (TU29 ship line, 0x38)
struct FNpcShipArch;            // NpcShips.h (TU42)
struct FSystemInfo;             // SystemInfo.h (TU58)

// A cargo candidate of the TU44 generator, baseline 12 bytes (element copy the routine).
struct FCargoPick
{
    unsigned int GoodId;    // +0x00
    unsigned int Count;     // +0x04 FitCargoToHold rewrites it
    float Weight;           // +0x08 the market count (1 when 0) as a float; 1.0f for the pick

    FCargoPick() = default;
    // (RET 8): the good and count of a market entry, then the weight.
    FCargoPick(FBaseCommodity const* Entry, float const* InWeight);
    FCargoPick(unsigned int InGoodId, unsigned int InCount, float InWeight)
        : GoodId(InGoodId)
        , Count(InCount)
        , Weight(InWeight)
    {
    }
};

// (cdecl; callers TU29 06ef5098, GenerateCargo 06f36bfd): shares 80% of the
// cargo hold of ShipArchId (pub::GetCargoHoldSize) minus the volume already listed among the
// picks by weight, turns each share into a count (share / good volume), and drops the picks
// left with none.
void __cdecl FitCargoToHold(unsigned int ShipArchId, std::vector<FCargoPick>* Cargo);

// (cdecl; callers GenerateCargo 06f36a2a / 06f36abc): for every good of Goods
// (outer) and every market entry of Market with that good (inner), Out gets {good, count,
// count or 1 as the weight}. Out is appended to.
void __cdecl FilterCargoPicks(std::vector<unsigned int> const* Goods,
                              std::vector<FBaseCommodity> const* Market,
                              std::vector<FCargoPick>* Out);

// (thiscall on Picks; callers GenerateCargo 06f36a4e / 06f36ae0; the same pick
// is inlined four more times there): a pick chosen by weight (rand() / 32768 of the total).
FCargoPick const* __cdecl PickWeightedCargo(std::vector<FCargoPick> const* Picks);

// (cdecl; caller TU29 06ef4ff4 for trade-guided ships): picks the good an NPC
// ship carries from the markets of its guide's two bases (FGuideDesc +0x5c / +0x60) and the
// MarketGoods of its faction, stores the pick in the guide description (+0x4c, +0x50), and
// fills Out and Params->Cargo with it fitted to the hold. The system argument is not read.
void __cdecl GenerateCargo(FSystemInfo* Unused, FContentNpcShipParams* Params,
                           std::vector<FCargoPick>* Out);

// pub::Reputation::GetReputation(From, To) (group / group) >= 0.2 -> 1
// (friendly), > -0.2 -> 2 (neutral), else 4 (hostile). Callers TU29 06ef4559, TU58 06f787f4 /
// .
unsigned int __cdecl GetFactionFeelingMask(unsigned int FromGroup, unsigned int ToGroup);

// the same thresholds over pub::Reputation::GetReputation(VibeHandle, Group)
// (vibe / group). Callers TU35 06f043fb, RM 06f80cb9 / 06f80d6c / 06f828b9.
unsigned int __cdecl GetVibeFeelingMask(unsigned int Group, int VibeHandle);

// of the total weight of Factions, *OutHostileFrom = the share of the factions
// Group is hostile to, *OutHostileTo = the share of the factions hostile to Group; both 0 for
// an empty list. Callers TU58 06f779e0 / 06f77a7e.
void __cdecl GetHostileFactionShares(weighted_vector<unsigned int> const* Factions,
                                     unsigned int const* Group, float* OutHostileFrom,
                                     float* OutHostileTo);

// Out = every faction group (GetFactionGroups order) whose FactionProps
// npc_ship list holds every id of NpcArchs, each once. Caller TU29 06ef42fe.
void __cdecl GetNpcArchFactions(std::vector<unsigned int> const* NpcArchs,
                                std::vector<unsigned int>* Out);

// Out = every faction group (GetFactionGroups order) with an npc_ship arch
// whose npc classes share an id with ClassIds, each once. Callers RM 06f80afc / 06f82757.
void __cdecl GetNpcClassFactions(std::vector<unsigned int> const* ClassIds,
                                 std::vector<unsigned int>* Out);

// Out = every faction group with an npc_ship arch that fits Ship
// (NpcArchFitsShipLine), each once. Caller TU29 06ef4272.
void __cdecl GetShipLineFactions(FEncounterShip const* Ship, std::vector<unsigned int>* Out);

// every class of Ship->ShipClasses is matched by Arch: a ship class (TU51
// FindShipClass) through one of its members in Arch->NpcClasses; any other id through
// Arch->NpcClasses or the arch ship's mission properties (TU51 GRmShipMissionProperties).
bool __cdecl NpcArchFitsShipLine(FEncounterShip const* Ship, FNpcShipArch const* Arch);

// one of Group's npc_ship archs has one of ClassIds among its npc classes.
// Callers RM 06f80e28 / 06f82978.
bool __cdecl FactionHasNpcClass(unsigned int Group, std::vector<unsigned int> const* ClassIds);

// Out = Group's npc_ship archs that fit Ship, in npc_ship order. Caller TU63
// .
void __cdecl GetFactionShipLineArchs(unsigned int Group, FEncounterShip const* Ship,
                                     std::list<FNpcShipArch*>* Out);

// a random npc_ship arch of Group fitting Ship; with Level > 0 only archs within
// one level of it, or else the closest one. Null when Group has no FactionProps or nothing
// fits. Callers TU29 06ef5738, TU31 06efbf2e / 06efbf46, RM 06f8364f / 06f97740.
FNpcShipArch* __cdecl PickFactionShipLineArch(unsigned int Group, FEncounterShip const* Ship,
                                              int Level);
