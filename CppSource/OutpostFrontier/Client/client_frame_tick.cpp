// Author: Giperion.

#include "client_frame_tick.h"
#include "client_tick_profile.h"

#include "client_equip_fx.h"
#include "client_state.h"
#include "menu_ui_widgets.h"
#include "cockpit_load.h"
#include "gameflow.h"
#include "menu_sound_arch.h"
#include "menu_universe_base.h"
#include "sound_config.h"
#include "game_options.h"
#include "font_atlas.h"
#include "font_imp.h"
#include "font_string.h"
#include "globals.h"
#include "hud_element_list.h"
#include "login_mgr.h"             // G_Server (the field), the field / the field
#include "main_command_handler.h"  // (the routine / the routine)
#include "menu_enter_state.h"      // / the routine
#include "nn_error.h"
#include "player_behavior.h"
#include "server_connect.h"
#include "space_camera.h"
#include "space_scene_draw.h"
#include "space_scene_load.h"
#include "space_scene_obj_draw.h"
#include "engine_light.h"
#include "star_sys.h"
#include "tradelane_fx.h"
#include "win3d.h"
#include "win3d_immediate.h"
#include "win3d_render.h"
#include "win3d_renderables.h"

#include "NeuronRuntime.h"
#include "particlefx.h"
#include "common_animdb.h"
#include "common_physys.h"
#include "common_misc.h"   // SinglePlayer
#include "common_timing.h" // Timing::UpdateGlobalTime
#include "neuroncomponents.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"

#include <float.h>
#include <stdio.h>
#include <windows.h>

namespace
{
    // / the field are RenderMgr.cpp globals: G_bRenderDither (the DitherControl
    // apply the branch) and G_RenderMipFilter (the MipFilterControlCallback apply
    // ), defined in engine_init.cpp.

    // the IServer* the frame saw last (.bss 0). Only the routine touches it
    // (0x005b28bb read, 0x005b28ca store).
    IServer* G_FrameLastServer = nullptr;

    IRenderPipeline* FramePipeline()
    {
        // Every pipeline call in the routine / the routine / the routine re-reads the NeuronRuntime
        // import (MOV EAX,[->NeuronRuntime::Pipeline]; MOV EAX,[EAX]) -- no null test anywhere.
        return NeuronRuntime::Pipeline;
    }

    // (module storage-0042507f), cdecl, returns the field.
    bool TryBeginFrame()
    {
        // the field = 0; an inactive application (the field)
        // returns that 0 straight away.
        G_bRenderFrameBegun = false;
        if (!ClientAppIsActive())
            return false;

        // the routine(0) behind the field.
        if (G_bFpExceptionTrap)
            SetFpuExceptionTrap(false);

        // when the device changed the precision (the field differs
        // from the field), the frame runs with the one the device left: EAX is loaded from
        // before the 0x18 / 0x35 tests.
        if (G_FpuPrecisionFound != G_FpuPrecisionWanted)
        {
            if (G_FpuPrecisionFound == 0x18)
                _control87(0x20000, 0x30000);
            else if (G_FpuPrecisionFound == 0x35)
                _control87(0x10000, 0x30000);
            else
                _control87(0, 0);
        }

        // with the field, the field = (BeginScene +0xe4 >= 0).
        if (G_bRenderPostCreate)
            G_bRenderFrameBegun = FramePipeline()->BeginScene() >= 0;

        if (!G_bRenderFrameBegun)
        {
            // Sleep(1000), then the routine(the field, options,
            // options).
            Sleep(1000);
            if (RenderMgrCreateBuffers(G_RenderMgrWindow, &g_display, &g_display))
            {
                // Clear(3, 0) +0x3c, SwapBuffers +0x40; only a
                // non-negative swap is followed by BeginScene, whose result is stored.
                FramePipeline()->ClearBuffers(3, nullptr);
                if (FramePipeline()->SwapBuffers() >= 0)
                    G_bRenderFrameBegun = FramePipeline()->BeginScene() >= 0;
            }
            // .
            if (!G_bRenderFrameBegun)
                return false;
        }

        // SetRenderState(0x1a D3DRS_DITHERENABLE) +0xf0, 1 iff the
        // options object's color_bpp (the field) is 16 AND the field != 0.
        const int DitherEnable = (g_display.color_bpp == 0x10 && G_bRenderDither != 0) ? 1 : 0;
        FramePipeline()->SetRenderState(D3DRS_DITHERENABLE, DitherEnable);
        // SetTextureStageState(0, 0x12, the field) +0xf8.
        FramePipeline()->SetTextureStageState(0, 0x12, G_RenderMipFilter);

        // .
        return G_bRenderFrameBegun;
    }

