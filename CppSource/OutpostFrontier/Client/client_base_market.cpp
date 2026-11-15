// Author: Giperion.

#include "client_base_market.h"

#include "login_mgr.h"    // G_Server (the field)
#include "player_spawn.h" // GetPlayerConnectionId (the field)

#include "common_goods.h"
#include "common_server.h" // IServer::GFGoodBuy / GFGoodSell, SGFGoodBuyInfo / SGFGoodSellInfo

#include <stdlib.h>

void ClientMarketFill(FClientMarket* Market, float PriceVariance, BaseData const* Base)
{
    // (thiscall, RET 8).
    GoodInfoList const* const Goods = GoodList_get();
    std::list<GoodInfo*> const* const List = Goods->get_list();
    if (List->empty())
        return;

    // the two bounds of the scale, each stored as a float.
    float const Low = static_cast<float>(1.0 - static_cast<double>(PriceVariance));
    float const Range = static_cast<float>(
        (static_cast<double>(PriceVariance) + 1.0) - static_cast<double>(Low));

    for (std::list<GoodInfo*>::const_iterator It = List->begin(); It != List->end(); ++It)
    {
        GoodInfo const* const Good = *It; // node +0x08

        // a default entry; stock and scale 0.
        FClientMarketGood Entry;
        Entry.Stock = 0;
        Entry.PriceScale = 0.0f;
        Entry.Field24 = 0;
        unsigned const Key = Good->Id; // +0x48

        // the given base's market, else the current base's.
        std::map<unsigned, MarketGoodInfo> const* const Source =
            Base != nullptr ? &Base->Market : &GetClientCurrentLocationBaseData()->Market;

        // the base's entry (seven dwords), or the good's id and price.
        std::map<unsigned, MarketGoodInfo>::const_iterator Found = Source->find(Key);
        if (Found != Source->end())
        {
            Entry.Info = Found->second;
        }
        else
        {
            Entry.Info.GoodId = Good->Id;   // +0x48
            Entry.Info.Price = Good->Price; // +0x58
            Entry.Info.MinInventory = 0;
            Entry.Info.MaxInventory = 0;
            Entry.Info.Type = TransactionBuyAndSell;
        }

        // stocked when there is a minimum and a positive maximum.
        if (Entry.Info.MinInventory != 0 && Entry.Info.MaxInventory > 0)
            Entry.Stock = 0x7f000000;

        // FILD rand(); FMUL 2^-15 (the field); FMUL Range; FADD Low.
        int const Roll = rand();
        Entry.PriceScale = static_cast<float>(static_cast<double>(Roll) * (1.0 / 32768.0) *
                                                  static_cast<double>(Range) +
                                              static_cast<double>(Low));

        // insert Key with a default value (the routine), then overwrite the
        // value with the entry.
        (*Market)[Key] = Entry;
    }
}

int ClientMarketCollectCategory(FClientMarket const* Market,
                                std::list<FClientMarketGood const*>* Out, unsigned Category)
{
    // (thiscall, RET 8): the map in key order.
    int Count = 0;
    for (FClientMarket::const_iterator It = Market->begin(); It != Market->end(); ++It)
    {
        FClientMarketGood const& Entry = It->second; // node +0x10
        if (Entry.Stock <= 0)                        // node +0x2c, JLE
            continue;
        GoodInfo const* const Good = GoodList::find_by_id(Entry.Info.GoodId);
        if (Good == nullptr)
            continue;
        if (Good->Category != Category) // +0x4c
            continue;
        Out->push_back(&Entry);
        ++Count;
    }
    return Count;
}

float ClientMarketGetPrice(FClientMarket const* Market, unsigned GoodId)
{
    // (thiscall, RET 4).
    FClientMarket::const_iterator It = Market->find(GoodId); // + key test
    if (It == Market->end())
        return 0.0f;

    // FLD Price; FMUL PriceScale; ftol; FILD.
    FClientMarketGood const& Entry = It->second;
    int const Truncated =
        static_cast<int>(static_cast<double>(Entry.Info.Price) * static_cast<double>(Entry.PriceScale));
    float Price = static_cast<float>(Truncated);

    // FCOM 9999999.0 (the field); TEST AH,0x41 / JNZ keeps a value that
    // is less or equal, the greater one becomes 9999999.
    if (static_cast<double>(Truncated) > 9999999.0)
        Price = 9999999.0f;
    // FCOM 0.0; TEST AH,5 / JP keeps a value that is not below 0; below 0 it
    // becomes 1.0 (the field).
    if (Price < 0.0f)
        Price = 1.0f;
    return Price;
}

int ClientMarketGetTransactionType(FClientMarket const* Market, unsigned GoodId)
{
    // (thiscall, RET 4).
    FClientMarket::const_iterator It = Market->find(GoodId);
    if (It == Market->end())
        return -1;
    return static_cast<int>(It->second.Info.Type); // +0x10
}

float ClientMarketGetMinLevel(FClientMarket const* Market, unsigned GoodId)
{
    // (thiscall, RET 4).
    FClientMarket::const_iterator It = Market->find(GoodId);
    if (It == Market->end())
        return 0.0f;
    return It->second.Info.MinLevel; // +0x14
}

float ClientMarketGetMinRep(FClientMarket const* Market, unsigned GoodId)
{
    // (thiscall, RET 4).
    FClientMarket::const_iterator It = Market->find(GoodId);
    if (It == Market->end())
        return -1.0f;
    return It->second.Info.MinRep; // +0x18
}

FClientMarketGood* ClientMarketFindEntry(FClientMarket* Market, unsigned GoodId)
{
    // (thiscall, RET 4): the routine lower_bound plus the key test; the node's
    // value (+0x10), or 0 for the end.
    FClientMarket::iterator It = Market->find(GoodId);
    if (It == Market->end())
        return nullptr;
    return &It->second;
}

void ClientMarketRequestBuy(FClientMarket const* Market, unsigned BaseId, unsigned GoodId, int Count)
{
    // (thiscall, RET 0xc): a 0x10-byte local {BaseId, 0, GoodId, Count}, then
    // G_Server (the field, not null-tested) slot 45 (+0xb4) with it and the field.
    (void)Market;
    SGFGoodBuyInfo Info;
    Info.Field_0x00 = BaseId;
    Info.Field_0x04 = 0;
    Info.Field_0x08 = GoodId;
    Info.Field_0x0c = static_cast<unsigned>(Count);
    G_Server->GFGoodBuy(Info, GetPlayerConnectionId());
}

void ClientMarketRequestSell(FClientMarket const* Market, unsigned BaseId, unsigned GoodId, int Count)
{
    // (thiscall, RET 0xc): a 0xc-byte local {BaseId, GoodId, Count}, then G_Server
    // slot 46 (+0xb8) with it and the field.
    (void)Market;
    SGFGoodSellInfo Info;
    Info.Field_0x00 = BaseId;
    Info.Field_0x04 = GoodId;
    Info.Field_0x08 = static_cast<unsigned>(Count);
    G_Server->GFGoodSell(Info, GetPlayerConnectionId());
}
