// Author: Giperion.

#pragma once

// baseline FLImeInputBox (RTTI .?AUFLImeInputBox@@ : IMEInputBox : CInputBox), vftable 0x005e2250,
// operator new(0xa0). A RichTextEditWin builds one at +0x52c while an IMEFocus singleton exists
// (the routine / the routine / the routine) and routes keys and characters through it; the
// box writes the composed text back into its owner (slot 6) and draws the composition through
// two Font::Strings.

// Slots (0x005e2250): 0 the routine (scalar deleting, dtor the routine), 1 / 2 IMEInputBox's
// OnSetFocus / OnKillFocus, 3 the branch Resize, 4 / 5 CInputBox's HandleKey / CharFilter,
// 6 the branch UpdatedText, 7 IMEInputBox::ToggleInsertMode, 8 CInputBox::MoveCaret,
// 9 IMEInputBox::CalcCaretPos, 10 CInputBox::RecalcCaretPos, 11 the branch Draw (RET 4),
// 12 the routine DrawString, 13 IMEInputBox::ImeShowToolbar.

// Layout after IMEInputBox (0x94): +0x94 owner RichTextEditWin*, +0x98 Font::String (the text
// DrawString paints), +0x9c Font::String (the composed text, measured for the X offset).

#include "imeui.h"

class FontStringChild;
class RichTextEditWin;

struct FLImeInputBox : public IMEInputBox
{
public:
    // (RET 4)
    explicit FLImeInputBox(RichTextEditWin* InOwner);

    ~FLImeInputBox() override;

    // slot 3 the branch (RET 4) -- IMEInputBox::Resize with {0, 0, right, bottom}.
    void Resize(const RECT* pRect) override;
    // slot 6 the branch (RET 4) -- the owner's SetText(Text, 0), then +0x9c SetText(Text).
    void UpdatedText(const unsigned short* pText) override;
    // slot 11 the branch -- RET 4.
    void Draw(void* pContext) override;
    // slot 12 the routine (RET 0x10)
    void DrawString(int X, int Y, unsigned Color, const wchar_t* Text) override;

    RichTextEditWin* Owner;             // +0x94
    FontStringChild* TextString;        // +0x98
    FontStringChild* CompositionString; // +0x9c

private:
    FLImeInputBox(FLImeInputBox const&) = delete;
    FLImeInputBox& operator=(FLImeInputBox const&) = delete;
};
