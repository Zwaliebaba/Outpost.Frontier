// Author: Giperion.

#include "server_connect.h"

#include "chat_init.h"          // ChatResetForDisconnect (the routine)
#include "common_ini.h"         // RefreshLoadingProgress
#include "common_server.h"      // IServer
#include "common_server_types.h" // SStartupInfo
#include "common_misc.h"        // SinglePlayer, SetMultiPlayer, CleanUpDatabases
#include "connect_lists.h"      // TeardownConnectLists (the routine)
#include "neuroncomponents.h"              // FDUMP
#include "globals.h"            // ShouldSkipLocalServerTick (the routine)
#include "login_mgr.h"
#include "menu_enter_state.h"   // MenuNavGetIntroModeRequest (the field)
#include "player_spawn.h"       // SetPlayerConnectionId (the field)

#include <string.h>
#include <windows.h>

IServer* G_Server = nullptr;
void* G_ChatServer = nullptr;

// "Server.dll", overwritten (at most 31 characters) by [OutpostFrontier]
// local_server in the routine.
char G_LocalServerDll[32] = "Server.dll";

namespace
{
    HMODULE G_ServerModule = nullptr;
    IServer* G_LoadedServer = nullptr;
    void* G_LoadedChatServer = nullptr;

    typedef IServer*(__cdecl* FGetServerInterface)();
    typedef void*(__cdecl* FGetChatServerInterface)();

    // the field != 0.
    bool IsIntroModeRequested()
    {
        return MenuNavGetIntroModeRequest() != 0;
    }
}

// (cdecl). Neither GetProcAddress result is tested.
IServer* LoadServerModule(char const* DllName)
{
    G_ServerModule = LoadLibraryA(DllName);
    if (G_ServerModule == nullptr)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Connect - failed to LoadLibrary(%s)\n",
              "network\\ServerConnect.cpp", 0x1f, DllName);
        return G_LoadedServer;
    }
    G_LoadedServer = reinterpret_cast<FGetServerInterface>(
        GetProcAddress(G_ServerModule, "GetServerInterface"))();
    G_LoadedChatServer = reinterpret_cast<FGetChatServerInterface>(
        GetProcAddress(G_ServerModule, "GetChatServerInterface"))();
    return G_LoadedServer;
}


IServer* GetLoadedServer()
{
    return G_LoadedServer;
}


void* GetLoadedChatServer()
{
    return G_LoadedChatServer;
}

// (the pushed argument is not read).
void FreeServerModule()
{
    if (G_ServerModule != nullptr)
    {
        FreeLibrary(G_ServerModule);
        G_ServerModule = nullptr;
        G_LoadedChatServer = nullptr;
        G_LoadedServer = nullptr;
    }
}

// . The same block is inlined at the head of the routine and in the failure
// tails of the routine / the routine.
void ShutdownServerConnection()
{
    if (!IsIntroModeRequested())
    {
        ChatResetForDisconnect(); // the routine (returns at once in SP)
        if (G_Server != nullptr)
        {
            G_LoginMgr.Logout();
            G_Server->Shutdown();       // +0x28
            FreeServerModule();
            G_Server = nullptr;
            G_ChatServer = nullptr;
        }
        SetPlayerConnectionId(0);
    }
    TeardownConnectLists();
    CleanUpDatabases();
}

// (cdecl).
bool StartServer(char const* IniPath)
{
    RefreshLoadingProgress();
    // ShutdownServerConnection's body, inlined.
    ShutdownServerConnection();
    RefreshLoadingProgress();

    if (SinglePlayer())
    {
        LoadServerModule(G_LocalServerDll);
        SetMultiPlayer(false);
    }
    else
    {
        LoadServerModule("RemoteServer.dll");
        SetMultiPlayer(true);
    }
    RefreshLoadingProgress();

    G_Server = GetLoadedServer();
    G_ChatServer = GetLoadedChatServer();
    if (G_Server == nullptr)
    {
        return false;
    }

    // baseline fills only the fields below of an uninitialised stack object.
    SStartupInfo Info;
    memset(&Info, 0, sizeof(Info));
    // is the exe's own module handle; nullptr names the same module.
    GetModuleFileNameA(nullptr, Info.ModulePath, 0x104);
    strcpy(Info.IniPath, IniPath);      // byte-copy loop
    Info.MaxPlayers = 1;
    Info.Port = 0x36d9;
    Info.bAllowPvp = 1;
    Info.bAllowNewPlayers = 1;
    return G_Server->Startup(Info);     // +0x24
}

void* GetLocalServerInterface()
{
    return G_Server;
}

void* GetLocalChatServerInterface()
{
    return G_ChatServer;
}

void TickLocalServer(float DeltaSeconds)
{
    if (G_Server == nullptr || ShouldSkipLocalServerTick())
    {
        return;
    }
    G_Server->Update();                 // +0x2c
    G_Server->ElapseTime(DeltaSeconds); // +0x30
}
