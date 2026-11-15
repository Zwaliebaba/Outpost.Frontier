// Author: Giperion.

#pragma once

// WinMain 0x005b3324 hands its result to the routine (EnterClientState).
// Returns the first client state: 6 = IntroFocus menu (the normal case), 2/3 = [Login]
// auto-login succeeded (in game / the routine variant), 1 = auto-login failed.
int ComputeInitialEnvironment();
