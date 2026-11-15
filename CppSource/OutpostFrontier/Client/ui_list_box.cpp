// Author: Giperion.

#include "ui_list_box.h"

#include "ui_page.h"
#include "ui_page_desc.h"
#include "win_factory.h"

UiListBox::UiListBox()
    : LocalWidth(0.0f)
    , LocalHeight(0.0f)
{
    // 0x0059c84b..0x0059c850: the ListBox ctor, then the UIListBox vftable.
    // +0x8b0 / +0x8b4 are left unset by baseline; the port zeroes them.
}

UiListBox::~UiListBox()
{
    // the ListBox dtor (thunk to the routine) and nothing of its own.
}

bool UiListBox::Create(Win3D* NewParent, void* Payload)
{
    // (RET 8): ListBox's resets and registration (the branch inlined), then
    // the payload's width / height into both the unprojected and the projected extent.
    SelectedIndex = -1;
    HoverIndex = -1;
    Field80 = -1;
    ItemCount = 0;
    VisibleRows = 0x10;
    FirstVisible = 0;
    bDrawFrame = 1;
    bDrawRowRules = 0;
    bSelectable = 1;
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);

    if (Payload != nullptr)
    {
        FUiPayloadBounds const* Desc = static_cast<FUiPayloadBounds const*>(Payload);
        LocalWidth = Desc->Right;
        Width = Desc->Right;
        LocalHeight = Desc->Bottom;
        Height = Desc->Bottom;
    }
    return true;
}

void UiListBox::SetTransform(float const* Transform)
{
    // (RET 4): slot 7 SetOrientation(Transform), slot 5 SetPosition(Transform + 9),
    // then the shared extent projection through the field with the new Z (Transform[11]).
    SetOrientation(Transform);
    SetPosition(Transform + 9);
    UiWidgetProjectExtent(Orientation, Transform[11], LocalWidth, LocalHeight, &Width, &Height);
}

// WINFACTORY<UIListBox> static (the branch -> the routine; Create
// ); atexit dtor the routine.
WINFACTORY<UiListBox> G_UIListBoxFactory("UIListBox");
