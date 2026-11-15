// Author: Giperion.

#include "ui_static_text.h"

#include "ui_page.h"
#include "ui_page_desc.h"
#include "win_factory.h"

UiStaticText::UiStaticText()
    : LocalWidth(0.0f)
    , LocalHeight(0.0f)
{
    // 0059c40c..0059c4ab (inlined ctor): the Win3D and TextWin parts run in their
    // constructors. +0xec / +0xf0 are left to slot 9 (the zeros are only C++ initialisation).
}

UiStaticText::~UiStaticText()
{
    // -> the TextWin dtor the routine.
}

bool UiStaticText::Create(Win3D* NewParent, void* Payload)
{
    // (RET 8).
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);

    InitTextDefaults();

    Flags = static_cast<unsigned char>(Flags & 0xfe);
    Width = 0.1f;
    Height = 0.1f;
    FlagE1 = 0;
    SetText(L"StaticText");                                           // vftable+0x70

    if (Payload != nullptr)
    {
        FUiPayloadText const* TextPayload = static_cast<FUiPayloadText const*>(Payload);
        LocalWidth = TextPayload->Right;
        Width = TextPayload->Right;
        LocalHeight = TextPayload->Bottom;
        Height = TextPayload->Bottom;
        SetText(TextPayload->CaptionUtf16);                           // vftable+0x70
        ColorD8 = 0xff000000u;                                        // bytes 0, 0, 0, 0xff
    }
    return true;
}

void UiStaticText::SetTransform(float const* Transform)
{
    // (RET 4): slot 7 (Transform), slot 5 (Transform + 9), then the extent
    // projection through the field with M = the argument block itself (EDI, not this+0x3c)
    // and Z = Transform[11]; +0xb4 / +0xb8 from +0xec / +0xf0.
    SetOrientation(Transform);
    SetPosition(Transform + 9);
    UiWidgetProjectExtent(Transform, Transform[11], LocalWidth, LocalHeight, &Width, &Height);
}

// WINFACTORY<UIStaticText> static (the branch -> the routine; Create
// ); atexit dtor the routine.
WINFACTORY<UiStaticText> G_UIStaticTextFactory("UIStaticText");
