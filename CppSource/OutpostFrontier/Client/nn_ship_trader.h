// Author: Giperion.

#pragma once

// The ship dealer of a base (baseline NN_ShipTrader, OutpostFrontier.exe 0x004b7440..0x004b9850).

// RTTI .?AVNN_ShipTrader@@ : WinControl, Event::Handler (COL 0x005ebd9c; the IDataViewOwner face
// at +0x7c has COL 0x005ebdb0). operator_new(0x448) (0x004b9738).
// WINFACTORY<NN_ShipTrader> the field "NN_ShipTrader": static init the branch (ctor
// , atexit the branch -> the routine); Create the routine (operator_new, the
// WinControl ctor the routine and the three vptrs inlined, the class-name pointer, slot 9; a
// failed Create is followed by the scalar deleting destructor).

// vftable 0x005d593c -- WinControl's with these overrides (raw dword dump):
//   0 the routine scalar deleting dtor (the routine), 9 the routine Create, 11 the routine
//   Update, 13 the branch Draw, 20 the branch KeyEvent, 42 the branch SetProperty.
// IDataViewOwner face +0x7c: vftable 0x005d5930 (WinControl's the branch / the routine).
// Event::Handler face +0x32c: vftable 0x005d5928, slot 0 the branch.

// The window lists at most three ships the base market sells (market category 3, ship
// packages), each with its icon, name, class and price, and the player's current ship; picking
// one shows it in the 3D view with its three info panes. The ctor writes no member: everything
// below is set by Create.

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>
#include <stdint.h>

struct GoodInfo;
class RichTextWin;
class ShipViewControl;

class NnShipTrader : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl's ctor and the vptrs; no member is written.
    NnShipTrader();
    // the Event::Handler (the routine) and WinControl (the routine)
    // destructors.
    ~NnShipTrader() override;

    // slot 9 the routine (RET 8) -- the market's ships and every control; Win3D's Create last.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine -- the children, then RefreshRows when the player's rank level moved.
    void Update() override;
    // slot 13 the branch -- the row frames' layers, the two frame columns, WinControl's Draw and
    // the drag rotation of the 3D ship.
    void Draw() override;
    // slot 20 the branch (RET 8) -- Escape closes the window; returns false.
    bool KeyEvent(unsigned Key, unsigned char* Modifiers) override;
    // slot 42 the branch (RET 0xc) -- clicks (0x26 / 0x27), hover (0x2c / 0x2d), close (0x4e).
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face the branch (RET 4) -- USER_CANCEL (0x10) closes the window.
    bool OnEvent(unsigned MsgId) override;

    // (RET 4) -- select a row (0..RowCount-1), -2 for the current ship.
    void SelectRow(int Row);
    // every row's rank / reputation status, the rows sorted by status, the row
    // controls refilled.
    void RefreshRows();

    WinControl* CancelButton;        // +0x330 "ShipTraderCancelButton"
    WinControl* NameFrames[3];       // +0x334 "ShipTraderShipNameFrame%d"
    WinControl* NameIconFrames[3];   // +0x340 "ShipTraderShipNameIconFrame%d"
    WinControl* ShipMeshes[3];       // +0x34c "ShipTraderShipMesh%d" (only with a ship good)
    WinControl* ShipNames[3];        // +0x358 "ShipTraderShipName%d"
    WinControl* ShipPrices[3];       // +0x364 "ShipTraderShipPrice%d"
    ShipViewControl* ShipView;       // +0x370 "ShipTrader3DShip"
    RichTextWin* InfoFacts;          // +0x374 "ShipInfoCard_Facts"
    RichTextWin* InfoStats;          // +0x378 "ShipInfoCard_Stats"
    RichTextWin* InfoValues;         // +0x37c "ShipInfoCard_Values"
    WinControl* BuyButton;           // +0x380 "ShipTraderBuyButton"
    WinControl* ViewName;            // +0x384 "ShipTraderViewName"
    WinControl* ViewClass;           // +0x388 "ShipTraderViewClass"
    WinControl* BuyText;             // +0x38c "ShipTraderBuytext"
    WinControl* PlayerCash;          // +0x390 "PlayerCash"
    WinControl* WelcomeText;         // +0x394 "WelcomeText"
    WinControl* WelcomeLine;         // +0x398 "WelcomeLine"
    WinControl* WelcomeHelp;         // +0x39c "WelcomeHelp"
    WinControl* ErrorHelp;           // +0x3a0 "ErrorHelp"
    int PlayerWorth;                 // +0x3a4 credits + loadout resale + ship resale
    int RowPrices[3];                // +0x3a8 hull price + addon prices
    GoodInfo const* RowPackages[3];  // +0x3b4 the package good (null when unknown)
    unsigned RowGoodIds[3];          // +0x3c0 the market entry's good id
    int RowCount;                    // +0x3cc market ships, at most 3
    int Selected;                    // +0x3d0 -1 none, -2 the current ship
    int Hovered;                     // +0x3d4 -1 none
    WinControl* CurrentShipButton;   // +0x3d8 "ShipTraderCurrentShip" (null without a ship)
    WinControl* CurrentShipFrame;    // +0x3dc "ShipTraderCurrentShipFrame"
    WinControl* CurrentShipMesh;     // +0x3e0 "ShipTraderCurrentShipMesh"
    unsigned char bDragStart;        // +0x3e4 the next drag starts a rotation
    unsigned char Pad3e5[3];
    int CurrentShipClass;            // +0x3e8 Archetype::Ship::ShipClass, 0..3
    int RowShipClasses[3];           // +0x3ec
    float BaseFeeling;               // +0x3f8 NnDealer::GetBaseFactionFeeling
    int RowStatus[3];                // +0x3fc 0 buyable, 1 rank too low (single player), 2 rep
    int RowRequiredLevels[3];        // +0x408 ftol(market MinLevel)
    int RowIndices[3];               // +0x414 the row's market order
    int RankLevel;                   // +0x420 the rank level RefreshRows used
    char const* RowMeshPaths[3];     // +0x424 the ship good's ItemIcon
    int RowNameIds[3];               // +0x430 Archetype::Ship::IdsName
    WinControl* RowClassTexts[3];    // +0x43c "ShipTraderShipClass%d"

