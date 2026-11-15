// Author: Giperion.

#pragma once

// baseline Intro.cpp (Client\Interface\Intro.cpp) -- the main-menu column.

// TypeSelect (RTTI .?AUTypeSelect@@ : Win3D, Event::Handler at +0x7c) is the five-button
// column IntroFocus::EnterState state 1 (0x005743f8) builds: New Game / Load Game /
// Multiplayer / Options / Exit. One static instance, the field (static init the branch ->
// ctor the routine, atexit the branch -> dtor the routine). Earlier port notes called it
// "CIntroScreen"; the RTTI name is TypeSelect.

// TextMsgButton (RTTI .?AUTextMsgButton@@ : TextWin, vftable 0x005e2844) is its caption
// button: a TextWin whose click sends parent slot 23 (0x3004, command, 0).

// Call path: IntroFocus::Update (0x00573eb0) polls TypeSelect+0x80 (the field) in state 1;
// TypeSelect::Slot23 starts the slide-out animation for commands 3 / 0xa / 0x10, and
// TypeSelect::Update copies the command into +0x80 once the buttons are gone.

#include "event_handler.h"
#include "text_win.h"
#include "win3d.h"

#include <stddef.h>
#include <windows.h>

class TextMsgButton : public TextWin
{
public:
    // the inlined Win3D + TextWin parts, +0xec = 0, vftable 0x005e2844,
    // +0xe6 = 1.
    TextMsgButton();
    // slot 0 the routine -> the routine = JMP the routine (the TextWin dtor, nothing more)
    ~TextMsgButton() override;

    // slot 15 the routine (RET 4) -- if (+0xe6) the routine (the rollover sound).
    void MouseEnter(float const* Ray) override;
    // slot 19 the routine (RET 4) -- Field1 == 2 && +0xe6 && Field0 == -1 && parent:
    // return parent->slot23(0x3004, +0xec, 0); else false.
    bool MouseEvent(FWin3DMouseEvent const* Event) override;

    // (thiscall, RET 0x14) -- +0xec = Command, +0xe6 = 1, slot 9 Create(Parent, 0),
    // +0xb4 = BoxWidth, +0xb8 = BoxHeight, slot 28 SetText(Caption),
    // ("ButtonText", 1.0f).
    void Init(Win3D* Parent, wchar_t const* Caption, float BoxWidth, float BoxHeight,
              int Command);
    // (thiscall, RET 8) -- +0xe3 = 1, +0xe4 = A, +0xe5 = B.
    void SetFlagsE4E5(unsigned char A, unsigned char B);

    int Command; // +0xec -- the slot 23 argument sent on a click
};

static_assert(sizeof(TextMsgButton) == 0xf0, "baseline TextMsgButton is 0xf0 bytes");
static_assert(offsetof(TextMsgButton, Command) == 0xec, "TextMsgButton +0xec command");

class TypeSelect : public Win3D, public EventHandler
{
public:

    TypeSelect();

    ~TypeSelect() override;

    // slot 9 the routine (RET 8)
    bool Create(Win3D* NewParent, void* Data) override;
    // slot 10 the routine
    void Destroy() override;
    // slot 11 the routine
    void Update() override;
    // slot 13 the routine -- JMP the routine
    void Draw() override;
    // slot 23 the routine (RET 0xc)
    bool Slot23(unsigned Msg, unsigned A, unsigned B) override;

    // Event::Handler slot 0 at +0x7c, the routine (RET 4) -- 0xf -> the routine(0), true.
    bool OnEvent(unsigned MsgId) override;

    typedef BOOL(WINAPI* FIsNetworkAliveFn)(LPDWORD Flags);

    int NextState;                    // +0x080 -- the field, polled by IntroFocus state 1
    TextMsgButton Buttons[5];         // +0x084 -- the field + i * 0xf0
    Win3D* Statics[5];                // +0x534 -- the field: the "bImg" StaticControls
    Win3D* Version;                   // +0x548 -- the field: the "Version" TextControl
    HMODULE SensApi;                  // +0x54c -- sensapi.dll
    FIsNetworkAliveFn IsNetworkAlive; // +0x550
    unsigned PollIntervalMs;          // +0x554 -- 1000 (Create)
    unsigned LastPollMs;              // +0x558 -- timeGetTime of the last probe
    unsigned Pad55c;                  // +0x55c -- never written
    double AnimTime;                  // +0x560 -- the field
    int PendingCommand;               // +0x568 -- the field
    unsigned char bAnimating;         // +0x56c -- the field
    unsigned char Pad56d[3];
    int Phase;                        // +0x570 -- frames spent hidden after the slide-out

private:
    // The "is the network there" probe shared by Create (0x00566a4f) and Update (0x00566564).
    unsigned char ProbeMultiplayerAvailable();
};

static_assert(offsetof(TypeSelect, NextState) == 0x80, "TypeSelect +0x80 next state");
static_assert(offsetof(TypeSelect, Buttons) == 0x84, "TypeSelect +0x84 buttons");
static_assert(offsetof(TypeSelect, Statics) == 0x534, "TypeSelect +0x534 statics");
static_assert(offsetof(TypeSelect, Version) == 0x548, "TypeSelect +0x548 version");
static_assert(offsetof(TypeSelect, SensApi) == 0x54c, "TypeSelect +0x54c sensapi");
static_assert(offsetof(TypeSelect, AnimTime) == 0x560, "TypeSelect +0x560 anim time");
static_assert(offsetof(TypeSelect, PendingCommand) == 0x568, "TypeSelect +0x568 command");
static_assert(offsetof(TypeSelect, bAnimating) == 0x56c, "TypeSelect +0x56c animating");
static_assert(offsetof(TypeSelect, Phase) == 0x570, "TypeSelect +0x570 phase");
static_assert(sizeof(TypeSelect) == 0x578, "baseline TypeSelect is 0x578 bytes");

// .
TypeSelect& GetTypeSelect();
