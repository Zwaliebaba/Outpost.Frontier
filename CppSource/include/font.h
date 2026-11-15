// Author: Giperion.

#pragma once

// IFont / FontImp -- baseline Font.cpp (`.?AUIFont@@` / `.?AUFontImp@@`).
// Exe-local plain C++ vtable (not NEURONCOMPONENTS). Font::String holds IFont* at +0x1c.
// IFont's own vtable is module storage (every slot _purecall the branch); FontImp's is
// (installed by the ctor the routine at 0x00417e11, the copy ctor the routine
// at 0x00418eea and the dtor the routine at 0x004164a0). The only implementation is FontImp
// (src/OutpostFrontier/Client/font_imp.h).

// Every slot that touches the face first re-derives the pixel height when the layout generation
// moved (the autoscale prelude, see FontImp::RefreshScale). Glyph metrics come from
// GDI on the font's own memory DC (+0xd8).

struct tagLOGFONTA;
struct HFONT__;

class IFont
{
public:
    // +0x00 the branch -- `LEA EAX,[ECX+0x4c]`: the id string's text (FontImp::Id, "%s,%d"
    // written by the routine). Caller not identified.
    virtual char const* Slot00() = 0;
    // +0x04 the branch -- `LEA EAX,[ECX+0x8]`: the nickname (the routine matches it).
    virtual char const* GetName() = 0;
    // +0x08 the branch -- `FLD [ECX+0xd0]`: fixed_height.
    virtual float GetFixedHeight() = 0;
    // +0x0c the branch -- the autoscale prelude, then the realized HFONT (+0x118). The glyph
    // atlas rasterizer selects it into its scratch DC (the routine, 0x00412a0c).
    virtual HFONT__* Slot03() = 0;
    // +0x10 the branch -- the autoscale prelude, then &this+0xdc, the LOGFONTA the face was
    // built from (`LEA EAX,[EBP+0xdc]` at 0x00416abe). FLImeInputBox (the routine, 0x005642bc)
    // hands it to IMEInputBox's constructor.
    virtual tagLOGFONTA const* GetLogFont() = 0;
    // +0x14 the branch -- the ratio-target autoscale prelude, then GetCharWidthW of one
    // character as a float (the routine word-wrap). Named GetGlyphAdvance: Windows.h maps
    // GetCharWidth to GetCharWidthA/W.
    virtual float GetGlyphAdvance(wchar_t Ch) = 0;
    // +0x18 the branch -- the autoscale prelude, then the realized TEXTMETRIC height (+0x120)
    // or, with no HFONT (+0x118), Slot07. ListBox::Update (the routine) divides the cursor
    // offset by it to find the hovered row.
    virtual int GetLineHeight() = 0;
    // +0x1c the branch -- `MOV EAX,[ECX+0xd4]`: the tmHeight the routine snapshots at init.
    virtual int Slot07() = 0;
    // +0x20 the branch -- measure a string: Out[0] = width, Out[1] = height.
    virtual void MeasureString(int* OutWH, wchar_t const* Text, int Len) = 0;
    // +0x24 the branch (RET 0xc) -- the ratio-target autoscale prelude, then the number of
    // leading characters whose summed GetCharWidthW stays within MaxWidth (Len == -1: wcslen).
    virtual int ProbeString(int MaxWidth, wchar_t const* Text, int Len) = 0;
    // +0x28 the branch (RET 0xc) -- the ratio-target autoscale prelude, then GetCharWidthW for
    // each of Len characters (Len == -1: wcslen) into OutWidths[i]; returns the sum. Callers: the
    // NumberString face build (the routine 0x004141ed, the branch 0x00415220) with the glyph
    // string's capacity as Len.
    virtual int GetCharWidths(wchar_t const* Text, int Len, int* OutWidths) = 0;
};

static_assert(sizeof(void*) == 4, "IFont is 32-bit exe");
