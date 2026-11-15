// Author: Giperion.

#pragma once

// baseline ToolTip (RTTI .?AUToolTip@@ : TextWin, vftable 0x005d5f7c, 29 slots, size 0x16c),
// built by WINFACTORY<ToolTip>::Create the routine (the inlined TextWin part, the +0xf4 text
// entry, then the routine) and kept in the singleton the field. Overrides against TextWin:
// 0 the routine (dtor the routine), 4 the routine (never hit), 9 the routine Create,
// 13 the branch Draw. SetText (slot 28) is TextWin's the branch.
// Feed path: ButtonControl vt+0x88 the routine / WireButton the routine
//   → the routine → the routine → the routine.
// Draw the branch + reposition the routine use Font::String at +0x7c.

#include "ids_strings.h"
#include "text_win.h"
#include "font_string.h"
#include "tooltip_hover.h"
#include "win_factory.h"

#include <stddef.h>

// — set by ToolTip factory (the branch / the routine path).
class NnToolTipControl : public TextWin
{
public:
    NnToolTipControl();
    ~NnToolTipControl() override;

    // — style + default scalars.
    void InitToolTipDefaults();

    // slot 4 the routine -- never picked.
    bool HitTest(float const* Ray) override;

    // slot 9 the routine -- TextWin's setup, hidden, sub-layer 7.
    bool Create(Win3D* NewParent, void* Data) override;

    // — thiscall; hover info on stack.
    void UpdateFromHover(FToolTipHoverInfo const* Info);

    // — copy hover into live fields + resolve caption + KeyMap append.
    void ApplyHover(FToolTipHoverInfo const* Info);

    // — per-frame delay timers; param is delta seconds.
    void TickToolTip(double DeltaSeconds);

    // — clear sticky-visible + hover flag.
    void HideNow();

    // — gates + background quads + FontStringDrawChildren.
    void Draw() override;

    // The body of the routine: the live hover's ids (+0xf4 / +0xf8), position (+0x13c),
    // half extents (+0x148) and live byte (+0x154) into Out; nothing else of Out is written.
    void ReadHover(FToolTipHoverInfo* Out) const;

    int HoverIds() const { return Hover_.Ids; }
    unsigned char bHasText() const { return bHasText_; }
    unsigned char bFedThisFrame() const { return bFedThisFrame_; }

private:
    // (0x004bda70..0x004bdd1d): re-anchors the tooltip under the cursor ray cached
    // by the Win3D input handler (Win3DGetMouseRay, the routine) and clamps it to the viewport.
    void RepositionAfterCaption(FToolTipHoverInfo const* Info);

    // +0xf4..+0x158: an embedded hover record, built by the routine (0x004be4cc..0x004be507)
    // and torn down by the routine (0x004bd6ca..0x004bd747). the routine copies only Ids,
    // IdsAlt, WorldPos, HalfExtents and bLive into it; its caption (+0xfc), Font::String
    // (+0x104) and AuxField (+0x138) are never written after construction.
    FToolTipHoverInfo Hover_;
    unsigned char bBuilt_;       // +0x158  the routine: the +0x6c bit 1 of the last tick
    // +0x159: set to 1 only by the routine while a mouse button is down (the field & 3,
    // ); the routine then hides a repeated hover and clears it on a new one.
    unsigned char bHasText_;
    unsigned char bFedThisFrame_; // +0x164
    float DelayRemain_;          // +0x15c
    float DelayAccum_;           // +0x160
    float DelayThreshold_;       // +0x168 = 0.25f
    float OffsetX_;              // +0xec = 4.0f
    float OffsetY_;              // +0xf0 = -42.0f
};

// — guarded TickToolTip on singleton. baseline forwards the double dt of
// as two dwords (0059e0d6..0059e0d8).
void NnToolTipTickGlobal(double DeltaSeconds);

// .
void NnToolTipHideGlobal();

// the singleton's +0x164 byte (set by the routine when a hover is fed, cleared
// by the routine), false without a singleton. Callers: 0x00494d06, HUD_Targetable_Objects
// Render 0x004ec95b.
bool NnToolTipWasFedThisFrame();

// (cdecl) -- with a singleton, its live hover into *Out (NnToolTipControl::
// ReadHover) and true; false (Out untouched) without one. Callers: NeuroNetNavMap::Update
// (0x00494f48, 0x00495131, 0x004954cc, 0x00495729).
bool NnToolTipReadHover(FToolTipHoverInfo* Out);

NnToolTipControl* GetNnToolTipSingleton();
void SetNnToolTipSingleton(NnToolTipControl* Tip);

// WINFACTORY<ToolTip> the field (vftable 0x005d6010), built by the static initialiser
// through the routine("ToolTip") and torn down by the branch (the routine).
// ("ToolTip", ...) reaches Create by name.
class ToolTipFactory : public WinFactory
{
public:
    explicit ToolTipFactory(char const* Name)
        : WinFactory(Name)
    {
    }

    // also installs the field.
    Win3D* Create(Win3D* Parent, void* Payload) override;
};
