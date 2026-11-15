// Author: Giperion.

#pragma once

// baseline NN_Dealer (RTTI .?AVNN_Dealer@@ : NN_Inventory : WinControl) -- the NeuroNet
// equipment / commodity / ship dealer window. It is opened next to an NN_Inventory (its +0x974),
// which points back at it through its own +0x360.

// WINFACTORY<NN_Dealer> "NN_Dealer": static init the routine (WinFactory head) with vftable
// ; Create the routine (operator_new(0xa00), the routine, then slot 9).

// vftable 0x005d192c (47 slots) -- NN_Inventory's with these overrides:
//   11 the routine Update     42 the routine SetProperty     45 the routine build the controls
// Slot 0 is NN_Inventory's the routine: NN_Dealer adds no destructor of its own.

// baseline layout (size 0xa00): NN_Inventory's 0x974 bytes, then the members below.

#include "nn_inventory.h"

#include <list>

class UiScroll;
struct FClientMarketGood;

// One reputation entry of the REQUEST_PLAYER_STATS block (IClient slot 94, the branch): the
// entries follow the block's first table, at +0x34 + [+0x20] * 8, and number [+0x30].
// compares +0 with a Reputation::get_id result and copies +4 into NN_Dealer's
// float BaseFeeling.
struct FPlayerStatsRepEntry
{
    unsigned GroupId; // +0x00
    float Feeling;    // +0x04
};
static_assert(sizeof(FPlayerStatsRepEntry) == 8, "player-stats reputation entry is 8 bytes");

class NnDealer : public NnInventory
{
public:
    // NN_Inventory's ctor (inlined), ResetFields again, the dealer fields, then
    // the player-stats request to the server.
    NnDealer();

    // slot 45 the routine -- build the dealer's controls: the five item rows, the three-pane
    // info card, the welcome block, the buy / sell arrows, the partner's transfer panel
    // (+0x364..+0x380 of NN_Inventory), the quantity panel and, for the ship dealer, the
    // continue / back / price controls.
    void Slot45() override;

    // slot 11 the routine -- follow the partner's visibility and info card panes, update the
    // children, and while active refresh the list and lay out the buy / sell / quantity / ship
    // purchase controls from the two windows' selections.
    void Update() override;

    // slot 42 the routine (RET 0xc) -- a switch over 0x15..0x45 (byte table 0x00482a5c):
    // hardpoint picks and drops, the sliders, the quantity edit and the clicks; 0x2c / 0x2d go
    // to NN_Inventory's SetProperty, every other message returns 0 (WinControl's slot 42 is
    // never called).
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // (thiscall, RET 8) -- show ErrorHelp (+0x9d4) and hide the buy / sell and
    // quantity controls; the text is the ids itself when Arg is 0, otherwise the ids' string
    // used as a swprintf format with Arg.
    void ShowErrorText(unsigned Ids, int Arg);

    // (thiscall, RET 8) -- select a row of the dealer's list: quantity back to 1,
    // the row's item on the ship model, the partner inventory's filter / hardpoint hint, and
    // the partner's own selection cleared.
    void SelectDealerRow(int Row, int Unused);

    // the ship dealer's "continue": pay the difference between the new ship
    // package (+0x9f8) and the trade-in (+0x9fc, the resale of both loadouts), then send the
    // server the new loadout, collision groups, ship, hull and cash.
    void BuyShip();

    // rebuild the commodity (1) / equipment (3) dealer's rows from the base
    // market, then the row count bookkeeping (scroll range, first row, selection) and the
    // selected good on the partner's ship view.
    void RefreshMarketList();

    // (RET 4) -- whether the equipment dealer (trade mode 3) offers GoodId to this
    // player: the base's reputation and rank requirements and a free hardpoint / launcher for
    // it. Always true in the other trade modes; false when not docked or without a market.
    bool CanBuyGood(unsigned GoodId);

    // (RET 4) -- the market list order of every trade mode but 3: each entry's
    // CanBuyGood byte (+0x24) refreshed, then goods this player can buy first, each group by the
    // equipment's name (_wcsicmp); the list is relinked in that order.
    void SortMarketGoodsByName(std::list<FClientMarketGood const*>* Goods);

    // (RET 4) -- the equipment dealer's (trade mode 3) order: CanBuyGood byte
    // (+0x24) first, then the archetype class, then the "special" flag (auto turret / guided
    // ammo / hp type 10 or 0x23 munition), then the package price; relinked in that order.
    void SortEquipmentMarketGoods(std::list<FClientMarketGood const*>* Goods);

    // (RET 8) -- how many of GoodId the player can buy now; *OutReason gets why
    // it is 0 (1 no room in the hold, 2 credits, 3 no ship, 9 / 10 nanobot / battery cap,
    // 11 ammo cap) or 0.
    int GetMaxBuyCount(unsigned GoodId, int* OutReason);

