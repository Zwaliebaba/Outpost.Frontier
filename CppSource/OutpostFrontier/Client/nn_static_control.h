// Author: Giperion.

#pragma once

// baseline StaticControl (RTTI .?AUStaticControl@@ : WinControl, StaticControl.cpp) -- the
// NeuroNet / HUD mesh that only draws: frames, gauges, cargo bars, the menu "bImg" plates.

// vftable 0x005e4d7c (45 slots). Overrides of WinControl (slot -> function):
//   0 the routine (scalar deleting around the routine), 5 the branch SetPosition,
//   7 the branch SetOrientation, 9 the branch Create (the INI parser), 11 the branch
//   Update, 13 the branch Draw, 19 the routine MouseEvent, 34 the routine ShowToolTip,
//   42 the branch SetProperty. Slots 15 / 16 stay Win3D's RET 4, slot 4 WinControl's
// ; slot 44 is WinControl's the branch.
// WINFACTORY<StaticControl> the field "StaticControl": static init the branch (ctor
// + atexit the branch -> the routine), Create the routine
// (operator_new(0x420) + the routine, slot 9, scalar delete on failure).

// Layout (ctor the routine after the routine):
//   +0x000 WinControl (0x32c)
//   +0x32c current roll, +0x330 current pitch, +0x334 current yaw (radians; the instance's
//          orientation is the parent orientation turned by these three, SetOrientation)
//   +0x338 "starting_yaw_pitch_roll": yaw, +0x33c pitch, +0x340 roll (Update snaps the current
//          angles back to these while rotation is off)
//   +0x344 "no_mesh_render", +0x345 "no_wire_render", +0x346 "no_coloring", +0x347 property
//          0xc (nothing in this class reads it), +0x348 use the two colours at +0x410 / +0x414
//          (property 0x18), +0x349 "enable_rotation", +0x34a "no_clear_render" (ctor 1; not
//          read by this class), +0x34b "render_to_window", +0x34c "zclear"
//   +0x350 the render-to-window camera (the routine base camera, vftable 0x005c8944, 0xc0)
//   +0x410 mesh colour RGBA, +0x414 wire colour RGBA (property 0x18)
//   +0x418 "force_radius" (the draw scale is force_radius / the instance's radius)
//   +0x41c "rotation_speed" (radians per second added to the yaw while rotation is on)
//   size 0x420

// The port-only menu adapter (the seven-argument Init, the screen rect, ButtonId,
// ContainsScreenPoint) is gone with its last user, menu_ui_widgets' FOverlayNav /
// CreateWinStaticChrome (the flat NN_Preferences shell, 2026-09-26); menu_enter_state's case-18
// chrome uses the baseline 15-argument Init. The flat registry's own StaticControl
// (MenuUiCreateStaticControl[Ex]) and the caption-id adapter ApplyIdsCaption are gone too
// (2026-09-26): their only user was the briefing window, now the branch itself
// (mission_briefing.cpp).

#include "space_camera.h"
#include "wincontrol_base.h"

#include <stddef.h>
#include <stdint.h>
#include <windows.h> // was reached through the removed menu_input.h

class NnStaticControl : public WinControl
{
public:

    NnStaticControl();
    // (slot 0 the routine is its scalar deleting wrapper)
    ~NnStaticControl() override;

    // slot 5 the branch (RET 4)
    void SetPosition(float const* NewPosition) override;
    // slot 7 the branch (RET 4)
    void SetOrientation(float const* NewOrientation) override;
    // slot 9 the branch (RET 8) -- Data is the INI_Reader positioned on this control's section
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;
    // slot 13 the branch
    void Draw() override;
    // slot 19 the routine (RET 4)
    bool MouseEvent(FWin3DMouseEvent const* Event) override;
    // slot 34 the routine
    void ShowToolTip() override;
    // slot 42 the branch (RET 0xc)
    intptr_t SetProperty(int Msg, uintptr_t A, uintptr_t B) override;

    // (RET 0x3c). Callers: WinControl::CreateStaticControl the routine
    // (0x005a0e19) and the branch (0x0043d222, the briefing window, mission_briefing.cpp).
    // ForceRadius / RotationSpeed arrive as ints and are stored as floats (FILD).
    void Init(char const* NewMeshPath, float const* NewOffset, char const* Hardpoint,
              float const* NewHalfExtents, unsigned char bNewNoZEnable, unsigned char bNewZClear,
              unsigned char bNewNoMeshRender, unsigned char bNewNoWireRender,
              unsigned char bNewNoColoring, unsigned char bNewRenderToWindow,
              unsigned char bNewNoClearRender, int NewForceRadius, int NewRotationSpeed,
              unsigned char bNewEnableRotation, float const* NewStartYpr);

    float CurRoll;                 // +0x32c
    float CurPitch;                // +0x330
    float CurYaw;                  // +0x334
    float StartYpr[3];             // +0x338 yaw, +0x33c pitch, +0x340 roll
    unsigned char bNoMeshRender;   // +0x344
    unsigned char bNoWireRender;   // +0x345
    unsigned char bNoColoring;     // +0x346
    unsigned char Field_0x347;     // +0x347 -- property 0xc; TODO: meaning (no reader found)
    unsigned char bUseColours;     // +0x348
    unsigned char bEnableRotation; // +0x349
    unsigned char bNoClearRender;  // +0x34a
    unsigned char bRenderToWindow; // +0x34b
    unsigned char bZClear;         // +0x34c
    unsigned char Pad34d[3];       // +0x34d
    FSpaceCamera Camera;           // +0x350
    unsigned char MeshColour[4];   // +0x410
    unsigned char WireColour[4];   // +0x414
    float ForceRadius;             // +0x418
    float RotationSpeed;           // +0x41c

private:
    NnStaticControl(NnStaticControl const&) = delete;
    NnStaticControl& operator=(NnStaticControl const&) = delete;
};

static_assert(offsetof(NnStaticControl, CurRoll) == 0x32c, "StaticControl +0x32c");
static_assert(offsetof(NnStaticControl, StartYpr) == 0x338, "StaticControl +0x338");
static_assert(offsetof(NnStaticControl, bNoMeshRender) == 0x344, "StaticControl +0x344");
static_assert(offsetof(NnStaticControl, bZClear) == 0x34c, "StaticControl +0x34c");
static_assert(offsetof(NnStaticControl, Camera) == 0x350, "StaticControl +0x350");
static_assert(offsetof(NnStaticControl, MeshColour) == 0x410, "StaticControl +0x410");
static_assert(offsetof(NnStaticControl, ForceRadius) == 0x418, "StaticControl +0x418");
static_assert(offsetof(NnStaticControl, RotationSpeed) == 0x41c, "StaticControl +0x41c");
