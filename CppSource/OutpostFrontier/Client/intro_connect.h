// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// FUN_005b20a0: WinMain 0x005b3324 hands its result to FUN_005b2a30 (EnterClientState).
// Returns the first client state: 6 = IntroFocus menu (the normal case), 2/3 = [Login]
// auto-login succeeded (in game / FUN_004c4810 variant), 1 = auto-login failed.
int ComputeInitialEnvironment();
