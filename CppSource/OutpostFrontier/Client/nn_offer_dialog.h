// Author: Giperion.

#pragma once

// baseline NN_OfferDialog (RTTI .?AVNN_OfferDialog@@ : WinControl, Event::Handler) -- the mission /
// bar offer box a Gf ScriptSequence raises: a text, a title by offer type, and Accept / Decline
// or a single Close button. The answer goes to the owner's callback.

// WINFACTORY<NN_OfferDialog> the field "NN_OfferDialog": static init the branch (ctor
// , atexit the branch -> the routine), Create the routine (operator_new(0x364),
// WinControl ctor the routine, the vptrs, the field = 1, +0x338 = 0, +0x10 = the factory
// name, slot 9; on failure the scalar deleting dtor).

// vftable 0x005d443c (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   42 the branch SetProperty.
// IDataViewOwner face vftable 0x005d442c; Event::Handler face +0x32c vftable 0x005d4424
// (the branch).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c Event::Handler vptr
//   +0x330 the answer callback, +0x334 its context (the routine)
//   +0x338 1: only the Accept button is shown and Escape does not answer
//   +0x33c OfferDialogAcceptButton (WireButtonControl), +0x340 OfferDialogAcceptText
//   +0x344 OfferDialogCloseButton (ButtonControl), +0x348 OfferDialogTitle
//   +0x34c OfferDialogOfferText (RichTextWin), +0x350 never written
//   +0x354 OfferDialogDeclineButton, +0x358 OfferDialogDeclineText
//   +0x35c OfferDialogClose2Button, +0x360 OfferDialogCloseText
//   size 0x364

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>

struct FmtStr;
class RichTextWin;

// The answer callback (cdecl): Answer 0 = Accept, 1 = Decline / Close / Escape.
typedef void(__cdecl* FNnOfferDialogCallback)(void* Context, int Answer);

class NnOfferDialog : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor, the field = 1, +0x338 = 0.
    NnOfferDialog();
    // the field = 0, then the Event::Handler (the routine) and WinControl
    // (the routine) base dtors.
    ~NnOfferDialog() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch (RET 4)
    bool OnEvent(unsigned MsgId) override;

    // (thiscall, RET 0x14) -- show Text under the title of offer Type (0..4) with
    // the buttons of that type (Accept / Decline / Close for 0, 1, 3; Close2 for 2, 4);
    // bSingleButton leaves only Accept, centred. Callers: the Gf ScriptSequence 0x00455256 /
    // .
    void Open(int Type, FmtStr const* Text, bool bSingleButton, FNnOfferDialogCallback Callback,
              void* Context);

    FNnOfferDialogCallback Callback; // +0x330
    void* Context;                   // +0x334
    unsigned char bSingleButton;     // +0x338
    unsigned char Pad339[3];
    WinControl* AcceptButton;        // +0x33c
    WinControl* AcceptText;          // +0x340
    WinControl* CloseButton;         // +0x344
    WinControl* Title;               // +0x348
    RichTextWin* OfferText;          // +0x34c
    unsigned Field_0x350;            // +0x350 TODO: never written by the class; meaning unknown
    WinControl* DeclineButton;       // +0x354
    WinControl* DeclineText;         // +0x358
    WinControl* Close2Button;        // +0x35c
    WinControl* CloseText;           // +0x360
};

static_assert(sizeof(void*) != 4 || sizeof(NnOfferDialog) == 0x364,
              "baseline NN_OfferDialog is 0x364 bytes (operator_new 004a69e8)");
static_assert(sizeof(void*) != 4 || offsetof(NnOfferDialog, Callback) == 0x330,
              "NN_OfferDialog +0x330 callback");
static_assert(sizeof(void*) != 4 || offsetof(NnOfferDialog, bSingleButton) == 0x338,
              "NN_OfferDialog +0x338 single-button byte");
static_assert(sizeof(void*) != 4 || offsetof(NnOfferDialog, AcceptButton) == 0x33c,
              "NN_OfferDialog +0x33c accept button");
static_assert(sizeof(void*) != 4 || offsetof(NnOfferDialog, CloseText) == 0x360,
              "NN_OfferDialog +0x360 close text");
