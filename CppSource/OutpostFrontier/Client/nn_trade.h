// Author: Giperion.

#pragma once

// The multiplayer player-to-player trade window (baseline NN_Trade, Client\NeuroNet\NN_Trade.cpp,
// OutpostFrontier.exe 0x004bfb60..0x004c3908).

// RTTI .?AVNN_Trade@@ : WinControl, Event::Handler. operator_new(0x440) (0x004c381c).
// WINFACTORY<NN_Trade> the field "NN_Trade": static init the branch (ctor the routine,
// atexit the branch -> the routine); Create the routine (operator_new, the routine, the
// class-name pointer, slot 9; a failed Create is followed by the scalar deleting destructor).

// vftable 0x005d62ac -- WinControl's with these overrides (raw dword dump):
//   0 the routine scalar deleting dtor (the routine), 9 the branch Create, 13 the branch
//   Draw, 20 the routine KeyEvent, 36 the branch GetHardpointTransform, 42 the routine
//   SetProperty.
// IDataViewOwner face +0x7c: vftable of WinControl (PTR_OnReceive 0x0053aaa0).
// Event::Handler face +0x32c: vftable 0x005d6294, slot 0 the branch (MOV AL,1; RET 4).

// The window sits between the two players' cargo lists. +0x338 is the local player's cargo
// (Rebuild from the player's EquipDescList), +0x334 the offer the local player puts up (sent
// with IServer::AddTradeEquip / DelTradeEquip / SetTradeMoney), +0x330 the other player's offer
// (filled from IClient slots 104..106 through SetTargetMoney / AddTargetItem /
// RemoveTargetItem). Picking an item of +0x338 or +0x334 opens the quantity panel (mode 3).

// The list items +0x428 / CTradeEquipList::SelectedItem point into the lists'
// std::list<EquipDesc> nodes and stay valid across inserts, as in baseline.

#include "event_handler.h"
#include "trade_equip_list.h"
#include "wincontrol_base.h"

#include <stddef.h>
#include <stdint.h>

class CShip;
class NnTextControl;
class RichTextWin;
class ScrollControl;
class ShipViewControl;

class NnTrade : public WinControl, public EventHandler
{
public:
    // WinControl's ctor, the vptrs, G_NnTrade = this, every member below 0
    // (+0x42c = -1). +0x394, +0x3e4, +0x438 and +0x43c are not written.
    NnTrade();
    // the three lists through their deleting destructors, G_NnTrade = 0, then the
    // Event::Handler (the routine) and WinControl (the routine) destructors.
    ~NnTrade() override;

    // slot 9 the branch (RET 8) -- build the whole window.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the branch -- the children, the caret of the quantity text and the two cargo
    // fill quads.
    void Draw() override;
    // slot 20 the routine (RET 8) -- the quantity keypad (mode 3 only).
    bool KeyEvent(unsigned Key, unsigned char* Modifiers) override;
    // slot 36 the branch (RET 8) -- WinControl's lookup, with this window's +0xa8 added and the
    // z translation forced to 0; a miss is the identity with no translation at all.
    bool GetHardpointTransform(char const* Name, float* OutTransform12) override;
    // slot 42 the routine (RET 0xc) -- hover (0x2c / 0x2d), clicks (0x26), scroll bars (0x3e)
    // and the close (0xef02). Every message returns 0; nothing is passed to WinControl.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face the branch -- true.
    bool OnEvent(unsigned MsgId) override;

