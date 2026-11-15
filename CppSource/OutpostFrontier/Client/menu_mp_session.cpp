// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "menu_mp_session.h"

#include "login_mgr.h"   // FServerInfo

bool MenuMpSessionIsReady(FServerInfo const* Server)
{
    // FUN_005accb0 (0x005accb0..0x005accc9): AL = 0; [arg+0x28] != 0 and [arg+0x98] != 0 ->
    // AL = 1. No null test on the argument.
    if (Server->Field_0x28 == 0)
        return false;
    if (Server->Field_0x98 == 0)
        return false;
    return true;
}
