// Author: Giperion.

#pragma once

// baseline WireStaticControl / WireButtonControl / RichTextWireButtonControl -- the NeuroNet
// "wire box" controls: a rectangle of thin lines around a hit box, optionally blinking, that the
// dialogs use as buttons (NN_Error's OK box, NN_Prompt's Yes / No / OK).

// RTTI: .?AVRichTextWireButtonControl@@ : WireButtonControl : WireStaticControl : WinControl.
// Every class keeps WinControl's IDataViewOwner face (the branch / the routine).

// WireStaticControl -- vftable 0x005e6084 (45 slots, COL 0x005f1608). Overrides of WinControl:
//   0 the routine (scalar deleting around the routine), 4 the routine (Win3D's: false),
//   5 the branch SetPosition, 7 the branch SetOrientation, 9 the routine Create,
//   11 the routine Update, 13 the branch Draw, 34 the routine ShowToolTip,
//   42 the branch SetProperty.
// WINFACTORY<WireStaticControl> the field "WireStaticControl": static init the branch
// (ctor the routine + atexit the branch), Create the routine (operator_new(0x388)).

// WireButtonControl -- vftable 0x005e614c (46 slots, COL 0x005f1670). Overrides of
// WireStaticControl: 0 the routine (scalar deleting around the routine), 4 the branch
// (JMP the routine, WinControl's HitTest), 11 the branch Update, 15 the routine MouseEnter,
// 16 the routine MouseLeave, 19 the routine MouseEvent, 34 the routine ShowToolTip,
// 42 the branch SetProperty; own slot 45 the routine Activate.
// WINFACTORY<WireButtonControl> the field "WireButtonControl": static init the branch
// (ctor the routine + atexit the branch), Create the routine (operator_new(0x390)).

// RichTextWireButtonControl -- vftable 0x005e622c (COL 0x005f17b4) is WireButtonControl's slot
// for slot: no override of its own, not even the destructor (slot 0 is the routine).
// WINFACTORY<RichTextWireButtonControl> the field "RichTextWireButtonControl": static init
// (ctor the routine + atexit the branch), Create the routine
// (operator_new(0x398)).

// Layout (the routine + the factory Creates):
//   +0x000 WinControl (0x32c)
//   +0x32c orientation[9] (ctor identity; SetOrientation copies the attached result here too)
//   +0x350 wire centre[3]  (ctor 0; Init / SetPosition / Draw: position + offset)
//   +0x35c state bits (ctor 0x60): 0 hovered, 1 focused (0x2c / 0x2d), 2 pressed, 3 always
//          white (Init), 4 lit (0xc), 5 lit while pressed (ctor), 6 pulse while hovered (ctor),
//          7 draw while inactive (0x34)
//   +0x35d bit0 flush both line batchers at the end of Draw (ctor clears bit 0 only)
//   +0x35e blink on (0x51), +0x35f blink phase, +0x360 blink timer
//   +0x364 dword the ToolTip record's +0x44 takes; nothing in the class writes it
//   +0x368 sound name (VC6 std::string, 16 bytes), +0x378 RGBA override (0x33) and +0x37c its
//          switch, +0x380 UI sound slot (the routine, -1 when not a UI sound), +0x384 edge mask
//          (bit 0 top, 1 right, 2 bottom, 3 left; Init: 0xf when zero)
//   size 0x388
// WireButtonControl adds +0x388 button bits (factory: bit 0 cleared, bit 2 set) -- 0 captured,
// 1 right button, 2 enabled -- and +0x38c the KeyMap command posted on a click (0: the parent
// hears 0x26); size 0x390.
// RichTextWireButtonControl adds +0x390 the caption RichTextWin (built by Init) and +0x394 a
// pointer to +0x390; size 0x398.

// PORT: the sound name is the port's std::string, not the VC6 one (AGENTS.md: no baseline ABI),
// so every field from +0x368 on sits at a different offset than baseline. Nothing outside these
// classes reaches those fields by offset; the static_asserts below stop at +0x364.

#include "rich_text_win.h"
#include "wincontrol_base.h"

#include <stddef.h>
#include <stdint.h>
#include <string>

class WireStaticControl : public WinControl
{
public:

    WireStaticControl();

    ~WireStaticControl() override;

    // slot 4 the routine -- Win3D's pick: false
    bool HitTest(float const* Ray) override;
    // slot 5 the branch
    void SetPosition(float const* NewPosition) override;
    // slot 7 the branch
    void SetOrientation(float const* NewOrientation) override;
    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 34 the routine
    void ShowToolTip() override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // (RET 0x18)
    void Init(float const* NewOffset, float const* NewHalfExtents, unsigned char bAlwaysWhite,
              unsigned char bNewNoZEnable, int NewToolTipIds, unsigned NewEdgeMask);

