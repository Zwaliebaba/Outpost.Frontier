// Author: Giperion.

#pragma once

// UIStaticText -- PAGE_*.txt caption widget (RTTI .?AUUIStaticText@@ : StaticText : TextWin :
// Win3D). StaticText has no vftable of its own (nothing but UIStaticText derives from it), so
// the port hangs UiStaticText directly on TextWin. UIStaticText's vftable 0x005e5a64 overrides
// TextWin 0x005d54cc in:
//   slot 0  the routine  scalar deleting destructor
//   slot 8  the branch  SetTransform
//   slot 9  the routine  Create
//   slot 13 0x00599ce0    draw: debug frame (dead -- the field has no writer), then
//                         JMP TextWin draw the routine (the port inherits TextWin::Draw)
// Size 0xf4 (operator_new in WINFACTORY<UIStaticText>::Create the routine, which inlines the
// Win3D + TextWin constructors). UIStaticText's own fields: +0xec / +0xf0.

#include "text_win.h"

#include <stddef.h>

class UiStaticText : public TextWin
{
public:
    UiStaticText();
    ~UiStaticText() override;

    // slot 8 (+0x20) -- the branch.
    void SetTransform(float const* Transform) override;
    // slot 9 (+0x24) -- the routine; args (Parent, FUiPayloadText*). Always returns true.
    bool Create(Win3D* NewParent, void* Payload) override;

    float LocalWidth;               // +0xec -- payload +0x00
    float LocalHeight;              // +0xf0 -- payload +0x04
};

static_assert(sizeof(UiStaticText) == 0xf4, "baseline UIStaticText is 0xf4 bytes");
static_assert(offsetof(UiStaticText, LocalWidth) == 0xec, "UIStaticText +0xec");
static_assert(offsetof(UiStaticText, LocalHeight) == 0xf0, "UIStaticText +0xf0");
