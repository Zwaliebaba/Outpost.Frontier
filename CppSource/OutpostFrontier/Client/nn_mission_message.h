// Author: Giperion.

#pragma once

// NeuroNetMissionMessage -- Client\nn\NN_Messaging.cpp (OutpostFrontier.exe
// ; the same file's voice queue is nn_messaging.h).

// RTTI .?AVNeuroNetMissionMessage@@ : WinControl, 0x36c bytes.
// WINFACTORY the field (static init the branch -> the routine, atexit the branch),
// Create the routine: operator_new(0x36c); vftable 0x005d24f4 (IDataViewOwner face 0x005d24e4
// at +0x7c); instance the field (G_NnMissionMessage, nn_modal.h), which the ctor sets and
// the dtor clears. NnMissionMessageDestroy (the routine, nn_modal.cpp) tears it down.

// One window, two uses, picked by the field (copied to +0x360 by Create):
//   kind 4 -- the scan notice (the routine, IClient slot 109): a ship is scanning the player's
//             cargo; "Drop Cargo" jettisons the scanned good, "Cancel" closes. It closes by
//             itself after 20 s, and at the next frame once the scanner is gone or hostile.
//   kind 6 -- the mission pop-up (the routine, IClient slot 95): a title and a text from the
//             server, with Yes / No / Later / Ok buttons picked by the field's bits 0..3.
//             The answer goes back through IServer::PopUpDialog as 1 / 2 / 4 / 8.

// vftable 0x005d24f4 -- WinControl's 0x005e5e44 with five overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the routine Update, 13 the routine Draw, 42 the branch SetProperty.

#include "wincontrol_base.h"

#include "common_text.h"

#include <stddef.h>
#include <stdint.h>

class RichTextWin;

class NeuroNetMissionMessage : public WinControl
{
public:
    // Inlined into the routine (0x0048b1f9..0x0048b21d): WinControl ctor, the vptrs,
    // = this, +0x364 = 0, +0x368 = 0.
    NeuroNetMissionMessage();

    ~NeuroNetMissionMessage() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 42 the branch (RET 0xc) -- 0x26, a button was clicked
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    unsigned char SavedCursorDrawFlag; // +0x32c -- the routine's result (kind 6, in space)
    unsigned char Pad32d[3];
    WinControl* CloseButton;        // +0x330 "MissionCloseButton"
    WinControl* Title;              // +0x334 "MissionMessageTitle"
    RichTextWin* Text;              // +0x338 "MissionMessageText"
    int Field_0x33c;                // +0x33c -- TODO: never written or read by this class
    WinControl* DropCargoButton;    // +0x340 "DropCargoButton" (kind 4)
    WinControl* DropCargoText;      // +0x344 "DropCargoText"
    WinControl* CancelButton;       // +0x348 "DCCancelButton"
    WinControl* CancelText;         // +0x34c "DCCancelText"
    WinControl* YesButton;          // +0x350 "MMYesButton" (kind 6, bit 0)
    WinControl* NoButton;           // +0x354 "MMNoButton" (bit 1)
    WinControl* LaterButton;        // +0x358 "MMLaterButton" (bit 2)
    WinControl* OkButton;           // +0x35c "MMOkButton" (bit 3)
    int Kind;                       // +0x360 -- the field when Create ran
    float Elapsed;                  // +0x364 -- kind 4: seconds shown (ctor 0)
    unsigned char bPausedGame;      // +0x368 -- Create paused the game (the routine); ctor 0
    unsigned char Pad369[3];

private:
    NeuroNetMissionMessage(NeuroNetMissionMessage const&) = delete;
    NeuroNetMissionMessage& operator=(NeuroNetMissionMessage const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(NeuroNetMissionMessage) == 0x36c,
              "baseline NeuroNetMissionMessage is 0x36c bytes (0x0048b1d8)");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetMissionMessage, CloseButton) == 0x330,
              "NeuroNetMissionMessage +0x330");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetMissionMessage, DropCargoButton) == 0x340,
              "NeuroNetMissionMessage +0x340");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetMissionMessage, OkButton) == 0x35c,
              "NeuroNetMissionMessage +0x35c");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetMissionMessage, Kind) == 0x360,
              "NeuroNetMissionMessage +0x360");
static_assert(sizeof(void*) != 4 || offsetof(NeuroNetMissionMessage, bPausedGame) == 0x368,
              "NeuroNetMissionMessage +0x368");

// (cdecl) -- the scan notice: ScannerId (the field) is scanning the player's
// GoodId (the field). Builds mad-lib 0x51e {good, ids 0x3ca, the player's count of it,
// ids 0x3cb} into the field, closes any open message and opens kind 4. Caller: IClient
// slot 109 (0x0053db53), only with a player ship.
void NnMissionMessageShowScanNotice(unsigned ScannerId, unsigned GoodId);

// (cdecl) -- the mission pop-up: Title into the field, Buttons into
// , NN_MainButton's open panel dropped (the routine), any open message closed,
// Text into the field, kind 6 opened. Caller: IClient slot 95 (0x0053c3ff).
void NnMissionMessagePopUp(FmtStr const* Title, FmtStr const* Text, unsigned Buttons);
