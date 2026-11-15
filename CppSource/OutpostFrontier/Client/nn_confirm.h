// Author: Giperion.

#pragma once

// baseline NN_Confirm (RTTI .?AVNN_Confirm@@ : WinControl, Event::Handler; the class lives in
// Client\nn\NN_LoadSave.cpp -- its FDUMP at 0x00488918 names that file) --
// the yes/no box. Its answer goes to the parent's slot 23 as (0x3006, 1 = yes / 0 = no, 0).

// WINFACTORY<NN_Confirm> the field "NN_Confirm": static init the branch (the routine,
// atexit the branch -> the routine), Create the routine (operator_new(0x348), WinControl
// ctor the routine, the three vptrs, slot 9 with the payload; on failure the scalar deleting
// dtor).

// vftable 0x005d22e4 (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the branch (JMP the routine -- the body WinControl's own slot 11 already is, so the
//   port does not override it), 13 the routine Draw, 23 the branch, 42 the branch
//   SetProperty.
// IDataViewOwner face +0x7c vftable 0x005d22d8; Event::Handler face +0x32c vftable 0x005d22d0
// (the branch).

// Layout:
//   +0x000 WinControl (0x32c), +0x32c Event::Handler vptr
//   +0x330 "Confirm_deleteBox" (StaticControl), +0x334 "Confirm_delTxt" (TextControl),
//   +0x338 "Confirm_delBtnYes" / +0x33c "Confirm_delBtnNo" (WireButtonControl),
//   +0x340 / +0x344 their captions (TextControl)
//   size 0x348

#include "event_handler.h"
#include "wincontrol_base.h"

#include <stddef.h>

// The slot 9 payload the routine / the routine build on their stacks: the question and the
// text style (never null by the time Create reads it).
struct FNnConfirmPayload
{
    wchar_t const* Text; // +0x00, handed to the text as property 0x1d
    char const* Style;   // +0x04, the text control's style
};

class NnConfirm : public WinControl, public EventHandler
{
public:
    // Inlined into the routine: WinControl ctor and the vptrs, nothing else.
    NnConfirm();

    ~NnConfirm() override;

    // slot 9 the routine (RET 8); Data is an FNnConfirmPayload.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the routine
    void Draw() override;
    // slot 23 the branch (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // Event::Handler face, the branch (RET 4)
    bool OnEvent(unsigned MsgId) override;

    WinControl* Frame;   // +0x330
    WinControl* Text;    // +0x334
    WinControl* YesBox;  // +0x338
    WinControl* NoBox;   // +0x33c
    WinControl* YesText; // +0x340
    WinControl* NoText;  // +0x344

private:
    // The answer sequence inlined at 0x00488719 / 0x00488759 / 0x00488872 / 0x004888c8: the
    // parent's slot 23 with (0x3006, bYes, 0) when there is a parent, slot 33 ReleaseMouse,
    // on every child, slot 10 Destroy, the scalar deleting dtor.
    void Answer(unsigned bYes);
};

static_assert(sizeof(void*) != 4 || sizeof(NnConfirm) == 0x348,
              "baseline NN_Confirm is 0x348 bytes (operator_new 00488e3f)");
static_assert(sizeof(void*) != 4 || offsetof(NnConfirm, Frame) == 0x330, "NN_Confirm +0x330");
static_assert(sizeof(void*) != 4 || offsetof(NnConfirm, YesBox) == 0x338, "NN_Confirm +0x338");
static_assert(sizeof(void*) != 4 || offsetof(NnConfirm, NoText) == 0x344, "NN_Confirm +0x344");

// (cdecl) -- unless an NN_Confirm is up (the routine), build one under Parent with
// Text and Style (null Style -> "ConfirmText"), centre it (slot 42 (0x42, &{0,0,0}, 0)) and
// capture the mouse; returns it, or null when none was made. Callers: NN_LoadSave 0x00486eac /
// , NN_Pause::DoAction 0x004a7492 / 0x004a754b; NN_LoadSave::SaveGame inlines the
// same body at 0x0048766a..0x004876f1 (style "ConfirmText"), ported as a call.
Win3D* NnConfirmOpen(Win3D* Parent, wchar_t const* Text, char const* Style);

// (cdecl) -- the same box with ids 0x75c formatted (swprintf) with Name, and Style
// defaulting to "ConfirmPlayerText". Returns nothing. Callers: 0x00569b02, 0x0056c710.
void NnConfirmOpenFormatted(Win3D* Parent, wchar_t const* Name, char const* Style);