    // (module storage-00425138), void -- flush the material batcher, EndScene, put
    // the render states the next frame starts from back, re-arm the FP trap, restore the FPU
    // precision. Unconditional: its callers only reach it when the routine returned true.
    void EndSceneHalf()
    {
        // MaterialBatcher slot 0x28 (1), no null test.
        NeuronRuntime::MaterialBatcher->FlushAll(1);

        // Intentional port-only addition: CLIENT-BETA-LABEL, removal only by Giperion.
        DrawClientTickProfile();

        // Pipeline slot 0xe8 (EndScene); EAX is not read.
        FramePipeline()->EndScene();

        // the state the next frame starts from, through the OpenGL.cpp
        // layer -- glDisable(0xb60), depth write on, glDisable(GL_BLEND), glEnable(GL_DEPTH_TEST),
        // depth func 4 (LESSEQUAL), glEnable(GL_CULL_FACE).
        Win3DDisable(kWin3DFog);            // (0xb60)
        Win3DSetDepthWrite(1);              // (1)
        Win3DDisable(kWin3DBlend);          // (0xbe2)
        Win3DEnable(kWin3DDepthTest);       // (0xb71)
        Win3DSetDepthFunc(4);               // (4)
        Win3DEnable(kWin3DCullFace);        // (0xb44)

        // the routine(1) behind the field.
        if (G_bFpExceptionTrap)
            SetFpuExceptionTrap(true);

        // when the precision the routine found (the field) differs
        // from the one the renderer wants (the field), put the wanted one back -- EAX is
        // loaded from the field here: 24-bit, 53-bit, or the (0, 0) query for anything else.
        if (G_FpuPrecisionFound != G_FpuPrecisionWanted)
        {
            if (G_FpuPrecisionWanted == 0x18)
                _control87(0x20000, 0x30000);
            else if (G_FpuPrecisionWanted == 0x35)
                _control87(0x10000, 0x30000);
            else
                _control87(0, 0);
        }
    }

    // (module storage-0042515d), void.
    void PresentHalf()
    {
        FClientTickScope PresentProfile(EClientTickSection::Present);
        // the routine.
        Win3DOnPresentFrame();
        // SwapBuffers +0x40, then NEG EAX / SBB AL,AL / INC AL --
        // = (result == 0), not (result >= 0).
        G_bRenderPostCreate = FramePipeline()->SwapBuffers() == 0;
    }
}

