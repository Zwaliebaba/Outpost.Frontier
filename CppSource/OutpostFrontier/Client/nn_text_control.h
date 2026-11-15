// Author: Giperion.

#pragma once

// TextControl -- the NeuroNet WinControl text label (RTTI .?AVTextControl@@ : WinControl).
// WINFACTORY<TextControl>::Create allocates 0x3a0 (operator_new at 0x0058c138); ctor
// , dtor the routine, vftable 0x005e4f24. EditControl (ctor at 0x0058c26a) derives
// from it and adds +0x3a0..+0x3ad.

// baseline Init the routine (RET 0x28), called by CreateTextControl the routine and
// CreateEditControl the routine:
//   +0x378 = the last argument; the style's font (the routine) is bound into the Font::String
//   at +0x334; Offset(+0xa8) = *Position; a hardpoint name is copied to +0x1d0 and resolved on
//   the parent; +0x32c/+0x330 = width/height; the ids through the routine; +0x358/+0x344.
// Draw the branch has its ordinary Font::String path. Its typewriter and cached-placement
// branches are still missing (tracker 0x0058a7f0 partial).

#include "font_string.h"
#include "wincontrol_base.h"

#include <stddef.h>

class NnTextControl : public WinControl
{
public:
    // .
    NnTextControl();
    // .
    ~NnTextControl() override;

    // (thiscall, RET 0x28). Arg6 is never read by the baseline body.
    void Init(char const* StyleName, float const* InPosition, char const* Hardpoint,
              float InWidth, float InHeight, float Arg6, unsigned Ids, int InDword358,
              int InDword344, unsigned char InByte378);
    // (The port-only seven-argument Init adapter for the removed flat menu builders is gone,
    // 2026-10-07.)

    // (thiscall, RET 4) -- the ids' string (the routine, "IDS?" when it is
    // missing; an empty string for ids 0) into the Font::String, after the routine.
    void SetIds(unsigned Ids);

    // (thiscall, RET 4) -- the routine(Width, Offset.z + Position.z) and
    // (Text) on the Font::String. Caller: HUD_Comm's caption, the routine
    // .
    void SetText(wchar_t const* Text);

    // (thiscall, RET 4) -- the same the routine, then the routine(0, Format):
    // Format (a printf format, narrow) goes to the Font::String's NumberString child. Callers:
    // , 0x004d2c87, 0x004d59ba, 0x004d5a49, 0x004ddfb0, 0x004ddfec, 0x004eeffa.
    void SetNumberText(char const* Format);

    // (thiscall, RET 4) -- a different font is stored in the Font::String (+0x1c),
    // every child slot gets vt+8 with it, and +0x28 = 0. Init inlines the same body.
    void SetFont(IFont* Font);

    // (thiscall, RET 8) -- +0x395 = bCached; a non-zero bInvalidate clears +0x394
    // (the "cached placement valid" byte the draw at 0x0058ac5f..0x0058ac98 tests and sets).
    // Callers: HUD_CruiseProgress::Draw 0x004d5fb6 / 0x004d6058.
    void SetCachedDraw(unsigned char bCached, unsigned char bInvalidate);

    // (thiscall, RET 4) -- the routine(StyleName); a non-null font that differs
    // from the Font::String's gets the routine's body (inlined there). Callers:
    // NN_Preferences::RefreshRows 0x004b097e / 0x004b135d.
    void SetFontByName(char const* StyleName);
    // (thiscall, RET 4) -- +0x32c = NewWidth, then the routine(NewWidth,
    // +0xb0 + +0x68). Callers: NN_Preferences::RefreshRows 0x004b098e / 0x004b1350.
    void SetWidth(float NewWidth);

    // anchor text to the centre of the requested rectangle.
    void SetPosition(float const* NewPosition) override;

    // slot 9 the routine (RET 8) -- Win3D's Create body inlined (reparent, register, flags),
    // then the INI_Reader payload's keys: font, offset, hardpoint, width, height, font_scale
    // (accepted, ignored), stringID, justification (+0x358), tooltip (+0x344). Last, the
    // parent's slot 5 with its own position. Always true.
    bool Create(Win3D* NewParent, void* Data) override;

