// Author: Giperion.

#pragma once

// baseline NN_ArrivalText (RTTI .?AVNN_ArrivalText@@ : WinControl) -- the "<BASE>, <SYSTEM>" /
// "<SYSTEM> <region>" caption that types itself out on arrival (the Ship's jump state 5, the
// space init, the base entry) and fades away after a hold time. One instance at a time: the ctor
// stores itself in the field (G_NnArrivalText, nn_modal.h) and the dtor clears it.

// WINFACTORY<NN_ArrivalText> the field "NN_ArrivalText": static init the branch
// (the routine + atexit the branch -> the routine), Create the routine (operator_new(0x33c),
// , the vptrs, the field = this, +0x338 = 0).

// vftable 0x005ce85c (45 slots) -- WinControl's 0x005e5e44 with three overrides:
//   0 the routine (scalar deleting dtor around the routine), 9 the routine Create,
//   11 the branch Update. IDataViewOwner face at +0x7c: WinControl's (the branch / 0053e4a0).

// Layout:
//   +0x000 WinControl (0x32c)
//   +0x32c "ArrivalText" TextControl (Create)
//   +0x330 float hold time once the caption is fully typed (Create: 2.0f)
//   +0x334 float delay before the caption shows (Create: 4.0f; the Show* methods reset it)
//   +0x338 byte: the caption waits for the letterbox to go away (ctor 0; the routine sets it)
//   size 0x33c

#include "wincontrol_base.h"

#include <stddef.h>

class NnTextControl;

class NnArrivalText : public WinControl
{
public:
    // Inlined into the routine (0x0045b569..0x0045b583): WinControl ctor, the vptrs,
    // = this, +0x338 = 0. +0x32c..+0x334 are left to Create.
    NnArrivalText();
    // the vptrs, the field = 0, then the routine (the WinControl dtor).
    ~NnArrivalText() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 11 the branch
    void Update() override;

    // (thiscall, RET 4) -- docked only: "<base>, <system>" (ids 0x55c) with the
    // delay set so the caption finishes typing HoldTime before ArrivalTime runs out.
    void ShowBaseArrival(float ArrivalTime);
    // (thiscall, RET 4) -- the system caption: ids 0x55e with the region the
    // system's strid_name falls in, or ids 0x55d alone; waits for the letterbox.
    void ShowSystemArrival(bool bResetDelay);
    // (thiscall, RET 4) -- "<NameIds>, <system>" (ids 0x55c), delay 2.0f.
    void ShowNamedArrival(unsigned NameIds);
    // (thiscall) -- bit 1 of the caption's Win3D flags (it is showing).
    bool IsCaptionVisible() const;

    NnTextControl* Caption;      // +0x32c
    float HoldTime;              // +0x330
    float Delay;                 // +0x334
    unsigned char bWaitForLetterbox; // +0x338
    unsigned char Pad339[3];     // +0x339

    // The teardown written out at 0x0045ace0, 0x0045ad90, 0x0045ae63 and in the routine /
    // the routine on every child (the next sibling read first), slot 10 Destroy,
    // then slot 0 with 1 (delete). `this` is gone afterwards.
    void DestroySelf();

private:
    NnArrivalText(NnArrivalText const&) = delete;
    NnArrivalText& operator=(NnArrivalText const&) = delete;
};

static_assert(sizeof(NnArrivalText) == 0x33c, "baseline NN_ArrivalText is 0x33c bytes (0x0045b548)");
static_assert(offsetof(NnArrivalText, Caption) == 0x32c, "NN_ArrivalText +0x32c caption");
static_assert(offsetof(NnArrivalText, HoldTime) == 0x330, "NN_ArrivalText +0x330 hold");
static_assert(offsetof(NnArrivalText, Delay) == 0x334, "NN_ArrivalText +0x334 delay");
static_assert(offsetof(NnArrivalText, bWaitForLetterbox) == 0x338, "NN_ArrivalText +0x338");

// The cdecl entry points. Each "ensure" makes the window through the WinFactory
// (the routine("NN_ArrivalText", "NN_ArrivalText", 0, 0)) when the field is null and gives up
// when it still is.
// ensure, then ShowBaseArrival(ArrivalTime). Caller 0x00558a04.
void NnArrivalTextShowBase(float ArrivalTime);
// ensure, then ShowNamedArrival(NameIds). Caller 0x00514cc3 (the routine).
void NnArrivalTextShowNamed(unsigned NameIds);
// tear the window down when there is one. Callers 0x0043b40d, 0x0043b862,
// , 0x0045a184.
void NnArrivalTextDestroy();
// a pending caption (delay above 0) has its delay cut to at most 1.0f; one whose
// delay has run out is torn down. Caller 0x00456883.
void NnArrivalTextHurry();
// ensure, then Delay = DelayBits (copied as a dword), no letterbox wait, and the
// caption's property 0x1e with Arg. Caller 0x0044eedd.
void NnArrivalTextShowRaw(float Delay, uintptr_t Arg);
// ensure, then ShowSystemArrival(true). Caller 0x0054adec (the space init).
void NnArrivalTextShowSystemNow();
