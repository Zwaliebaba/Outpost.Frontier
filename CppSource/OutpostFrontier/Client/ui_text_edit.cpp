// Author: Giperion.

#include "ui_text_edit.h"

#include "ui_page_desc.h"
#include "win_factory.h"

UiTextEdit::UiTextEdit()
    : LocalWidth(0.0f)
    , LocalHeight(0.0f)
{
    // inline: RichTextEditWin ctor the routine, then the three UITextEdit
    // vftables. UITextEdit writes no field of its own; the zeros are only C++ initialisation.
}

UiTextEdit::~UiTextEdit()
{
    // -> the RichTextEditWin dtor.
}

bool UiTextEdit::Create(Win3D* NewParent, void* Data)
{
    // (RET 8): RichTextWin's Create (the routine) called directly.
    bool const bResult = RichTextWin::Create(NewParent, nullptr);
    // the routine, then the routine(u"TextEdit" 0x005e57ec, 0).
    ClearText();
    SetText(L"TextEdit", nullptr);

    if (Data != nullptr)
    {
        FUiPayloadText const* const Payload = static_cast<FUiPayloadText const*>(Data);
        LocalWidth = Payload->Right;                                        // +0x530
        NormWidth = Payload->Right;                                         // +0x330
        LocalHeight = Payload->Bottom;                                      // +0x534
        NormHeight = Payload->Bottom;                                       // +0x334
        // the routine, then the routine(payload +8, 0) -- baseline does
        // not test the caption pointer.
        ClearText();
        SetText(Payload->CaptionUtf16, nullptr);
    }

    SetEditable(false);                                                     // (0)
    return bResult;
}

// WINFACTORY<UITextEdit> static (the branch -> the routine; Create
// ); atexit dtor the routine.
WINFACTORY<UiTextEdit> G_UITextEditFactory("UITextEdit");
