// Author: Giperion.

// gf\Gameflow.cpp (the GF state, the field, vftable 0x005cae24) and the supt\Support.cpp
// functions it drives (0x00558850..0x005592aa). See gameflow.h.

#include "gameflow.h"
#ifdef OUTPOST_FRONTIER_WALK_MODE
#include "base_walk.h"
#endif

#include "character_info.h"         // LoadCharacterInfo / PreloadCharacterScripts / ...
#include "chat_init.h"              // ChatShowMsgWin (the routine)
#include "character_select_equip.h" // GetPlayerBaseIdbaseline / SetPlayerBaseIdbaseline / ...
#include "client_cliobj.h"          // Archetype::ReleaseMaterialEntrySlot (the routine)
#include "client_frame_tick.h"      // / the routine / the routine, the routine
#include "event_handler.h"
#include "fade_to_black.h"
#include "flash_light.h"            // LoadFlashlightTextures / ReleaseFlashlightTextures
#include "gf_character_object.h"    // ReleaseSharedCharacterLod (the routine)
#include "gf_hotspot.h"
#include "gf_script_object.h"       // G_GfScriptManager (the field)
#include "globals.h"
#include "hud_command_dispatch.h"   // HudCommandRecord (the routine)
#include "input_manager.h"
#include "loading_screen.h"         // / the routine
#include "main_command_handler.h"   // ClientQuitRequested (the routine)
#include "menu_sound_arch.h"        // TickClientSoundMixer (the routine)
#include "menu_ui_widgets.h"
#include "menu_universe_base.h"     // the LocationController (the field)
#include "nn_arrival_text.h"        // NnArrivalTextShowBase (the routine)
#include "nn_modal.h"               // NnSystemInit / NnSystemShutdown (the routine / the routine)
#include "room_camera.h"             // FRoomCamera (the routine's receiver)
#include "room_control.h"
#include "sound_config.h"           // SetClientAudioMode (the routine)
#include "space_letterbox.h"
#include "win3d.h"                  // Win3DDrawAll (the routine)
#include "win3d_immediate.h"
#include "win3d_render.h"

#include "common_newchar.h"         // RtcSlider
#include "common_misc.h"            // SinglePlayer
#include "neuroncomponents.h"                  // FDUMP
#include "NeuronRuntime.h"                  // NeuronRuntime::Engine (IEngine::ReleaseHandle, G_InvalidEngineHandle)

#include <math.h>
#include <stddef.h>
#include <windows.h>

// baseline runs this file's static initializers before Win3D.cpp's, and the mouse depends on it.
// _initterm(0x0060f000, 0x006101bc) (0x005b8153) walks the table upwards; the branch (the
// hotspot handler below, the field, registers USER_MOVE 5 / USER_BUTTON0 6) sits at
// , the branch (the Win3D input handler the field, 5, 4, 6..13, 1, 2) at
// . the routine calls the handlers of one id in reverse registration order
// (005764b9..005764d6), so Win3D, registered second, sees every mouse event first and a click
// on a window (Win3DPick -> MouseEvent) never reaches a room hotspot. MSVC orders the default
// (user) segment by object-file link order, which put win3d.obj first and let a character's
// hotspot take clicks through the NeuroNet windows; the lib segment runs ahead of every user
// initializer whatever the link order. Nothing constructed here reads another file's dynamic
// initializer: the HUD registry is a pointer allocated on first insert (EnsureHudCmdRegistry),
// GF only stamps its vptr, the instance cache fills constants.
#pragma warning(push)
#pragma warning(disable : 4073) // initializers put in library initialization area
#pragma init_seg(lib)
#pragma warning(pop)

namespace
{
    char const kGameflowCpp[] = "Client\\gf\\Gameflow.cpp";
    char const kSupportCpp[] = "Client\\supt\\Support.cpp";