    // (RET 4) -- clamp to the quantity bar's range, format with thousands commas
    // into the quantity text, store in +0x364.
    void SetQuantity(int NewQuantity);
    // (RET 4) -- 1 closed, 2 browsing, 3 quantity panel; 4 is 3 re-entered after the
    // other side changed its offer (the "insufficient cargo space" line when the chosen quantity
    // no longer fits).
    void SetMode(int NewMode);
    // (RET 4) -- IClient slot 105: the other player offered Item.
    void AddTargetItem(EquipDesc const* Item);
    // (RET 4) -- IClient slot 106: the other player took Item back.
    void RemoveTargetItem(EquipDesc const* Item);
    // (RET 4) -- IClient slot 101: the other player's ship.
    void SetTarget(unsigned ObjectId);
    // (RET 4) -- the other player's "accepted" text: ids 0x759 green / 0x75a red.
    void SetTargetAccepted(unsigned char bAccepted);
    // (RET 4) -- the same for the local player's text (+0x410).
    void SetPlayerAccepted(unsigned char bAccepted);
    // (RET 4) -- IClient slot 104: CTradeEquipList::SetMoney (the routine) inlined
    // on the other player's list, without its return value.
    void SetTargetMoney(int Money);
    // (RET 8) -- open or close List's info card (the player's +0x338 or the other
    // player's +0x330); List's select mode follows bShow.
    void ShowInfoCard(CTradeEquipList* List, unsigned char bShow);

    CTradeEquipList* TargetList;       // +0x330 "hp_target_inventory_list" (5 rows, card, no hl)
    CTradeEquipList* CenterList;       // +0x334 "hp_center_inventory_list" (5 rows, no card, hl)
    CTradeEquipList* PlayerList;       // +0x338 "hp_player_inventory_list" (5 rows, card, hl)
    CTradeEquipListLine* QuantityLine; // +0x33c the item row of the quantity panel (no owner)
    WinControl* TradeClose;            // +0x340 "TradeClose" -- TRADE_closeinterfacex, hp_tradecancel
    unsigned Field344;                 // +0x344 ctor 0; no other writer or reader in 004bfb60..004c3908. TODO: meaning
    WinControl* QuantityTitle;         // +0x348 "QuantTitTxt"
    ScrollControl* QuantityBar;        // +0x34c "QuantScrollBar"
    WinControl* QuantityFrame;         // +0x350 "QuantFrame"
    WinControl* QuantityAccept;        // +0x354 "QuantAccept"
    WinControl* QuantityReject;        // +0x358 "QuantReject"
    NnTextControl* QuantityText;       // +0x35c "QuantText"
    unsigned char bQuantityTyped;      // +0x360 a digit was typed since the panel opened
    unsigned char Pad361[3];
    int Quantity;                      // +0x364
    WinControl* PlayerCardFrame;       // +0x368 "PICFrame"
    WinControl* PlayerCardClose;       // +0x36c "PICClose"
    ShipViewControl* PlayerCardModel;  // +0x370 "PICModel"
    WinControl* PlayerCardTitle;       // +0x374 "PICTitle"
    RichTextWin* PlayerCardInfo;       // +0x378 "PICInfo"
    WinControl* TargetCardFrame;       // +0x37c "TICFrame"
    WinControl* TargetCardClose;       // +0x380 "TICClose"
    ShipViewControl* TargetCardModel;  // +0x384 "TICModel"
    WinControl* TargetCardTitle;       // +0x388 "TICTitle"
    RichTextWin* TargetCardInfo;       // +0x38c "TICInfo"
    int Mode;                          // +0x390
    unsigned Field394;                 // +0x394 not written by the ctor; no access in 004bfb60..004c3908. TODO: meaning
    WinControl* PlayerName;            // +0x398 "PlayerName"
    WinControl* TargetName;            // +0x39c "TargetName"
    WinControl* TitleFrame;            // +0x3a0 "TitleFrame"
    WinControl* TitleArrow;            // +0x3a4 "TitleArrow"
    WinControl* TitleText;             // +0x3a8 "TitleText"
    unsigned Field3ac;                 // +0x3ac ctor 0; no other access. TODO: meaning
    unsigned Field3b0;                 // +0x3b0 ctor 0; no other access. TODO: meaning
    WinControl* PlayerCargoText;       // +0x3b4 "PlayerCargoText"
    WinControl* PlayerCargoFull;       // +0x3b8 "PlayerCargoBarFull" -- TRADE_cargofull
    WinControl* PlayerCargoEmpty;      // +0x3bc second "PlayerCargoBarFull" -- TRADE_cargoempty
    double PlayerCargoUsed;            // +0x3c0
    int PlayerNanobots;                // +0x3c8
    int PlayerNanobotLimit;            // +0x3cc (-1 -> 50000000)
    int PlayerBatteries;               // +0x3d0
    int PlayerBatteryLimit;            // +0x3d4 (-1 -> 50000000)
    WinControl* TargetCargoText;       // +0x3d8 "TargetCargoText"
    WinControl* TargetCargoFull;       // +0x3dc "TargetCargoBarFull" -- TRADE_cargofull
    WinControl* TargetCargoEmpty;      // +0x3e0 second "TargetCargoBarFull" -- TRADE_cargoempty
    unsigned Field3e4;                 // +0x3e4 never accessed in 004bfb60..004c3908 (the gap before the double). TODO: meaning
    double TargetCargoUsed;            // +0x3e8
    double TargetHoldSize;             // +0x3f0
    int TargetNanobots;                // +0x3f8
    int TargetNanobotLimit;            // +0x3fc (-1 -> 50000000)
    int TargetBatteries;               // +0x400
    int TargetBatteryLimit;            // +0x404 (-1 -> 50000000)
    int NanobotsOffered;               // +0x408
    int BatteriesOffered;              // +0x40c
    NnTextControl* PlayerAcceptedText; // +0x410 "PlayerAcceptedText"
    NnTextControl* TargetAcceptedText; // +0x414 "TargetAcceptedText"
    unsigned char Byte418;             // +0x418 ctor 0; no other access. TODO: meaning
    unsigned char Byte419;             // +0x419 ctor 0; no other access. TODO: meaning
    unsigned char Pad41a[2];
    WinControl* AcceptTradeFrame;      // +0x41c "AcceptTradeFrame"
    WinControl* AcceptTradeText;       // +0x420 "AcceptTradeText"
    CTradeEquipListLine* SelectedLine; // +0x424 the row the quantity panel was opened from
    EquipDesc* SelectedItem;           // +0x428 its item (a node of that row's list)
    unsigned TargetObjectId;           // +0x42c ctor -1
    unsigned TargetOwnerPlayer;        // +0x430 the other ship's CSimple +0xb4
    unsigned char bHudTargetWasExpanded; // +0x434 the field +0x4d0 when the window opened
    unsigned char bHudStatusWasExpanded; // +0x435 the field +0x4d0 when the window opened
    unsigned char Pad436[2];
    CShip* PlayerShip;                 // +0x438 set by Create
    CShip* TargetShip;                 // +0x43c set by SetTarget

private:
    NnTrade(NnTrade const&) = delete;
    NnTrade& operator=(NnTrade const&) = delete;
};

