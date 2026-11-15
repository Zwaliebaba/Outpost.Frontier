// Author: Giperion.

#pragma once

// baseline CargoBarControl (RTTI .?AUCargoBarControl@@ : WinControl) -- the hold bar of the
// NeuroNet inventory and dealer windows and the HUD cargo gauge: a quad strip filled to
// Used / Capacity, with an optional highlighted head (+0x33c) and tail (+0x338) segment, a
// line-loop border and a "used/capacity" tooltip.

// WINFACTORY<CargoBarControl> "CargoBarControl": ctor the routine (called from the static
// initializer at 0x0055e90a), Create the routine (operator_new(0x350), the routine, slot 9).
// Built directly by WinControl::CreateCargoBarControl (the routine -> Init the routine).

// vftable 0x005e1924 (45 slots) -- WinControl's with these overrides:
//    0 the routine scalar deleting dtor (WinControl's the routine only)
//    4 the branch HitTest          5 the branch SetPosition     6 the branch (Win3D's)
//    7 the branch SetOrientation   9 the branch Create         11 the branch Update
//   13 the branch Draw            15 the routine MouseEnter     16 the routine MouseLeave
//   19 the routine (WinControl's)  27 the branch GetHitPriority (0x64)
//   35 the routine RefreshAttachment (RET -- no hardpoint re-attach)
//   40 the branch GetAttachOrientation   41 the routine GetAttachPosition
//   42 the branch SetProperty

// Layout (ctor the routine, size 0x350):
//   +0x32c "tooltip2" IDS -- the caption prefix of the tooltip ("%s %d/%d")
//   +0x330 capacity, +0x334 used, +0x338 tail segment, +0x33c head segment (floats)
//   +0x340 fill colour RGBA bytes ("color", ctor 0,0,0,0)
//   +0x344 segment colour RGBA bytes ("color_highlight", ctor 0xff,0xff,0,0xff)
//   +0x348 bHorizontal, +0x349 bHovered, +0x34a bToolTip, +0x34b (properties 0x2c / 0x2d),
//   +0x34c bBorder (ctor 1), +0x34d bShow (ctor 1)

#include "wincontrol_base.h"

#include <stddef.h>
#include <stdint.h>

class CargoBarControl : public WinControl
{
public:

    CargoBarControl();

    // (RET 0x20) -- the argument list of the routine after the name.
    void Init(char const* MeshPath, float const* NewOffset, char const* Hardpoint,
              float const* NewHalfExtents, unsigned char bNewHorizontal, int NewToolTip2Ids,
              unsigned NewColor, unsigned NewHighlightColor);

    // --- Win3D / WinControl overrides ---
    bool HitTest(float const* Ray) override;                       // slot 4  the branch
    void SetPosition(float const* NewPosition) override;           // slot 5  the branch
    void SetOrientation(float const* NewOrientation) override;     // slot 7  the branch
    bool Create(Win3D* NewParent, void* Data) override;            // slot 9  the branch
    void Update() override;                                        // slot 11 the branch
    void Draw() override;                                          // slot 13 the branch
    void MouseEnter(float const* Ray) override;                    // slot 15 the routine
    void MouseLeave(float const* Ray) override;                    // slot 16 the routine
    int GetHitPriority() override;                                 // slot 27 the branch
    void RefreshAttachment() override;                             // slot 35 the routine
    void GetAttachOrientation(float* OutOrientation9) override;    // slot 40 the branch
    void GetAttachPosition(float* OutPosition3) override;          // slot 41 the routine
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override; // slot 42 the branch

    int ToolTip2Ids;                  // +0x32c
    float Capacity;                   // +0x330
    float Used;                       // +0x334
    float TailSegment;                // +0x338
    float HeadSegment;                // +0x33c
    unsigned char Color[4];           // +0x340
    unsigned char HighlightColor[4];  // +0x344
    unsigned char bHorizontal;        // +0x348
    unsigned char bHovered;           // +0x349
    unsigned char bToolTip;           // +0x34a
    unsigned char Field34b;           // +0x34b
    unsigned char bBorder;            // +0x34c
    unsigned char bShow;              // +0x34d
    unsigned char Pad34e[2];          // +0x34e

private:
    CargoBarControl(CargoBarControl const&) = delete;
    CargoBarControl& operator=(CargoBarControl const&) = delete;
};

static_assert(offsetof(CargoBarControl, ToolTip2Ids) == 0x32c, "CargoBarControl +0x32c");
static_assert(offsetof(CargoBarControl, Color) == 0x340, "CargoBarControl +0x340");
static_assert(offsetof(CargoBarControl, bShow) == 0x34d, "CargoBarControl +0x34d");
static_assert(sizeof(CargoBarControl) == 0x350, "baseline CargoBarControl is 0x350 bytes");

// The tooltip hover record (tooltip_hover.h) as baseline inlines it at each hover site: the
// per-site start values after construction and the caption copy (the routine + wcsncpy). The
// construction and the teardown (the routine) are the record's own ctor / dtor. Defined in
// cargo_bar_control.cpp; NN_Inventory's SetProperty 0x4f uses them too.
struct FToolTipHoverInfo;
void ToolTipHoverInit(FToolTipHoverInfo* Info);
void ToolTipHoverSetCaption(FToolTipHoverInfo* Info, wchar_t const* Text);
