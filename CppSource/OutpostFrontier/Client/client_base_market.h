// Author: Giperion.

#pragma once

// The market of the base the player is at, as the client keeps it (Gf\LocationController.cpp).

// Every loaded base's runtime (the 0x4c-byte object the routine allocates, see
// menu_universe_base.cpp) carries at +0x30 a std::map from good id to FClientMarketGood,
// constructed by the routine and filled by the routine from the base's BaseData market with a
// random price scale. the routine (GetClientLocationBaseMarket) hands out its address; the
// methods below are thiscall on it.

// Map node: left +0x0, parent +0x4, right +0x8, key +0xc, value +0x10 (0x28 bytes), colour +0x38,
// _Isnil +0x39 (the walk at 0x00437ffe reads it).

#include "common_goods.h"

#include <list>
#include <map>

class BaseData;

// The mapped value (0x28 bytes, copied as ten dwords at 0x00437f1a / 0x00437f46).
struct FClientMarketGood
{
    MarketGoodInfo Info; // +0x00 BaseData's entry, or {Id, GoodInfo price} when the base has none
    int Stock;           // +0x1c 0x7f000000 when Info.MinInventory != 0 and Info.MaxInventory > 0
    float PriceScale;    // +0x20 1 - variance + rand() / 32768 * 2 * variance
    // +0x24 -- never written by the routine (the ten-dword copies take it from an unset stack
    // slot) and never read by the market methods. Its low byte is NN_Dealer::CanBuyGood's result,
    // stored by the dealer's list sorts (0x0048304e in the routine, 0x00483310 in the routine)
    // and read by their comparisons; the list refresh the routine passes it on to the row
    // (NnDealer::SortMarketGoodsByName / SortEquipmentMarketGoods / RefreshMarketList).
    unsigned Field24;
};

static_assert(sizeof(FClientMarketGood) == 0x28, "the client market value is 0x28 bytes");

typedef std::map<unsigned, FClientMarketGood> FClientMarket;

// (thiscall, RET 8) -- one entry per good of GoodList_get(), in list order: Base's
// market entry (Base == null: the current base's BaseData, the routine) or a default one, its
// stock, and a price scale drawn with rand(). Existing entries are overwritten.
void ClientMarketFill(FClientMarket* Market, float PriceVariance, BaseData const* Base);

// (thiscall, RET 8) -- append to Out every entry with Stock > 0 whose good's
// Category (GoodInfo +0x4c) is Category, in key order; the count appended.
int ClientMarketCollectCategory(FClientMarket const* Market,
                                std::list<FClientMarketGood const*>* Out, unsigned Category);

// (thiscall, RET 4) -- ftol(Price * PriceScale) clamped to 9999999, a negative
// result 1.0; 0.0 for a good the market does not hold.
float ClientMarketGetPrice(FClientMarket const* Market, unsigned GoodId);

// (thiscall, RET 4) -- the entry's TransactionType (+0x10), -1 without an entry.
int ClientMarketGetTransactionType(FClientMarket const* Market, unsigned GoodId);

// (thiscall, RET 4) -- the entry's MinLevel (+0x14), 0.0 without an entry.
float ClientMarketGetMinLevel(FClientMarket const* Market, unsigned GoodId);

// (thiscall, RET 4) -- the entry's MinRep (+0x18), -1.0 without an entry.
float ClientMarketGetMinRep(FClientMarket const* Market, unsigned GoodId);

// (thiscall, RET 4) -- the entry of GoodId (map node +0x10), null without one.
FClientMarketGood* ClientMarketFindEntry(FClientMarket* Market, unsigned GoodId);

// (thiscall, RET 0xc; the market itself is not read) -- ask the server to sell the
// player Count of GoodId at BaseId: IServer slot 45 GFGoodBuy({BaseId, 0, GoodId, Count}) with
// the player's connection id.
void ClientMarketRequestBuy(FClientMarket const* Market, unsigned BaseId, unsigned GoodId, int Count);

// (thiscall, RET 0xc; the market itself is not read) -- the reverse: IServer slot 46
// GFGoodSell({BaseId, GoodId, Count}) with the player's connection id.
void ClientMarketRequestSell(FClientMarket const* Market, unsigned BaseId, unsigned GoodId, int Count);

// (thiscall on the field, RET 4) -- the market of BaseId's runtime (+0x30). On a
// miss baseline FDUMPs (LocationController.cpp:0x100) and reads through the null entry.
FClientMarket* GetClientLocationBaseMarket(unsigned BaseId);

// (thiscall on the field) -- the BaseData of the base the player is at
// (controller +0x34). Same miss behaviour.
BaseData* GetClientCurrentLocationBaseData();