    // slot 4 the branch (RET 4) -- only with a tooltip (+0x344) or +0x35c: the Width x Height
    // text rectangle (identity orientation, centre = Position + Offset + {W/2, -H/2, 0}) against
    // the ray (the routine). A hit feeds the tooltip (the routine) when +0x344 is set and tells
    // the parent's WinControl face property 0x46 with this control; true.
    bool HitTest(float const* Ray) override;

    // slot 19 the routine (RET 4) -- only with +0x35c: a {-1, 3} event tells the parent
    // property 0x27; a captured control leaves the capture stack and on {-1, 1} tells the
    // parent 0x26 (true); any other {-1, n != 1} event pushes it on the capture stack (true).
    bool MouseEvent(FWin3DMouseEvent const* Event) override;

    // slot 7 the branch (RET 4) -- +0x328 = 1; Orientation(+0x3c) = NewOrientation times the
    // attach orientation (slot 40) through the math engine's MulMatrix (the field). Unlike
    // WinControl's the routine, no mesh update and no child walk.
    void SetOrientation(float const* NewOrientation) override;

    // slot 12 the branch -- slot 12 on every inactive child; a running typewriter (+0x389) is
    // stopped and the whole pending text (+0x370) shown.
    void UpdateInactive() override;

    // slot 11 the routine -- Win3D's Update (the routine), then the typewriter: +0x368 gathers
    // frame time and every +0x36c seconds one more character (+0x364) is revealed; the run ends
    // (sound slot 0xb stopped, whole text shown) at the pending text's length or when the
    // control stops being drawable. Last, the +0x379 blink swaps +0x35e between the colours at
    // +0x381 and +0x385 every second.
    void Update() override;

    virtual void Draw() override;

    // slot 42 the routine -- property switch 6..0x51 (jump table 0058b6a8 / 0058b6e0); every
    // other id goes to WinControl the routine.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    FFontString* GetFontString() const { return FontString; }

    // (thiscall, RET 4) -- the Font::String's +0x08 (sentinel 0x7fff from
    // ; meaning not recovered) = Value. Caller: HUD_ContactList::Create 0x004cf850
    // (Value 1 on every name text).
    void SetFontStringField08(int Value) { FontString->Field08 = Value; }

