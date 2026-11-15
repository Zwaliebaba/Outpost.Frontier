// Author: Giperion.

#pragma once

// baseline TextWin (RTTI .?AUTextWin@@ : Win3D) -- a Win3D that draws one Font::String at its
// position. Base of Rollover (0x005d5454), ToolTip (0x005d5f7c), TextMsgButton (0x005e2844)
// and StaticText -> UIStaticText (0x005e5a64).

// vftable 0x005d54cc (29 slots): Win3D's 28 with these overrides --
//   0 the routine scalar deleting dtor around the routine,
//   4 the routine HitTest (the ray's point at this object's depth inside the text box),
//   9 the routine Create, 10 the routine Destroy, 13 the routine Draw
// -- and one slot of its own, 28 the branch SetText.

// The TextWin constructor has no body of its own in the exe: every derived constructor
// inlines it (e.g. WINFACTORY<UIStaticText>::Create 0x0059c40c..0x0059c4a6): the Win3D part,
// the Font::String at +0x7c (the routine), then the five colours +0xcc..+0xdc zeroed. The
// rest is left for Create's the routine.

// Layout:
//   +0x07c Font::String (0x34)
//   +0x0b0 text scale (the routine: 0.002f)
//   +0x0b4 width, +0x0b8 height -- the text box, in world units at this object's depth
//   +0x0bc..+0x0c8 bounds {0, 0, width, -height}, rebuilt by Draw
//   +0x0cc idle colour (the field), +0x0d0 pulse base colour (the field),
//   +0x0d4, +0x0d8 (zeroed, used by subclasses), +0x0dc inactive colour (the field)
//   +0x0e0..+0x0e3 flag bytes, +0x0e6 use-palette byte, +0x0e8 draw-origin word
//   size 0xec

#include "font_string.h"
#include "win3d.h"

#include <stddef.h>

class TextWin : public Win3D
{
public:
    TextWin();

    ~TextWin() override;

    // slot 4 the routine
    bool HitTest(float const* Ray) override;
    // slot 9 the routine
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 10 the routine
    void Destroy() override;
    // slot 13 the routine
    void Draw() override;

    // slot 28 (+0x70) the branch -- the routine on the Font::String at +0x7c
    virtual void SetText(wchar_t const* NewText);

    // (thiscall) -- the text defaults Create installs.
    void InitTextDefaults();

    // (thiscall, RET 8) -- the routine(FontName); when it differs from the
    // Font::String's +0x1c font: store it, hand it to every child's slot +8, zero the layout
    // generation (+0xa4). TextScale (+0xb0) = Scale on both paths.
    void SetFont(char const* FontName, float Scale);

    FFontString Text;               // +0x07c
    float TextScale;                // +0x0b0
    float Width;                    // +0x0b4
    float Height;                   // +0x0b8
    float Bounds[4];                // +0x0bc
    unsigned long ColorIdle;        // +0x0cc
    unsigned long ColorPulse;       // +0x0d0
    unsigned long ColorD4;          // +0x0d4
    unsigned long ColorD8;          // +0x0d8
    unsigned long ColorInactive;    // +0x0dc
    unsigned char FlagE0;           // +0x0e0
    unsigned char FlagE1;           // +0x0e1
    unsigned char FlagE2;           // +0x0e2
    unsigned char FlagE3;           // +0x0e3
    unsigned char FieldE4;          // +0x0e4 -- not written by TextWin
    unsigned char FieldE5;          // +0x0e5 -- not written by TextWin
    unsigned char bUsePalette;      // +0x0e6
    unsigned char PadE7;            // +0x0e7
    unsigned FieldE8;               // +0x0e8 -- Font::String draw-origin +0x10

private:
    TextWin(TextWin const&) = delete;
    TextWin& operator=(TextWin const&) = delete;
};

static_assert(sizeof(TextWin) == 0xec, "baseline TextWin is 0xec bytes");
static_assert(offsetof(TextWin, Text) == 0x7c, "TextWin +0x7c Font::String");
static_assert(offsetof(TextWin, TextScale) == 0xb0, "TextWin +0xb0 scale");
static_assert(offsetof(TextWin, Width) == 0xb4, "TextWin +0xb4 width");
static_assert(offsetof(TextWin, Bounds) == 0xbc, "TextWin +0xbc bounds");
static_assert(offsetof(TextWin, ColorIdle) == 0xcc, "TextWin +0xcc idle colour");
static_assert(offsetof(TextWin, ColorInactive) == 0xdc, "TextWin +0xdc inactive colour");
static_assert(offsetof(TextWin, FlagE0) == 0xe0, "TextWin +0xe0 flags");
static_assert(offsetof(TextWin, bUsePalette) == 0xe6, "TextWin +0xe6");
static_assert(offsetof(TextWin, FieldE8) == 0xe8, "TextWin +0xe8");

// (cdecl, 8-byte return in EAX:EDX) -- a width/height at depth Z in world units
// to screen units through the Win3D camera (+0x78 XScale, +0x7c YScale, +0xa4 ZNear).
void TextWinProjectExtent(float Width, float Height, float Z, float* OutWidth,
                          float* OutHeight);

// (cdecl, 8-byte return) -- a world point to its screen position through the
// Win3D camera's Project slot (0x34); {0, 0} when it is behind the near plane.
void TextWinProjectPoint(float const* World, float* OutX, float* OutY);
