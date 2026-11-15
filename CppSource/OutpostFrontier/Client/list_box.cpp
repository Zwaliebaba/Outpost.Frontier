// Author: Giperion.

#include "list_box.h"

#include "font_atlas.h"
#include "font_imp.h"
#include "globals.h"
#include "input_manager.h"
#include "nn_ui_pulse.h"
#include "text_win.h"
#include "win3d_immediate.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

unsigned char G_ListBoxShadeRgba[4] = {0x37u, 0x49u, 0x5au, 0xffu};
unsigned char G_ListBoxLightRgba[4] = {0x9bu, 0xcdu, 0xffu, 0xffu};

namespace
{
    unsigned const kRowCapacity = 0x100u;
}

ListBox::ListBox()
{
    // the Win3D part (inlined), the row array (vector ctor iterator with
    // , which stores 0), the timers, the state Create also resets, the font and
    // the text scale. The item data at +0x8c and +0x898..+0x8b4 are not written.
    for (unsigned Index = 0; Index < kRowCapacity; ++Index)
        Items[Index] = nullptr;
    Timer890 = 0.0f;
    Timer894 = 0.0f;
    SelectedIndex = -1;
    HoverIndex = -1;
    Field80 = -1;
    ItemCount = 0;
    VisibleRows = 0x10;
    FirstVisible = 0;
    bDrawFrame = 1;
    bDrawRowRules = 0;
    bSelectable = 1;
    Font = FindMenuFontImp("ListText");
    TextScale = 0.002f;                 // = 0x3b03126f
}

ListBox::~ListBox()
{
    // the vector dtor iterator over +0x48c runs the routine on each row from
    // the last to the first (Release through slot 1, then clear); the Win3D dtor follows.
    for (unsigned Index = kRowCapacity; Index != 0; --Index)
    {
        FontStringChild*& Row = Items[Index - 1];
        if (Row != nullptr)
        {
            Row->Release();
            Row = nullptr;
        }
    }
}

bool ListBox::HitTest(float const* Ray)
{
    // the ray's point at this object's depth inside [x, x + width] and
    // [y - height, y]. The y product is stored as a float; the x difference stays on the
    // x87 stack.
    double const T = static_cast<double>(Position[2]) / Ray[2];
    float const PointY = static_cast<float>(T * Ray[1]);
    double const DeltaX = T * Ray[0] - Position[0];
    if (!(DeltaX >= 0.0))
        return false;
    if (!(DeltaX <= Width))
        return false;
    double const DeltaY = static_cast<double>(Position[1]) - PointY;
    if (!(DeltaY >= 0.0))
        return false;
    if (!(DeltaY <= Height))
        return false;
    return true;
}

bool ListBox::Create(Win3D* NewParent, void* /*Data*/)
{
    // (RET 8).
    SelectedIndex = -1;
    HoverIndex = -1;
    Field80 = -1;
    ItemCount = 0;
    FirstVisible = 0;
    bDrawRowRules = 0;
    VisibleRows = 0x10;
    bDrawFrame = 1;
    bSelectable = 1;
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);
    return true;
}

void ListBox::Update()
{
    // .
    Win3D::Update(); // , inlined

    double const Delta = GetClientFrameTimeCapped();
    Timer890 = static_cast<float>(Delta + Timer890);
    Timer894 = static_cast<float>(Delta + Timer894);

    float ScreenX = 0.0f;
    float ScreenY = 0.0f;
    TextWinProjectPoint(Position, &ScreenX, &ScreenY);
    float ScreenWidth = 0.0f;
    float ScreenHeight = 0.0f;
    TextWinProjectExtent(Width, Height, Position[2], &ScreenWidth, &ScreenHeight);

    HoverIndex = -1;

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ); // / the field
    double const PointX = static_cast<double>(CursorX);
    if (PointX < ScreenX)
        return;
    if (!(PointX <= static_cast<double>(ScreenWidth) + ScreenX))
        return;
    double const PointY = static_cast<double>(CursorY);
    if (PointY < ScreenY)
        return;
    if (!(PointY <= static_cast<double>(ScreenHeight) + ScreenY))
        return;

    // IFont slot 0x18 is the line height in pixels.
    int const LineHeight = Font->GetLineHeight();
    int const Row = static_cast<int>((PointY - ScreenY) / LineHeight); // ftol
    if (Row < 0)
        return;
    if (Row >= ItemCount)
        return;
    HoverIndex = FirstVisible + Row;
}