    // ------------------------------------------------------------------------------------
    // supt\Support.cpp globals.
    // ------------------------------------------------------------------------------------
    bool G_bGameflowMissionBriefing = false;   // the base frame answers 8
    bool G_bGameflowStateFive = false;         // the base frame answers 5, once
    bool G_bGameflowQuit = false;              // answers 1; only ever zeroed
    float G_GameflowMsRemainder = 0.0f;        // the tick's sub-millisecond rest
    // the arrival caption's delay in milliseconds. Zeroed by the routine before
    // it is read there; its only other writer is 0x00454f0e (SetGameflowArrivalDelayMs).
    unsigned G_GameflowArrivalDelayMs = 0;
    unsigned G_GameflowHoldUntilFrame = 0;     // the routine

    // / the field -- the last cursor point the tracking saw (the branch, a
    // static initializer, zeroes both).
    int G_HotspotLastCursor[2] = {0, 0};
    bool G_bHotspotTracking = false;
    FHotspot* G_HoveredHotspot = nullptr;
    int G_HotspotInputLock = 0;                // the routine / the routine

    // the routine's store; the MissionBriefing state's +0x10, which its update
    // hands to the routine by address (0x0043d440 LEA ECX,[ESI+0x10]).
    unsigned G_GameflowLocationBase = 0;

    // 's and the routine's shared first half, and the routine's body: find the
    // hotspot under Point and move the hover to it -- the old one gets bHovered = 0 and a leave
    // (2), the new one bHovered = 1 and an enter (1), both with Point.
    void MoveHotspotHover(int const* Point)
    {
        FHotspot* Hit = nullptr;
        FindHotspotAt(Point, &Hit);
        if (Hit == G_HoveredHotspot)
            return;
        if (G_HoveredHotspot != nullptr)
        {
            G_HoveredHotspot->bHovered = 0;
            G_HoveredHotspot->SendEvent(2, Point);
        }
        G_HoveredHotspot = Hit;
        if (Hit != nullptr)
        {
            Hit->bHovered = 1;
            G_HoveredHotspot->SendEvent(1, Point);
        }
    }

    // (thiscall on an 8-byte point, RET 4): a packed mouse lParam into
    // {low word, high word}, both zero-extended.
    void UnpackCursorPoint(int* Out, unsigned long Packed)
    {
        Out[0] = static_cast<int>(Packed & 0xffffu);
        Out[1] = static_cast<int>(Packed >> 16);
    }

    // (cdecl) -- the tracking for a mouse move. Locked -> false. Otherwise record
    // Point, move the hover, and answer whether something is hovered.
    bool TrackHotspotMouseMove(int const* Point)
    {
        if (G_HotspotInputLock != 0)
            return false;
        G_bHotspotTracking = true;
        G_HotspotLastCursor[0] = Point[0];
        G_HotspotLastCursor[1] = Point[1];
        MoveHotspotHover(Point);
        return G_HoveredHotspot != nullptr;
    }

    // (cdecl) -- the tracking for a mouse button message. As the routine, then
    // the hovered hotspot (if any) gets event 3 for WM_LBUTTONDOWN (0x201) and -1 (which
    // ignores) for anything else; true when there was a hovered hotspot.
    bool TrackHotspotMouseButton(unsigned Message, int const* Point)
    {
        if (G_HotspotInputLock != 0)
            return false;
        G_bHotspotTracking = true;
        G_HotspotLastCursor[0] = Point[0];
        G_HotspotLastCursor[1] = Point[1];
        MoveHotspotHover(Point);
        int Event = -1;
        if (Message == 0x201)
            Event = 3;
        if (G_HoveredHotspot == nullptr)
            return false;
        G_HoveredHotspot->SendEvent(Event, Point);
        return true;
    }

    // (cdecl) -- once a frame from the routine: while tracking and not locked,
    // re-test the last cursor point (the room may have moved under a still mouse).
    void RefreshHoveredHotspot()
    {
        if (!G_bHotspotTracking)
            return;
        if (G_HotspotInputLock != 0)
            return;
        G_bHotspotTracking = true;
        MoveHotspotHover(G_HotspotLastCursor);
    }

