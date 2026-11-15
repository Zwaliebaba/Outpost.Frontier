// Author: Giperion.

#pragma once

// baseline NN_NetworkOperation (RTTI .?AVNN_NetworkOperation@@ : WinControl, Event::Handler) --
// the "waiting for the server" notice WaitForServerResponse raises while a multiplayer connect
// is pending (the routine / the routine / the routine).

// WINFACTORY<NN_NetworkOperation> the field "NN_NetworkOperation": static init the branch
// (the routine + atexit the branch -> the routine), Create the routine
// (operator_new(0x340)).

// vftable 0x005d0554 (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   13 the routine Draw (the routine, then WinControl::Draw -- shared by the NN dialogs),
//   20 the branch KeyEvent, 42 the branch SetProperty.
// Event::Handler face +0x32c vftable 0x005d0540 (the branch).

// Layout:
//   +0x000 WinControl (0x32c), +0x32c Event::Handler vptr
//   +0x330 result callback, cdecl (Arg, Result) -- only ever called with 0
//   +0x334 callback argument
//   +0x338 software-cursor draw flag saved by the routine
//   +0x33c ExitText (TextControl)
//   size 0x340

#include "event_handler.h"
#include "nn_exit.h" // FNnDialogResultFn
#include "wincontrol_base.h"

#include <stddef.h>

class NnNetworkOperation : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor, the routine, ++the field, +0x330 and
    // +0x334 = 0.
    NnNetworkOperation();

    ~NnNetworkOperation() override;

    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the routine
    void Draw() override;
    // slot 20 the branch
    bool KeyEvent(unsigned Key, unsigned char* Modifiers) override;
    // slot 42 the branch
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch
    bool OnEvent(unsigned MsgId) override;

    FNnDialogResultFn ResultFn;    // +0x330
    void* ResultArg;               // +0x334
    unsigned char bCursorDrawFlag; // +0x338
    unsigned char Pad339[3];       // +0x339
    WinControl* ExitText;          // +0x33c

    // (+0x338), the callback with 0, slot 33 ReleaseMouse, the routine on every
    // child, slot 10 Destroy, delete -- inlined identically at 004723e3, 00472463 and in
    // (00473b74..00473bd0).
    void Close();
};

static_assert(sizeof(NnNetworkOperation) == 0x340,
              "baseline NN_NetworkOperation is 0x340 bytes (operator_new 00474688)");
static_assert(offsetof(NnNetworkOperation, ResultFn) == 0x330, "NN_NetworkOperation +0x330");
static_assert(offsetof(NnNetworkOperation, bCursorDrawFlag) == 0x338, "NN_NetworkOperation +0x338");
static_assert(offsetof(NnNetworkOperation, ExitText) == 0x33c, "NN_NetworkOperation +0x33c");

// (cdecl) -- unless one is registered, build an NN_NetworkOperation, centre it
// (SetProperty 0x42 with {0,0,0}), set its text to Ids (SetProperty 0x1e) and capture the mouse.
void NnNetworkOperationOpen(unsigned Ids);
// (cdecl) -- close the registered NN_NetworkOperation, if any: the same sequence as
// its own cancel path, callback included.
void NnNetworkOperationClose();
// (cdecl) -- the routine("NN_NetworkOperation") != 0.
bool NnNetworkOperationIsOpen();
