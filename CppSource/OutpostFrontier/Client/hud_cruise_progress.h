// Author: Giperion.

#pragma once

// -------------------------------------------------------------------------------------------
// HUD_CruiseProgress / [CruiseProgress] -- the cruise-engine charge caption under the crosshair
// ("<ids 0x585> - NN%" while the engine charges, "<ids 0x64f> - N%" while the charge timer is
// still negative).

// RTTI HUD_CruiseProgress : HUDItem @0, WinControl @0x38 (IDataViewOwner @0xb4). Size 0x42c.
// vftables:
//   HUDItem face 0x005d7f74 (11 slots) -- only slot 0 overridden (the routine, scalar deleting
//     dtor around the routine); the Render slot is inherited, the window draws from Win3D.
//   WinControl face 0x005d7ebc (45 slots) -- overrides 0 the branch (thunk, ECX -= 0x38, into
// ), 9 the routine Create, 11 the routine Update, 12 the routine
//     UpdateInactive, 13 the branch Draw; every other slot is WinControl's 0x005e5e44 entry.

// Factory: the static HUDFACTORY<HUD_CruiseProgress> the field ("CruiseProgress"; XC entry
// , ctor the routine, atexit the branch -> the routine), vftable 0x005d7ff4
// whose Create the routine is operator new(0x42c) + the routine.
// -------------------------------------------------------------------------------------------

#include "hud_element_list.h"
#include "wincontrol_base.h"

class INI_Reader;

class HUD_CruiseProgress : public HUDItem, public WinControl
{
public:
    // (thiscall, RET 4) -- the reader is not read.
    explicit HUD_CruiseProgress(INI_Reader& Reader);
    // (scalar deleting the routine; WinControl-face thunk the branch)
    ~HUD_CruiseProgress() override;

    // WinControl slot 9 the routine (RET 8) -- the "CruiseProgressText" label and the two
    // ids captions.
    bool Create(Win3D* NewParent, void* Data) override;
    // WinControl slot 11 the routine -- shown (and active) while the cursor is drawn and there
    // is a player ship, hidden otherwise; then Win3D's child walk.
    void Update() override;
    // WinControl slot 12 the routine (also in vftable 0x005d96fc) -- the show half of Update,
    // then Win3D's inactive child walk.
    void UpdateInactive() override;
    // WinControl slot 13 the branch -- word and draw the label from the player engine's
    // cruise charge.
    void Draw() override;

    WinControl* CruiseProgressText;  // +0x364 (WinControl +0x32c) "CruiseProgressText"
    wchar_t ChargingCaption[0x30];   // +0x368 (+0x330) ids 0x585, "%s - %d%%" while charging
    wchar_t CountdownCaption[0x30];  // +0x3c8 (+0x390) ids 0x64f, "%s - %d%" before the charge
    // +0x428 (+0x3f0) -- set while the "charging" caption is up; the first frame that sets it
    // invalidates the label's cached draw (the routine's second argument).
    unsigned char bCharging;
    unsigned char Pad429[3];

private:
    HUD_CruiseProgress(HUD_CruiseProgress const&) = delete;
    HUD_CruiseProgress& operator=(HUD_CruiseProgress const&) = delete;
};
