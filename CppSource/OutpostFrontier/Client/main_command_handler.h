// Author: Giperion.

#pragma once

// main.cpp's global key-command handler (baseline object the field, vftable 005e6d20).

// static initializer (XC entry 0061013c): the routine ctor + atexit the branch
// ctor -- registers USER_EXIT_GAME 0xc5, USER_PAUSE 0x14, USER_RESET 0x15,
//                 USER_FULLSCREEN 0x17, USER_SCREEN_SHOT 0xcb through the routine
// vt[0] OnCommand(MsgId) -- the switch below
// atexit -> thunk_FUN_00576010 (Event::Handler dtor: unregister)

// The object is a file-scope static in main_command_handler.cpp; nothing else calls into it.

// "leave the session" flag. USER_EXIT_GAME sets it (005b245e). baseline also
// writes it at 0054abe5 / 0054ae4d / 005b292b / 005b29ac / 005b2ad0 / 005b2c50 and reads it in
// the frame function the routine (005b28cf / 005b29f8 / 005b2a16): a set flag sends the frame
// to state 6 (IntroFocus) and is cleared there (TickClientFrame, client_frame_tick.cpp).
bool ClientQuitRequested();
void SetClientQuitRequested(bool bQuit);

// USER_SCREEN_SHOT: next free "<GetScreenShotPath>\screen<N>.BMP", then
// IRPTexture::PrintScreen(0, path). Returns the running shot counter (the field), or -1 when
// GetScreenShotPath fails.
int TakeClientScreenShot();
