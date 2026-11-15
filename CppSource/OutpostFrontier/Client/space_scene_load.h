// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// The head of the connect slice FUN_0054ab40 before the system enter: FUN_005020a0
// (JumpEffectIniLoad, jump_effect.cpp) and FUN_00510f90. The system enter itself is StarSysEnter (star_sys.h).

bool LoadSpaceSceneSlice();