    // give the partner inventory the ship this dealer shows and reset the
    // continue / buy labels (the "back to your own ship" step of the ship dealer).
    void RestorePlayerShipView();

    // (RET 4) -- ship dealer (trade mode 2): show the ship package GoodId -- its
    // hull and its first addon list -- in this window's ship view, remember the package
    // (+0x97c) and its price (+0x9f8).
    void ShowShipPackage(unsigned GoodId);

    // the good id of the selected row of a commodity (1) or equipment (3)
    // dealer, read from the market of the base the player is docked at; 0 when nothing is
    // selected, in the other trade modes, or when no market is loaded.
    unsigned GetSelectedMarketGood();

    // (cdecl) -- the feeling of the player's group towards the faction that runs
    // the base the player is docked at; 1.0 when it cannot be found.
    static float GetBaseFactionFeeling();

    // (cdecl) -- the server's player-stats reply reached a client with no
    // NN_PlayerInfo open (the routine's fallback): when an NN_Dealer is registered and the
    // player's base has a docking object in the current system, the reputation entry of that
    // object's group is copied into BaseFeeling (+0x990). Table holds Count 8-byte entries.
    static void ApplyPlayerStatsFeeling(int Count, FPlayerStatsRepEntry const* Table);

    // --- baseline members (baseline offsets) ---
    NnInventory* Inventory;          // +0x974 -- the inventory this dealer sits next to
    int TradeMode;                   // +0x978 -- 1 (ctor); Slot23 0xef02 resets it to 0
    int Field97c;                    // +0x97c
    int Field980;                    // +0x980 -- 1
    unsigned char Field984;          // +0x984 -- 0
    unsigned char Pad985[3];
    int Field988;                    // +0x988 -- 1
    unsigned char Field98c;          // +0x98c -- 0; RestorePlayerShipView clears it
    unsigned char Pad98d[3];
    float BaseFeeling;               // +0x990 -- GetBaseFactionFeeling() (ctor)
    WinControl* WelcomeText;         // +0x994 "WelcomeText"
    WinControl* WelcomeLine;         // +0x998 "WelcomeLine"
    WinControl* WelcomeHelp;         // +0x99c "WelcomeHelp"
    // +0x994..+0x9f0: the controls Slot45 (the routine) creates; the names are the baseline
    // control names it passes to the factory (stores at 0x00481339..0x00481ffa).
    WinControl* BuyText;             // +0x9a0 "BuyText"
    WinControl* BuyArrow;            // +0x9a4 "BuyArrow"
    WinControl* SellText;            // +0x9a8 "SellText"
    WinControl* SellArrow;           // +0x9ac "SellArrow"
    WinControl* BuySellTradeButton;  // +0x9b0 "BuySellTradeButton"
    WinControl* QuantityPrompt;      // +0x9b4 "QuantityPrompt"
    UiScroll* QuantitySlider;        // +0x9b8 "QuantitySlider" (UIScroll)
    WinControl* QuantityText;        // +0x9bc "QuantityText"
    WinControl* QuantityPrice;       // +0x9c0 "QuantityPrice"
    WinControl* QuantityTotal;       // +0x9c4 "QuantityTotal"
    WinControl* QuantityTotalText;   // +0x9c8 "QuantityTotalText"
    WinControl* QuantityEdit;        // +0x9cc "QuantityEdit" (EditControl)
    WinControl* TransferItemQuantity; // +0x9d0 "TransferItemQuantity"
    WinControl* ErrorHelp;           // +0x9d4 "ErrorHelp"
    WinControl* PlusMinus;           // +0x9d8 "PlusMinus"
    int Field9dc;                    // +0x9dc -- no reference found in the listing
    WinControl* ContinueButton;      // +0x9e0 "ContinueButton" (ship dealer only)
    WinControl* ContinueText;        // +0x9e4 "ContinueText" (ship dealer only)
    WinControl* ShipBuyProblem;      // +0x9e8 "ShipBuyProblem" (ship dealer with a ship)
    WinControl* BackToShipDealer;    // +0x9ec "BackToShipDealer" (ship dealer with a ship)
    WinControl* ShipDealerShipPrice; // +0x9f0 "ShipDealerShipPrice" (ship dealer with a ship)
    unsigned char Field9f4;          // +0x9f4 -- 1
    unsigned char Pad9f5[3];
    int Field9f8;                    // +0x9f8 -- 0
    int Field9fc;                    // +0x9fc -- the partner ship's value (RestorePlayerShipView)

private:
    NnDealer(NnDealer const&) = delete;
    NnDealer& operator=(NnDealer const&) = delete;
};
