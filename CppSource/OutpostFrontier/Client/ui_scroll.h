// Author: Giperion.

#pragma once

// UIScroll / UICycle — PAGE_*.txt and RichTextWin scrollbars (RTTI .?AUUIScroll@@ : WinControl,
// .?AUUICycle@@ : UIScroll; IDataViewOwner face at +0x7c).

// baseline:
//   WINFACTORY<UIScroll> the field, static init the branch → the routine("UIScroll"),
//     Create the routine: operator_new(0x430), ctor the routine, then slot 9.
//   WINFACTORY<UICycle>  the field, static init the branch → the routine("UICycle"),
//     Create the routine: operator_new(0x430), ctor the routine, then the UICycle vftables
// / 0x005e5674 (+0x7c). UICycle adds no field.
//   vftable 0x005e55ac overrides of WinControl 0x005e5e44:
//     slot 0  the routine  scalar deleting destructor (dtor the routine)
//     slot 4  the branch  hit test: mouse ray → local, inside (0..W, 0..-H) → parent
//                           Resolve()->vt+0xa8(0x25, this->Resolve(0))
//     slot 5  the branch  SetPosition (+ offset +0xa8 + scroll offset +0x41c, inverse)
//     slot 9  the routine  OnCreateFromDesc
//     slot 11 the branch  update: WinControl the routine, then the hover flags of the hit
//                           rects from the cached mouse ray (the routine)
//     slot 13 the branch  draw (UICycle the branch)
//     slot 14 the branch  thumb drag → value, parent vt+0x5c(0xf005, this, 0) and
//                           parent Resolve()->vt+0xa8(0x3e, this, ftol(value))
//     slot 16 the branch  +0x3b8 = -1
//     slot 19 the branch  input (UICycle the branch)
//     slot 42 the branch  commands 6 (scroll offset), 0x37 (range), 0x38 (value),
//                           0x3b (get value), else WinControl the routine

// Every override above is here, with the object layout, construction and the non-virtual
// members other modules call (the routine .. the routine, the routine), plus the two arrow
// helpers of the draws (the routine vertical, the routine horizontal).

// Parent notifications (slot 14, slot 19, the draw's auto-repeat): the parent's Win3D slot 23
// with (0xf005, this, 0), then the parent's WinControl face (slot 2) SetProperty(0x3e, this,
// ftol(value)). Slot 4 sends SetProperty(0x25, this, 0), slot 19 SetProperty(0x29, this, 0).

#include "wincontrol_base.h"

#include "x86math.h"

#include <vector>

struct FTexShape;

// One scrollbar part in local coordinates (0x14 bytes): X0/Y0 top-left, X1/Y1 bottom-right
// (Y grows downwards as negative values), bHot = the mouse is over it (slot 11).
struct FScrollHitRect
{
    float X0;          // +0x00
    float Y0;          // +0x04
    float X1;          // +0x08
    float Y1;          // +0x0c
    bool bHot;         // +0x10
};

class UiScroll : public WinControl
{
public:
    // .
    UiScroll();
    // .
    ~UiScroll() override;

    // slot 4 (+0x10) — the branch (RET 4).
    bool HitTest(float const* Ray) override;
    // slot 5 (+0x14) — the branch (RET 4).
    void SetPosition(float const* NewPosition) override;

    // slot 9 (+0x24) — the routine (RET 8); args (Parent, FUiPayloadScrollCycle*).
    bool Create(Win3D* NewParent, void* Payload) override;

    // slot 11 (+0x2c) — the branch.
    void Update() override;
    // slot 13 (+0x34) — the branch.
    void Draw() override;
    // slot 14 (+0x38) — the branch (RET 4): thumb drag.
    void MouseOver(float const* Ray) override;
    // slot 16 (+0x40) — the branch (RET 4).
    void MouseLeave(float const* Ray) override;
    // slot 19 (+0x4c) — the branch (RET 4).
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 42 (+0xa8) — the branch (RET 0xc).
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // — rebuild the five part rects from the size, range and value.
    void Relayout();
    // — clamp min <= max, page, value into range.
    void ClampValues();
    // (RET 8) — +0x3c0 = Min, +0x3c4 = Max, clamp, relayout.
    void SetRange(float Min, float Max);
    // (RET 8).
    void GetRange(float* OutMin, float* OutMax) const;
    // (RET 4) — +0x3c8 = Value, clamp, relayout; returns 1.
    bool SetValue(float Value);
    // (RET 4) — +0x3cc = +0x428 = Page, clamp, relayout; returns 1.
    bool SetPage(float Page);
    // (RET 4) — +0x3d0 = Step, clamp, relayout; returns 1.
    bool SetStep(float Step);
    // (RET 8) — +0x334 / +0x338, arrow size = min * 0.2, relayout.
    void SetSize(float Width, float Height);
    // (RET 8) — +0x40c / +0x410.
    void SetArrowColors(unsigned long const* Idle, unsigned long const* Hot);
    // (RET 8) — +0x414 / +0x418.
    void SetBarColors(unsigned long const* Idle, unsigned long const* Hot);
    // (RET 4) — move by the change of the scroll offset (+0x41c).
    void SetScrollOffset(float const Offset[3]);

