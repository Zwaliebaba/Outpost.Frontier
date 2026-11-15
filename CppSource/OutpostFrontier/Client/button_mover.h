// Author: Giperion.

#pragma once

// baseline ButtonMover / ButtonScaler (RTTI .?AVButtonMover@@ / .?AVButtonScaler@@) -- two small
// timed animators a UI class inherits to slide (ButtonMover) or scale (ButtonScaler) a set of
// WinControls over a fixed duration. Shared, not HUD-specific: NN_MainButton (0x0045b744 /
// ), the object at 0x0043d715 / 0x0043d725, NavBar (+0x32c / +0x34c, wave 4),
// HUD_Target_Base (ButtonMover at +0x364) and HUD_Target / HUD_Status (ButtonScaler at +0x4e0)
// all embed them as base-class subobjects.

// baseline layout of both (ctor the routine / the routine, 0x20 bytes):
//   +0x00 vftable (ButtonMover 0x005e18f8, ButtonScaler 0x005e1904), two slots each:
//         slot 0 the branch (RET)      -- OnFinished: called once the animation completes
//         slot 1 the branch (RET 8)    -- OnStep(Control, Value): true = the derived class
//                                           applied the value itself, skip the default
//   +0x04 std::vector (VC6: allocator byte, first, last, end) of entries
//         ButtonMover:  0x1c bytes {Start[3], End[3], WinControl*}
//         ButtonScaler: 0x0c bytes {From, To, WinControl*}
//   +0x14 float elapsed, +0x18 bool active, +0x1c float duration
// Neither class has a destructor of its own (no virtual dtor slot); the derived destructors free
// the vector inline. The port holds the entries in a std::vector (no baseline ABI, AGENTS.md), so
// the member layout is not byte-exact.

// The two classes' slots carry distinct names (OnMove* / OnScale*) because HUD_Target and
// HUD_Status inherit BOTH and override each face with a different body (HUD_Target: mover
// slot 0 the routine, scaler slot 0 the routine); one shared C++ name would force a single
// override for both faces.

#include <vector>

class WinControl;

// baseline 0x1c-byte ButtonMover entry (the routine builds it on the stack in this order).
struct FButtonMoveEntry
{
    float Start[3];      // +0x00
    float End[3];        // +0x0c
    WinControl* Control; // +0x18
};

// baseline 0x0c-byte ButtonScaler entry (the routine).
struct FButtonScaleEntry
{
    float From;          // +0x00
    float To;            // +0x04
    WinControl* Control; // +0x08
};

class ButtonMover
{
public:
    // empty list, inactive; elapsed and duration are left unwritten.
    ButtonMover();

    // slot 0 (the branch) -- called by Update / Finish when the move completes.
    virtual void OnMoveFinished();
    // slot 1 (the branch, RET 8) -- per-control hook with the interpolated position; false
    // lets Update apply it through the control's SetProperty(6, Position, 0).
    virtual bool OnMoveStep(WinControl* Control, float const* Position);

    // (RET 0xc) -- append {Start, End, Control}.
    void AddButton(WinControl* Control, float const* Start, float const* End);
    // (RET 4) -- Duration = Seconds, active, elapsed = 0, and clear the list (the
    // callers Start first and AddButton after, e.g. HUD_Target 0x004e3028).
    void Start(float Seconds);
    // advance by the frame time and move every control (see .cpp).
    void Update();
    // the +0x18 active flag (the same function serves ButtonScaler).
    bool IsActive() const { return bActive; }
    // when active: stop and put every control at its End, then OnFinished.
    void Finish();
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only (hud_widescreen.cpp): how far the running move is, 0..1; 1 when idle.
    float GetProgress() const { return bActive && Duration > 0.0f ? Elapsed / Duration : 1.0f; }
#endif

protected:
    // No destructor in baseline; derived destructors free the list inline.
    ~ButtonMover() {}

    std::vector<FButtonMoveEntry> Entries; // +0x04
    float Elapsed;                         // +0x14
    bool bActive;                          // +0x18
    float Duration;                        // +0x1c
};

class ButtonScaler
{
public:
    // empty list, inactive.
    ButtonScaler();

    // slot 0 (the branch) -- called by Update when the scale completes.
    virtual void OnScaleFinished();
    // slot 1 (the branch, RET 8) -- per-control hook with the interpolated scale; false lets
    // Update rescale the control's engine instance.
    virtual bool OnScaleStep(WinControl* Control, float const* Scale);

    // (RET 0xc) -- append {From, To, Control}.
    void AddButton(WinControl* Control, float From, float To);
    // (RET 4) -- Duration = Seconds, active, elapsed = 0, clear the list.
    void Start(float Seconds);
    // advance by the frame time and rescale every control (see .cpp).
    void Update();
    // shared with ButtonMover (same +0x18 byte).
    bool IsActive() const { return bActive; }

protected:
    ~ButtonScaler() {}

    std::vector<FButtonScaleEntry> Entries; // +0x04
    float Elapsed;                          // +0x14
    bool bActive;                           // +0x18
    float Duration;                         // +0x1c
};
