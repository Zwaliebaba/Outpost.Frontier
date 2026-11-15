// Author: Giperion.

#pragma once

// UITextMsgButton -- PAGE_*.txt text button (RTTI .?AUUITextMsgButton@@ : RichTextWin :
// BaseRichTextWin : WinControl; IDataViewOwner face at +0x7c).

// baseline:
//   WINFACTORY<UITextMsgButton> the field, static init the branch -> the routine
//   Create the routine: operator_new(0x4c8), RichTextWin ctor the routine, two empty
//   RenderDisplayLists at +0x4a8 / +0x4b4, vftables 0x005e5af4 / 0x005e5ae4 (+0x7c).
//   vftable 0x005e5af4 overrides of RichTextWin 0x005e476c:
//     slot 0  the routine  scalar deleting destructor (dtor the routine)
//     slot 5  the branch  SetPosition -- style-mesh offset (+0xa8..+0xb0), then 0057b0a0
//     slot 7  the branch  SetOrientation -- the routine, then the style mesh kept upright
//     slot 9  the routine  Create
//     slot 13 the branch  Draw -- hover/idle/inactive caption colour, then the routine
//     slot 15 the branch  MouseEnter -- if (+0x498) the routine ("ui_rollover" sound)
//     slot 19 the branch  MouseEvent -- click -> parent slot 23 (0x3004, +0x4a4, 0)

// Layout (after RichTextWin's 0x498):
//   +0x498 interactive (Create writes 1), +0x49c / +0x4a0 payload width / height,
//   +0x4a4 message argument, +0x4a8 caption RDL, +0x4b4 text RDL, +0x4c0 caption TRA
//   attributes, +0x4c4 never written; size 0x4c8.

#include "rich_text_win.h"

#include <stddef.h>

class UiTextMsgButton : public RichTextWin
{
public:
    UiTextMsgButton();

    ~UiTextMsgButton() override;

    // slot 5 the branch (RET 4)
    void SetPosition(float const* NewPosition) override;
    // slot 7 the branch (RET 4)
    void SetOrientation(float const* NewOrientation) override;
    // slot 9 the routine (RET 8); Data is an FUiPayloadTextMsgButton*.
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 13 the branch
    void Draw() override;
    // slot 15 the branch (RET 4) -- an interactive button plays the rollover sound.
    void MouseEnter(float const* Ray) override;
    // slot 19 the branch (RET 4)
    bool MouseEvent(FWin3DMouseEvent const* Event) override;

    // rebuild the content: Justify(1), RDLRef(+0x4a8), Style(0x828b),
    // TRA(+0x4c0, 0xffffff00, 0), RDLRef(+0x4b4).
    void ApplyCaptionLayout();

    unsigned char bInteractive;            // +0x498
    unsigned char Pad499[3];               // +0x499
    float LocalWidth;                      // +0x49c
    float LocalHeight;                     // +0x4a0
    int MessageArg;                        // +0x4a4
    RenderDisplayList CaptionBodyList;     // +0x4a8
    RenderDisplayList TextBodyList;        // +0x4b4
    TextRenderAttributes CaptionAttrs;     // +0x4c0
    unsigned Field4c4;                     // +0x4c4 -- never written

private:
    UiTextMsgButton(UiTextMsgButton const&) = delete;
    UiTextMsgButton& operator=(UiTextMsgButton const&) = delete;
};

static_assert(sizeof(UiTextMsgButton) == 0x4c8, "module state allocates 0x4c8");
static_assert(offsetof(UiTextMsgButton, bInteractive) == 0x498, "UITextMsgButton +0x498");
static_assert(offsetof(UiTextMsgButton, MessageArg) == 0x4a4, "UITextMsgButton +0x4a4");
static_assert(offsetof(UiTextMsgButton, CaptionBodyList) == 0x4a8, "UITextMsgButton +0x4a8");
static_assert(offsetof(UiTextMsgButton, TextBodyList) == 0x4b4, "UITextMsgButton +0x4b4");
static_assert(offsetof(UiTextMsgButton, CaptionAttrs) == 0x4c0, "UITextMsgButton +0x4c0");