    float WireOrientation[9];      // +0x32c
    float WireCenter[3];           // +0x350
    unsigned char WireState;       // +0x35c
    unsigned char WireState2;      // +0x35d
    unsigned char bBlink;          // +0x35e
    unsigned char bBlinkPhase;     // +0x35f
    float BlinkTimer;              // +0x360
    int ToolTipAux;                // +0x364 -- not written by any WireStaticControl code
    std::string SoundName;         // baseline +0x368 (VC6 std::string)
    unsigned char Colour[4];       // baseline +0x378
    unsigned char bUseColour;      // baseline +0x37c
    int SoundSlot;                 // baseline +0x380
    unsigned EdgeMask;             // baseline +0x384

protected:
    // The blink half of slots 11 (0x005a3aa8..0x005a3afe, again at 0x005a408d..0x005a40e6).
    void UpdateBlink();

private:
    WireStaticControl(WireStaticControl const&) = delete;
    WireStaticControl& operator=(WireStaticControl const&) = delete;
};

static_assert(offsetof(WireStaticControl, WireOrientation) == 0x32c, "WireStaticControl +0x32c");
static_assert(offsetof(WireStaticControl, WireCenter) == 0x350, "WireStaticControl +0x350");
static_assert(offsetof(WireStaticControl, WireState) == 0x35c, "WireStaticControl +0x35c");
static_assert(offsetof(WireStaticControl, BlinkTimer) == 0x360, "WireStaticControl +0x360");
static_assert(offsetof(WireStaticControl, ToolTipAux) == 0x364, "WireStaticControl +0x364");

class WireButtonControl : public WireStaticControl
{
public:
    // The WireButtonControl part of the routine / the routine (inlined there):
    // +0x388 = (+0x388 & 0xfe) | 4, +0x38c = 0, then the vptrs.
    WireButtonControl();

    ~WireButtonControl() override;

    // slot 4 the branch -- JMP the routine
    bool HitTest(float const* Ray) override;
    // slot 11 the branch
    void Update() override;
    // slot 15 the routine (RET 4)
    void MouseEnter(float const* Ray) override;
    // slot 16 the routine (RET 4)
    void MouseLeave(float const* Ray) override;
    // slot 19 the routine (RET 4)
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 34 the routine
    void ShowToolTip() override;
    // slot 42 the branch (RET 0xc) -- 0x2e reads the command id, the rest is WireStatic's
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // slot 45 (+0xb4) the routine -- a left click: post the command, else tell the parent 0x26
    virtual void Activate();

    // (RET 0x20)
    void Init(char const* NewSoundName, float const* NewOffset, float const* NewHalfExtents,
              unsigned char bAlwaysWhite, unsigned char bNewNoZEnable, int NewToolTipIds,
              int NewCommandId, unsigned NewEdgeMask);

    unsigned char ButtonState;     // baseline +0x388
    int CommandId;                 // baseline +0x38c

private:
    WireButtonControl(WireButtonControl const&) = delete;
    WireButtonControl& operator=(WireButtonControl const&) = delete;
};

class RichTextWireButtonControl : public WireButtonControl
{
public:
    // The RichTextWireButtonControl part of the routine (inlined there): +0x394 = &+0x390,
    // +0x390 = 0.
    RichTextWireButtonControl();
    // No destructor of its own: slot 0 is WireButtonControl's the routine. The caption window
    // is a child and goes with the tree.
    ~RichTextWireButtonControl() override;

    // (RET 0x24)
    void Init(char const* NewSoundName, float const* NewOffset, float const* NewHalfExtents,
              unsigned char bAlwaysWhite, unsigned char bNewNoZEnable, int NewToolTipIds,
              unsigned CaptionIds, int NewCommandId, unsigned NewEdgeMask);

    RichTextWin* TextWin;          // baseline +0x390
    RichTextWin** TextWinRef;      // baseline +0x394 -- &TextWin

private:
    RichTextWireButtonControl(RichTextWireButtonControl const&) = delete;
    RichTextWireButtonControl& operator=(RichTextWireButtonControl const&) = delete;
};

// (cdecl Controls, Count) -- join a column of wire controls: recompute each one's
// edge mask (+0x384) from its own state bits (+0x35c), its neighbours' and whether the next one
// is visible (Flags bits 0 and 1), so a hovered or focused row and its lit neighbours share
// edges. Every element must be a wire control; baseline tests none of them for null. Callers:
// NN_Inventory's Draw (0x0047980e / 0x0047981c) on the ItemButton and ItemView columns.
void WireControlsJoinColumn(WinControl* const* Controls, int Count);
