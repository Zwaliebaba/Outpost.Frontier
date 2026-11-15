// Author: Giperion.

#pragma once

// baseline UIRichTextWin (RTTI .?AUUIRichTextWin@@ : RichTextWin : BaseRichTextWin : WinControl :
// Win3D). vftable 0x005e58d4 (48 slots) + IDataViewOwner face 0x005e58c8 at +0x7c.
// Overrides against RichTextWin (0x005e476c): slot 0 (the routine scalar deleting dtor ->
// -> RichTextWin dtor the routine) and slot 9 (the routine).
// Factory: WINFACTORY<UIRichTextWin> the field (static init the branch, ctor
// , Create the routine: operator_new(0x498) + RichTextWin ctor the routine).
// No fields of its own.

#include "rich_text_win.h"

class UiRichTextWin : public RichTextWin
{
public:
    UiRichTextWin();
    // vftables back, then the RichTextWin dtor.
    ~UiRichTextWin() override;

    // slot 9 (+0x24) -- the routine (RET 8); Data is an FUiPayloadBounds*.
    bool Create(Win3D* NewParent, void* Data) override;
};

static_assert(sizeof(UiRichTextWin) == 0x498, "module state allocates 0x498");