    // +0x3b4 — baseline callers write the byte directly (0047934f, 004aa451, 0055d452, ...).
    void SetStepMode(bool bStepMode) { bStepMode_ = bStepMode; }
    // +0x3cc alone -- NN_Inventory's slot 45 writes the float directly (0x00479356), without
    // 's +0x428 copy, clamp and relayout.
    void SetPageField(float Page) { Page_ = Page; }
    // +0x32c / +0x32d -- NN_Preferences::RefreshRows writes both bytes directly
    // (0x004b0c42..0x004b0c4c greyed: 0 / 1; 0x004b0cee..0x004b0cf8 live: 1 / 0).
    void SetActiveField(bool bActive) { bActive_ = bActive; }
    void SetCustomColorField(bool bCustomColor) { bCustomColor_ = bCustomColor; }

    // — FLD +0x3c8.
    float GetValue() const { return Value_; }
    // (RET 4) — +0x42c = the byte argument; no relayout.
    void SetHorizontal(bool bHorizontal) { bHorizontal_ = bHorizontal; }

protected:
    // (RET 0xc) — vertical arrow triangle in Rect, pointing up when bUp; drawn only
    // when bDraw. Always leaves the current colour at +0x414 (hot) or +0x40c.
    void DrawArrowVertical(FScrollHitRect const* Rect, bool bDraw, bool bUp);
    // (RET 0xc) — the same for a horizontal scrollbar, pointing left when bLeft.
    void DrawArrowHorizontal(FScrollHitRect const* Rect, bool bDraw, bool bLeft);
    // The model-view bind and the seven render states both draws open with (0x00596450 /
    // ).
    void BeginDraw();
    // The parent notifications every value change ends with (after ClampValues and Relayout):
    // parent slot 23 (0xf005, this, 0), then the parent's WinControl SetProperty(0x3e, this,
    // ftol(value)).
    void NotifyParent();
    // The mouse ray on the scrollbar plane in local coordinates (+0x3dc inverse), through the
    // TU's math engine the field.
    FVector RayToLocal(float const* Ray) const;
    // The same, but the branch transforms through the routine (the field).
    FVector RayToLocalViaHud(float const* Ray) const;

    bool bActive_;                   // +0x32c — ctor 1; gates the draw hover tint and slot 14
    bool bCustomColor_;                // +0x32d — ctor 0; draw uses +0x32e for every part
    unsigned char CustomColor_[4];     // +0x32e — ctor the field (80 80 80 ff)
    float Width_;                      // +0x334 — payload +0x00
    float Height_;                     // +0x338 — payload +0x04
    FScrollHitRect TopArrow_;          // +0x33c — up (left) arrow
    FScrollHitRect BottomArrow_;       // +0x350 — down (right) arrow
    FScrollHitRect TrackBefore_;       // +0x364 — track before the thumb
    FScrollHitRect TrackAfter_;        // +0x378 — track after the thumb
    FScrollHitRect Thumb_;             // +0x38c
    // +0x3a0 allocator byte, +0x3a4..+0x3ac vector — {&TopArrow_, &BottomArrow_, &Thumb_}.
    std::vector<FScrollHitRect*> HitRects_;
    float ArrowSize_;                  // +0x3b0 — min(Width, Height) * 0.2
    bool bStepMode_;                   // +0x3b4 — ctor 0; set by owners (see SetStepMode)
    int DragPart_;                     // +0x3b8 — ctor -1, slot 16 -1; 0 = up arrow held,
                                       //          1 = down arrow held (auto-repeat in the draw)
    float RepeatTimer_;                // +0x3bc — ctor 0; slot 19 sets 0.3f, the draw counts it
                                       //          down by the frame time and repeats below 0
    float RangeMin_;                   // +0x3c0 — payload +0x08
    float RangeMax_;                   // +0x3c4 — payload +0x0c
    float Value_;                      // +0x3c8
    float Page_;                       // +0x3cc — payload +0x10
    float Step_;                       // +0x3d0 — payload +0x14
    bool bDragging_;                   // +0x3d4 — ctor 0; slots 11/14/19
    float DragOffset_;                 // +0x3d8 — ctor 0; slot 14
    float InverseLayout_[12];          // +0x3dc — the routine identity, zero translation;
                                       //          slot 5 GeneralInverse of +0x3c
    unsigned long ArrowIdleColor_;     // +0x40c — the field
    unsigned long ArrowHotColor_;      // +0x410 — the field
    unsigned long BarIdleColor_;       // +0x414 — the field
    unsigned long BarHotColor_;        // +0x418 — the field
    float ScrollOffset_[3];            // +0x41c
    float DefaultPage_;                // +0x428 — ctor +0x3cc (0), payload +0x10, SetPage
    bool bHorizontal_;                 // +0x42c — payload +0x18
};

// UICycle — the same object with its own draw (slot 13 the branch) and input (slot 19
// ): only the two arrows, stepping by +0x3d0. vftable 0x005e5684; every other slot
// is UIScroll's. No field of its own.
class UiCycle : public UiScroll
{
public:
    // slot 13 (+0x34) — the branch.
    void Draw() override;
    // slot 19 (+0x4c) — the branch (RET 4).
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
};

// clear the UI ShapeDB (the field) if UITextures.ini was loaded.
void UiScrollClearShapes();
