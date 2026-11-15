// Author: Giperion.

#pragma once

// baseline WinButton (RTTI .?AUWinButton@@ : WinObject, vftable 0x005d7024, 0xf4 bytes) and
// HUD_Button (RTTI .?AUHUD_Button@@ : WinButton, HUDItem @ +0xf4; vftables 0x005d6fac and
// , 0x154 bytes).

// WinButton is a WinObject whose mesh spins about its Y axis while the cursor is over it and
// eases back to rest afterwards; its hit test is the mesh's projected screen rectangle. It has
// no constructor or destructor of its own in the PE: HUD_Button's factory the routine inlines
// the constructor (0x004c8d84 .. 0x004c8dc4), and slot 0 is WinObject's the routine.

// WinButton layout (after WinObject's 0xac):
//   +0xac  FTransform rest pose (the routine: identity rotation, zero translation). Slot 7
//          snapshots the live orientation + position into it, slot 5 rebuilds the live
//          transform from it; +0xd0 is its translation
//   +0xdc  bHover -- slot 14 stores the hit test, slot 16 clears it
//   +0xe0  spin angle in degrees
//   +0xe4  spin speed, 180.0f degrees per second
//   +0xe8  0.85f, +0xec 0.75f -- written by the ctor, read nowhere in the PE
//   +0xf0  byte, cleared by the ctor, read nowhere in the PE

// WinButton overrides of WinObject's vftable:
//   4 the routine HitTest, 5 the branch SetPosition, 7 the branch SetOrientation,
//   11 the routine Update, 13 the routine Draw, 14 the routine MouseOver,
//   16 the branch MouseLeave.
// HUD_Button overrides on top: 0 the routine (dtor the routine), 11 the branch Update,
// 13 the branch Draw, 19 the branch MouseEvent. Its HUDItem face overrides only slot 0
// (the branch, the this-0xf4 thunk).

#include "hud_element_list.h"
#include "win_object.h"
#include "x86math.h"

#include <stddef.h>

struct WinButton : public WinObject
{
    // (inlined into the routine)
    WinButton();

    // slot 4 the routine -- the cursor (the field / the field) inside the mesh's
    // projected screen rectangle, edges included. The ray is not read.
    bool HitTest(float const* Ray) override;
    // slot 5 the branch -- position into +0x60 and the rest pose, then the live transform
    // rebuilt from the rest pose
    void SetPosition(float const* NewPosition) override;
    // slot 7 the branch -- orientation into +0x3c, then the live transform becomes the rest
    // pose
    void SetOrientation(float const* NewOrientation) override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine
    void Draw() override;
    // slot 14 the routine -- bHover = slot 4 (Ray)
    void MouseOver(float const* Ray) override;
    // slot 16 the branch -- bHover = 0
    void MouseLeave(float const* Ray) override;

    // (thiscall, RET 4) -- the twelve dwords into +0x3c, then the holder's
    // instance matrix (the routine) and the routine. The rest pose is left alone.
    void SetTransform(FTransform const* NewTransform);

    FTransform RestPose;         // +0xac
    unsigned char bHover;        // +0xdc
    unsigned char PadDD[3];
    float SpinAngle;             // +0xe0
    float SpinSpeed;             // +0xe4
    float FieldE8;               // +0xe8
    float FieldEC;               // +0xec
    unsigned char ByteF0;        // +0xf0
    unsigned char PadF1[3];
};

static_assert(sizeof(WinButton) == 0xf4, "baseline WinButton is 0xf4 bytes");
static_assert(offsetof(WinButton, RestPose) == 0xac, "WinButton +0xac rest pose");
static_assert(offsetof(WinButton, bHover) == 0xdc, "WinButton +0xdc hover");
static_assert(offsetof(WinButton, SpinSpeed) == 0xe4, "WinButton +0xe4 spin speed");

// (cdecl Holder, Out) -- the instance's projected screen rectangle as four floats
// (left, top, right, bottom): IEngine::GetInstanceExtent (+0x88) gives the bounding sphere
// centre and radius, GetInstancePosition (+0x90) moves the centre, and
// get_projected_bounding_box through the Win3D camera (the field) projects it. False without
// an instance or when the projection fails.
bool WinButtonProjectedRect(FClientMeshHolder* Holder, float* OutRect4);

struct HUD_Button : public WinButton, public HUDItem
{
    // after operator new(0x154) and WinObject's ctor
    explicit HUD_Button(INI_Reader& Reader);

    ~HUD_Button() override;

    // slot 11 the branch
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 19 the branch
    bool MouseEvent(FWin3DMouseEvent const* Event) override;

    TString<32> Action;          // +0x12c "action"
    int CommandId;               // +0x150 the command the action maps to
};

static_assert(offsetof(HUD_Button, Action) == 0x12c, "HUD_Button +0x12c action");
static_assert(offsetof(HUD_Button, CommandId) == 0x150, "HUD_Button +0x150 command");
static_assert(sizeof(HUD_Button) == 0x154, "baseline HUD_Button is 0x154 bytes");

// / the routine -- UHUD_Button::HUDFACTORY, the static object the field built
// by the XC entry the branch ("Button") and torn down by the branch.
class HUD_ButtonFactory : public HUDItem_Factory
{
public:
    explicit HUD_ButtonFactory(char const* SectionName) : HUDItem_Factory(SectionName) {}

    HUDItem* Create(INI_Reader& Reader) override;
};
