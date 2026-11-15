// Author: Giperion.

#pragma once

// UIListBox -- PAGE_*.txt list widget (RTTI .?AUUIListBox@@ : ListBox : Win3D).

// baseline:
//   WINFACTORY<UIListBox> the field, static init the branch -> the routine("UIListBox")
//   Create the routine: operator_new(0x8b8) + ListBox ctor the routine, then vftable
// UIListBox has no constructor body of its own. ListBox is 0x8b0 bytes (its
//   by-value copies in NN_LoadSave settle that, list_box.h), so the two floats at +0x8b0 /
//   +0x8b4 -- written only by slot 9 and read only by slot 8 -- are UIListBox's; baseline's ctor
//   leaves them unset.
//   vftable 0x005e5cb4 overrides of ListBox 0x005e5f9c:
//     slot 0  the routine  scalar deleting destructor (ListBox dtor + operator_delete)
//     slot 8  the branch  SetTransform + extent projection
//     slot 9  the routine  Create from the page payload
//     slot 13 0x0059a6a0    a debug frame guarded by the field, then JMP ListBox::Draw
//                           (the routine). the field has four readers and no writer in
//                           the PE (no instruction and no pointer to it outside those reads),
//                           so the .bss byte stays 0 and slot 13 behaves as ListBox::Draw --
//                           the port does not override it.

#include "list_box.h"

class UiListBox : public ListBox
{
public:
    UiListBox();
    ~UiListBox() override;

    // slot 8 (+0x20) -- the branch.
    void SetTransform(float const* Transform) override;
    // slot 9 (+0x24) -- the routine.
    bool Create(Win3D* NewParent, void* Payload) override;

    float GetWidth() const { return Width; }
    float GetHeight() const { return Height; }

    float LocalWidth;                       // +0x8b0 -- unprojected width (slot 9 payload)
    float LocalHeight;                      // +0x8b4 -- unprojected height (slot 9 payload)

private:
    UiListBox(UiListBox const&) = delete;
    UiListBox& operator=(UiListBox const&) = delete;
};

static_assert(sizeof(UiListBox) == 0x8b8, "module state allocates 0x8b8");
static_assert(offsetof(UiListBox, LocalWidth) == 0x8b0, "UIListBox +0x8b0");