    // the Support file's Event::Handler (vftable 0x005e1494, one slot
    // ). Static initializer the branch: the routine registers USER_MOVE (5) and
    // USER_BUTTON0 (6) through the routine; atexit the branch -> thunk_FUN_00576010.
    class FGameflowHotspotHandler : public EventHandler
    {
    public:
        FGameflowHotspotHandler()
        {
            // the two ids from a stack array, registered in order.
            static unsigned const kIds[2] = {5, 6};
            for (unsigned Index = 0; Index < 2; ++Index)
                RegisterEvent(kIds[Index]);
        }

        // (thiscall, RET 4).
        bool OnEvent(unsigned MsgId) override
        {
            // the software cursor the field / the field.
            int Point[2] = {0, 0};
            int Wheel = 0;
            GetClientSoftwareCursor(&Point[0], &Point[1], &Wheel);

            if (MsgId == 5)
                return OnMove(Point);
            if (MsgId == 6)
                return OnButton(Point);
            return false;
        }

    private:
        // (USER_MOVE).
        bool OnMove(int const* Point)
        {
            bool bHovering = false;
            if (G_HotspotInputLock == 0)
            {
                G_HotspotLastCursor[0] = Point[0];
                G_HotspotLastCursor[1] = Point[1];
                G_bHotspotTracking = true;
                MoveHotspotHover(Point);
                bHovering = G_HoveredHotspot != nullptr;
                if (bHovering)
                    return true;
            }
            // tracking off; a hovered hotspot (only possible here when
            // locked) gets bHovered = 0 and a leave with point {0, 0}.
            G_bHotspotTracking = false;
            if (G_HoveredHotspot != nullptr)
            {
                int const Zero[2] = {0, 0};
                G_HoveredHotspot->bHovered = 0;
                G_HoveredHotspot->SendEvent(2, Zero);
            }
            G_HoveredHotspot = nullptr;
            return bHovering;
        }

        // (USER_BUTTON0).
        bool OnButton(int const* Point)
        {
            // only a button-down record (+0x18), and not while locked.
            if (HudCommandRecord()->Byte18 == 0)
                return false;
            if (G_HotspotInputLock != 0)
                return false;
            G_HotspotLastCursor[0] = Point[0];
            G_HotspotLastCursor[1] = Point[1];
            G_bHotspotTracking = true;
            FHotspot* Hit = nullptr;
            FindHotspotAt(Point, &Hit);
            if (Hit != G_HoveredHotspot)
            {
                if (G_HoveredHotspot != nullptr)
                {
                    G_HoveredHotspot->bHovered = 0;
                    G_HoveredHotspot->SendEvent(2, Point);
                }
                G_HoveredHotspot = Hit;
                // nothing under the cursor -> false.
                if (Hit == nullptr)
                    return false;
                Hit->bHovered = 1;
                G_HoveredHotspot->SendEvent(1, Point);
            }
            // the hovered hotspot is clicked.
            if (G_HoveredHotspot == nullptr)
                return false;
            G_HoveredHotspot->SendEvent(3, Point);
            return true;
        }
    };

    FGameflowHotspotHandler G_GameflowHotspotHandler;