void ListBox::Draw()
{
    // .
    Win3DDisable(kWin3DDepthTest); // (0xb71)
    Win3DDisable(kWin3DTexture2D); // (0xde1)

    float ScreenX = 0.0f;
    float ScreenY = 0.0f;
    TextWinProjectPoint(Position, &ScreenX, &ScreenY);
    float ScreenWidth = 0.0f;
    float ScreenHeight = 0.0f;
    TextWinProjectExtent(Width, Height, Position[2], &ScreenWidth, &ScreenHeight);
    int const BoxWidth = static_cast<int>(floor(static_cast<double>(ScreenWidth)));

    // The row origin lives on the stack as {x, y, x, y}; Paint reads the first two, and y
    // advances by the line height per row.
    float Origin[4] = {ScreenX, ScreenY, ScreenX, ScreenY};

    int Index = FirstVisible;
    int End = FirstVisible + VisibleRows;
    if (End > ItemCount)
        End = ItemCount;
    for (; Index < End; ++Index)
    {
        // the selected row in the field, the +0x80 row in the UI pulse
        // (the routine), every other row in the HUD chrome colour the field.
        unsigned long Color = 0;
        if (SelectedIndex == Index)
        {
            Color = G_NnPaletteArgb_679ba4;
        }
        else if (Field80 == Index)
        {
            unsigned long Pulsed = 0;
            NnUiPulseBlendArgb(&Pulsed);
            Color = Pulsed;
        }
        else
        {
            memcpy(&Color, G_HudChromeColorRgba, sizeof(Color));
        }
        FontAtlasSetDrawColor(static_cast<unsigned>(Color));

        FontStringChild* const Row = Items[Index];
        if (Row != nullptr)
        {
            // Slot 4 (+0x10, the branch) returns the text pointer at +0x4.
            wchar_t const* const Text = Row->Text_;
            if (Text != nullptr)
            {
                int TextExtent[2] = {0, 0};
                Font->MeasureString(TextExtent, Text, -1); // IFont +0x20
                int Overflow = 0;
                if (TextExtent[0] > BoxWidth)
                    Overflow = TextExtent[0] - BoxWidth;
                // Slot 9 (+0x24) takes the overflow as a float in its third dword.
                float const OverflowF = static_cast<float>(Overflow);
                void* OverflowArg = nullptr;
                memcpy(&OverflowArg, &OverflowF, sizeof(OverflowF));
                Row->Paint(Origin, nullptr, OverflowArg);
            }
        }

        // the line height read as an unsigned dword (FILD qword with a zero high
        // half).
        unsigned const LineHeight = static_cast<unsigned>(Font->GetLineHeight());
        Origin[1] = static_cast<float>(static_cast<double>(LineHeight) + Origin[1]);
    }
    Origin[0] = Origin[2];
    Origin[1] = Origin[3];

    // glMatrixMode(0x1701) is the empty RET at 0x00401e90.
    Win3DPushProjection();
    Win3DSetOrtho2D();

    float FrameWidth = 0.0f;
    float FrameHeight = 0.0f;
    TextWinProjectExtent(Width, Height, Position[2], &FrameWidth, &FrameHeight);
    float const Left = Origin[0];
    float const Top = Origin[1];
    float const Right = static_cast<float>(static_cast<double>(FrameWidth) + Left);
    float Bottom = static_cast<float>(static_cast<double>(FrameHeight) + Top);

    if (bDrawFrame != 0)
    {
        // top and left edges in the dark shade, right and bottom in the light
        // one. the routine is glColor3ubv: alpha forced to 0xff.
        Win3DBegin(kWin3DLines);
        Win3DColor3ub(G_ListBoxShadeRgba[0], G_ListBoxShadeRgba[1], G_ListBoxShadeRgba[2]);
        Win3DVertex3f(Left, Top, 0.0f);
        Win3DVertex3f(Right, Top, 0.0f);
        Win3DVertex3f(Left, Top, 0.0f);
        Win3DVertex3f(Left, Bottom, 0.0f);
        Win3DColor3ub(G_ListBoxLightRgba[0], G_ListBoxLightRgba[1], G_ListBoxLightRgba[2]);
        Win3DVertex3f(Right, Top, 0.0f);
        Win3DVertex3f(Right, Bottom, 0.0f);
        Win3DVertex3f(Left, Bottom, 0.0f);
        Win3DVertex3f(Right, Bottom, 0.0f);
        Win3DEnd();
    }

    if (bDrawRowRules != 0)
    {
        // VisibleRows + 1 horizontal rules in the light shade, one line height
        // apart from the top.
        int const LineHeight = Font->GetLineHeight();
        Win3DColor3ub(G_ListBoxLightRgba[0], G_ListBoxLightRgba[1], G_ListBoxLightRgba[2]);
        Win3DBegin(kWin3DLines);
        if (VisibleRows >= 0)
        {
            int Offset = 0;
            int Rule = 0;
            do
            {
                Bottom = static_cast<float>(static_cast<double>(Offset) + Top);
                Win3DVertex3f(Left, Bottom, 0.0f);
                Win3DVertex3f(Right, Bottom, 0.0f);
                ++Rule;
                Offset += LineHeight;
            } while (Rule <= VisibleRows);
        }
        Win3DEnd();
    }

    Win3DPopProjection();    // (after the no-op glMatrixMode(0x1701))
    Win3DDrawChildren(this);
}