    float Width;                    // +0x32c
    float Height;                   // +0x330
    FFontString* FontString;        // +0x334 -- operator_new(0x34) + the routine in the ctor
    int Field338;                   // +0x338 -- ctor -1; property 0x22 (A)
    int Field33c;                   // +0x33c -- ctor -1; property 0x22 (B)
    int Field340;                   // +0x340 -- ctor 0; property 0x21 returns it
    int Dword344;                   // +0x344 -- Init's 9th argument
    unsigned long TextColour;       // +0x348 -- ctor the field with the top byte forced 0xff
    int Field34c;                   // +0x34c -- property 0x23 (A)
    int Field350;                   // +0x350 -- property 0x23 (B)
    unsigned char Byte354;          // +0x354 -- property 0xc (A != 0)
    unsigned char Pad355[3];
    int Dword358;                   // +0x358 -- Init's 8th argument
    unsigned char Byte35c;          // +0x35c -- property 0x24 (A != 0)
    unsigned char bColourOverride;  // +0x35d -- property 0x33 (B == 1); property 0x51 sets it
    unsigned char OverrideColour[4]; // +0x35e -- property 0x33: *(DWORD*)A (unaligned dword)
    unsigned char bTypewriter;      // +0x362 -- property 0x50 (A == 1)
    unsigned char Pad363;
    int TypedCount;                 // +0x364 -- ctor 1; property 0x50 resets it to 1; Update
                                    //   adds one per TypeInterval (INC dword, CMP to wcslen)
    float TypeElapsed;              // +0x368 -- ctor 0; property 0x50 resets it to 0; Update's
                                    //   FLD/FADD/FSTP float accumulator
    float TypeInterval;             // +0x36c -- ctor 0.05f; property 0x50: B * 0.001f
    wchar_t* PendingText;           // +0x370 -- the typewriter's full text (free()d)
    unsigned PendingCapacity;       // +0x374
    unsigned char Byte378;          // +0x378 -- Init's last argument; 0 = property 6 re-centres
    unsigned char bBlink;           // +0x379 -- property 0x51; Update's colour blink runs
    unsigned char BlinkPhase;       // +0x37a -- toggled once per whole second of blink time
    unsigned char Pad37b;
    float BlinkElapsed;             // +0x37c -- ctor 0; Update's FADD/FST float accumulator
    unsigned char Byte380;          // +0x380 -- ctor 1
    unsigned char BlinkColourOn[4]; // +0x381 -- ctor 0; copied to +0x35e while BlinkPhase != 0
    unsigned char BlinkColourOff[4]; // +0x385 -- ctor 0; copied to +0x35e while BlinkPhase == 0
    unsigned char bTyping;          // +0x389 -- a typewriter run holds the field
    unsigned char Pad38a[2];
    float CachedX;                  // +0x38c -- Draw's cached screen x (0x0058aca9 / 0x0058ace0)
    float CachedY;                  // +0x390 -- Draw's cached screen y (0x0058acb7)
    unsigned char Byte394;          // +0x394 -- ctor 0; the cached placement is valid
    unsigned char Byte395;          // +0x395 -- ctor 0; Draw uses the cached placement
    unsigned char Pad396[2];
    unsigned CachedSurfaceWidth;    // +0x398 -- the field when the cache was filled
    unsigned char bInCaptureList;   // +0x39c -- dtor unlinks through the routine when set
    unsigned char Pad39d[3];
};

static_assert(offsetof(NnTextControl, Width) == 0x32c, "TextControl +0x32c width");
static_assert(offsetof(NnTextControl, FontString) == 0x334, "TextControl +0x334 Font::String");
static_assert(offsetof(NnTextControl, TextColour) == 0x348, "TextControl +0x348 colour");
static_assert(offsetof(NnTextControl, Dword358) == 0x358, "TextControl +0x358");
static_assert(offsetof(NnTextControl, bColourOverride) == 0x35d, "TextControl +0x35d");
static_assert(offsetof(NnTextControl, OverrideColour) == 0x35e, "TextControl +0x35e");
static_assert(offsetof(NnTextControl, bTypewriter) == 0x362, "TextControl +0x362");
static_assert(offsetof(NnTextControl, TypeInterval) == 0x36c, "TextControl +0x36c");
static_assert(offsetof(NnTextControl, PendingText) == 0x370, "TextControl +0x370");
static_assert(offsetof(NnTextControl, Byte378) == 0x378, "TextControl +0x378");
static_assert(offsetof(NnTextControl, TypedCount) == 0x364, "TextControl +0x364");
static_assert(offsetof(NnTextControl, TypeElapsed) == 0x368, "TextControl +0x368");
static_assert(offsetof(NnTextControl, bBlink) == 0x379, "TextControl +0x379");
static_assert(offsetof(NnTextControl, BlinkElapsed) == 0x37c, "TextControl +0x37c");
static_assert(offsetof(NnTextControl, BlinkColourOn) == 0x381, "TextControl +0x381");
static_assert(offsetof(NnTextControl, BlinkColourOff) == 0x385, "TextControl +0x385");
static_assert(offsetof(NnTextControl, bTyping) == 0x389, "TextControl +0x389");
static_assert(offsetof(NnTextControl, CachedX) == 0x38c, "TextControl +0x38c");
static_assert(offsetof(NnTextControl, Byte394) == 0x394, "TextControl +0x394");
static_assert(offsetof(NnTextControl, CachedSurfaceWidth) == 0x398, "TextControl +0x398");
static_assert(offsetof(NnTextControl, bInCaptureList) == 0x39c, "TextControl +0x39c");
static_assert(sizeof(NnTextControl) == 0x3a0, "baseline TextControl is 0x3a0 bytes");

