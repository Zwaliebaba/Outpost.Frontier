// Author: Giperion.

#pragma once

// UIShipViewControl -- the PAGE_*.page ship-preview widget (baseline UIWin.cpp): a ShipViewControl
// whose size comes from the page line ("UIShipViewControl name x y w h", FUiPayloadBounds) and
// whose view box follows the widget's transform.

// baseline:
//   WINFACTORY<UIShipViewControl> the field, static init the branch -> the routine
//   ("UIShipViewControl"), atexit the branch -> the routine. Create the routine:
//   operator_new(0x57c), ShipViewControl ctor the routine, vftables 0x005e59a4 / 0x005e5998
//   (+0x7c), then slot 9.
//   vftable 0x005e59a4 is ShipViewControl's 0x005e4bcc with three slots replaced:
//     slot 0 the routine  scalar deleting destructor (the routine: the two vftables back, then
//                          ShipViewControl's destructor the routine)
//     slot 8 the branch  SetTransform
//     slot 9 the routine  Create (payload {Width, Height})

// Layout past ShipViewControl (baseline 0x570 bytes):
//   +0x570 the view box's half extent {Width * 0.5, Height * -0.5, 0}; size 0x57c.

#include "ship_view_control.h"

class UiShipViewControl : public ShipViewControl
{
public:
    // The inlined part of the routine after the routine: only the vftables; +0x570..+0x578 are
    // written by Create.
    UiShipViewControl();
    // .
    ~UiShipViewControl() override;

    // slot 8 (+0x20) -- the branch (RET 4).
    void SetTransform(float const* Transform) override;
    // slot 9 (+0x24) -- the routine (RET 8).
    bool Create(Win3D* NewParent, void* Payload) override;

    float HalfExtent[3]; // +0x570

private:
    UiShipViewControl(UiShipViewControl const&) = delete;
    UiShipViewControl& operator=(UiShipViewControl const&) = delete;
};
