// Author: Giperion.

#include "ui_rich_text_win.h"

#include "ui_page_desc.h"
#include "win_factory.h"

UiRichTextWin::UiRichTextWin()
{
    // inline: the routine (RichTextWin ctor), then vft 0x005e58d4 and the
    // +0x7c face 0x005e58c8. No UIRichTextWin fields of its own.
}

UiRichTextWin::~UiRichTextWin()
{
    // vftables back to UIRichTextWin, JMP the routine (RichTextWin dtor).
}

bool UiRichTextWin::Create(Win3D* NewParent, void* Data)
{
    // (RET 8): AL = the routine(Parent, 0) (RichTextWin slot 9, no payload);
    // if (AL && Payload) { +0x330 = Payload[0]; +0x334 = Payload[1]; } return AL.
    bool const bCreated = RichTextWin::Create(NewParent, nullptr);
    if (bCreated && Data != nullptr)
    {
        FUiPayloadBounds const* const Bounds = static_cast<FUiPayloadBounds const*>(Data);
        NormWidth = Bounds->Right;
        NormHeight = Bounds->Bottom;
    }
    return bCreated;
}

// WINFACTORY<UIRichTextWin> static (the branch -> the routine; Create
// ); atexit dtor the routine.
WINFACTORY<UiRichTextWin> G_UIRichTextWinFactory("UIRichTextWin");