// "TextControl" is the WINFACTORY<NnTextControl> static G_TextControlFactory (nn_text_control.cpp,
// , Create the routine).

// EditControl -- a TextControl the player types into (RTTI .?AVEditControl@@ : TextControl),
// built by CreateEditControl the routine (NN_Inventory's jettison quantity 0x00479402, the
// dealer's QuantityEdit 0x00481dfe). WINFACTORY<EditControl> the field "EditControl":
// static init the branch (ctor the routine, atexit the branch -> the routine); its Create
// is the template's with the ctor inlined: operator_new(0x3b0), TextControl's
// , vftable 0x005e501c / face +0x7c 0x005e500c, then the five fields below.
// vftable 0x005e501c is TextControl's 0x005e4f24 with three overrides (raw dword diff): 11
// Update, 20 the branch KeyEvent, 42 the branch SetProperty. Slot 0 is
// TextControl's the routine: EditControl adds no destructor body.

// The typed text lives in the Font::String itself; a blinking '|' (the field) is appended to
// it and removed again every 0.5 s (the field), so the text read back through the Font::String
// may end in the cursor. Every edit tells the parent's WinControl face (slot 2) through
// SetProperty(0x45, text, bEnter).
class NnEditControl : public NnTextControl
{
public:
    // The ctor the routine inlines (0x0058c26a..0x0058c299).
    NnEditControl();

    // slot 11 the branch -- while editable: the blink timer runs (the routine) and every
    // 0.5 s the cursor is appended or removed; then TextControl's Update the routine. Not
    // editable: nothing at all (not even TextControl's Update).
    void Update() override;
    // slot 20 the branch (RET 8) -- Enter, Backspace and characters (numpad digits folded
    // onto '0'..'9', UTF-8 byte -> wide char) while active and editable.
    bool KeyEvent(unsigned Key, unsigned char* Modifiers) override;
    // slot 42 the branch (RET 0xc) -- 0x1d drops the cursor flag before TextControl's text
    // property; 0x43 editable on/off (+ the maximum length); 0x44 digits only; everything else
    // is TextControl's the routine.
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // (thiscall) -- with the cursor shown: the text without its last character back
    // into the Font::String (the routine's two calls), the flag cleared. Caller: SetProperty
    // 0x43 with 0 (0x0058c06f); KeyEvent and Update inline the same removal.
    void RemoveCursor();

    float CursorTimer;          // +0x3a0 -- ctor 0; reset by every key that edits
    unsigned char bCursorShown; // +0x3a4 -- the text currently ends in '|'
    unsigned char Pad3a5[3];
    int MaxLength;              // +0x3a8 -- ctor -1; > 0 caps wcslen(text) + 1 (property 0x43 B)
    unsigned char bEditable;    // +0x3ac -- ctor 1; property 0x43 A
    unsigned char bDigitsOnly;  // +0x3ad -- ctor 0; property 0x44 A & 1
    unsigned char Pad3ae[2];

private:
    NnEditControl(NnEditControl const&) = delete;
    NnEditControl& operator=(NnEditControl const&) = delete;
};

static_assert(offsetof(NnEditControl, CursorTimer) == 0x3a0, "EditControl +0x3a0");
static_assert(offsetof(NnEditControl, bCursorShown) == 0x3a4, "EditControl +0x3a4");
static_assert(offsetof(NnEditControl, MaxLength) == 0x3a8, "EditControl +0x3a8");
static_assert(offsetof(NnEditControl, bEditable) == 0x3ac, "EditControl +0x3ac");
static_assert(offsetof(NnEditControl, bDigitsOnly) == 0x3ad, "EditControl +0x3ad");
static_assert(sizeof(NnEditControl) == 0x3b0, "baseline EditControl is 0x3b0 bytes");
