// Author: Giperion.

#include "client_state.h"
#include "oracle_mode.h" // Oracle instrumentation (P0.5)

#include "common_ini.h"
#include "loading_screen.h"
#include "dataconfig_progress.h"
#include "globals.h"
#include "input_manager.h"
#include "login_mgr.h"
#include "menu_enter_state.h"
#include "page_account_select.h"
#include "page_character_create.h"        // PageCharacterCreate (the field)
#include "page_character_select.h"        // PageCharacterSelect (the field)
#include "page_character_select_single.h" // PageCharacterSelectSingle (the field)
#include "page_server_select.h"
#include "page_options.h"
#include "server_connect.h"
#include "type_select.h"
#include "mission_briefing.h"
#include "connect_flow.h"
#include "gameflow.h"
#include "space_state.h"
#include "character_select_equip.h" // GetPlayerBaseIdbaseline (the routine's the field)
#include "main_command_handler.h"   // SetClientQuitRequested (the field)
#include "nn_modal.h"               // G_bNnScreenUp (the field)
#include "player_spawn.h"           // ResetMissionOfferAnswerbaseline (the routine)
#include "fade_to_black.h"          // SetFadeImmediate / SetFadeTarget (the routine / 00412050)
#include "gf_script_object.h"       // G_GfScriptManager (the field)
#include "menu_universe_base.h"     // ResetClientLocationController / EnterClientLocationBase
#include "nav_bar.h"                // NavBarDestroy (the routine)
#include "sound_config.h"           // SetClientAudioMode (the routine)
#include "win3d.h"                  // Win3DFindByName (the routine)

#include "common_universe.h"        // Universe::get_base_id