    // (cdecl, double) -- the base's world tick.
    void TickGameflow(double DeltaSeconds)
    {
        // the field + Dt, split into whole milliseconds (floor of
        // the sum * 1000.0) and the rest (the sum rounded to float, less ms * 0.001).
        double const Sum = static_cast<double>(G_GameflowMsRemainder) + DeltaSeconds;
        float const SumAsFloat = static_cast<float>(Sum);
        double const Milliseconds = floor(Sum * 1000.0);
        G_GameflowMsRemainder =
            static_cast<float>(static_cast<double>(SumAsFloat) - Milliseconds * 0.001);
        // the routine(ftol(ms)) on the field -- the Gf script
        // manager's tick (sequences advanced, finished ones deleted, the script fade stepped).
        G_GfScriptManager.Update(static_cast<int>(Milliseconds));

        float const Dt = static_cast<float>(DeltaSeconds);
        // the routine on the field.
        TickClientLocationController(Dt);

#ifdef OUTPOST_FRONTIER_WALK_MODE
        BaseWalkTick(Dt);
#endif

        // every RoomObject whose +0x04 is clear gets slot 5 (Dt).
        std::list<FRoomObject*>& Objects = GetRoomObjectList();
        for (std::list<FRoomObject*>::iterator It = Objects.begin(); It != Objects.end(); ++It)
        {
            FRoomObject* const Object = *It;
            if (Object->bNoUpdate == 0)
                Object->Update(DeltaSeconds);
        }

#ifdef OUTPOST_FRONTIER_WALK_MODE
        BaseWalkAfterWorldUpdate();
#endif

        // the routine(the routine(), Dt) -- the letterbox update for
        // the room camera (the room-camera twin of the routine, which the space camera uses).
        UpdateRoomLetterbox(
            static_cast<FRoomCamera*>(static_cast<ICamera*>(GetClientRoomCamera())), Dt);
        UpdateFade(Dt);
        RefreshHoveredHotspot();

        // with the letterbox target and amount and the fade target and
        // level all exactly 0.0 (each FCOMP 0.0 / TEST AH,0x44 / JP out -- only an equal compare
        // falls through), a current room runtime whose +0x1338 is clear, and the software cursor
        // not drawn: draw it again.
        if (!(GetLetterboxTarget() == 0.0f))
            return;
        if (!(GetLetterboxAmount() == 0.0f))
            return;
        if (!(GetFadeTarget() == 0.0f))
            return;
        if (!(GetFadeLevel() == 0.0f))
            return;
        // the routine's lookup through the controller's static +0x38.
        FRoomRuntime* const Runtime = static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime());
        if (Runtime == nullptr)
            return;
        if (RoomControlGetField1338(Runtime) != 0)
            return;
        if (GetClientMouseCursorDrawFlag() != 0)
            return;
#ifdef OUTPOST_FRONTIER_WALK_MODE
        if (BaseWalkIsLooking())
            return;
#endif
        SetClientMouseCursorDrawFlag(1);
    }

    // (cdecl) -- the base frame: tick, draw, sound, and the next state code.
    int UpdateGameflowBaseFrame()
    {
        // the capped frame time, zeroed while the frame counter has not
        // passed the field (CMP / JA keeps it only when count > hold, unsigned).
        double Dt = GetClientFrameTimeCapped();
        if (!(GetClientFrameCount() > G_GameflowHoldUntilFrame))
            Dt = 0.0;

        // the routine holds the world -> only the hover is dropped.
        if (!ShouldSkipLocalServerTick())
        {
            TickGameflow(Dt);
            TickMaterialAnimationPlayback(static_cast<float>(Dt));
        }
        else
        {
            ReleaseHoveredHotspot();
        }

        // the scene.
        if (BeginClientRenderFrame())
        {
            Win3DSetViewport(nullptr, kSupportCpp, 0x222);
            Win3DSetPipelineClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            ClearWin3DBuffers(kWin3DClearColour | kWin3DClearDepth); // (0x4100)
            DrawClientLocation();                           // on the field
            ClearWin3DBuffers(kWin3DClearDepth);            // (0x100)
            Win3DSetDepthFunc(4);
            Win3DDrawAll();
            if (!IsClientServerTickPaused())
                DrawFade(static_cast<ICamera*>(GetClientRoomCamera()));
            EndClientRenderScene();
            PresentClientRenderFrame();
        }

        // the routine -- the loading screen's timer reset (the field..e0).
        LoadingScreenReset();

        // the routine(3), the routine(room camera, Dt).
        SetClientAudioMode(3);
        TickClientSoundMixer(GetClientRoomCamera(), static_cast<float>(Dt));

        // .
        int Result = 0;
        if (G_bGameflowMissionBriefing)
            Result = 8;
        else
            Result = G_bGameflowQuit ? 1 : 0;
        if (G_bGameflowStateFive)
        {
            G_bGameflowStateFive = false;
            Result = 5;
        }
        return Result;
    }

    // --------------------------------------------------------------------------------------
    // The GF state object (the field), RTTI .?AUGF@@ (COL 0x005e8734), vftable 0x005cae24:
    // fifteen slots, raw PE dwords at 0x005cae24..0x005cae5c (the next dword, 0x25287325, is
    // string data):
    //   0 the branch  1 the branch  2 the branch  3 the branch  4 the branch
    //   5 the branch  6 0x0053e4a0    7 the branch  8 the branch  9 the branch
    //   10 0x00401e90   11 the branch 12 the branch 13 the routine 14 the branch
    // --------------------------------------------------------------------------------------
    struct GF : public IEnvironment
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

        // Its static initializer the branch only stamps the vptr; the three bytes start at 0
        // (.bss).
        unsigned char bFirstFramePending = 0; // +0x04
        unsigned char bBaseEntered = 0;       // +0x05
        unsigned char bFromIntro = 0;         // +0x06
    };

    // Slot 0 (+0x00, the branch, RET 0x10) -- a key message (A, Message, B, C). With the
    // "CTID" window up (the routine) it is consumed at once. Otherwise every arm of the switch
    // on Message - 0x100 (0x100/0x104 and 0x101/0x105 ask the routine -- `XOR AL,AL / RET`,
    // always false -- and fall through; 0x102/0x103 and anything else go straight there) ends in
    // GetKeyState(VK_SHIFT), GetKeyState(VK_CONTROL), their results dropped, and true.
    bool GF::Slot00(unsigned A, unsigned Message, unsigned B, unsigned C)
    {
        (void)A;
        (void)Message;
        (void)B;
        (void)C;
        if (Win3DFindByName("CTID") != nullptr)
            return true;
        GetKeyState(VK_SHIFT);
        GetKeyState(VK_CONTROL);
        return true;
    }

    // Slot 1 (+0x04, the branch, RET 0xc) -- the routine(A, B, C): false.
    bool GF::Slot01(unsigned A, unsigned B, unsigned C)
    {
        (void)A;
        (void)B;
        (void)C;
        return false;
    }

    // Slot 2 (+0x08, the branch, RET 0xc) -- a mouse move (A, B, Packed): the point unpacked
    // (the routine zeroes it, the routine fills it), the routine, and true whatever it said.
    bool GF::Slot02(unsigned A, unsigned B, unsigned long Packed)
    {
        (void)A;
        (void)B;
        int Point[2] = {0, 0};
        UnpackCursorPoint(Point, Packed);
        TrackHotspotMouseMove(Point);
        return true;
    }

    // Slot 3 (+0x0c, the branch, RET 0x10) -- a mouse button (A, Message, B, Packed):
    // (Message, point) is the answer.
    bool GF::Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed)
    {
        (void)A;
        (void)B;
        int Point[2] = {0, 0};
        UnpackCursorPoint(Point, Packed);
        return TrackHotspotMouseButton(Message, Point);
    }

    // Slot 4 (+0x10, the branch, shared with MissionBriefing, RET 0xc) -- true.
    bool GF::Slot04(unsigned A, unsigned B, unsigned C)
    {
        (void)A;
        (void)B;
        (void)C;
        return true;
    }

    // Slot 5 (+0x14, the branch) -- JMP the routine.
    void GF::Slot05()
    {
        ReleaseHoveredHotspot();
    }

    // Slot 6 (+0x18, 0x0053e4a0) -- a bare RET 4 (the ICF-folded body IDPMsgHandler::OnAddHost
    // shares; that name says nothing about GF).
    void GF::Slot06(unsigned A)
    {
        (void)A;
    }

    // Slot 7 (+0x1c, the branch) -- 1, the environment type the routine and the sound frame
    // read (IntroFocus answers 2).
    int GF::GetEnvironmentType()
    {
        return 1;
    }

    void GF::PreInit()
    {
        // the routine(the field), then +0x04 = +0x05 = 0.
        InitGameflowData();
        bFirstFramePending = 0;
        bBaseEntered = 0;
    }

    void GF::Shutdown()
    {
        // JMP the routine.
        ShutdownGameflow();
    }

    // Slot 10 (+0x28, 0x00401e90) -- a bare RET. Code 4 of the state machine calls it.
    void GF::Slot10()
    {
    }

    // Slot 11 (+0x2c, the branch, RET 8) -- the routine(Width, Height), which reads neither.
    void GF::Resize(int Width, int Height)
    {
        (void)Width;
        (void)Height;
        ResetGameflowRoomLetterbox();
    }

    bool GF::Init(int Param)
    {
        // (RET 4): +0x04 = 1, +0x05 = 0, +0x06 = the argument's low byte (MOV DL,
        // [ESP+4]); true. the routine case 3 pushes a dword whose low byte it set to 1 and
        // maybe cleared (0x005b2b75 / 0x005b2b9b) -- only that byte is read here.
        bFirstFramePending = 1;
        bBaseEntered = 0;
        bFromIntro = static_cast<unsigned char>(Param);
        return true;
    }

    void GF::Destroy()
    {
        // the routine, the routine, +0x04 = 0.
        LeaveGameflowBase();
        // the routine -- the NN system teardown.
        NnSystemShutdown();
        bFirstFramePending = 0;
    }

    int GF::Update(float DeltaSeconds)
    {
        // (RET 4). The argument is not read.
        (void)DeltaSeconds;
        GF* const State = this;
        if (State->bFirstFramePending == 1)
        {
            // the first frame enters the base.
            State->bFirstFramePending = 0;
            RefreshLoadingProgress();
            // the routine(1) -- the NN system (NN_MainButton and the voice init).
            NnSystemInit(true);
            RefreshLoadingProgress();

            unsigned const Base = GetPlayerBaseIdbaseline();        // *the routine()
            unsigned Location = GetPlayerFactionScratchbaseline();
            if (!SinglePlayer() && Location != 0)
            {
                FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid non-empty location %d in multiplayer\n",
                      kGameflowCpp, 0xa7, Location);
                Location = 0;
            }
            if ((Base & 0x3fffffffu) != 0)
                EnterGameflowBase(Base, Location, State->bFromIntro != 0);
            RefreshLoadingProgress();
            State->bBaseEntered = 1;
            if (!SinglePlayer() && !ClientQuitRequested())
            {
                // the routine(1) -- the field = 1 and the chat window
                // 's +0x6c |= 3.
                ChatShowMsgWin(true);
            }
            RefreshLoadingProgress();
        }

        // .
        if (!ClientQuitRequested() && State->bBaseEntered == 1)
            return UpdateGameflowBaseFrame();

        // a black frame -- the routine({0, 0, 0, 0xff}) outside the
        // scene, then the scene with the Win3D roots unless the client is quitting.
        unsigned char const Black[4] = {0, 0, 0, 0xff};
        ClearWin3DBuffersToColor(Black);
        if (BeginClientRenderFrame())
        {
            if (!ClientQuitRequested())
                Win3DDrawAll();
            EndClientRenderScene();
            PresentClientRenderFrame();
        }
        // the routine -- the loading screen's timer reset.
        LoadingScreenReset();
        return 0;
    }

    // . Its static initializer the branch only stamps the vptr 0x005cae24.
    GF G_Gameflow;
    static_assert(sizeof(void*) != 4 || offsetof(GF, bFirstFramePending) == 0x04, "GF +0x04");
    static_assert(sizeof(void*) != 4 || offsetof(GF, bFromIntro) == 0x06, "GF +0x06");

    // (0x804 bytes). See ResetEngineInstanceCache in gameflow.h.
    struct FEngineInstanceCache
    {
        // (.CRT entry 0x0060f1d0): every handle -1 and every entry null
        // (0x00434927 / 0x0043492d, 0x100 slots), Count (the field) 0, then atexit of
        // the destructor below.
        FEngineInstanceCache()
        {
            for (int Index = 0; Index < 0x100; ++Index)
            {
                Handles[Index] = G_InvalidEngineHandle;
                Entries[Index] = nullptr;
            }
            Count = 0;
        }

        // the routine's body on this table.
        ~FEngineInstanceCache()
        {
            Reset();
        }

        // listing 004348b0..00434916. Count is re-read after every release
        // (0x004348e2); the handle goes to IEngine slot 15 without a -1 test.
        void Reset()
        {
            for (int Index = 0; Index < Count; ++Index)
            {
                Archetype::ReleaseMaterialEntrySlot(&Entries[Index]);
                NeuronRuntime::Engine->ReleaseHandle(Handles[Index]);         // vt+0x3c
            }
            for (int Index = 0; Index < 0x100; ++Index)
            {
                Handles[Index] = G_InvalidEngineHandle;
                Entries[Index] = nullptr;
            }
            Count = 0;
        }

        void* Handles[0x100];                         // +0x000
        Archetype::FCliMaterialEntry* Entries[0x100]; // +0x400 (the field)
        int Count;                                    // +0x800 (the field)
    };

    FEngineInstanceCache G_EngineInstanceCache;
}

