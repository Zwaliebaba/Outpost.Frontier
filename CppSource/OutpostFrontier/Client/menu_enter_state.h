// Author: Giperion.

#pragma once

#include "common_server_types.h" // CHARACTER_ID
#include "image.h"

#include <windows.h> // was reached through the removed menu_input.h

// baseline Intro.cpp IntroFocus (RTTI .?AUIntroFocus@@, vftable 0x005e3034) -- the main-menu
// client state. One static instance, the field (static init the branch -> ctor
// , atexit the branch -> dtor the routine). Methods:
//   slot 7  the routine -- returns 2 (the environment type the routine tests)
//   slot 12 the routine -- Init (RET 4), client_state.cpp
//   slot 13 the routine -- Exit, client_state.cpp
//   slot 14 the routine -- Update (RET 4): MenuStateDriverUpdate
// EnterState (RET 4, jump table 0x00575360 over state - 1, 19 entries):
//                   EnterMenuState
// start the single-player session (Init and EnterState case 1)

// FMenuNavState is the object after its vftable pointer; each comment gives the baseline offset
// from the object start (client_state.cpp static_asserts them through struct IntroFocus).

class WinControl;
struct Win3D;

struct FMenuNavState
{
    int CurrentState;           // +0x004
    int PrevState;              // +0x008
    // +0x00c -- the character the next state works on: case 16 copies the new-game record's
    // id here, poll indices 3 / 8 copy the page's pick; case 12 selects it when Flag20c is set.
    CHARACTER_ID CharacterId;
    unsigned char Flag20c;      // +0x20c -- CharacterId holds a pick (Init clears it)
    unsigned char Pad20d[3];
    // +0x210 -- the menu logo Image (image.h, vftable 0x005e2718, 0x18 bytes): the ctor
    // builds it (0x00573847..0x0057384d), Init loads the field into it (slot 1,
    // ), Exit releases it (the routine inlined, 0x00573e90..0x00573ea2), the Update
    // frame tail draws it (slot 2, 0x005742dd), the dtor the routine is Image's.
    Image LogoImage;
    Win3D* ActivePage;          // +0x228 -- slot 10 on it before every page change
    unsigned char Flag22c;      // +0x22c -- the first settled frame hid the loading screen
    unsigned char Flag22d;      // +0x22d -- case 4 (multiplayer) set it: case 1 restarts SP
    unsigned char Pad22e[2];
    float Field230;             // +0x230 -- Init zeroes it, the frame tail advances it
};

// The Update poll reads the next-state fields of the ported intro pages directly:
// PageServerSelect / PageAccountSelect +0xb4, PageCharacterSelect +0x2b4 (the field),
// PageCharacterSelectSingle +0xb4 (the field).
int MenuNavGetIntroModeLatch();
void MenuNavSetIntroModeRequest(int Mode);
int MenuNavGetIntroModeRequest();          // (the routine tests it for != 0)
bool MenuNavIsServerSelectActive();        // MOV AL,[the field]
void MenuNavConsumeIntroModeRequest(); // latch = request; request = 0

// EnterState(State) on the IntroFocus.
void EnterMenuState(FMenuNavState* Nav, int State);

// IntroFocus::Update(DeltaSeconds). Returns the client-state code (0 = stay).
int MenuStateDriverUpdate(FMenuNavState* Nav, float DeltaSeconds);

// the single-player session start: SetMultiPlayer(0), +0x22d = 0, the
// "SinglePlayer" login, then ConnectToServer && LoginToServer && Reset / list copy /
// RequestCharacterInfo. False when any step fails.
bool MenuNavStartSinglePlayer(FMenuNavState* Nav);

// The TypeSelect column (EnterState case 1) gets its input from the Win3D manager as in baseline
// (the routine mouse buttons, the routine hover, the routine draw); the flat-shell bridge
// that used to expose it to the port-only menu layer is gone.

void MenuEnterStateReleaseActivePage(FMenuNavState* Nav);
