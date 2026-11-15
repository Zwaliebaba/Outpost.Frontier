// Author: Giperion.

#include "connect_handshake.h"

#include "client_frame_tick.h"    // BeginClientRenderFrame / EndClientRenderFrame
#include "common_server.h"        // IServer
#include "common_misc.h"          // SinglePlayer
#include "common_timing.h"        // Timing::read
#include "globals.h"              // PumpMessages, GetClientFrameTimeCapped
#include "hud_command_dispatch.h" // HudPostCommand (the routine)
#include "login_mgr.h"
#include "menu_enter_state.h"     // / the routine
#include "server_connect.h"       // ShutdownServerConnection (the routine)
#include "nn_blocking_dialog.h"   // / the routine
#include "nn_network_operation.h" // / the routine / the routine
#include "win3d.h"                // Win3DUpdateAll / Win3DDrawAll

// the DirectPlay connect HRESULT HandleConnectError records; zeroed
// and polled by WaitForServerResponse.
long G_ConnectResult = 0;

// 1 while WaitForServerResponse runs. Nothing in OutpostFrontier.exe
// reads it (XREF: 005a89bb / 005a8b6a / 005a8b83 / 005a8bc8, all writes).
bool G_bWaitingForServer = false;

// HandleConnectError.
extern "C" __declspec(dllexport) void __cdecl HandleConnectError(long Result)
{
    G_ConnectResult = Result;
}

// (cdecl).
int WaitForServerResponse(unsigned char* bResponse, float MpTimeout, float SpTimeout)
{
    G_bWaitingForServer = true;
    int Result = 1;
    G_ConnectResult = 0;
    float const StartTime = static_cast<float>(Timing::read()); // FSTP float

    if (!SinglePlayer())
    {
        BlockingDialogOpen(); // the routine
    }

    // . The x87 test `TEST AH,5; JNP` leaves the loop only when
    // Timeout < Elapsed; equal or unordered keeps pumping.
    while (*bResponse == 0 && !G_bServerDisconnected && G_ConnectResult == 0 && G_Server != nullptr)
    {
        G_Server->Update(); // +0x2c
        double const Elapsed = Timing::read() - static_cast<double>(StartTime);
        double const Timeout = static_cast<double>(SinglePlayer() ? SpTimeout : MpTimeout);
        if (Timeout < Elapsed)
        {
            break;
        }
    }

    if (!SinglePlayer())
    {
        BlockingDialogClose(); // the routine
    }

    if (!SinglePlayer() && *bResponse == 0)
    {
        if (G_bServerDisconnected)
        {
            Result = 0;             // -> 005a8b4e
        }
        else if (G_ConnectResult == 0)
        {
            // the NN_NetworkOperation wait.
            NnNetworkOperationOpen(0x736);
            int ExitCode = 0;              // [ESP+0xc], the routine's out slot
            while (*bResponse == 0)        // / 005a8b2c
            {
                // a drop or a DirectPlay error ends the wait as it is.
                if (G_bServerDisconnected || G_ConnectResult != 0)
                {
                    break;
                }
                // the dialog was dismissed, or WM_QUIT -> 2 (005a8b37).
                if (!NnNetworkOperationIsOpen() || !PumpMessages(&ExitCode))
                {
                    Result = 2;
                    break;
                }
                // no server object -> 0 (005a8b3e).
                if (G_Server == nullptr)
                {
                    Result = 0;
                    break;
                }
                G_Server->Update(); // +0x2c
                // FSUB the float start, FLD SpTimeout (param_3), FCOMPP,
                // `TEST AH,5; JNP` -> 0 when SpTimeout < Elapsed; equal or unordered goes on.
                double const Elapsed = Timing::read() - static_cast<double>(StartTime);
                if (static_cast<double>(SpTimeout) < Elapsed)
                {
                    Result = 0;
                    break;
                }
                Win3DUpdateAll(GetClientFrameTimeCapped()); // (the routine())
                if (BeginClientRenderFrame())
                {
                    Win3DDrawAll();
                    EndClientRenderFrame(); // + the routine
                }
            }
            NnNetworkOperationClose(); // the routine
            if (G_bServerDisconnected)
            {
                Result = 0;            // -> 005a8b4e
            }
        }
    }
    else if (G_bServerDisconnected)
    {
        Result = 0;
    }

    if (G_ConnectResult != 0)
    {
        // DPNERR 0x80158410 -> 6, 0x80158610 -> 4, else 5.
        if (G_ConnectResult == static_cast<long>(0x80158410))
        {
            Result = 6;
        }
        else
        {
            Result = (G_ConnectResult != static_cast<long>(0x80158610)) ? 5 : 4;
        }
        G_bWaitingForServer = false;
        return Result;
    }

    if (Result == 2 && !SinglePlayer())
    {
        // the player dismissed the NN_NetworkOperation wait. Unless the
        // server-select page is up (the routine, the field), drop the connection and send
        // the menu back to mode 3.
        if (!MenuNavIsServerSelectActive())
        {
            ShutdownServerConnection();
            MenuNavSetIntroModeRequest(3); // (3)
            HudPostCommand(0xc5, 0);       // (0xc5, 0)
        }
    }

    G_bWaitingForServer = false;
    return Result;
}
