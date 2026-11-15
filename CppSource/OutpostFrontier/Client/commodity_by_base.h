// Author: Giperion.

#pragma once

// The commodity-by-base price table of the NeuroNet inventory (the "NN_CommodityByBase" window,
// WINFACTORY the field, ctor the routine) and the inventory info card's trade report
// (the routine), which read the same three tables:

// std::vector of 0xc-byte rows (static init the branch, dtor the branch)
// std::map space-object id -> base name   (static init the branch via
// , dtor the branch)
// std::map space-object id -> system name (static init the branch via
// , dtor the branch)

// The rows are keyed by the base's space object (Universe::IBase::SpaceObjectId, baseline BASE
// vt+0x5c), the id the player's visit map is keyed by too. baseline's map value is a two-dword
// wide string {wchar_t* Ptr; unsigned Capacity} assigned with the routine (malloc / realloc
// + wcsncpy) and freed with free() (the routine); the port keeps std::wstring.

// NN_Inventory opens the window with the selected commodity's archetype (its "trade routes"
// button, 0x0047a9bb); a row click closes it and hands the row's space object to the nav map
// (the routine).

#include "common_text.h" // RenderDisplayList
#include "event_handler.h"
#include "wincontrol_base.h"

#include <map>
#include <stddef.h>
#include <string>
#include <vector>

class UiScroll;

// One row of the field (0xc bytes).
struct FCommodityByBaseRow
{
    unsigned SpaceObjectId; // +0x00 the base's space object (IBase vt+0x5c)
    int Price;              // +0x04 ftol of the market price, or of GoodInfo::Price
    bool bBaseSells;        // +0x08 ids 0x3e5 " - Base Sells this Item"
};

extern std::vector<FCommodityByBaseRow> G_CommodityByBaseRows;
extern std::map<unsigned, std::wstring> G_CommodityByBaseBaseNames;
extern std::map<unsigned, std::wstring> G_CommodityByBaseSystemNames;

// (cdecl) -- false for ids 0 and for the ten base-name ids 0x3fc27, 0x3fc2e,
// 0x3fc28, 0xeb41, 0xeb3f, 0x300ea, 0x30052, 0x3009c, 0x3009d, 0x300bf; true otherwise.
bool IsCommodityByBaseListedName(int Ids);

// (cdecl) -- empty the three tables, then one row for every base whose space
// object the player knows (visit flag 4), with the price of ArchId's good there.
void BuildCommodityByBaseRows(unsigned ArchId);

// (cdecl) -- bubble-sort the rows by Column: 1 price (high first) then base
// name, 2 base name, 3 system name then price then base name, 4 base-sells first then price;
// any other value leaves the rows as they are.
void SortCommodityByBaseRows(int Column);

// (cdecl) -- the info card's trade report for ArchId: rebuild the rows, sort
// them by price, and append one "\n<price> <base>, <system>[ sells][ best]" line per row.
void AppendCommodityTradeReport(RenderDisplayList* Card, unsigned ArchId);

// One visible line of the window: four TextControls ("ItemName" style), 0x10 bytes.
struct FCommodityByBaseRowControls
{
    WinControl* Price;   // +0x0 "CommodityByBasePrice%d"
    WinControl* BuySell; // +0x4 "CommodityByBuySellPrice%d"
    WinControl* Base;    // +0x8 "CommodityByBaseBase%d"
    WinControl* System;  // +0xc "CommodityByBaseSystem%d"
};

// baseline NN_CommodityByBase (RTTI .?AVNN_CommodityByBase@@ : WinControl, Event::Handler).

// WINFACTORY<NN_CommodityByBase> the field "NN_CommodityByBase": static init the branch
// (the routine + atexit the branch -> the routine), Create the routine
// (operator_new(0x424), WinControl ctor the routine, the vptrs, no field written, slot 9).

// vftable 0x005d027c (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine: the routine on +0x32c, then
// the two base destructors), 9 the routine Create, 11 the branch Update,
//   42 the branch SetProperty.
// IDataViewOwner face +0x7c vftable 0x005d026c; Event::Handler face +0x32c vftable 0x005d0264
// (the routine).
class NnCommodityByBase : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl's ctor and the vptrs; no member is written before
    // Create, so none is here.
    NnCommodityByBase();
    // only the base destructors.
    ~NnCommodityByBase() override;

    // slot 9 the routine (RET 8) -- build the table for the archetype in Data.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch -- close when the window may not stand, else update the children
    // and colour the column titles and the rows under the mouse.
    void Update() override;
    // slot 42 the branch (RET 0xc) -- the scroll bar (0x3e) and the clicks (0x26 / 0x3e).
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face the routine (RET 4) -- event 0xf closes the window.
    bool OnEvent(unsigned MsgId) override;

    // fill the 13 visible lines from FirstRow and set up the scroll bar.
    void RefreshRows();

    unsigned ArchId;                      // +0x330 Create's Data
    WinControl* CloseButton;              // +0x334 "CommodityByBaseCloseButton"
    WinControl* PriceTitle;               // +0x338 "CommodityByBasePriceTitle"   (column 1)
    WinControl* BaseTitle;                // +0x33c "CommodityByBaseBaseTitle"    (column 2)
    WinControl* SystemTitle;              // +0x340 "CommodityByBaseSystemTitle"  (column 3)
    WinControl* BuySellTitle;             // +0x344 "CommodityByBaseBuySellTitle" (column 4)
    UiScroll* Scroll;                     // +0x348 "CommodityByBaseScroll"
    int SortColumn;                       // +0x34c
    int FirstRow;                         // +0x350
    FCommodityByBaseRowControls Rows[13]; // +0x354

private:
    // The close sequence of the close button, a row click, Update and OnEvent: every child tree
    // (the routine), slot 10 Destroy, then the scalar deleting destructor.
    void Close();

    NnCommodityByBase(NnCommodityByBase const&) = delete;
    NnCommodityByBase& operator=(NnCommodityByBase const&) = delete;
};

static_assert(sizeof(NnCommodityByBase) == 0x424,
              "baseline NN_CommodityByBase is 0x424 bytes (operator_new 0x004707a8)");
static_assert(offsetof(NnCommodityByBase, ArchId) == 0x330, "NN_CommodityByBase +0x330");
static_assert(offsetof(NnCommodityByBase, Scroll) == 0x348, "NN_CommodityByBase +0x348");
static_assert(offsetof(NnCommodityByBase, Rows) == 0x354, "NN_CommodityByBase +0x354");
