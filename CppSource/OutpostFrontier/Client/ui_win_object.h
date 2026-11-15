// Author: Giperion.

#pragma once

// UIWinObject — PAGE_*.txt mesh widget (RTTI .?AUUIWinObject@@ : WinObject : Win3D).

// baseline:
//   WINFACTORY<UIWinObject> the field, static init the branch → the routine
//   Create the routine: operator_new(0x100), Win3D ctor the routine, WinObject part
//   (mesh holder +0x7c the routine / +0x90 the routine, holder vftable 0x005d2efc,
//   holder+0x10 = -1, colour +0xa5, byte +0xa4 &= 0xf8), then +0xb8 the routine identity
//   3x3 + zero translation, vftable 0x005e5d34.
//   vftable 0x005e5d34 overrides of WinObject 0x005d709c:
//     slot 0  the routine  scalar deleting destructor (dtor the routine = WinObject's)
//     slot 5  the branch  SetPosition
//     slot 8  the branch  SetTransform
//     slot 9  the routine  Create
//     slot 13 0x0059ab50    draw: debug frame (dead — the field has no writer), then
//                           WinObject draw the routine

// The WinObject part (mesh holder, +0xa4 draw flags, +0xa5 tint, slots 10 / 11 / 13) is the
// WinObject base (win_object.h); the debug frame of slot 13 is dead, so Draw is WinObject's.

#include "win_object.h"

#include <stddef.h>

class UiWinObject : public WinObject
{
public:
    UiWinObject();
    ~UiWinObject() override;

    // slot 5 (+0x14) — the branch.
    void SetPosition(float const* NewPosition) override;
    // slot 8 (+0x20) — the branch.
    void SetTransform(float const* Transform) override;
    // slot 9 (+0x24) — the routine: false when Payload is null or the mesh load fails.
    bool Create(Win3D* NewParent, void* Payload) override;

    FClientMeshHolder const& Mesh() const { return MeshHolder; }

private:
    float Width_;                           // +0xac — payload +0x00
    float Height_;                          // +0xb0 — payload +0x04
    float Depth_;                           // +0xb4 — payload +0x0c (5th float)
    float PageBlock_[12];                   // +0xb8 — slot 8 argument copy (ctor: identity)
    float Origin_[3];                       // +0xe8 — slot 8: Block * (0, 0, 0)
    float Corner_[3];                       // +0xf4 — slot 8: Block * (Width, -Height, 0)
};

static_assert(sizeof(UiWinObject) == 0x100, "baseline UIWinObject is 0x100 bytes");
