// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// HUD_DamageIndicator / [DamageIndicator] -- Client\Space\HUD_DamageIndicator.cpp. Four copies
// of interface\hud\HUD_damagebottom.3db, turned 180 / 0 / -90 / 90 degrees about Z, that flash
// on the side of the screen a hit came from and fade over 0.7 s.

// RTTI HUD_DamageIndicator : HUDItem @0, WinControl @0x38 (IDataViewOwner @0xb4). Size 0x414.
// vftables:
//   HUDItem face 0x005d8104 (11 slots) -- only slot 0 overridden (the routine, scalar deleting
//     dtor around the routine).
//   WinControl face 0x005d804c (45 slots) -- overrides 0 the branch (thunk, ECX -= 0x38),
//     9 the branch Create (the same body sits in slot 9 of vftable 0x005d96cc), 11
// Update, 12 the routine UpdateInactive, 13 the branch Draw; every other
//     slot is WinControl's 0x005e5e44 entry.

// Factory: the static HUDFACTORY<HUD_DamageIndicator> the field ("DamageIndicator"; XC
// entry the branch, ctor the routine, atexit the branch -> the routine), vftable
// whose Create the routine is operator new(0x414) + the routine.

// is the one instance (written by the ctor at 0x004d63c9, cleared by the dtor at
// ). The client ship's hit handlers read it and, when it is set, call:
// client object slot 104 the routine (0x00545ec3), its first argument
// client object slot 105 the routine (0x00545f33)
// the explosion spawn the routine (0x005363f2)
// All three callers read it through GetHudDamageIndicator(): client_gameobj.cpp (slots 104 /
// 105) and client_explosion.cpp (the routine).
// -------------------------------------------------------------------------------------------

#include "client_mesh_load.h"
#include "hud_element_list.h"
#include "wincontrol_base.h"

class INI_Reader;
class Vector;

// One arrow (0x2c bytes): the HUD mesh holder and its flash timer.
// (thiscall, RET 8) builds it; the routine (the unwind copy) and the inlined
// blocks of the routine destroy it.
struct FHudDamageIndicatorPart
{
    // the holder (the routine, five 1.0f, anim -1), Flash = 0, the mesh
    // (the routine(Path, 0, 1)), an FDUMP when no instance came of it, then the instance's
    // matrix: a rotation of Degrees about Z at (0, 0, -1) (IEngine +0x9c, +0x6c).
    FHudDamageIndicatorPart(char const* Path, float Degrees);
    // the routine when an instance is bound, then the holder dtor the routine.
    ~FHudDamageIndicatorPart();

    FClientMeshHolder Holder;  // +0x00
    float Flash;               // +0x28 -- 0..1; +0.7 (or +0.35) per hit, -dt per frame

private:
    FHudDamageIndicatorPart(FHudDamageIndicatorPart const&) = delete;
    FHudDamageIndicatorPart& operator=(FHudDamageIndicatorPart const&) = delete;
};

class HUD_DamageIndicator : public HUDItem, public WinControl
{
public:
    // (thiscall, RET 4) -- the reader is not read.
    explicit HUD_DamageIndicator(INI_Reader& Reader);
    // (scalar deleting the routine; WinControl-face thunk the branch)
    ~HUD_DamageIndicator() override;

    // WinControl slot 9 the branch (RET 8) -- SetOffset(0, 0, -1), SetPosition(Position),
    // Win3D::Create.
    bool Create(Win3D* NewParent, void* Data) override;
    // WinControl slot 11 the routine -- show while the cursor is drawn and there is a player,
    // fade the four timers by the frame time, Win3D's child walk.
    void Update() override;
    // WinControl slot 12 the routine -- the show half, the fade, the inactive child walk.
    void UpdateInactive() override;
    // WinControl slot 13 the branch -- draw each arrow whose timer is above zero.
    void Draw() override;

    // (thiscall, RET 4) -- a hit whose source is WorldPos: into the player ship's
    // frame, turned by pi/8 about Z, and +0.7 on the quadrant's arrow.
    void FlashFromPosition(Vector const* WorldPos);
    // (thiscall, RET 8) -- FlashTowards(Origin + Direction * -1000).
    void FlashAlongRay(Vector const* Origin, Vector const* Direction);
    // (thiscall, RET 8) -- FlashTowards(Point); the second argument is not read.
    void FlashTowardsPoint(Vector const* Point, int Unused);
    // (thiscall, RET 4) -- Point in the frame of the ship's physical centre pushed
    // back by its radius: a hit from straight ahead (|z| above |x| and |y|, z >= 0) adds 0.35 to
    // all four arrows first; then +0.7 on the quadrant's arrow as in FlashFromPosition.
    void FlashTowards(Vector const* Point);

    FHudDamageIndicatorPart PartDeg180;     // +0x364 (WinControl +0x32c)
    FHudDamageIndicatorPart PartDeg0;       // +0x390 (+0x358)
    FHudDamageIndicatorPart PartDegMinus90; // +0x3bc (+0x384)
    FHudDamageIndicatorPart PartDeg90;      // +0x3e8 (+0x3b0)

private:
    HUD_DamageIndicator(HUD_DamageIndicator const&) = delete;
    HUD_DamageIndicator& operator=(HUD_DamageIndicator const&) = delete;
};

// the live HUD_DamageIndicator, or null.
HUD_DamageIndicator* GetHudDamageIndicator();
