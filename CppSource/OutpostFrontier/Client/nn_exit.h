// Author: Giperion.

#pragma once

// baseline NN_Exit (RTTI .?AVNN_Exit@@ : WinControl, Event::Handler) -- the "quit OutpostFrontier?"
// confirmation raised by SC_CLOSE / Alt+F4 (the routine).

// WINFACTORY<NN_Exit> the field "NN_Exit": static init the branch (the routine + atexit
// -> the routine), Create the routine (operator_new(0x350)).

// vftable 0x005d035c (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the routine Update, 13 the routine Draw, 42 the branch SetProperty.
// IDataViewOwner face +0x7c vftable 0x005d0350; Event::Handler face +0x32c vftable 0x005d0348
// (the branch).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c Event::Handler vptr
//   +0x330 result callback, cdecl (Arg, Result): 0 = quit confirmed, 1 = cancelled
//   +0x334 callback argument
//   +0x338 bImmediate -- Update closes at once (the routine's argument)
//   +0x339 software-cursor draw flag saved by the routine
//   +0x33c ExitText (TextControl), +0x340 ExitButtonBox0/1 (WireButtonControl),
//   +0x348 ExitButtonText0/1 (TextControl)
//   size 0x350

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>

typedef void (*FNnDialogResultFn)(void* Arg, int Result);

class NnExit : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor, then +0x330 / +0x334 / +0x338 = 0.
    NnExit();

    ~NnExit() override;

    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 42 the branch
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch
    bool OnEvent(unsigned MsgId) override;

    FNnDialogResultFn ResultFn;   // +0x330
    void* ResultArg;              // +0x334
    unsigned char bImmediate;     // +0x338
    unsigned char bCursorDrawFlag; // +0x339
    unsigned char Pad33a[2];      // +0x33a
    WinControl* ExitText;         // +0x33c
    WinControl* ButtonBox[2];     // +0x340
    WinControl* ButtonText[2];    // +0x348

private:
    // The close sequence every exit path shares: the routine(+0x339), the callback with
    // Result, slot 33 ReleaseMouse, the routine on every child, slot 10 Destroy, delete.
    void Close(int Result);
};

static_assert(sizeof(NnExit) == 0x350, "baseline NN_Exit is 0x350 bytes (operator_new 00472039)");
static_assert(offsetof(NnExit, ResultFn) == 0x330, "NN_Exit +0x330 callback");
static_assert(offsetof(NnExit, bImmediate) == 0x338, "NN_Exit +0x338 immediate");
static_assert(offsetof(NnExit, ExitText) == 0x33c, "NN_Exit +0x33c ExitText");
static_assert(offsetof(NnExit, ButtonBox) == 0x340, "NN_Exit +0x340 button boxes");
static_assert(offsetof(NnExit, ButtonText) == 0x348, "NN_Exit +0x348 button texts");

// (cdecl) -- the player's ship is gone: a mission message of kind 5 (the story star,
// ) whose FmtStr is the bare ids 0x723. Caller: the space state's frame 0x0054b1e2,
// once per loss (space_state.cpp, latched in its +0x09).
void NnExitPostShipLostMessage();

// (cdecl) -- unless an NN_Exit is registered, build one through the factory, centre
// it (SetProperty 0x42 with {0,0,0}), store bImmediate at +0x338 and capture the mouse.
void NnExitOpen(bool bImmediate);