// , cdecl(double dt) -- WinMain passes the routine read before the message pump
// (0x005b3347). Listing-traced 0x005b2890..0x005b2a1f. Every state (space the branch, GF
// , IntroFocus the routine, MissionBriefing the branch) begins, draws and
// presents its own frame inside its slot 14; the frame function only runs the timers, the
// disconnect check, the Win3D roots, the local server and the state machine.
void TickClientFrame(double DeltaSeconds)
{
    // Port-only QPC diagnostics requested by Giperion; CLIENT-BETA-LABEL.
    FClientTickProfileFrame ProfileFrame;
    // Timing::UpdateGlobalTime(dt) unless the routine.
    if (!ShouldSkipLocalServerTick())
        Timing::UpdateGlobalTime(DeltaSeconds);

    // BL = the server pointer changed since the last frame (the field
    // against the field); the new one is remembered.
    bool bServerChanged = false;
    if (G_FrameLastServer != G_Server)
    {
        bServerChanged = true;
        G_FrameLastServer = G_Server;
    }

    // no quit pending (the field), multiplayer, and the server dropped
    // us (the field): the "connection lost" box the routine(0x737, 0, 0) when the connect
    // response had arrived (the field); both flags cleared; the routine; unless the server
    // select page is up (the routine), the intro mode request 3 (the routine) and the quit flag.
    if (!ClientQuitRequested() && !SinglePlayer() && G_bServerDisconnected)
    {
        if (G_bServerConnected)
            NnErrorRunModal(0x737, false, nullptr);
        G_bServerDisconnected = false;
        G_bServerConnected = false;
        ShutdownServerConnection();
        if (!MenuNavIsServerSelectActive())
        {
            MenuNavSetIntroModeRequest(3);
            SetClientQuitRequested(true);
        }
    }

    // the routine(dt) -- the only per-frame Update of the registered
    // Win3D roots (modal dialogs included); the NN modal loops call it themselves (FUN_004739xx).
    FClientTickScope UiProfile(EClientTickSection::Ui);
    Win3DUpdateAll(DeltaSeconds);
    UiProfile.Stop();

    // the field non-null and the routine false -> IServer slot 11
    // Update, slot 12 ElapseTime((float)dt) -- both inside TickLocalServer.
    FClientTickScope ServerProfile(EClientTickSection::Server);
    TickLocalServer(static_cast<float>(DeltaSeconds));
    ServerProfile.Stop();

    // EBP: the state to enter (0 = stay); ECX: the field as last read.
    int NextState = 0;
    IEnvironment* CurrentState = g_client.current_state;
    bool bForceIntro = false;

    // the server went away this frame (changed and now null) while the
    // current state is not the IntroFocus (the routine): in multiplayer the intro mode request
    // 3, then back to the intro.
    if (bServerChanged && G_Server == nullptr && CurrentState != GetIntroFocusState())
    {
        if (!SinglePlayer())
            MenuNavSetIntroModeRequest(3);
        bForceIntro = true;
    }
    else
    {
        // a live state runs its slot 14 (+0x38) with (float)dt unless a
        // quit is pending; the field is read again after it.
        if (CurrentState != nullptr)
        {
            if (ClientQuitRequested())
            {
                bForceIntro = true;
            }
            else
            {
                FClientTickScope GameProfile(EClientTickSection::Game);
                NextState = CurrentState->Update(static_cast<float>(DeltaSeconds));
                GameProfile.Stop();
                CurrentState = g_client.current_state;
            }
        }
        // a quit pending now (set before or by the slot 14) forces
        // the intro.
        if (!bForceIntro && ClientQuitRequested())
            bForceIntro = true;
    }

    // state 6 (IntroFocus) and the quit flag cleared.
    if (bForceIntro)
    {
        NextState = 6;
        SetClientQuitRequested(false);
    }

    // a state queued by the routine (the field) wins and is
    // cleared.
    NextState = ConsumePendingClientState(NextState);

    // the field = the routine(NextState, the field).
    if (NextState != 0)
    {
        CurrentState = EnterClientState(NextState, CurrentState);
        g_client.current_state = CurrentState;
    }

    // no state left -> PostQuitMessage(0).
    if (CurrentState == nullptr)
        PostQuitMessage(0);
}

void TickMaterialAnimationPlayback(float DeltaSeconds)
{
    // . No null tests: both NeuronRuntime globals are live from engine init on.
    NeuronRuntime::TextureLib->TickAllPlayback(0, DeltaSeconds);   // slot 0x20 (0, dt)
    NeuronRuntime::MatAnimLibrary->TickAnimationInstances(-1, DeltaSeconds);         // slot 0x18 (-1, dt)
}

void EndClientRenderScene()
{
    EndSceneHalf();
    EndClientProfileRender();
}

void PresentClientRenderFrame()
{
    // every baseline caller sits inside its the routine success branch.
    PresentHalf();
}

bool BeginClientRenderFrame()
{
    // ; the result is the field (G_bRenderFrameBegun).
    BeginClientProfileRender();
    const bool bBegun = TryBeginFrame();
    if (!bBegun)
        EndClientProfileRender();
    return bBegun;
}

void EndClientRenderFrame()
{
    // + the routine back to back.
    EndSceneHalf();
    EndClientProfileRender();
    PresentHalf();
}
