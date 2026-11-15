// Author: Giperion.

#pragma once

// baseline BlockingDialog (RTTI .?AVBlockingDialog@@ : WinControl) -- the invisible input blocker
// WaitForServerResponse (the routine) raises around its multiplayer server pump
// (the routine before the loop, the routine after it). It draws nothing of its own; while it is
// up it holds the key focus, the mouse capture and the client timing pause.

// WINFACTORY<BlockingDialog> the field "BlockingDialog": static init the branch
// (the routine + atexit the branch -> the routine), Create the routine
// (operator_new(0x330)).

// vftable 0x005d08e4 (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   13 the routine Draw (the routine, then WinControl::Draw).
// No Event::Handler face; slot 42 stays WinControl's the routine.

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c software-cursor draw flag: 1 from the factory, then the routine's result
//   size 0x330

#include "wincontrol_base.h"

#include <stddef.h>

class BlockingDialog : public WinControl
{
public:
    // Inlined into the routine: WinControl ctor, byte +0x32c = 1, the routine, ++the field.
    BlockingDialog();

    ~BlockingDialog() override;

    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the routine
    void Draw() override;

    unsigned char bCursorDrawFlag; // +0x32c
    unsigned char Pad32d[3];       // +0x32d
};

static_assert(sizeof(BlockingDialog) == 0x330,
              "baseline BlockingDialog is 0x330 bytes (operator_new 004747d8)");
static_assert(offsetof(BlockingDialog, bCursorDrawFlag) == 0x32c, "BlockingDialog +0x32c");

// (cdecl) -- unless one is registered, build a BlockingDialog, send it SetProperty
// 0x42 with {0,0,0} (WinControl's switch ignores 0x42) and capture the mouse.
void BlockingDialogOpen();
// (cdecl) -- close the registered BlockingDialog, if any. While the letterbox is
// visible (the routine) the saved cursor flag is dropped to 0 first.
void BlockingDialogClose();