static_assert(sizeof(NnTrade) == 0x440, "baseline NN_Trade is 0x440 bytes (operator_new 0x004c381c)");
static_assert(offsetof(NnTrade, TargetList) == 0x330, "NN_Trade +0x330");
static_assert(offsetof(NnTrade, Quantity) == 0x364, "NN_Trade +0x364");
static_assert(offsetof(NnTrade, Mode) == 0x390, "NN_Trade +0x390");
static_assert(offsetof(NnTrade, PlayerCargoUsed) == 0x3c0, "NN_Trade +0x3c0");
static_assert(offsetof(NnTrade, TargetCargoUsed) == 0x3e8, "NN_Trade +0x3e8");
static_assert(offsetof(NnTrade, PlayerAcceptedText) == 0x410, "NN_Trade +0x410");
static_assert(offsetof(NnTrade, SelectedItem) == 0x428, "NN_Trade +0x428");
static_assert(offsetof(NnTrade, TargetShip) == 0x43c, "NN_Trade +0x43c");

// the open trade window: set by the ctor, cleared by the dtor. Read at
// , 0x0045cd37/0x0045cd57, 0x004e28ce/0x004e2914/0x004e497d (HUD_Target) and by the
// IClient trade slots 0x005a5fdc..0x005a61ad.
extern NnTrade* G_NnTrade;

// != 0.
bool NnTradeIsOpen();