void ListBox::MouseOver(float const* /*Ray*/)
{
    // (RET 4): both timers to 10.0f (0x41200000).
    Timer894 = 10.0f;
    Timer890 = 10.0f;
}

bool ListBox::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4): a button press (-1) that is not the plain kind 1 selects the
    // hovered row and tells the parent through slot 23 -- 0xf001 for kind 2, 0xf002 for
    // kind 3 -- with the row's text and index.
    if (bSelectable == 0)
        return false;
    if (Event->Field1 == 1)
        return false;
    if (Event->Field0 != -1)
        return false;

    SelectedIndex = HoverIndex;
    if (HoverIndex >= ItemCount)
        SelectedIndex = -1;
    if (Parent == nullptr)
        return false;

    wchar_t const* Text = nullptr;
    int const Selected = SelectedIndex;
    if (Selected >= 0 && Selected < ItemCount && Selected < 0x100 && Items[Selected] != nullptr)
        Text = Items[Selected]->Text_; // slot 4 (+0x10)

    unsigned const TextArg = static_cast<unsigned>(reinterpret_cast<uintptr_t>(Text));
    if (Event->Field1 == 2)
    {
        Parent->Slot23(0xf001, TextArg, static_cast<unsigned>(SelectedIndex));
        return true;
    }
    if (Event->Field1 == 3)
    {
        Parent->Slot23(0xf002, TextArg, static_cast<unsigned>(SelectedIndex));
        return true;
    }
    return false;
}

void ListBox::SetRowText(int Index, wchar_t const* Text)
{
    // / 0x005a1f7a: create the row on demand (the routine), then slot 2 (+0x8)
    // binds the list's font and slot 5 (+0x14) sets the text.
    if (Items[Index] == nullptr)
        Items[Index] = new FontStringChild();
    FontStringChild* const Row = Items[Index];
    if (Row == nullptr)
        return;
    Row->BindFont(Font);
    Row->SetText(Text);
}

bool ListBox::SetItemText(int Index, wchar_t const* Text)
{
    // (RET 8).
    if (Index >= ItemCount)
        return false;
    if (Index >= 0x100)
        return false;
    SetRowText(Index, Text);
    return true;
}

int ListBox::GetItemData(int Index)
{
    // (RET 4).
    if (Index == -1)
        return 0;
    if (Index >= ItemCount)
        return 0;
    if (Index >= 0x100)
        return 0;
    return ItemData[Index];
}

int ListBox::AddItem(wchar_t const* Text, int Data)
{
    // (RET 8).
    int const Index = ItemCount;
    if (Index >= 0x100)
        return -1;
    ItemCount = Index + 1;
    if (Index < ItemCount && Index < 0x100)
        SetRowText(Index, Text);
    if (Index < ItemCount && Index < 0x100)
        ItemData[Index] = Data;
    return Index;
}