private:
    // The close sequence of KeyEvent, SetProperty 0x26 / 0x27 / 0x4e and OnEvent: the NavBar
    // (the routine(0), the routine), the routine on every child, slot 10, scalar deleting dtor.
    void Close();
    // baseline indexes the row arrays with Selected == -2 in SelectRow (0x004b8f29, 0x004b8f5b,
    // ) and so reads the dwords 8 bytes before the array. The layout is baseline's
    // (static_asserts below), so this reads the same fields.
    int RowDword(size_t ArrayOffset, int Index) const;

    NnShipTrader(NnShipTrader const&) = delete;
    NnShipTrader& operator=(NnShipTrader const&) = delete;
};

static_assert(sizeof(NnShipTrader) == 0x448, "baseline NN_ShipTrader is 0x448 bytes (operator_new 0x004b9738)");
static_assert(offsetof(NnShipTrader, CancelButton) == 0x330, "NN_ShipTrader +0x330");
static_assert(offsetof(NnShipTrader, ShipView) == 0x370, "NN_ShipTrader +0x370");
static_assert(offsetof(NnShipTrader, PlayerWorth) == 0x3a4, "NN_ShipTrader +0x3a4");
static_assert(offsetof(NnShipTrader, RowCount) == 0x3cc, "NN_ShipTrader +0x3cc");
static_assert(offsetof(NnShipTrader, bDragStart) == 0x3e4, "NN_ShipTrader +0x3e4");
static_assert(offsetof(NnShipTrader, BaseFeeling) == 0x3f8, "NN_ShipTrader +0x3f8");
static_assert(offsetof(NnShipTrader, RankLevel) == 0x420, "NN_ShipTrader +0x420");
static_assert(offsetof(NnShipTrader, RowClassTexts) == 0x43c, "NN_ShipTrader +0x43c");
