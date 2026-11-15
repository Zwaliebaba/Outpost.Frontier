// Author: Giperion.

#pragma once

// Content.dll TU35 (06f03fa0-06f056d0): loot properties (missions\lootprops.ini,
// [mLootProps] and [PhantomLoot]). Both loaders are called from the TU32 game-table
// loader the routine (Encounters.cpp) with an open INI_Reader.

#include "common_id.h"

#include <vector>

class INI_Reader;

// [mLootProps] record, 0x1c bytes (vector the field..the field).
struct FLootProps
{
    ID_String Nickname;                                       // +0x00 CreateID(nickname)
    float DropChance;                                        // +0x04 drop_properties[0] * 0.01
    // +0x08..+0x18: drop_properties[1..5] as read (get_value_int).
    // TODO: meaning of each value is set by the readers (the routine area, TU53).
    int DropProperties[5];
};
static_assert(sizeof(FLootProps) == 0x1c, "baseline mLootProps record is 0x1c bytes");

// [PhantomLoot] record, 0x18 bytes (vector the field..the field).
struct FPhantomLoot
{
    ID_String Nickname;                                       // +0x00 CreateID(nickname)
    int ToughnessMin;                                        // +0x04 toughness_range[0]
    int ToughnessMax;                                         // +0x08 toughness_range[1], -1 -> 0x7fffffff
    float PercentChance;                                      // +0x0c percent_chance * 0.01
    int NumToDropMin;                                         // +0x10 num_to_drop[0]
    int NumToDropMax;                                         // +0x14 num_to_drop[1]
};
static_assert(sizeof(FPhantomLoot) == 0x18, "baseline PhantomLoot record is 0x18 bytes");

// append one record per [mLootProps] section.
void __cdecl LoadLootProps(INI_Reader* Reader);
// append one record per [PhantomLoot] section.
void __cdecl LoadPhantomLoot(INI_Reader* Reader);

// first record whose nickname is "default_commodity", or null.
FLootProps* __cdecl FindDefaultCommodityLootProps();
// first record whose nickname is "default_equipment", or null.
FLootProps* __cdecl FindDefaultEquipmentLootProps();
// first record whose nickname is *Nickname, or null.
FLootProps* __cdecl FindLootProps(unsigned int const* Nickname);

// (cdecl; caller SpaceSupport loot the routine 06f5d30f): every [PhantomLoot]
// record whose toughness_range holds *Toughness (signed, both ends inclusive), appended to
// Out in file order.
void __cdecl CollectPhantomLoot(int const* Toughness, std::vector<FPhantomLoot const*>* Out);

// ---------------------------------------------------------------------------------------
// TU35 -- mission offers (06f041c0-06f04be0).
// ---------------------------------------------------------------------------------------

struct FBaseNpcMission;         // BaseDB.h
struct FBarMissionOfferData;    // BaseDB.h
struct FContentPlayerState;     // ContentPlayerState.h

// (cdecl): a loader for Mission->Type into Out, then up to five attempts to
// build a random mission for Player at BaseId for Group, each at a new difficulty
// (the routine); a difficulty of 0.0 gives false at once. True when the loader was made
// and the last attempt succeeded. Out keeps the loader either way (the caller releases it
// on false). Caller FBaseNpc::ChooseOffer (the routine 06ea8714).
bool __cdecl CreateBarMissionOffer(FBaseNpcMission const* Mission, FContentPlayerState* Player,
                                   unsigned int BaseId, unsigned int Group,
                                   FBarMissionOfferData* Out);

struct FBaseVisit;              // BaseSupport.h
struct FContentBase;            // BaseDB.h

// (cdecl; callers BaseSupport msg 1 06eb4e4c, the routine 06eb561e): the
// mission vendor offers of Base for State into Visit->VendorOffers. The factions of the
// base that the player is not hostile to (the routine) get num_offers random missions
// (the routine); each becomes a pub::GF::MissionVendorOfferCreate offer. The reason of
// an empty vendor goes to pub::GF::ReportWhyMissionVendorEmpty.
void __cdecl BuildMissionVendorOffers(FContentBase* Base, FBaseVisit* Visit,
                                      FContentPlayerState* State);
// (cdecl; callers BaseSupport msg 2 06eb4d9e, the routine 06eb560c): every
// offer gets pub::GF::MissionVendorOfferDestroy and its loader slot 5 (the mission) and
// Release; then the list is emptied.
void __cdecl ClearMissionVendorOffers(FBaseVisit* Visit);