#include "common_server.h"          // IServer (CharacterSkipAutosave)
#include "common_server_types.h"    // CHARACTER_ID
#include "neuroncomponents.h"                  // FDUMP

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace
{
    // baseline IntroFocus (the field), RTTI .?AUIntroFocus@@ (COL 0x005f04a0): the vftable
    // pointer, then FMenuNavState (menu_enter_state.h). Vftable 0x005e3034, fifteen slots (raw
    // PE dwords; the next dword is string data):
    //   0 0x0043d8f0 (XOR AL,AL / RET 0x10)  1 0x004b50e0 (XOR AL,AL / RET 0xc)  2 0x004b50e0
    //   3 0x0043d8f0  4 0x004b50e0  5 0x00401e90 (RET)  6 0x0053e4a0 (RET 4)
    //   7 the branch (MOV EAX,2)  8 / 9 / 10 0x00401e90  11 the branch (RET 8)
    //   12 the routine  13 the branch  14 the routine
    // Static init the branch: ctor the routine (+0x0c byte 0 -- the CHARACTER_ID ctor,
    // +0x224 = 0 and +0x210 vptr 0x005e2718 -- the Image ctor, then the IntroFocus vptr), atexit
    // -> dtor the routine (the Image dtor on +0x210 only). Both are the
    // compiler-generated special members of this struct.
    struct IntroFocus : public IEnvironment
    {
        FMenuNavState Nav;

        // Slots 0 and 3 (0x0043d8f0): XOR AL,AL / RET 0x10.
        bool Slot00(unsigned A, unsigned Message, unsigned B, unsigned C) override
        {
            (void)A;
            (void)Message;
            (void)B;
            (void)C;
            return false;
        }

        // Slots 1, 2 and 4 (0x004b50e0): XOR AL,AL / RET 0xc.
        bool Slot01(unsigned A, unsigned B, unsigned C) override
        {
            (void)A;
            (void)B;
            (void)C;
            return false;
        }

        bool Slot02(unsigned A, unsigned B, unsigned long Packed) override
        {
            (void)A;
            (void)B;
            (void)Packed;
            return false;
        }

        bool Slot03(unsigned A, unsigned Message, unsigned B, unsigned long Packed) override
        {
            (void)A;
            (void)Message;
            (void)B;
            (void)Packed;
            return false;
        }

        bool Slot04(unsigned A, unsigned B, unsigned C) override
        {
            (void)A;
            (void)B;
            (void)C;
            return false;
        }

        // Slot 5 (0x00401e90): a bare RET.
        void Slot05() override
        {
        }

        // Slot 6 (0x0053e4a0): a bare RET 4.
        void Slot06(unsigned A) override
        {
            (void)A;
        }

        // Slot 7 (the branch): MOV EAX,2 / RET -- the environment type the routine and
        // case 3 test for.
        int GetEnvironmentType() override
        {
            return 2;
        }

        // Slot 8 (0x00401e90): a bare RET (InitializeEngine 0x005b1e04..0x005b1e0d calls it).
        void PreInit() override
        {
        }

        // Slot 9 (0x00401e90): a bare RET (the atexit teardown 0x005b21d2..0x005b21db).
        void Shutdown() override
        {
        }

        // Slot 10 (0x00401e90): a bare RET.
        void Slot10() override
        {
        }

        // Slot 11 (+0x2c) is the branch (.rdata 0x005e3060), the same body as the GF
        // state's: the routine(Width, Height), which reads neither argument. RET 8.
        void Resize(int Width, int Height) override
        {
            (void)Width;
            (void)Height;
            ResetGameflowRoomLetterbox();
        }

        bool Init(int Param) override
        {
            // (RET 4). Param is not read.
            (void)Param;
            ShutdownServerConnection();
            ResetMissionOfferAnswerbaseline(); // (0x00573869)
            G_LoginMgr.Reset();
            Nav.Flag20c = 0;
            Nav.PrevState = 0;

            // latch the field = request the field, request = 0.
            MenuNavConsumeIntroModeRequest();
            int const Latch = MenuNavGetIntroModeLatch();
            if (Latch == 2 || Latch == 4)
            {
                Nav.CurrentState = 0x13; // back from the game: EnterState case 19
            }
            else if (Latch == 3)
            {
                Nav.CurrentState = 5; // PageServerSelect
            }
            else
            {
                // 0 / 1 start at the column; any other latch keeps CurrentState. Both start
                // the single-player session (the routine); failure -> state 13 (quit).
                if (Latch == 0 || Latch == 1)
                    Nav.CurrentState = 1;
                if (!MenuNavStartSinglePlayer(&Nav))
                    Nav.CurrentState = 0xd;
            }

            // +0x228 = 0, Image(+0x210)->slot 1(&the field); the
            // result is not read.
            Nav.ActivePage = nullptr;
            Nav.LogoImage.Load(GetIntroConfigState()->Logo.Text);

            MouseSetCursor("Arrow", true);
            SetClientMouseCursorDrawFlag(1);

            // with bDrawScripts (the field) and latch != 4, the intro
            // base is entered. 0x00573919 sets LocationController+0x5a before any base work;
            // LoadLocationBase reads it through `this+0x5a` at 0x004398a4 and returns before
            // server synchronization or looking up the player's as-yet-unset system id.
            if (GetIntroConfigState()->bDrawScripts && Latch != 4)
            {
                // the field (intro.ini's pick) 1 -> Intro2_Base,
                // 2 -> Intro3_Base, anything else -> Intro1_Base; Universe::get_base_id.
                int const Pick = GetIntroConfigState()->IntroBasePick;
                SetClientLocationNoServerNotify(true); // = 1 (0x00573919)
                char const* BaseNickname = "Intro1_Base";
                if (Pick == 1)
                    BaseNickname = "Intro2_Base";
                else if (Pick == 2)
                    BaseNickname = "Intro3_Base";
                unsigned const BaseId = Universe::get_base_id(BaseNickname);

                Nav.Flag22c = 0;
                Nav.Field230 = 0.0f;
                SetClientAudioMode(3);           // (3) (0x00573961)
                SetFadeImmediate(1.0f);          // (1.0) (0x0057396b)
                // the routine(0, (float)(1.0 / the field)).
                SetFadeTarget(0.0f,
                              static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
                // the routine on the field -- the Gf script manager takes id 0xf.
                G_GfScriptManager.RegisterEvents();
                Nav.Field230 = 0.0f;
                ResetClientLocationController(); // on the field (0x005739a0)
                EnterClientLocationBase(BaseId); // (BaseId) (0x005739ab)
                NavBarDestroy();                 // (0x005739b0)
            }
            return true;
        }

        void Destroy() override
        {
            // .
            MenuEnterStateReleaseActivePage(&Nav); // +0x228 slot 10, +0x228 = 0
            // TypeSelect::Destroy (the routine) inlined.
            GetTypeSelect().Destroy();
            // PageCharacterSelect's slot 10 (the routine) inlined on
            // .
            GetPageCharacterSelect().Destroy();
            // PageCharacterSelectSingle's slot 10 (the branch)
            // inlined on the field (its child walk copies the class name with the routine,
            // the constructor form of the branch's the routine).
            GetPageCharacterSelectSingle().Destroy();
            GetPageCharacterCreate().Destroy(); // on the field (0x00573e6d)
            GetPageServerSelect().Destroy();  // on the field (0x00573e77)
            GetPageAccountSelect().Destroy(); // on the field (0x00573e81)
            GetPageOptions().Destroy(); // on the field (0x00573e8b)
            // the routine inlined on the logo Image -- its BigImage
            // (+0x224) released (slot 2) and cleared.
            Nav.LogoImage.Release();
        }

        int Update(float DeltaSeconds) override
        {
            // the whole IntroFocus frame, the scene step and the draw included.
            // The column's mouse input arrives through the Win3D manager (the routine /
            // ), not through this slot.
            return MenuStateDriverUpdate(&Nav, DeltaSeconds);
        }
    };

    // . The baseline IntroFocus offsets hold on the 32-bit build.
    IntroFocus G_IntroFocus;
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.CurrentState) == 0x04,
                  "IntroFocus +0x04 current state");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.PrevState) == 0x08,
                  "IntroFocus +0x08 previous state");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.CharacterId) == 0x0c,
                  "IntroFocus +0x0c CHARACTER_ID");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.Flag20c) == 0x20c,
                  "IntroFocus +0x20c");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.LogoImage) == 0x210,
                  "IntroFocus +0x210 logo Image");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.ActivePage) == 0x228,
                  "IntroFocus +0x228 active page");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.Flag22c) == 0x22c,
                  "IntroFocus +0x22c");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.Flag22d) == 0x22d,
                  "IntroFocus +0x22d");
    static_assert(sizeof(void*) != 4 || offsetof(IntroFocus, Nav.Field230) == 0x230,
                  "IntroFocus +0x230");

    // The previous state's slot 13 as every case of the routine sends it: TEST ESI,ESI / JZ,
    // then CALL [vftable + 0x34] (e.g. 0x005b2b38..0x005b2b40).
    void DestroyPreviousState(IEnvironment* PreviousState)
    {
        if (PreviousState != nullptr)
            PreviousState->Destroy();
    }

    // the routine (the space state)->slot 12 Init(0); true -> it,
    // false -> 0. Case 2 comes here after the loading screen and the previous state's slot 13;
    // case 7 jumps straight here (0x005b2ae5) -- no second loading screen, the previous state
    // already destroyed.
    IEnvironment* InitSpaceStateTail()
    {
        IEnvironment* const Space = GetSpaceState();
        if (!Space->Init(0))
            return nullptr;
        return Space;
    }

    IEnvironment* EnterInGameState(IEnvironment* PreviousState)
    {
        // the routine(1), the routine -- the loading screen; then
        // the previous state's slot 13 (0x005b2b38..0x005b2b40).
        LoadingScreenSetClear(1);
        LoadingScreenStart();
        DestroyPreviousState(PreviousState);
        return InitSpaceStateTail();
    }

    IEnvironment* EnterMenuState(IEnvironment* PreviousState)
    {
        // case 6 (0x005b2bc7..0x005b2be8): the previous state's slot 13, then
        // (IntroFocus)->slot 12 Init(0) -- its answer is not tested; IntroFocus is
        // returned either way.
        DestroyPreviousState(PreviousState);
        IEnvironment* const Intro = GetIntroFocusState();
        Intro->Init(0);
        return Intro;
    }

    // case 3 (0x005b2b64..0x005b2bc6) -- into GF, the base: the routine returns
    // , whose vftable 0x005cae24 has the COL 0x005e8734 -> .?AUGF@@ (environment
    // type 1). Code 3 is what the routine and the menu driver (0x00573fef) return when the
    // player's base is set; it is not a preferences screen.
    IEnvironment* EnterBaseState(IEnvironment* PreviousState)
    {
        // the routine(1), the routine -- the loading screen
        // (the field).
        LoadingScreenSetClear(1);
        LoadingScreenStart();

        // the routine("NN_Preferences"); none -> the field = 0.
        if (Win3DFindByName("NN_Preferences") == nullptr)
            G_bNnScreenUp = false;

        // , 0x005b2b8b..0x005b2ba4: the flag starts at 1 and drops to 0 unless the
        // previous state's slot 7 (+0x1c) answers 2 (IntroFocus); then the previous state's slot
        // 13. baseline keeps the flag in the low byte of its own PreviousState argument slot and
        // pushes that whole dword (0x005b2bae); GF's Init reads only the low byte (MOV DL,
        // [ESP+4] at 0x004372a0), so the port passes 0 / 1.
        int bFromIntro = 1;
        if (PreviousState != nullptr)
        {
            if (PreviousState->GetEnvironmentType() != 2)
                bFromIntro = 0;
            PreviousState->Destroy();
        }
        // GF->slot 12 (+0x30) Init(bFromIntro); true -> GF is the new
        // state, false -> 0 (GF's Init, the branch, always answers true).
        IEnvironment* const Gameflow = GetGameflowState();
        if (!Gameflow->Init(bFromIntro))
            return nullptr;
        return Gameflow;
    }

    // case 8 -- into MissionBriefing (the routine = the field, COL 0x005e88d4).
    // Code 8 is what the GF base frame answers after the routine (RequestGameflowMissionBriefing).
    // The state is mission_briefing.cpp (vftable 0x005cb094).
    IEnvironment* EnterMissionBriefingState(IEnvironment* PreviousState)
    {
        // the routine(1), the routine -- the loading screen.
        LoadingScreenSetClear(1);
        LoadingScreenStart();
        // the previous state's slot 13.
        DestroyPreviousState(PreviousState);
        // The briefing's update (the branch) draws and presents its own frame, as GF's does.

        // the routine()->slot 12 Init(0); false -> 0.
        IEnvironment* const Briefing = GetMissionBriefingState();
        if (!Briefing->Init(0))
            return nullptr;
        return Briefing;
    }

    // case 7 (0x005b2a53..0x005b2b07) -- restart the single-player session on the
    // character in the field. the routine stores that character and queues state 7
    // (the field = 7); its callers are 0x00488144 and 0x004bc8d5, the load-game paths.
    IEnvironment* EnterLoadGameState(IEnvironment* PreviousState)
    {
        // the routine(1), the routine -- the loading screen.
        LoadingScreenSetClear(1);
        LoadingScreenStart();
        // the previous state's slot 13. On the failure exits below
        // baseline returns this same, already destroyed, state (ESI) -- reproduced.
        DestroyPreviousState(PreviousState);

        // the routine, the routine(&the field), the routine. A
        // failure leaves through caseD_8, which returns the previous state (ESI).
        ShutdownServerConnection();
        if (!ConnectAndLogin(g_display.ini_path.Text))
            return PreviousState;
        if (G_LoginMgr.RequestCharacterInfo() == 0)
            return PreviousState;

        // the routine(&the field); false -> the WARNING FDUMP (main.cpp
        // line 0x6fd), the field = 1, return the previous state.
        if (!G_LoginMgr.SelectCharacterById(G_LoadGameCharacterId))
        {
            FDUMP(0x100002, "%s(%d) : *** WARNING: Selection of character id %s failed\n",
                  "Client\\main.cpp", 0x6fd,
                  G_LoadGameCharacterId.Buffer);
            SetClientQuitRequested(true);
            return PreviousState;
        }

        // the routine (the player's base is set) -> GF->slot 12 Init(1)
        // (true -> GF, false -> 0).
        if ((GetPlayerBaseIdbaseline() & 0x3fffffffu) != 0)
        {
            IEnvironment* const Gameflow = GetGameflowState();
            if (!Gameflow->Init(1))
                return nullptr;
            return Gameflow;
        }

        // -> the branch: in space, the tail case 2 shares -- the routine Init(0).
        // The previous state is already gone and the loading screen already up.
        return InitSpaceStateTail();
    }
}

