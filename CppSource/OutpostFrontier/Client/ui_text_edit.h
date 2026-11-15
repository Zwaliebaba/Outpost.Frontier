// Author: Giperion.

#pragma once

// UITextEdit -- PAGE_*.txt edit field (RTTI .?AUUITextEdit@@ : RichTextEditWin : RichTextWin :
// BaseRichTextWin : WinControl; Event::Handler face at +0x498).

// baseline:
//   WINFACTORY<UITextEdit> the field, static init the branch -> the routine("UITextEdit")
//   Create the routine: operator_new(0x538), RichTextEditWin ctor the routine, then vftables
// (main) / 0x005e5bc8 (+0x7c) / 0x005e5bc0 (+0x498 Event::Handler).
//   slot 0 the routine scalar deleting destructor (dtor the routine)
//   slot 9 the routine Create

// baseline offsets are kept in the comments only: RichTextEditWin holds std containers (current
// MSVC STL), so the port's layout past +0x4c4 no longer matches baseline's 0x538 bytes.

#include "rich_text_win.h"

class UiTextEdit : public RichTextEditWin
{
public:
    UiTextEdit();
    ~UiTextEdit() override;

    // slot 9 (+0x24) -- the routine (RET 8); Data is an FUiPayloadText*.
    bool Create(Win3D* NewParent, void* Data) override;

    float LocalWidth;      // +0x530 -- payload +0x00 (also +0x330)
    float LocalHeight;     // +0x534 -- payload +0x04 (also +0x334)
};
