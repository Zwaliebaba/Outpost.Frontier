// Author: Giperion.

// The MissionBriefing client state (the field, vftable 0x005cb094). See mission_briefing.h.

#include "mission_briefing.h"

#include "best_path.h"              // BestPathGetWaypointCount (the routine), G_BestPaths (the field)
#include "character_select_equip.h" // SetPlayerBaseIdbaseline (the routine), GetPlayerStarSystemIdbaseline
#include "chat_init.h"              // ChatIsInputOpen (the routine)
#include "client_frame_tick.h"      // / the routine / the routine
#include "gameflow.h"               // / the field / the field
#include "input_manager.h"          // MouseSetCursor (the routine), SetClientMouseCursorDrawFlag
#include "loading_screen.h"         // LoadingScreenReset (the routine)
#include "menu_sound_arch.h"        // TickClientSoundMixer (the routine), SetClientBackgroundMusic
#include "menu_ui_init.h"           // IsAnyUiSoundPlaying (the routine)
#include "nn_modal.h"               // NnSystemInit / NnSystemShutdown (the routine / the routine)
#include "nn_static_control.h"      // StaticControl::Init (the routine)
#include "nn_story_star.h"          // StoryStarReset (the routine)
#include "win3d.h"                  // Win3DDrawAll (the routine), Win3DDeleteTree (the routine)
#include "win3d_render.h"           // ClearWin3DBuffersToColor (the routine)
#include "win_factory.h"            // WinFactoryCreate (the routine)

#include "common_universe.h"        // Universe::get_system
#include "neuroncomponents.h"                  // FDUMP

#include <windows.h>

namespace
{
    // .rdata 0x005cb0d0 -- the only mesh the branch hands the routine.
    char const kBriefingWindowMesh[] = "Interface\\NeuroNet\\Inventory\\repairblackbox.3db";

    // RTTI .?AUMissionBriefing@@ (COL 0x005e88d4), vftable 0x005cb094: fifteen slots, raw PE
    // dwords at 0x005cb094..0x005cb0cc (the next dword is string data):
    //   0 the branch  1 the branch  2 the branch  3 the branch  4 the branch
    //   5 0x00401e90    6 0x0053e4a0    7 the branch  8 the branch  9 the branch
    //   10 0x00401e90   11 0x0053aaa0   12 the branch 13 the branch 14 the branch
    struct MissionBriefing : public IEnvironment
    {
        bool Slot00(unsigned A, unsigned Message, unsigned B, unsigned C) override;
        bool Slot01(unsigned A, unsigned B, unsigned C) override;
        bool Slot02(unsigned A, unsigned B, unsigned long Packed) override;
        bool Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed) override;
        bool Slot04(unsigned A, unsigned B, unsigned C) override;
        void Slot05() override;
        void Slot06(unsigned A) override;
        int GetEnvironmentType() override;
        void PreInit() override;
        void Shutdown() override;
        void Slot10() override;
        void Resize(int Width, int Height) override;
        bool Init(int Param) override;
        void Destroy() override;
        int Update(float DeltaSeconds) override;

