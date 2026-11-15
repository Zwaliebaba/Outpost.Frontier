// Author: Giperion.

#pragma once

// baseline NN_Scanner (RTTI .?AVNN_Scanner@@ : NN_Inventory, Event::Handler) -- the cargo scan
// window the target panel opens (HUD_Target the routine -> the routine): an NN_Inventory in
// commodity mode that lists the scanned ship's cargo instead of the player's, with the player
// cash, jettison button, ship-view toggle, cargo label and full cargo bar hidden.

// Layout (size 0x984, WINFACTORY Create the routine = operator_new(0x984)):
//   +0x000 NN_Inventory (0x974)
//   +0x974 Event::Handler face (vftable 0x005d5604)
//   +0x978 EquipDescList -- the scanned cargo (VC6 list: allocator byte, +0x97c head,
//          +0x980 size)

// vftable 0x005d561c (NN_Inventory's 47 slots) overrides:
//    0 the routine scalar deleting dtor (dtor the routine)
//    9 the routine Create      11 the branch Update      23 the branch Slot23
//   42 the branch SetProperty 46 the routine Slot46 (`MOV AL,1 / RET`)
// Event::Handler vftable 0x005d5604: slot 0 the routine OnEvent.

// WINFACTORY<NN_Scanner> "NN_Scanner": the field, static init the branch (ctor
// , atexit the branch -> the routine), vftable 0x005d56dc (Create the routine:
// the NN_Inventory ctor the routine inlined, the handler vptr, the empty list, then slot 9).

#include "event_handler.h"
#include "nn_inventory.h"

#include "common_goods.h"

class NnScanner : public NnInventory, public EventHandler
{
public:
    // Inlined in the routine (0x004b72af..0x004b7326): the routine, the handler vptr, an
    // empty +0x978 list (the inlined clear walks no node), the NN_Scanner vptrs.
    NnScanner();
    // the +0x978 list, the handler (the routine), then the routine.
    ~NnScanner() override;

    // slot 9 the routine (RET 8) -- NN_Inventory's Create, then (on success) bind the
    // controls, the title ids 0xbcb, hide the controls a scan does not use and switch to the
    // commodity filter; RegisterEvent(0xf) either way.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch -- close when the scan can no longer stand, otherwise the child walk
    // and the rows of the scanned cargo.
    void Update() override;
    // slot 23 the branch (RET 0xc) -- 0xef02: UI sound 0xe and the routine; else NN_Inventory.
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch (RET 0xc) -- 0x26 on the close button, 0x49 (A = the cargo list,
    // B = the scanned ship's archetype), 0x4a (show); then NN_Inventory's SetProperty.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;
    // slot 46 the routine -- true (read by NN_Inventory at 0x0047f6bf).
    bool Slot46() override;

    // Event::Handler slot 0 the routine (RET 4) -- command 0xf closes the window.
    bool OnEvent(unsigned MsgId) override;

    EquipDescList ScannedCargo;  // +0x978

private:
    NnScanner(NnScanner const&) = delete;
    NnScanner& operator=(NnScanner const&) = delete;
};

// (cdecl) -- open the NN_Scanner window on Cargo, the cargo of the ship whose
// archetype is ShipArchId. Caller: HUD_Target the routine (0x004e55b1).
void OpenNnScanner(EquipDescList const* Cargo, unsigned ShipArchId);
