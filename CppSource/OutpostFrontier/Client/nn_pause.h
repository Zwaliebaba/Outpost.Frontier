// Author: Giperion.

#pragma once

// baseline NN_Pause (RTTI .?AVNN_Pause@@ : WinControl, Event::Handler;
// Client\nn\NN_Pause.cpp) -- the in-game pause window raised by USER_PAUSE
// (0x14) through the routine.

// WINFACTORY<NN_Pause> the field "NN_Pause": static init the branch (ctor the routine,
// atexit the branch -> the routine), Create the routine (operator_new(0x374), WinControl
// ctor the routine, the three vptrs, +0x331 = 0, slot 9; on failure the scalar deleting dtor).

// vftable 0x005d462c (45 slots, COL 0x005eb86c) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the branch Update, 13 the routine Draw (the body NN_Error / NN_NetworkOperation
//   share), 23 the branch, 42 the branch SetProperty.
// IDataViewOwner face +0x7c vftable 0x005d461c; Event::Handler face +0x32c vftable 0x005d4614
// (the branch).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c Event::Handler vptr
//   +0x330 software-cursor draw flag saved when the window opened (the routine)
//   +0x331 close pending -- Update deletes the window (set when NN_Confirm answered yes)
//   +0x334 PauseTitle (TextControl)
//   +0x338 PauseButtonBox0..4 (WireButtonControl), +0x34c PauseButtonText0..4 (TextControl),
//   +0x360 the action of each button (1..7, see DoAction); unused rows are all zero
//   size 0x374

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>

class NnPause : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor, +0x331 = 0.
    NnPause();

    ~NnPause() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 23 the branch (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch (RET 4)
    bool OnEvent(unsigned MsgId) override;

    // (thiscall, RET 4) -- carry out a button's action.
    void DoAction(int Action);
    // (thiscall) -- close at once: resume, release the mouse, delete the window.
    // Caller: NN_Exit::Create 0x00471844.
    void CloseNow();

    unsigned char bCursorDrawFlag;   // +0x330
    unsigned char bClosePending;     // +0x331
    unsigned char Pad332[2];         // +0x332
    WinControl* Title;               // +0x334
    WinControl* ButtonBox[5];        // +0x338
    WinControl* ButtonText[5];       // +0x34c
    int ButtonAction[5];             // +0x360

private:
    // The tail every closing path shares (0x004a7222..0x004a7270 and the copies after it):
    // (+0x330) inline (the routine(0), the routine), slot 33 ReleaseMouse,
    // on every child, slot 10 Destroy, the scalar deleting dtor.
    void ResumeAndDelete();
};

static_assert(sizeof(void*) != 4 || sizeof(NnPause) == 0x374,
              "baseline NN_Pause is 0x374 bytes (operator_new 004a7738)");
static_assert(sizeof(void*) != 4 || offsetof(NnPause, bCursorDrawFlag) == 0x330,
              "NN_Pause +0x330 cursor flag");
static_assert(sizeof(void*) != 4 || offsetof(NnPause, Title) == 0x334, "NN_Pause +0x334 title");
static_assert(sizeof(void*) != 4 || offsetof(NnPause, ButtonBox) == 0x338,
              "NN_Pause +0x338 button boxes");
static_assert(sizeof(void*) != 4 || offsetof(NnPause, ButtonText) == 0x34c,
              "NN_Pause +0x34c button texts");
static_assert(sizeof(void*) != 4 || offsetof(NnPause, ButtonAction) == 0x360,
              "NN_Pause +0x360 button actions");

// (cdecl) -- open the pause window unless a NeuroNet screen is up (the field),
// the server tick is held (the routine), the room camera sequence runs (the field) or the
// software cursor is shown in space with no player object; one live NN_Pause at most.
// Caller: the key-command handler the branch case USER_PAUSE (0x005b2466).
void NnPauseOpen();