        // Its static initializer the branch only stamps the vptr; the rest is .bss zero until
        // slot 8 runs.
        unsigned char bFirstFramePending = 0; // +0x04
        Win3D* ParentWindow = nullptr;        // +0x08 "BriefingParentWindow"
        // +0x0c / +0x0d / +0x10: G_bMissionBriefingNavMapUp, G_bMissionBriefingContinue and
        // (gameflow.cpp).
        unsigned char bIdle = 0;              // +0x14
    };

    // The frame both halves of the branch draw (0x0043d381..0x0043d3bc and 0x0043d4c5..
    // ): the routine(0, Dt), then with a scene begun the routine({0, 0, 0, 0xff}),
    // , the routine, the routine.
    void DrawBriefingFrame(float DeltaSeconds)
    {
        TickClientSoundMixer(nullptr, DeltaSeconds);
        if (!BeginClientRenderFrame())
            return;
        unsigned char const Black[4] = {0, 0, 0, 0xff};
        ClearWin3DBuffersToColor(Black);
        Win3DDrawAll();
        EndClientRenderScene();
        PresentClientRenderFrame();
    }

    // The exit both the branch and the branch take (0x0043d312..0x0043d337 /
    // ): idle, first frame pending again, no continue request, the window
    // hidden, the player's base cleared (the routine(&0)) and the NN system torn down.
    void LeaveBriefing(MissionBriefing* State)
    {
        State->bIdle = 1;
        State->bFirstFramePending = 1;
        G_bMissionBriefingContinue = 0;
        State->ParentWindow->Flags = static_cast<unsigned char>(State->ParentWindow->Flags & 0xfc);
        SetPlayerBaseIdbaseline(0);
        NnSystemShutdown();
    }

    // Slot 0 (the branch, RET 0x10) -- a key message (A, Message, B, C). Unless the chat
    // input "CTID" is up (the routine), GetKeyState(VK_SHIFT) and GetKeyState(VK_CONTROL) with
    // their results dropped; true either way.
    bool MissionBriefing::Slot00(unsigned A, unsigned Message, unsigned B, unsigned C)
    {
        (void)A;
        (void)Message;
        (void)B;
        (void)C;
        if (!ChatIsInputOpen())
        {
            GetKeyState(VK_SHIFT);
            GetKeyState(VK_CONTROL);
        }
        return true;
    }

    // Slots 1, 2 and 4 (the branch, MOV AL,1 / RET 0xc) -- true.
    bool MissionBriefing::Slot01(unsigned A, unsigned B, unsigned C)
    {
        (void)A;
        (void)B;
        (void)C;
        return true;
    }

    bool MissionBriefing::Slot02(unsigned A, unsigned B, unsigned long Packed)
    {
        (void)A;
        (void)B;
        (void)Packed;
        return true;
    }

    bool MissionBriefing::Slot04(unsigned A, unsigned B, unsigned C)
    {
        (void)A;
        (void)B;
        (void)C;
        return true;
    }

    // Slot 3 (the branch, MOV AL,1 / RET 0x10) -- true.
    bool MissionBriefing::Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed)
    {
        (void)A;
        (void)Message;
        (void)B;
        (void)Packed;
        return true;
    }

    // Slots 5 and 10 (0x00401e90) -- a bare RET.
    void MissionBriefing::Slot05()
    {
    }

    void MissionBriefing::Slot10()
    {
    }

    // Slot 6 (0x0053e4a0, RET 4) -- a bare RET.
    void MissionBriefing::Slot06(unsigned A)
    {
        (void)A;
    }

    // Slot 7 (the branch) -- 3, the environment type (GF answers 1, IntroFocus 2).
    int MissionBriefing::GetEnvironmentType()
    {
        return 3;
    }

    // Slot 8 (the branch).
    void MissionBriefing::PreInit()
    {
        MissionBriefing* const State = this;
        // .
        State->bFirstFramePending = 1;
        G_bMissionBriefingNavMapUp = 0;
        G_bMissionBriefingContinue = 0;
        SetGameflowLocationBase(0);
        State->bIdle = 1;
        // +0x08 = the routine("BriefingParentWindow", "StaticControl",
        // 0, 0); null skips the rest (JZ 0x0043d253).
        State->ParentWindow = WinFactoryCreate("BriefingParentWindow", "StaticControl", nullptr,
                                               nullptr);
        if (State->ParentWindow == nullptr)
            return;
        // the routine with three zeroed vectors -- Offset (local +0x1c),
        // HalfExtents (+0x10), StartYpr (+0x04) -- no hardpoint, and the bytes NoZEnable 0,
        // ZClear 0, NoMeshRender 0, NoWireRender 1, NoColoring 0, RenderToWindow 0,
        // NoClearRender 1, ForceRadius 0, RotationSpeed 0, EnableRotation 0.
        float const Offset[3] = {0.0f, 0.0f, 0.0f};
        float const HalfExtents[3] = {0.0f, 0.0f, 0.0f};
        float const StartYpr[3] = {0.0f, 0.0f, 0.0f};
        static_cast<NnStaticControl*>(State->ParentWindow)
            ->Init(kBriefingWindowMesh, Offset, nullptr, HalfExtents, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0,
                   StartYpr);
        // slot 5 SetPosition({-0.62f, 0.3f, -1.25f}).
        float const Position[3] = {-0.62f, 0.3f, -1.25f}; // / 0x3e99999a / 0xbfa00000
        State->ParentWindow->SetPosition(Position);
        // hidden until the first frame.
        State->ParentWindow->Flags = static_cast<unsigned char>(State->ParentWindow->Flags & 0xfc);
    }

    // Slot 9 (the branch) -- the routine's body on the window (children first, next sibling
    // read before each recursion, then slot 10 and the deleting destructor), then +0x08 = 0.
    void MissionBriefing::Shutdown()
    {
        if (ParentWindow == nullptr)
            return;
        Win3DDeleteTree(ParentWindow);
        ParentWindow = nullptr;
    }

    // Slot 11 (0x0053aaa0, RET 8) -- a bare RET.
    void MissionBriefing::Resize(int Width, int Height)
    {
        (void)Width;
        (void)Height;
    }

    // Slot 12 (the branch, RET 4) -- +0x14 = 0, true. The argument is not read.
    bool MissionBriefing::Init(int Param)
    {
        (void)Param;
        bIdle = 0;
        return true;
    }

    // Slot 13 (the branch).
    void MissionBriefing::Destroy()
    {
        MissionBriefing* const State = this;
        // only while the briefing is up (first frame done) and the
        // window exists.
        if (State->bFirstFramePending != 0 || State->ParentWindow == nullptr)
            return;
        // .
        LeaveBriefing(State);
        // .
        StoryStarReset();
        // the window's first child -- the NeuroNetNavMap -- gets slot 23
        // (0xef02, 0, 0), the close.
        Win3D* const NavMap = State->ParentWindow->GetChildHead();
        if (NavMap != nullptr)
            NavMap->Slot23(0xef02, 0, 0);
    }

    // Slot 14 (the branch, RET 4). EBX = 0, EBP = DeltaSeconds throughout.
    int MissionBriefing::Update(float DeltaSeconds)
    {
        MissionBriefing* const State = this;

        // FCOMP 0.0, TEST AH,0x44, JNP -- the jump (to the frame) is
        // taken only when DeltaSeconds equals 0 (C3 alone; unordered sets C3 and C2 and falls
        // through). Otherwise the frame is drawn only with the continue request set.
        if (DeltaSeconds == 0.0f || G_bMissionBriefingContinue != 0)
        {
            DrawBriefingFrame(DeltaSeconds);
            // with the continue request up, wait while a UI sound plays.
            if (G_bMissionBriefingContinue != 0 && IsAnyUiSoundPlaying())
                return 0;
        }

        // .
        if (State->bIdle != 0)
            return 0;

        if (State->bFirstFramePending != 0)
        {
            // no route, an empty route list, or a route whose Text
            // MadLibId (node +0x3c) is 1 or 2 -- no briefing: the routine and back to space.
            if (BestPathGetWaypointCount() == 0)
            {
                StoryStarReset();
                return 2;
            }
            if (G_BestPaths.empty())
            {
                StoryStarReset();
                return 2;
            }
            unsigned const MadLibId = G_BestPaths.begin()->second.Text.MadLibId;
            if (MadLibId == 2 || MadLibId == 1)
            {
                StoryStarReset();
                return 2;
            }

            // +0x04 = 0, the routine(0) (no NN_MainButton).
            State->bFirstFramePending = 0;
            NnSystemInit(false);
            // the window shown.
            State->ParentWindow->Flags = static_cast<unsigned char>(State->ParentWindow->Flags | 3);
            // the routine(&+0x10).
            SetPlayerBaseIdbaseline(GetGameflowLocationBase());
            // the nav map inside the window, built with +0x0c = 1 (the
            // nav map's Create reads it through the routine), then slot 23 (0xef01, 0, 0) and
            // +0x6c |= 3. No null test on the create.
            G_bMissionBriefingNavMapUp = 1;
            Win3D* const NavMap = WinFactoryCreate("NeuroNetNavMap", "NeuroNetNavMap",
                                                   State->ParentWindow, nullptr);
            NavMap->Slot23(0xef01, 0, 0);
            NavMap->Flags = static_cast<unsigned char>(NavMap->Flags | 3);
            G_bMissionBriefingNavMapUp = 0;
            // the routine("arrow", 0), the routine(1).
            MouseSetCursor("arrow", false);
            SetClientMouseCursorDrawFlag(1);
            // the player's system's [Music] space track (ISystem slot 21)
            // through the routine(&id, 1, -1.0f); no system, no music change.
            Universe::ISystem const* const System =
                Universe::get_system(GetPlayerStarSystemIdbaseline());
            if (System != nullptr)
                SetClientBackgroundMusic(System->GetSpaceMusic(), 1, -1.0f);
        }

        // .
        DrawBriefingFrame(DeltaSeconds);

        if (G_bMissionBriefingContinue == 0)
        {
            // the routine -- the loading screen's timer reset (the field).
            LoadingScreenReset();
            // the routine(1).
            SetClientMouseCursorDrawFlag(1);
            return 0;
        }

        // the nav map asked to continue -- leave, the routine, 2.
        LeaveBriefing(State);
        StoryStarReset();
        return 2;
    }

    // . Its static initializer the branch only stamps the vptr 0x005cb094; the
    // rest is .bss zero until slot 8 runs.
    MissionBriefing G_MissionBriefing;
}

IEnvironment* GetMissionBriefingState()
{
    // MOV EAX,0x00668774.
    return &G_MissionBriefing;
}
