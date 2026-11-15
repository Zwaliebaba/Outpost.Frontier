// Author: Giperion.

#include "connect_flow.h"

#include "chat_init.h"      // ChatResetForConnect (the routine)
#include "common_ini.h"     // RefreshLoadingProgress
#include "common_server.h"  // IServer
#include "common_misc.h"    // SinglePlayer
#include "connect_lists.h"  // PreConnectReset (the routine)
#include "login_mgr.h"
#include "nn_error.h"       // NnErrorRunModal (the routine)
#include "player_spawn.h"   // GetPlayerConnectionId (the field)
#include "server_connect.h"

// (cdecl).
bool ConnectToServer(char const* IniPath)
{
    RefreshLoadingProgress();
    PreConnectReset();

    if (StartServer(IniPath))
    {
        int const Result = G_LoginMgr.Connect(true);
        if (Result == 1)
        {
            return true;
        }
        // the NN_Error modal the routine(ids, 1, 0) -- 0x73e for 4 and
        // 0x73f for 6 unconditionally; any other result shows 0x73d unless the server already
        // dropped us (the field).
        if (Result == 4)
        {
            NnErrorRunModal(0x73e, true, nullptr);
        }
        else if (Result == 6)
        {
            NnErrorRunModal(0x73f, true, nullptr);
        }
        else if (!G_bServerDisconnected)
        {
            NnErrorRunModal(0x73d, true, nullptr);
        }
    }

    // ShutdownServerConnection's body, inlined.
    ShutdownServerConnection();
    return false;
}

// (cdecl).
bool LoginToServer()
{
    if (G_Server == nullptr)
    {
        return false;
    }

    int Result;
    if (SinglePlayer())
    {
        G_LoginMgr.bCreateAccount = 0;      // the field
        Result = G_LoginMgr.Login();
        if (Result != 1 && G_LoginMgr.LoginStatus == 2)
        {
            G_LoginMgr.bCreateAccount = 1;
            Result = G_LoginMgr.Login();
        }
    }
    else
    {
        Result = G_LoginMgr.Login();
    }

    if (Result == 1)
    {
        ChatResetForConnect(); // the routine (returns at once in single player)
        return true;
    }

    if (Result != 2)
    {
        // the NN_Error modal the routine(ids, 1, 0) keyed on the login
        // status byte (the field): 0x742 for 0, 0x753 for 1, 0x743 for 7 unconditionally;
        // any other status shows 0x739 unless the server already dropped us (the field).
        if (G_LoginMgr.LoginStatus == 0)
        {
            NnErrorRunModal(0x742, true, nullptr);
        }
        else if (G_LoginMgr.LoginStatus == 1)
        {
            NnErrorRunModal(0x753, true, nullptr);
        }
        else if (G_LoginMgr.LoginStatus == 7)
        {
            NnErrorRunModal(0x743, true, nullptr);
        }
        else if (!G_bServerDisconnected)
        {
            NnErrorRunModal(0x739, true, nullptr);
        }
    }

    // ShutdownServerConnection's body, inlined.
    ShutdownServerConnection();
    return false;
}

// (cdecl).
bool ConnectAndLogin(char const* IniPath)
{
    if (!ConnectToServer(IniPath))
    {
        return false;
    }
    return LoginToServer();
}
