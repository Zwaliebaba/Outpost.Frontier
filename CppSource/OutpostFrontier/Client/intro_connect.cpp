// Author: Giperion.

#include "intro_connect.h"

#include "character_select_equip.h" // GetPlayerBaseIdbaseline (the routine's the field)
#include "connect_flow.h"
#include "globals.h"
#include "login_mgr.h"
#include "menu_mp_session.h"

#include "common_misc.h" // SinglePlayer
#include "neuroncomponents.h" // FDUMP

#include <string.h>
#include <windows.h>

int ComputeInitialEnvironment()
{
    // (0x005b20a0..0x005b218d). Default result 6 (menu, IntroFocus). The port-only
    // -spquickstart prefix that used to run here is gone: the baseline [Login] auto-login below is
    // the same quick start (it needs [Login] char just as the shortcut did).
    int Result = 6;
    // the [Login] auto-login is on when the login name (+0x188, the
    // copy the routine returns) is not empty and the character name (the field, +0x1e8) is
    // not empty. CLoginMgr::LoadConfig (the routine, main.cpp) fills both from [Login].
    bool const bAutoLogin =
        !G_LoginMgr.LoginName.empty() && G_LoginMgr.SelectCharacterName[0] != 0;
    if (bAutoLogin)
    {
        // SinglePlayer() || the routine(&the field) -- the
        // current server (+0x94) has its +0x28 and +0x98 fields set.
        if (SinglePlayer() || MenuMpSessionIsReady(&G_LoginMgr.CurrentServer))
        {
            Result = 1;
            // .
            if (ConnectAndLogin(g_display.ini_path.Text) &&
                G_LoginMgr.RequestCharacterInfo() != 0 &&
                G_LoginMgr.SelectAutoCharacter())
            {
                // the player's base (the field & 0x3fffffff) is set.
                Result = (GetPlayerBaseIdbaseline() & 0x3fffffffu) != 0 ? 3 : 2;
            }
            if (Result == 1)
            {
                // .
                FDUMP(0x100001, "%s(%d) : *** ERROR: %s", "Client\\main.cpp",
                      0x476, "ComputeInitialEnvironment() failed - exiting OutpostFrontier");
            }
        }
    }
    return Result;
}