// the character state 7 selects. Static init the branch zeroes its first byte
// (the CHARACTER_ID ctor); the routine (QueueLoadGameCharacter) copies a character in.
CHARACTER_ID G_LoadGameCharacterId;

// written 7 by the routine only; the frame (0x005b29b3..0x005b29be) takes it
// over the state's own answer and clears it.
int G_PendingClientStateId = 0;

void QueueLoadGameCharacter(CHARACTER_ID const* CharId, bool bSkipAutosave)
{
    // (cdecl): an empty id does nothing; otherwise queue state 7, copy the id
    // (CHARACTER_ID::operator=, COMMON.DLL) and, for the autosave, IServer::CharacterSkipAutosave
    // (+0x60) -- no server test.
    if (CharId->Buffer[0] == 0)
        return;
    G_PendingClientStateId = 7;
    G_LoadGameCharacterId = *CharId;
    if (bSkipAutosave)
        G_Server->CharacterSkipAutosave(GetPlayerConnectionId());
}

int ConsumePendingClientState(int StateId)
{

    if (G_PendingClientStateId != 0)
    {
        StateId = G_PendingClientStateId;
        G_PendingClientStateId = 0;
    }
    return StateId;
}

IEnvironment* EnterClientState(int StateId, IEnvironment* PreviousState)
{
    OracleNoteScreen("client", static_cast<unsigned>(StateId)); // Oracle instrumentation (P0.5)
    // (cdecl). 0x005b2a3c: RefreshLoadingProgress on entry, and again on every
    // way out (each case's tail and the branch).
    RefreshLoadingProgress();

    IEnvironment* Result = PreviousState;
    switch (StateId)
    {
    case 1:
        // caseD_1 (0x005b2c0f..0x005b2c1d): the previous state's slot 13, PostQuitMessage(0),
        // 0 returned.
        DestroyPreviousState(PreviousState);
        PostQuitMessage(0);
        Result = nullptr;
        break;
    case 2:
        Result = EnterInGameState(PreviousState);
        break;
    case 3:
        Result = EnterBaseState(PreviousState);
        break;
    case 4:
        // caseD_4 (0x005b2bfc..0x005b2c0e): GF (the routine)->slot 10 (+0x28), the previous
        // state returned as is. GF's slot 10 is 0x00401e90, a bare RET.
        GetGameflowState()->Slot10();
        break;
    case 5:
        // caseD_5 (0x005b2be9..0x005b2bfb): the space state (the routine)->slot 10 (+0x28), the
        // previous state returned as is. SpaceApp's slot 10 (vftable 0x005e0554 + 0x28 =
        // ) is a bare RET too.
        GetSpaceState()->Slot10();
        break;
    case 6:
        Result = EnterMenuState(PreviousState);
        break;
    case 7:
        Result = EnterLoadGameState(PreviousState);
        break;
    case 8:
        Result = EnterMissionBriefingState(PreviousState);
        break;
    default:
        // DEC / CMP 7 / JA -- any other id returns the previous state.
        break;
    }
    RefreshLoadingProgress();
    return Result;
}

bool IsClientIntroEnvironment()
{
    // the field (current state) non-null && current->slot 7 (+0x1c) == 2 (MOV
    // AL,1); otherwise XOR AL,AL. Only IntroFocus (slot 7 = the branch) answers 2.
    IEnvironment* const Current = g_client.current_state;
    if (Current == nullptr)
        return false;
    return Current->GetEnvironmentType() == 2;
}

IEnvironment* GetIntroFocusState()
{
    // MOV EAX,0x0067a598 (the IntroFocus singleton); RET.
    return &G_IntroFocus;
}

void CallClientStateUpdateSlot(float DeltaSeconds)
{
    // (and 00474209 / 00474384, 0056385b..00563872): ECX = the field;
    // non-null -> CALL [vftable + 0x38] with the frame time as a float, the result ignored.
    IEnvironment* const Current = g_client.current_state;
    if (Current != nullptr)
        Current->Update(DeltaSeconds);
}
