// Author: Giperion.

#pragma once

// baseline ListBox (RTTI .?AUListBox@@ : Win3D) -- a Win3D drawing up to 0x100 single-line
// Font::String rows with a bevelled frame and optional row rules. Base of UIListBox
// (0x005e5cb4); also embedded by value twice in NN_LoadSave (ctor the routine, 0x004859de /
// +0x5dc and +0xe98, see nn_load_save.h).

// ctor the routine, dtor the routine (scalar deleting the routine).
// vftable 0x005e5f9c (28 slots): Win3D's with these overrides --
//   0 the routine, 4 the routine HitTest, 9 the branch Create, 11 the branch Update,
//   13 the branch Draw, 14 the branch MouseOver, 19 the routine MouseEvent.
// Non-virtual: the routine SetItemText, the routine GetItemData, the routine AddItem.

// Layout:
//   +0x07c hovered row (-1 = none), rebuilt by Update from the software cursor
//   +0x080 row drawn with the pulse colour (the ListBox itself never sets it; NN_LoadSave's
//          Update the routine copies the hovered row into it)
//   +0x084 IFont* ("ListText", the routine), +0x088 text scale 0.002f (the field)
//   +0x08c int item data[0x100] -- never initialised by the ctor
//   +0x48c Font::String row objects[0x100] (the routine on demand; Release on teardown)
//   +0x88c first visible row, +0x890 / +0x894 two timers (Update adds the frame time,
//          MouseOver sets both to 10.0f)
//   +0x898 / +0x89c box width / height in world units
//   +0x8a0 draw the frame, +0x8a1 draw the row rules, +0x8a2 rows are selectable
//   +0x8a4 item count, +0x8a8 visible rows (0x10), +0x8ac selected row (-1 = none)
//   size 0x8b0. NN_LoadSave settles it: its two embedded ListBoxes sit at +0x5dc and +0xe98
//   with a Vector written between them at +0xe8c..+0xe97 (Create 0x0048663f..0x00486654) --
//   0x5dc + 0x8b0 = 0xe8c. The two floats UIListBox keeps at +0x8b0 / +0x8b4 are its own
//   (UIListBox allocates 0x8b8, the routine; see ui_list_box.h).

#include "font_string.h"
#include "win3d.h"

#include <stddef.h>

class ListBox : public Win3D
{
public:

    ListBox();

    ~ListBox() override;

    // slot 4 the routine
    bool HitTest(float const* Ray) override;
    // slot 9 the branch
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 14 the branch
    void MouseOver(float const* Ray) override;
    // slot 19 the routine
    bool MouseEvent(FWin3DMouseEvent const* Event) override;

    // (thiscall, RET 8) -- replace the text of an existing row.
    bool SetItemText(int Index, wchar_t const* Text);
    // (thiscall, RET 4) -- the data word of a row, 0 when out of range.
    int GetItemData(int Index);
    // (thiscall, RET 8) -- append a row; its index, or -1 when full.
    int AddItem(wchar_t const* Text, int Data);

    int HoverIndex;                         // +0x07c
    int Field80;                            // +0x080
    IFont* Font;                            // +0x084
    float TextScale;                        // +0x088
    int ItemData[0x100];                    // +0x08c
    FontStringChild* Items[0x100];          // +0x48c
    int FirstVisible;                       // +0x88c
    float Timer890;                         // +0x890
    float Timer894;                         // +0x894
    float Width;                            // +0x898
    float Height;                           // +0x89c
    unsigned char bDrawFrame;               // +0x8a0
    unsigned char bDrawRowRules;            // +0x8a1
    unsigned char bSelectable;              // +0x8a2
    unsigned char Pad8a3;                   // +0x8a3
    int ItemCount;                          // +0x8a4
    int VisibleRows;                        // +0x8a8
    int SelectedIndex;                      // +0x8ac

private:
    // + slots 2 / 5 of the row object: the shared body of SetItemText/AddItem.
    void SetRowText(int Index, wchar_t const* Text);

    ListBox(ListBox const&) = delete;
    ListBox& operator=(ListBox const&) = delete;
};

static_assert(sizeof(void*) != 4 || sizeof(ListBox) == 0x8b0,
              "NN_LoadSave embeds ListBoxes at +0x5dc and +0xe98 with a Vector at +0xe8c");
static_assert(offsetof(ListBox, HoverIndex) == 0x7c, "ListBox +0x7c");
static_assert(offsetof(ListBox, Font) == 0x84, "ListBox +0x84 font");
static_assert(offsetof(ListBox, ItemData) == 0x8c, "ListBox +0x8c item data");
static_assert(offsetof(ListBox, Items) == 0x48c, "ListBox +0x48c rows");
static_assert(offsetof(ListBox, FirstVisible) == 0x88c, "ListBox +0x88c");
static_assert(offsetof(ListBox, Width) == 0x898, "ListBox +0x898");
static_assert(offsetof(ListBox, bDrawFrame) == 0x8a0, "ListBox +0x8a0");
static_assert(offsetof(ListBox, ItemCount) == 0x8a4, "ListBox +0x8a4");
static_assert(offsetof(ListBox, SelectedIndex) == 0x8ac, "ListBox +0x8ac");

// (the branch) and the field (the branch): the frame's dark and light
// RGB bytes, handed to glColor3ubv (the routine).
extern unsigned char G_ListBoxShadeRgba[4];
extern unsigned char G_ListBoxLightRgba[4];
