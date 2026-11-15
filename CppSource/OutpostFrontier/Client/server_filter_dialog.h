// Author: Giperion.

#pragma once

// baseline ServerFilterDialog (RTTI .?AVServerFilterDialog@@ : WinControl, COL 0x005efe30) -- the
// modal box PageServerSelect's "SetFilter" button opens over the server list (the routine from
// PageServerSelect slot 23, 0x00571517..0x00571606). It edits the page's ServerFilter in place
// and keeps a copy to put back on cancel.

// WINFACTORY<ServerFilterDialog> the field "ServerFilterDialog": static init the branch
// (the routine, atexit the branch -> the routine), Create the routine (operator_new(0x39c),
// WinControl ctor the routine, the five rows through `eh vector constructor iterator`
// (the routine, ctor the branch zeroing the three pointers, dtor the branch zeroing them
// again), ServerFilter's ctor the routine inlined at +0x378, the vptrs, +0x374 / +0x32c /
// +0x330 / +0x334 = 0, +0x10 = the factory's name, slot 9 with the payload; on failure the
// scalar deleting dtor).

// vftable 0x005e20ec (45 slots) -- WinControl's with these overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   19 the routine MouseEvent, 20 the routine KeyEvent, 23 the routine Slot23,
//   42 the routine SetProperty.
// IDataViewOwner face +0x7c vftable 0x005e20e0 (WinControl's the branch / the routine).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c "FilterOk" (RichTextWireButtonControl), +0x330 "FilterCancel" (ButtonControl),
//   +0x334 "FilterTitle" (RichTextWin)
//   +0x338 five FServerFilterDialogRow (0xc each): "FilterCheck%d" / "FilterLabel%d" /
//          "FilterData%d" (only row 0 has the edit box)
//   +0x374 ServerFilter* -- the caller's filter, edited live (ReadControls)
//   +0x378 ServerFilter  -- the copy Create takes, assigned back on every cancel path
//   size 0x39c

#include "server_filter.h"
#include "wincontrol_base.h"

#include <stddef.h>

class RichTextEditWin;
class RichTextWin;

// one 0x20-byte description per row, five rows up to the field. Row 0 is
// .data as linked except for the dwords the branch writes; rows 1..4 are the branch's
// stores. The toggle is placed at {X, Y, Z} (read in the order +0x08, +0x00, +0x04), the label
// at {LabelX, Y + 0.015, Z}, the edit box at {DataX, Y + 0.015, Z}, each plus the dialog's
// +0xa8 offset.
struct FServerFilterRowDesc
{
    float Y;                  // +0x00
    float Z;                  // +0x04
    float X;                  // +0x08
    float LabelX;             // +0x0c
    float DataX;              // +0x10
    unsigned char bHasData;   // +0x14 -- build "FilterData%d"; the label is 0.28 wide, else 0.38
    unsigned LabelIds;        // +0x18
    int ToolTipIds;           // +0x1c -- the toggle's two tooltip ids and the label's IdsAlt
};

// +0x338 element (ctor the branch / dtor the branch zero all three).
struct FServerFilterDialogRow
{
    // (thiscall on the row, RET 8) -- the three controls from Desc under Dialog
    // (placed relative to its +0xa8 offset), then the row counter the field raised.
    void Build(WinControl* Dialog, FServerFilterRowDesc const& Desc);

    WinControl* Check = nullptr;      // +0x0 -- ToggleButtonControl
    RichTextWin* Label = nullptr;     // +0x4
    RichTextEditWin* Data = nullptr;  // +0x8 -- null unless the row description has one
};

class ServerFilterDialog : public WinControl
{
public:
    // Inlined into the routine: WinControl ctor, the rows zeroed, the ServerFilter ctor at
    // +0x378, the pointers zeroed.
    ServerFilterDialog();
    // (scalar deleting the routine) -- the rows zeroed, then WinControl's dtor.
    ~ServerFilterDialog() override;

    // slot 9 the routine (RET 8); Data is the ServerFilter* to edit.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 19 the routine (RET 4)
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 20 the routine (RET 8)
    bool KeyEvent(unsigned Key, unsigned char* Modifiers) override;
    // slot 23 the routine (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // slot 42 the routine (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    WinControl* OkButton;                 // +0x32c
    WinControl* CancelButton;             // +0x330
    RichTextWin* Title;                   // +0x334
    FServerFilterDialogRow Rows[5];       // +0x338
    ServerFilter* Target;                 // +0x374
    ServerFilter Backup;                  // +0x378

private:
    // the five toggles and the ping box into *Target.
    void ReadControls();
    // *Target into the five toggles and the ping box.
    void WriteControls();
    // The close every exit path spells inline (0x00563555..0x00563587, 0x0056367f..0x005636b1,
    // , 0x00563747..0x00563777): slot 33 ReleaseMouse, the routine on
    // every child (the next sibling read first), slot 10 Destroy, the scalar deleting dtor.
    void Close();

    ServerFilterDialog(ServerFilterDialog const&) = delete;
    ServerFilterDialog& operator=(ServerFilterDialog const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(ServerFilterDialog) == 0x39c,
              "baseline ServerFilterDialog is 0x39c bytes (operator_new 0x00563919)");
static_assert(sizeof(void*) != 4 || offsetof(ServerFilterDialog, Rows) == 0x338,
              "ServerFilterDialog +0x338");
static_assert(sizeof(void*) != 4 || offsetof(ServerFilterDialog, Target) == 0x374,
              "ServerFilterDialog +0x374");
static_assert(sizeof(void*) != 4 || offsetof(ServerFilterDialog, Backup) == 0x378,
              "ServerFilterDialog +0x378");

// (cdecl) -- build a ServerFilterDialog under Parent for Filter, centre it (slot 42
// (0x42, &{0, 0, 0}, 0)), capture the mouse (slot 32) and pump frames while one is registered.
// False only when the message pump sees WM_QUIT (the routine); true otherwise, including when
// the factory failed. Caller: PageServerSelect slot 23 (0x0057157f).
bool ServerFilterDialogRun(Win3D* Parent, ServerFilter* Filter);
