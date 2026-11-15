// Author: Giperion.

#pragma once

// baseline NN_Error (RTTI .?AVNN_Error@@ : WinControl, Event::Handler) -- the modal error box
// ("OutpostFrontier Encountered an Error" + explanation + OK). Raised by the routine (non-blocking),
// (blocking on an ids string: the connect / login failures) and the routine
// (blocking on a narrow message: the fatal-error path of WinMain).

// WINFACTORY<NN_Error> the field "NN_Error": static init the branch (the routine +
// atexit the branch -> the routine), Create the routine (operator_new(0x344)).

// vftable 0x005d0634 (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   13 the routine Draw, 20 the branch KeyEvent, 42 the branch SetProperty.
// Event::Handler face +0x32c vftable 0x005d0620 (the branch).

// Layout:
//   +0x000 WinControl (0x32c), +0x32c Event::Handler vptr
//   +0x330 software-cursor draw flag saved by the routine
//   +0x334 "Text" (TextControl, the explanation), +0x338 "ErrorOkayButton" (WireButtonControl),
//   +0x33c "ErrorOkayText" (TextControl), +0x340 "ErrorDialogTitle" (TextControl)
//   size 0x344

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>

class NnError : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor, the routine, ++the field. No field of its
    // own is written before Create.
    NnError();

    ~NnError() override;

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

    unsigned char bCursorDrawFlag; // +0x330
    unsigned char Pad331[3];       // +0x331
    WinControl* Text;              // +0x334
    WinControl* OkayButton;        // +0x338
    WinControl* OkayText;          // +0x33c
    WinControl* Title;             // +0x340

private:
    // The dismiss sequence, inlined at 00472803, 004728d4 and 0047295a: the field = 0, the
    // parent's slot 23 with (0x3006, 0, 0), the routine(+0x330), slot 33 ReleaseMouse,
    // on every child, slot 10 Destroy, delete.
    void Dismiss();
};

static_assert(sizeof(NnError) == 0x344, "baseline NN_Error is 0x344 bytes (operator_new 00474908)");
static_assert(offsetof(NnError, bCursorDrawFlag) == 0x330, "NN_Error +0x330");
static_assert(offsetof(NnError, Text) == 0x334, "NN_Error +0x334 Text");
static_assert(offsetof(NnError, OkayButton) == 0x338, "NN_Error +0x338 okay button");
static_assert(offsetof(NnError, Title) == 0x340, "NN_Error +0x340 title");

// (cdecl) -- unless one is registered, build an NN_Error under Parent, centre it,
// hide the title (flags &= 0xfc), set the explanation to Ids and capture the mouse. Returns at
// once.
void NnErrorOpen(unsigned Ids, Win3D* Parent);
// (cdecl) -- the routine("NN_Error").
Win3D* NnErrorFind();
// (cdecl) -- NnErrorOpen's body, then pump frames (Sleep 10, the routine, server
// step, the routine, draw) until the dialog is gone or the message queue sees WM_QUIT.
// bUpdateServer picks IServer::Update over the client/server run-permission handshake.
void NnErrorRunModal(unsigned Ids, bool bUpdateServer, Win3D* Parent);
// (cdecl) -- true at once when an NN_Error is already up; otherwise raise one with
// the title shown and Message (narrow, converted with mbstowcs) as the explanation, pump frames
// until it goes away, and return the field == 0.
bool NnErrorRunModalMessage(char const* Message);
