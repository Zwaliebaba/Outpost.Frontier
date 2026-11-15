// Author: Giperion.

#pragma once

// baseline Intro.cpp PageAccountSelect (RTTI .?AVPageAccountSelect@@ : UIPage::MasterWin) -- the
// multiplayer "enter your CD key" page (PAGE_AccountSelect.page: AccountBoxShort, SelectServer,
// Back, AccountID, Explain). One static instance, the field (static init the branch -> ctor
// , atexit the branch -> dtor the routine). vftables: main 0x005e2ef4, IUIPage
// face (+0x7c) 0x005e2ed8, Event::Handler face (+0x80) 0x005e2ed0 (slot 0 the branch).

// Call path: IntroFocus EnterState case 6 (0x00574c1a) loads the page descriptor (+0x8c),
// calls slot 9 Create(0, 0) and makes this the active page (+0x228). IntroFocus::Update poll
// index 2 (0x00574021) moves to the state in +0xb4 (the field) and clears it. IntroFocus
// Exit calls slot 10 (0x00573e81).

// baseline layout past MasterWin (+0x00..+0x8b): +0x8c FUiPageDesc, +0xb4 Result, +0xb8 slide
// parameter (double), +0xc0 leaving, +0xc4 leave direction, +0xd4 the box's home position,
// +0xe0..+0xf0 the five widgets, +0xf4 / +0x100 two RenderDisplayLists. The port's MasterWin
// has no baseline byte layout (see page_options.h), so the members below keep only the order.

#include "common_text.h" // RenderDisplayList
#include "ui_page.h"
#include "ui_page_desc.h"

class UiRichTextWin;
class UiTextEdit;
class UiTextMsgButton;

class PageAccountSelect : public UIPage::MasterWin
{
public:
    // .
    PageAccountSelect();
    // .
    ~PageAccountSelect() override;

    // slot 9 the routine (RET 8).
    bool Create(Win3D* NewParent, void* Payload) override;
    // slot 10 the routine.
    void Destroy() override;
    // slot 11 the branch -- the slide in / out, then Win3D's child walk.
    void Update() override;
    // slot 23 the routine (RET 0xc).
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;
    // Event::Handler face (+0x80) slot 0 the branch (RET 4).
    bool OnEvent(unsigned MsgId) override;

    // (RET 4) -- the routine(bEnabled) on AccountID, SelectServer and Back.
    void SetToolTipsEnabled(bool bEnabled);

    FUiPageDesc Desc;              // +0x8c -- EnterState case 6 loads it (the field)
    int Result;                    // +0xb4 -- the field: the next IntroFocus state
    double SlideT;                 // +0xb8 -- Create -1.5; 0..2 while in, 0..-0.9 while leaving
    unsigned char bLeaving;        // +0xc0
    int LeaveDirection;            // +0xc4 -- 1 on to ServerSelect (Result 5), -1 back (1)
    float BoxHome[3];              // +0xd4 -- AccountBoxShort's position after Create
    UiTextEdit* AccountId;         // +0xe0 -- "AccountID"
    UiTextMsgButton* SelectServer; // +0xe4 -- "SelectServer"
    UiTextMsgButton* Back;         // +0xe8 -- "Back"
    UiRichTextWin* Explain;        // +0xec -- "Explain"
    Win3D* AccountBox;             // +0xf0 -- "AccountBoxShort" (UIWinObject)
    RenderDisplayList ExplainText; // +0xf4 -- StyleNode 0x9ca4 + IDS 0x60005
    RenderDisplayList Text601eb;   // +0x100 -- StyleNode 0x9ca4 + IDS 0x601eb; no reader here

private:
    // The leave sequence Slot23 and OnEvent spell out inline, identical at all four sites
    // (0x005730fd.., 0x0057322d.., 0x005732dd.., 0x005733bc..): leaving from SlideT 0, both
    // buttons inert and their surfaces frozen, the edit field and the explanation hidden, the
    // slide sound, the three ToolTips off, then the direction.
    void BeginLeave(int Direction);
    // The key check both "SelectServer" (0x3004 / 0x3ea) and 0xf004 spell out: the typed key
    // lower-cased and narrowed into CPlayerAccount::SetTextKey; refused -> the 0x76a error and
    // no direction, accepted -> leave towards ServerSelect. Always true.
    bool SubmitTypedKey();

    PageAccountSelect(PageAccountSelect const&) = delete;
    PageAccountSelect& operator=(PageAccountSelect const&) = delete;
};

// .
PageAccountSelect& GetPageAccountSelect();
