// Author: Giperion.

#pragma once

// baseline Intro.cpp PageOptions (RTTI .?AUPageOptions@@ : UIPage::MasterWin) -- the main-menu
// Options page. One static instance, the field (static init the branch -> ctor
// , atexit the branch -> dtor the routine). vftables: main 0x005e2fbc,
// IUIPage face (+0x7c) 0x005e2fa4, Event::Handler face (+0x80) 0x005e2f9c (slot 0
// , false).

// Call path: TypeSelect command 0xa -> IntroFocus state 10. EnterState case 10 (0x00574a2f)
// releases the active page, calls slot 9 Create(0, 0) and makes this the active page (+0x228).
// Create builds the NN_Preferences WinControl (+0xb8, the field). IntroFocus::Update poll
// index 4 (0x00573f96) watches +0xb8 / +0xb4 (the field) to leave the state.

// baseline layout: MasterWin (+0x00..+0x8b, Data = 0 -- the ctor passes no page data), the
// FUiPageDesc at +0x8c (the routine / the routine, never loaded), +0xb4 Result,
// +0xb8 Preferences. The port's MasterWin has no Event::Handler face, so the members below
// do not sit at their baseline byte offsets.

#include "ui_page.h"
#include "ui_page_desc.h"

class WinControl;

class PageOptions : public UIPage::MasterWin
{
public:
    // MasterWin ctor with no page data (0x005735e9 PUSH 0), the desc ctor,
    // +0xb4 = 0.
    PageOptions();
    // the desc dtor, then the MasterWin dtor.
    ~PageOptions() override;

    // slot 9 the routine (RET 8).
    bool Create(Win3D* NewParent, void* Payload) override;
    // slot 10 is the branch = JMP the routine (MasterWin::Destroy): not overridden.

    // slot 23 the routine (RET 0xc).
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    FUiPageDesc Desc;        // baseline +0x8c
    int Result;              // baseline +0xb4 -- the field
    WinControl* Preferences; // baseline +0xb8 -- the field
};

// .
PageOptions& GetPageOptions();