void ResetEngineInstanceCache()
{
    // (thiscall, ECX = 0x00667e50 at every call site).
    G_EngineInstanceCache.Reset();
}

IEnvironment* GetGameflowState()
{
    // MOV EAX,0x006686a0.
    return &G_Gameflow;
}

void InitGameflowData()
{
    // . 0x00558850: IDPMsgHandler::OnDisconnect (ICF-folded bare RET) -- nothing.
    BuildClientLocationMaps();   // on the field
    LoadCharacterInfo();
    PreloadCharacterScripts();   // JMP the routine
}

bool ShutdownGameflow()
{
    // .
    ShutdownCharacterInfo();
    ClearClientLocationMaps();   // on the field
    ClearRoomObjectList();
    // the routine on the field -- the engine-instance cache (per
    // slot the routine on the +0x400 material entry and IEngine slot 15 on the instance, then
    // every slot reset).
    ResetEngineInstanceCache();
    return ReportLeakedHotspots(); // JMP the routine
}

bool EnterGameflowBase(unsigned BaseId, unsigned LocationId, bool bFromIntro)
{
    // .
    // the routine -- the loading screen update (the routine(-1.0f) on
    // , the field = 1).
    LoadingScreenStart();
    ResetClientLocationController();         // on the field
    SetClientLocationInTransition();          // (controller +0x59) = 1
    G_GameflowArrivalDelayMs = 0;
    SetPlayerBaseIdbaseline(BaseId);            // (&BaseId)
    SetPlayerFactionScratchbaseline(LocationId); // (LocationId)
    SetGameflowLocationBase(BaseId);
    LoadFlashlightTextures();
    // the routine(2) -- the sound list the field walked by category 2.
    PreloadClientSoundArchesByCategory(2);
    RefreshLoadingProgress();
    SetClientAudioMode(3);                    // (3)
    RefreshLoadingProgress();
    SetFadeImmediate(1.0f);                   // (1.0f)
    // the routine(0, (float)(1.0 / the field)).
    SetFadeTarget(0.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
    RefreshLoadingProgress();
    ResetHotspotTracking();
    MouseSetCursor("arrow", false);           // ("arrow", 0)
    RefreshLoadingProgress();
    // the routine on the field -- the Gf script manager's Event::Handler
    // registers id 0xf (the routine).
    G_GfScriptManager.RegisterEvents();
    G_GameflowMsRemainder = 0.0f;
    RefreshLoadingProgress();
    G_bGameflowMissionBriefing = false;
    G_bGameflowStateFive = false;
    G_bGameflowQuit = false;
    RoomControlSetLandingPending(bFromIntro ? 0 : 1);
    // the field = 1 -- the LocationController's this+0x58 (0x00668708 + 0x58),
    // which the next the routine reads (0x0043bb13) and clears (0x0043bb7d).
    SetClientLocationKeepAudio();
    RefreshLoadingProgress();

    // .
    if (LocationId == 0)
    {
        EnterClientLocationBase(BaseId);
    }
    else
    {
        SetClientLocationBase(BaseId);
        // (room audio) then the routine (the location).
        ApplyClientLocationAudio(LocationId);
        ChangeClientLocation(LocationId);
    }
    RefreshLoadingProgress();

    // the arrival caption, unless the room runtime's +0x517 is set or
    // the base was entered from the intro. A runtime whose the routine is false zeroes the
    // delay first.
    FRoomRuntime* const Runtime = static_cast<FRoomRuntime*>(GetClientCurrentLocationRuntime());
    bool const bShowCaption = !bFromIntro;
    if ((Runtime == nullptr || RoomControlGetField517(Runtime) == 0) && bShowCaption)
    {
        if (Runtime != nullptr && !RoomControlHasLandingScript(Runtime))
            G_GameflowArrivalDelayMs = 0;
        // FILD qword {the field, 0} (unsigned), FMUL float 0.001f, FSTP float.
        float const DelaySeconds = static_cast<float>(
            static_cast<double>(G_GameflowArrivalDelayMs) * static_cast<double>(0.001f));
        NnArrivalTextShowBase(DelaySeconds);
    }
    RefreshLoadingProgress();
    SetClientFpsCap(RtcSlider::get_fps_cap());
    return true;
}

void RequestGameflowMissionBriefing()
{
    // .
    SetFadeImmediate(1.0f);
    G_bGameflowMissionBriefing = true;
}

void LeaveGameflowBase()
{
#ifdef OUTPOST_FRONTIER_WALK_MODE
    BaseWalkLeaveBase();
#endif
    // .
    SetClientFpsCap(RtcSlider::get_default_fps_cap());
    LeaveClientLocationBase();                // on the field
    // the routine on the field -- every script sequence deleted, id 0xf
    // unregistered.
    G_GfScriptManager.Shutdown();
    // the routine -- the shared character LOD set (the field) released and
    // cleared.
    ReleaseSharedCharacterLod();
    ResetCharacterScripts();
    // the routine -- every sound stopped, the live-source leak warning.
    StopAllClientSounds();
    // the routine(2) (the the field sound list by category).
    UnloadClientSoundArchesByCategory(2);
    ReleaseFlashlightTextures();
    ResetEngineInstanceCache();               // the routine on the field
    SetPlayerBaseIdbaseline(0);                 // (&0)
    SetPlayerFactionScratchbaseline(0);         // (0)
}

void SetGameflowArrivalDelayMs(unsigned DelayMs)
{
    G_GameflowArrivalDelayMs = DelayMs;
}

void HoldGameflowTime()
{
    // .
    G_GameflowHoldUntilFrame = GetClientFrameCount() + 2;
}

void RequestGameflowStateFive()
{
    // .
    G_bGameflowStateFive = true;
}

void SetGameflowLocationBase(unsigned BaseId)
{
    // .
    G_GameflowLocationBase = BaseId;
}

unsigned GetGameflowLocationBase()
{
    // as the branch reads it (MissionBriefing state +0x10).
    return G_GameflowLocationBase;
}

unsigned char G_bMissionBriefingNavMapUp = 0;
unsigned char G_bMissionBriefingContinue = 0;

unsigned char IsMissionBriefingNavMapUp()
{
    // .
    return G_bMissionBriefingNavMapUp;
}

void RequestMissionBriefingContinue()
{
    // .
    G_bMissionBriefingContinue = 1;
}

void ResetGameflowRoomLetterbox()
{
    // .
    void* const Camera = GetClientRoomCamera();
    if (Camera == nullptr)
        return;
    ForgetLetterboxCamera();
    // the routine(Camera, 0) -- PUSH 0 is the float 0.0f.
    UpdateRoomLetterbox(static_cast<FRoomCamera*>(static_cast<ICamera*>(Camera)), 0.0f);
}

void ResetHotspotTracking()
{
    // .
    G_bHotspotTracking = false;
    G_HoveredHotspot = nullptr;
    G_HotspotInputLock = 0;
}

void ClearHoveredHotspot()
{
    // .
    G_HoveredHotspot = nullptr;
}

void ClearHoveredHotspotIf(FHotspot* Hotspot)
{
    // .
    if (Hotspot == G_HoveredHotspot)
        G_HoveredHotspot = nullptr;
}

void ReleaseHoveredHotspot()
{
    // .
    G_bHotspotTracking = false;
    if (G_HoveredHotspot != nullptr)
    {
        int const Zero[2] = {0, 0};
        G_HoveredHotspot->bHovered = 0;
        G_HoveredHotspot->SendEvent(2, Zero);
    }
    G_HoveredHotspot = nullptr;
}

void LockHotspotInput()
{
    // .
    G_HotspotInputLock = 1;
}

void UnlockHotspotInput()
{
    // .
    G_HotspotInputLock = 0;
}
