// Author: Giperion.

#include "menu_enter_state.h"

#include "character_select_equip.h"
#include "client_frame_tick.h" // / the routine / the routine, the routine
#include "chat_init.h"    // ChatShowMsgWin (the routine)
#include "chat_msg_win.h" // ChatCreateMsgWin (the routine)
#include "connect_flow.h"
#include "connect_lists.h" // PreConnectReset (the routine)
#include "neuroncomponents.h" // FDUMP
#include "dataconfig_progress.h" // GetIntroConfigState (bDrawScripts, the field)
#include "fade_to_black.h" // SetFadeImmediate / SetFadeTarget (the routine / the routine)
#include "font_imp.h"
#include "gf_script_object.h" // G_GfScriptManager (the field)
#include "globals.h"
#include "menu_universe_base.h" // LeaveClientLocationBase (the routine)
#include "ids_strings.h"
#include "input_manager.h"
#include "loading_screen.h"
#include "login_mgr.h"
#include "menu_sound_arch.h" // TickClientSoundMixer (the routine)
#include "nn_preferences.h"  // NnPreferences::bClosed (+0x32c)
#include "nn_static_control.h"
#include "nn_text_control.h"
#include "page_account_select.h" // PageAccountSelect (the field)
#include "page_character_create.h" // PageCharacterCreate (the field)
#include "page_character_select.h" // PageCharacterSelect (the field), G_NoCharacterId
#include "page_character_select_single.h" // PageCharacterSelectSingle (the field)
#include "page_server_select.h"  // PageServerSelect (the field)
#include "page_options.h"
#include "player_spawn.h" // ResetMissionOfferAnswerbaseline (the routine)
#include "room_camera.h"  // FRoomCamera (the routine's receiver)
#include "room_control.h" // GetRoomObjectList (the routine), FRoomObject
#include "server_connect.h"
#include "sound_config.h" // SetClientAudioMode (the routine)
#include "space_letterbox.h" // UpdateRoomLetterbox (the routine)
#include "text_win.h"
#include "type_select.h"
#include "ui_page.h"
#include "ui_page_desc.h"
#include "win3d.h"           // Win3DDrawAll (the routine)
#include "win3d_engine_submit.h"
#include "win3d_immediate.h" // Win3DSetViewport / Win3DSetPipelineClearColor
#include "win3d_render.h"    // ClearWin3DBuffers (the routine)
#include "win_factory.h"
#include "wincontrol_base.h"

#include "common_net.h" // CPlayerAccount (cases 2 and 4)
#include "common_misc.h" // SinglePlayer, SetMultiPlayer, GetFullFilename

#include <math.h>
#include <process.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#include <list>

namespace
{
    // the the routine source name of the driver's frame (checked in PE bytes).
    char const kIntroCpp[] = "Client\\Interface\\Intro.cpp";

    // EnterState case 1 column: x = -0.33 (PUSH 0xbea8f5c3), y = the field - i *
    // , z = -1 (PUSH 0xbf800000) -- 0x00574651..0x00574670.
    float const kMainColumnPosX = -0.330000013f;
    float const kMainColumnPosZ = -1.0f;
    float const kMainColumnPosY0 = 0.155000001f;
    float const kMainColumnPosYStep = 0.081500001f;
    float const kMainColumnBoxWidth = 0.25f;     // PUSH 0x3e800000 before each the routine
    float const kFontLineScale = 0.002f;

    // The "Version" TextControl position, 0x00574857..0x00574861: PUSH 0xbf800000 / 0xbeb33333 /
    // into the routine.
    float const kVersionPosX = 0.319999993f;
    float const kVersionPosY = -0.349999994f;
    float const kVersionPosZ = -1.0f;

    char const kFrontButtonMesh[] = "Interface\\NeuroNet\\Inventory\\FRONT_button.cmp";
    char const kShortOptionsBox[] = "Interface\\NeuroNet\\Inventory\\FRONT_shortoptions_box.cmp";
    char const kInfoWindowMesh[] = "Interface\\LoadSave\\FRONT_infowindow.cmp";

    char const kPageServerSelect[] = "Interface\\intro\\PAGE_ServerSelect.page";
    char const kPageAccountSelect[] = "Interface\\intro\\PAGE_AccountSelect.page";
    char const kPageCharacterSelect[] = "Interface\\intro\\PAGE_CharacterSelect.page";
    char const kPageCharacterCreate[] = "Interface\\intro\\PAGE_CharacterCreate.page";
    char const kPageCharSelectSingle[] = "Interface\\intro\\PAGE_CharSelectSingle.page";

    // The five TextMsgButtons: caption IDS and slot-23 command (0x005746a2..0x005747d2).
    struct FMainColumnSlot
    {
        unsigned LabelIds;
        int Command;
    };

    FMainColumnSlot const kMainColumnSlots[5] = {
        {0x4f4u, 0x10},
        {0x4f5u, 0x03},
        {0x4f6u, 0x04},
        {0x4f8u, 0x0a},
        {0x4f9u, 0x0b},
    };

    // The page fields the Update poll reads are the ported pages' own: PageCharacterSelect's
    // Result / SelectedCharacter (+0x2b4 / +0x399, the field / the field) and
    // PageCharacterSelectSingle's (+0xb4 / +0x118, the field / the field).

    // request / the field latch (menu Init / case19).
    int GIntroModeRequest = 0;
    int GIntroModeLatch = 0;

    // set by case 5, cleared when state 5 is left.
    unsigned char GServerSelectActive = 0;
    // a Win3D the driver deletes when state 10 is left while it is inactive.
    // baseline has no writer of a non-null value: a raw scan of the whole PE finds exactly three
    // references, all in the routine (0x00573f02 / 0x00573f18 reads, 0x00573f28 = 0), and no
    // instruction takes its address. It stays null, as here.
    Win3D* GOptionsChild_67c20c = nullptr;

    // is G_NoCharacterId (page_character_select.h); the field is
    // PageCharacterSelect::CurrentCharacter.

    // The intro base teardown the driver inlines three times -- poll index 3 0x0057404e..
    // , poll index 8 0x005740df..0x0057412c, EnterState case 12 0x00574a8b..0x00574ad8:
    // on the field, the routine on the field, the routine(&0),
    // (0), the routine(1.0), the routine(0, (float)(1.0 / the field)),
    // then the field = LocationController+0x5a = 0. LoadLocationBase reads that byte
    // through its `this` pointer at 0x004398a4.
    void LeaveIntroBase()
    {
        LeaveClientLocationBase();
        G_GfScriptManager.Shutdown();
        SetPlayerBaseIdbaseline(0);
        SetPlayerFactionScratchbaseline(0);
        SetFadeImmediate(1.0f);
        SetFadeTarget(0.0f, static_cast<float>(1.0 / static_cast<double>(GetFadeDuration())));
        SetClientLocationNoServerNotify(false);
    }

    // Every intro PAGE screen is its own static object now (the field / 67aa10 / 67b180 /
    // 67a890 / 67ae10 / 67a7d0); the per-attach IntroPageHost the port used to allocate is gone
    // with the last of them (PageCharacterCreate, case 14).
    void ReleaseActivePageInternal(FMenuNavState* Nav)
    {
        // baseline (e.g. 0x00574fbe..0x00574fcf): if (+0x228) { vtable+0x28(); +0x228 = 0 } --
        // `this` is never tested.
        if (Nav->ActivePage != nullptr)
        {
            Nav->ActivePage->Destroy();   // slot 10 (+0x28)
            Nav->ActivePage = nullptr;
        }
    }

    // Case 18 (0x00574fbe..0x0057529b; analysis leaves the range uninspected, decoded from the
    // raw bytes). The five chrome statics are members of the page object PageCharacterSelectSingle
    // (the field): +0xf4..+0xfc the three "bImg", +0x100 "bigBox", +0x104 "smallBox" -- the
    // absolute stores the field..the field (0x00574ffe, 0x0057508e, 0x00575108, 0x0057518a,
    // ; the page's own methods read them through `this`).
    struct FCase18Static
    {
        char const* Nickname;
        char const* Mesh;
        float Offset[3];
    };

    FCase18Static const kCase18Statics[5] = {
        {"bImg", kFrontButtonMesh, {-0.34799999f, 0.19499999f, -1.0f}},          // /3e47ae14/bf800000
        {"bImg", kFrontButtonMesh, {-0.34900001f, 0.11050000f, -1.0f}},          // /3de24dd3/bf800000
        {"bImg", kFrontButtonMesh, {-0.34900001f, 0.029999999f, -1.0f}},         // /3cf5c28f/bf800000
        {"bigBox", kShortOptionsBox, {0.019999999f, -0.019999999f, -0.98000002f}}, // /bca3d70a/bf7ae148
        {"smallBox", kInfoWindowMesh, {-0.34999999f, -0.19699999f, -1.0f}},      // /be49ba5e/bf800000
    };

    // The page member each static is stored into (page +0xf4 / +0xf8 / +0xfc / +0x100 / +0x104).
    Win3D*& Case18StaticSlot(PageCharacterSelectSingle& Page, int Index)
    {
        if (Index < 3)
            return Page.ButtonImages[Index];
        if (Index == 3)
            return Page.BigBox;
        return Page.SmallBox;
    }

    // , before the page is loaded: each static is the routine(name,
    // "StaticControl", page, 0) then StaticControl::Init the routine(mesh, &Offset, 0,
    // &{0,0,0}, NoZEnable 0, ZClear 0, NoMeshRender 0, NoWireRender 1, NoColoring 1,
    // RenderToWindow 0, NoClearRender 1, ForceRadius 0, RotationSpeed 0, EnableRotation 0,
    // &{0,0,0}) -- the zero vectors are the routine(0, 0, 0) / three MOV 0 (the first). The three
    // "bImg" skip the Init on a null create (JE 0x00575072 / 0x005750ec / 0x00575166); "bigBox"
    // and "smallBox" have no null test. Every entry into case 18 builds them again (baseline keeps
    // no latch; the page's slot 10 on the way out destroys the old ones as its children).
    void EnterCase18Chrome(PageCharacterSelectSingle& Page)
    {
        float const Zero[3] = {0.0f, 0.0f, 0.0f};
        for (int Index = 0; Index < 5; ++Index)
        {
            FCase18Static const& Slot = kCase18Statics[Index];
            Win3D* const Created = WinFactoryCreate(Slot.Nickname, "StaticControl", &Page, nullptr);
            Case18StaticSlot(Page, Index) = Created;
            if (Index < 3 && Created == nullptr)
                continue;
            static_cast<NnStaticControl*>(Created)->Init(Slot.Mesh, Slot.Offset, nullptr, Zero, 0, 0,
                                                         0, 1, 1, 0, 1, 0, 0, 0, Zero);
        }
    }
}

void MenuEnterStateReleaseActivePage(FMenuNavState* Nav)
{
    ReleaseActivePageInternal(Nav);
}

bool MenuNavStartSinglePlayer(FMenuNavState* Nav)
{
    // (thiscall on the IntroFocus).
    SetMultiPlayer(false);
    Nav->Flag22d = 0;
    // the routine -- the chat MsgWin tree destroyed.
    ChatDestroyMsgWin();
    // the three std::wstring assigns on the login context.
    G_LoginMgr.LoginName = L"SinglePlayer";  // +0x188 (0x00573a60)
    G_LoginMgr.LoginPassword = L"";          // +0x198 (0x00573ae5)
    G_LoginMgr.ServerPassword = L"";         // +0x130 (0x00573b47)
    if (!ConnectToServer(g_display.ini_path.Text)) // (the field)
        return false;
    if (!LoginToServer())
        return false;
    G_LoginMgr.Reset();
    G_LoginMgr.ReloadFactions(); // (0x00573bce): the NewChar faction records
    return G_LoginMgr.RequestCharacterInfo() != 0;
}

namespace
{
    // EnterState case 1 (0x005743f8..0x00574908).
    void EnterCase1(FMenuNavState* Nav)
    {
        G_LoginMgr.DestroyGun();

        if (Nav->Flag22d != 0)
        {
            // back from multiplayer -- restart the single-player
            // session. The body is the routine inlined after the Init prologue calls.
            ShutdownServerConnection();
            ResetMissionOfferAnswerbaseline(); // (0x00574417)
            G_LoginMgr.Reset();
            if (!MenuNavStartSinglePlayer(Nav))
                Nav->CurrentState = 0xd;
        }

        SetClientMouseCursorDrawFlag(1); // (1)
        if (Nav->ActivePage != nullptr)
        {
            Nav->ActivePage->Destroy(); // slot 10
            Nav->ActivePage = nullptr;
        }

        TypeSelect& Column = GetTypeSelect();
        Column.NextState = 0;
        Column.bAnimating = 0;
        Column.PendingCommand = 0;
        Column.Create(nullptr, nullptr);      // slot 9 (0, 0)

        // the caption box height is the "Normal" font's line height
        // (slot 6, unsigned) times 0.002.
        IFont* const NormalFont = FindMenuFontImp("Normal");
        float const BoxHeight =
            static_cast<float>(static_cast<unsigned>(NormalFont->GetLineHeight())) *
            kFontLineScale;

        // five "bImg" StaticControls, FRONT_button.cmp, one per row.
        for (int Index = 0; Index < 5; ++Index)
        {
            Win3D* const Created = WinFactoryCreate("bImg", "StaticControl", &Column, nullptr);
            Column.Statics[Index] = Created;
            if (Created == nullptr)
                continue;
            float const PosY =
                kMainColumnPosY0 - static_cast<float>(Index) * kMainColumnPosYStep;
            // (bytes analysis left uninspected): the routine(
            // FRONT_button.cmp, &Vector(-0.33, y, -1) (the routine), hardpoint 0,
            // &Vector(0,0,0) half extents, NoZEnable 0, ZClear 0, NoMeshRender 0, NoWireRender 1,
            // NoColoring 1, RenderToWindow 0, NoClearRender 1, ForceRadius 0, RotationSpeed 0,
            // EnableRotation 0, &{0,0,0} (+0x70..+0x78 zeroed at 0x00574634..0x00574644)).
            float const Offset[3] = {kMainColumnPosX, PosY, kMainColumnPosZ};
            float const ZeroVector[3] = {0.0f, 0.0f, 0.0f};
            static_cast<NnStaticControl*>(Created)->Init(kFrontButtonMesh, Offset, nullptr,
                                                         ZeroVector, 0, 0, 0, 1, 1, 0, 1, 0, 0,
                                                         0, ZeroVector);
        }

        // the captions, the routine(the field, ids, buf, 0x40),
        // then the routine(&TypeSelect, buf, 0.25, height, command).
        for (int Index = 0; Index < 5; ++Index)
        {
            wchar_t Caption[0x40];
            GetIdsStringWWithRawFallback(kMainColumnSlots[Index].LabelIds, Caption, 0x40);
            Column.Buttons[Index].Init(&Column, Caption, kMainColumnBoxWidth, BoxHeight,
                                       kMainColumnSlots[Index].Command);
        }
        // .
        for (int Index = 0; Index < 5; ++Index)
        {
            Column.Buttons[Index].FieldE8 = 1;
            Column.Buttons[Index].SetFlagsE4E5(1, 1);
        }
        Column.AnimTime = -2.0; // /e4 = 0 / 0xc0000000

        // the "Version" TextControl.
        Win3D* const VersionWin = WinFactoryCreate("Version", "TextControl", &Column, nullptr);
        Column.Version = VersionWin;
        if (VersionWin != nullptr)
        {
            // the routine("Normal", &(0.32, -0.35, -1), no hardpoint,
            // width 0.25, height = the caption box height (ESI), Arg6 1.0, no ids, +0x358 = 1,
            // +0x344 = 0, +0x378 = 0) -- the text comes from slot 42 below.
            NnTextControl* const VersionText = static_cast<NnTextControl*>(VersionWin);
            float const VersionPos[3] = {kVersionPosX, kVersionPosY, kVersionPosZ};
            VersionText->Init("Normal", VersionPos, nullptr, kMainColumnBoxWidth, BoxHeight, 1.0f,
                              0u, 1, 0, 0);

            wchar_t Format[0x100];
            GetIdsStringWWithRawFallback(0x4f7u, Format, 0x100);
            // the word at the field (the high half of
            // , g_client.packed_version), its high byte then its low byte.
            unsigned const VersionWord =
                (static_cast<unsigned>(g_client.packed_version) >> 16) & 0xffffu;
            wchar_t Text[0x100];
            swprintf(Text, 0x100, Format, (VersionWord >> 8) & 0xffu, VersionWord & 0xffu);
            // slot 42 (+0xa8) with property 0x1d: set the text. TODO(UI_CTRL): the port's
            // WinControl::SetProperty handles 1..11 only, so the text is not shown yet.
            VersionText->SetProperty(0x1d, reinterpret_cast<uintptr_t>(Text), 0);
        }

        Column.RegisterEvent(0xf); // on +0x7c, USER_CANCEL
        Nav->ActivePage = &Column;
        // CHARACTER_ID::operator=(&the field, &the field).
        GetPageCharacterSelect().CurrentCharacter = G_NoCharacterId;
    }
}

void EnterMenuState(FMenuNavState* Nav, int State)
{
    // (thiscall, RET 4; module storage..0x005743f7 plus case 1 up to 0x0057490c lie
    // outside the analysis inventory -- implementation notes). No test of `this`.
    // sprintf("EnterState %d\n") into a local nobody reads, then
    // RefreshLoadingProgress.
    RefreshLoadingProgress();

    switch (State)
    {
    case 1:
        EnterCase1(Nav);
        break;

    case 2:
        // 0x574caf: CALL Common::SinglePlayer; if true → Current=18; else LAN/account path.
        if (SinglePlayer())
        {
            Nav->CurrentState = 18;
        }
        else
        {
            // the routine -- the loading screen runs.
            LoadingScreenStart();
            // a CPlayerAccount on the stack (EH state 4, destroyed at
            // on both arms). LoadAccount's result is not tested (0x00574ce3);
            // only VerifyAccount decides (JZ 0x00574f47 -> state 5).
            CPlayerAccount Account;
            Account.LoadAccount();
            if (!Account.VerifyAccount())
            {
                Nav->CurrentState = 5;
                break;
            }
            // CLoginMgr +0x1a8 (the field) = 1 -- LoginToServer sends it.
            G_LoginMgr.bCreateAccount = 1;
            // the login name is the account's signature of the current
            // server's +0x64 string (the routine's copy, EH state 5; null _Ptr -> the empty
            // string), rebuilt from its c-string (EH state 7) and assigned to +0x188
            // (the field). The three temporaries die in reverse order (0x00574dd3 temp,
            // signature, 0x00574dfe..0x00574e7b the FServerInfo copy).
            G_LoginMgr.LoginName = std::wstring(
                Account.GetServerSignature(G_LoginMgr.GetCurrentServer().Field_0x64.c_str())
                    .c_str());
            // +0x198 (the field) = L"" (the field), through a
            // temporary (EH state 8).
            G_LoginMgr.LoginPassword = std::wstring(L"");
            // the routine; true -> the routine and state 9, false ->
            // state 4.
            if (LoginToServer())
            {
                G_LoginMgr.Reset();
                Nav->CurrentState = 9;
            }
            else
            {
                Nav->CurrentState = 4;
            }
        } // ~CPlayerAccount
        break;

    case 3:
        // 0x574962: mov [ebp+4], 2
        Nav->CurrentState = 2;
        break;

    case 4:
        // .
        ShutdownServerConnection();
        ResetMissionOfferAnswerbaseline(); // (0x00574973)
        G_LoginMgr.Reset();
        SetMultiPlayer(true);
        Nav->Flag22d = 1;
        ChatCreateMsgWin();              // (0x00574994), the chat MsgWin
        Nav->CurrentState = 5;
        {
            // a CPlayerAccount on the stack (EH state 3); no account
            // (LoadAccount false, JZ 0x005749d1) or a failed check (VerifyAccount false) ->
            // state 6, the account page.
            CPlayerAccount Account;
            if (!Account.LoadAccount() || !Account.VerifyAccount())
                Nav->CurrentState = 6;
        } // ~CPlayerAccount
        // the routine -- the loading screen runs.
        LoadingScreenStart();
        break;

    case 5:
    {
        // CHARACTER_ID::operator= into the field (PageCharacterSelect
        // +0x8c) from the field.
        GetPageCharacterSelect().CurrentCharacter = G_NoCharacterId;
        // the routine -- the loading screen runs; 0x00574b63: the routine.
        LoadingScreenStart();
        ShutdownServerConnection();
        // the active page closed (slot 10), the software cursor drawn
        // (the routine(1)), the chat window hidden in multiplayer (the routine(0)).
        ReleaseActivePageInternal(Nav);
        SetClientMouseCursorDrawFlag(1);
        if (!SinglePlayer())
            ChatShowMsgWin(false);
        // unless the server page is already the active one: the server
        // lists reset (the routine), the Gun made (the routine), the (again closed) active
        // page replaced by PageServerSelect (the field) -- its descriptor (+0x8c,
        // ) loaded with the routine, then slot 9 Create(0, 0) -- and
        // = 1. A failed load goes back to state 1 (the branch).
        PageServerSelect& ServerPage = GetPageServerSelect();
        if (Nav->ActivePage == &ServerPage)
            break;
        G_LoginMgr.ResetServerLists();
        G_LoginMgr.CreateGun();
        ReleaseActivePageInternal(Nav);
        if (!ServerPage.Desc.LoadFromFile(GetFullFilename(kPageServerSelect)))
        {
            Nav->CurrentState = 1;
            break;
        }
        ServerPage.Create(nullptr, nullptr);
        Nav->ActivePage = &ServerPage;
        GServerSelectActive = 1; // = 1 (0x00574c0e)
        break;
    }

    case 6:
    {
        // the active page closed (slot 10), the software cursor drawn
        // (the routine(1)), the chat window hidden in multiplayer (the routine(0)); unless the
        // account page is already the active one, the (again closed) active page is replaced by
        // PageAccountSelect (the field): its descriptor (+0x8c, the field) loaded with
        // , then slot 9 Create(0, 0). A failed load goes back to state 1
        // (the branch).
        ReleaseActivePageInternal(Nav);
        SetClientMouseCursorDrawFlag(1);
        if (!SinglePlayer())
            ChatShowMsgWin(false);
        PageAccountSelect& AccountPage = GetPageAccountSelect();
        if (Nav->ActivePage == &AccountPage)
            break;
        ReleaseActivePageInternal(Nav);
        if (!AccountPage.Desc.LoadFromFile(GetFullFilename(kPageAccountSelect)))
        {
            Nav->CurrentState = 1;
            break;
        }
        AccountPage.Create(nullptr, nullptr);
        Nav->ActivePage = &AccountPage;
        break;
    }

    case 7:
    case 11:
    case 13:
        // 0x57533d epilogue only — no body.
        break;

    case 8:
    case 15:
        // 0x5752d4: mov [ebp+4], 9
        Nav->CurrentState = 9;
        break;

    case 9:
    {
        // the player lists reset (the routine), the active page closed
        // (slot 10, +0x228 = 0), the chat window hidden in multiplayer (the routine(0)), then
        // PageCharacterSelect (the field): its descriptor (+0x28c, the field) loaded with
        // , slot 9 Create(0, 0), +0x228 = the page. A failed load goes back to
        // state 1 (the branch).
        PreConnectReset();
        ReleaseActivePageInternal(Nav);
        if (!SinglePlayer())
            ChatShowMsgWin(false);
        PageCharacterSelect& CharacterPage = GetPageCharacterSelect();
        if (!CharacterPage.Desc.LoadFromFile(GetFullFilename(kPageCharacterSelect)))
        {
            Nav->CurrentState = 1;
            break;
        }
        CharacterPage.Create(nullptr, nullptr);
        Nav->ActivePage = &CharacterPage;
        break;
    }

    case 10:
        // release +0x228, PageOptions (the field) slot 9 (0, 0),
        // +0x228 = &PageOptions.
        ReleaseActivePageInternal(Nav);
        GetPageOptions().Create(nullptr, nullptr);
        Nav->ActivePage = &GetPageOptions();
        break;

    case 12:
        // .
        {
            // the routine(1), the routine -- the loading screen.
            LoadingScreenSetClear(1);
            LoadingScreenStart();
            // with bDrawScripts (the field) and latch != 4, the intro
            // base IntroFocus::Init entered is left.
            if (GetIntroConfigState()->bDrawScripts && GIntroModeLatch != 4)
                LeaveIntroBase();
            bool bSelected = false;
            if (Nav->Flag20c != 0)
            {
                // the routine(&+0xc).
                bSelected = G_LoginMgr.SelectCharacterById(Nav->CharacterId);
            }
            else
            {
                // the routine; a hit is copied (0x200 bytes) to a
                // local and handed to the routine; no autosave leaves the result false.
                CCharacterSet::CharacterInfo const* const AutoSave =
                    G_LoginMgr.FindAutoSaveCharacter();
                if (AutoSave != nullptr)
                {
                    CHARACTER_ID const Local = AutoSave->Id;
                    bSelected = G_LoginMgr.SelectCharacterById(Local);
                }
            }
            RefreshLoadingProgress();
            if (bSelected)
            {
                G_LoginMgr.DestroyGun();
                Nav->CurrentState = 0x14;
            }
            else
            {
                Nav->CurrentState = 5;
            }
        }
        break;

    case 14:
    {
        // release +0x228 (slot 10), GetFullFilename(PAGE_CharacterCreate),
        // on the field; false -> CurrentState 9, true -> +0x228 = the page.
        ReleaseActivePageInternal(Nav);
        PageCharacterCreate& CreatePage = GetPageCharacterCreate();
        if (!CreatePage.Load(GetFullFilename(kPageCharacterCreate)))
        {
            Nav->CurrentState = 9;
            break;
        }
        Nav->ActivePage = &CreatePage;
        break;
    }

    case 16:
        // New Game. the routine (the Restart.fl record the
        // single-player session listed); none -> FDUMP and back to state 1; else its id into
        // +0xc (CHARACTER_ID::operator=), Flag20c = 1, CurrentState = 0xc.
        {
            CCharacterSet::CharacterInfo const* const Restart = G_LoginMgr.FindRestartCharacter();
            if (Restart == nullptr)
            {
                FDUMP(0x100001, "%s(%d) : *** ERROR: %s",
                      "Client\\Interface\\Intro.cpp", 0x1d95,
                      "New game character file not found\n");
                Nav->CurrentState = 1;
            }
            else
            {
                Nav->CharacterId = Restart->Id;
                Nav->Flag20c = 1;
                Nav->CurrentState = 0xc;
            }
        }
        break;

    case 17:
        // minimise the game window (the field), run Flasher.exe and
        // wait for it (_spawnl(_P_WAIT, "Flasher.exe", "Flasher.exe", 0)), maximise again.
        ShowWindow(g_client.window, SW_MINIMIZE); // 6
        _spawnl(_P_WAIT, "Flasher.exe", "Flasher.exe", nullptr);
        ShowWindow(g_client.window, SW_MAXIMIZE); // 3
        break;

    case 18:
    {
        // the active page's slot 10, +0x228 = 0.
        ReleaseActivePageInternal(Nav);
        // the full path of PAGE_CharSelectSingle.page (kept in EDI).
        char const* const PagePath = GetFullFilename(kPageCharSelectSingle);
        // the five statics under the page (the field), before it is
        // loaded.
        PageCharacterSelectSingle& SinglePage = GetPageCharacterSelectSingle();
        EnterCase18Chrome(SinglePage);
        // the routine(path) on the page's descriptor (+0x8c,
        // ); true -> page slot 9 (0, 0) and +0x228 = the page.
        if (SinglePage.Desc.LoadFromFile(PagePath))
        {
            SinglePage.Create(nullptr, nullptr);
            Nav->ActivePage = &SinglePage;
        }
        else
        {
            // page +0x108 (the field / the field) = -2.0, then
            // CurrentState = 1.
            SinglePage.SlideT = -2.0;
            Nav->CurrentState = 1;
        }
        break;
    }

    case 19:
        // back from the game (latch 2 / 4). the routine relists the
        // characters; failure -> epilogue. Latch 2 -> PageCharacterSelect (state 9);
        // otherwise select the character by the name the field kept (the routine ->
        // ) and, on success, straight back into the game (0x14).
        if (G_LoginMgr.RequestCharacterInfo() == 0)
        {
            break;
        }
        if (GIntroModeLatch == 2)
        {
            Nav->CurrentState = 9;
            break;
        }
        if (G_LoginMgr.SelectCharacter(
                G_LoginMgr.FindCharacterByName(GetPlayerCharacterNamebaseline())))
        {
            Nav->CurrentState = 0x14;
        }
        break;

    default:
        break;
    }
    RefreshLoadingProgress();
}

int MenuStateDriverUpdate(FMenuNavState* Nav, float DeltaSeconds)
{
    // (thiscall, RET 4).
    int TransitionCode = 0; // [esp+0x18]
    bool bDidEnter = false; // BL

    // leave the old state, enter the new one, until it settles.
    while (Nav->CurrentState != Nav->PrevState)
    {
        bDidEnter = true;
        RefreshLoadingProgress(); // EBP = RefreshLoadingProgress (0x00573ee3)
        int const Leaving = Nav->PrevState;
        // sprintf("ExitState %d\n") into a local nobody reads (0x00573ef3).

        if (Leaving == 5)
        {
            GServerSelectActive = 0; // = 0 (0x00573f34)
        }
        else if (Leaving == 10)
        {
            // the field missing or inactive (flags bit0 clear)
            // -> CurrentState = 1, delete it (slot 0 with 1), the field = 0.
            if (GOptionsChild_67c20c == nullptr || (GOptionsChild_67c20c->Flags & 1) == 0)
            {
                Nav->CurrentState = 1;
                if (GOptionsChild_67c20c != nullptr)
                    delete GOptionsChild_67c20c;
                GOptionsChild_67c20c = nullptr;
            }
        }

        RefreshLoadingProgress();
        Nav->PrevState = Nav->CurrentState;
        EnterMenuState(Nav, Nav->CurrentState);
        RefreshLoadingProgress();
    }

    // per-state poll. Byte table 0x00574380 over CurrentState - 1
    // (20 entries, states 1..20); index 10 is the no-poll exit 0x00574182.
    static unsigned char const kPollIndex[20] = {0, 10, 10, 10, 1, 2, 10, 10, 3, 4,
                                                 10, 10, 5, 6, 10, 10, 7, 8, 10, 9};
    unsigned const PollSlot = static_cast<unsigned>(Nav->CurrentState - 1);
    unsigned char const Poll = PollSlot < 20u ? kPollIndex[PollSlot] : 10;
    switch (Poll)
    {
    case 0:
    {
        // (state 1): TypeSelect+0x80 (the field). Not cleared here -- EnterState
        // case 1 clears it.
        int const Next = GetTypeSelect().NextState;
        if (Next != 0)
        {
            Nav->CurrentState = Next;
            if (Next == 0xc)
                RefreshLoadingProgress();
        }
        break;
    }
    case 1:
    {
        // (state 5): PageServerSelect+0xb4 (the field), cleared once taken.
        PageServerSelect& ServerPage = GetPageServerSelect();
        if (ServerPage.Result != 0)
        {
            Nav->CurrentState = ServerPage.Result;
            ServerPage.Result = 0;
        }
        break;
    }
    case 2:
    {
        // (state 6): PageAccountSelect+0xb4 (the field), cleared once taken.
        PageAccountSelect& AccountPage = GetPageAccountSelect();
        if (AccountPage.Result != 0)
        {
            Nav->CurrentState = AccountPage.Result;
            AccountPage.Result = 0;
        }
        break;
    }
    case 3:
    {
        // (state 9): PageCharacterSelect (the field).
        PageCharacterSelect& CharacterPage = GetPageCharacterSelect();
        // its +0x5f8 (the field) set -> the intro base left under
        // bDrawScripts (0x0057404e..0x0057409b), the field = 0 again when the latch is 4,
        // then +0x5f9 (the field) = 1 for the page's Update.
        if (CharacterPage.bLoadRequested != 0)
        {
            if (GetIntroConfigState()->bDrawScripts)
                LeaveIntroBase();
            if (GIntroModeLatch == 4)
                SetClientLocationNoServerNotify(false);
            CharacterPage.bIntroBaseLeft = 1;
        }
        // +0x2b4 (the field) -> CurrentState, +0x399 (the field)
        // -> +0xc, Flag20c = 1; the field is not cleared.
        if (CharacterPage.Result != 0)
        {
            Nav->CurrentState = CharacterPage.Result;
            Nav->CharacterId = CharacterPage.SelectedCharacter;
            Nav->Flag20c = 1;
        }
        break;
    }
    case 4:
    {
        // (state 10): PageOptions. +0xb8 (the field) is the NN_Preferences
        // window, +0xb4 (the field) its result.
        PageOptions& Options = GetPageOptions();
        WinControl* const Preferences = Options.Preferences;
        if (Preferences == nullptr)
        {
            Nav->CurrentState = 1;
            Options.Result = 0;
            break;
        }
        // the byte at +0x32c of the NN_Preferences object (NnPreferences::bClosed,
        // set by its close paths and by a failed Create).
        bool const bClosed = static_cast<NnPreferences*>(Preferences)->bClosed != 0;
        if (bClosed)
        {
            Win3DDeleteTree(Preferences);
            Options.Preferences = nullptr;
            Nav->CurrentState = 1;
            Options.Result = 0;
            break;
        }
        if (Options.Result != 0)
        {
            Nav->CurrentState = Options.Result;
            Options.Result = 0;
        }
        break;
    }
    case 5:
        // (state 13): the single-player start failed -- client state 1 (quit).
        TransitionCode = 1;
        break;
    case 6:
    {
        // (state 14): PageCharacterCreate (the field) -- the routine
        // (a result is set) -> CurrentState = the routine (+0xb4). The field is not cleared.
        PageCharacterCreate& CreatePage = GetPageCharacterCreate();
        if (CreatePage.HasResult())
            Nav->CurrentState = CreatePage.GetResult();
        break;
    }
    case 7:
        // (state 17): no poll.
        break;
    case 8:
    {
        // (state 18): PageCharacterSelectSingle (the field).
        PageCharacterSelectSingle& SinglePage = GetPageCharacterSelectSingle();
        // its +0x368 (the field) set -> the intro base left under
        // bDrawScripts (0x005740df..0x0057412c), then +0x369 (the field) = 1 for the page's
        // Update. Unlike index 3 there is no latch-4 write.
        if (SinglePage.bLoadRequested != 0)
        {
            if (GetIntroConfigState()->bDrawScripts)
                LeaveIntroBase();
            SinglePage.bIntroBaseLeft = 1;
        }
        // +0xb4 (the field) -> CurrentState, +0x118 (the field)
        // -> +0xc, Flag20c = 1; the field is not cleared.
        if (SinglePage.Result != 0)
        {
            Nav->CurrentState = SinglePage.Result;
            Nav->CharacterId = SinglePage.SelectedCharacter;
            Nav->Flag20c = 1;
        }
        break;
    }
    case 9:
        // (state 20): into the game -- the routine (the player's base,
        // & 0x3fffffff, is set) ? 3 : 2.
        TransitionCode = (GetPlayerBaseIdbaseline() & 0x3fffffffu) != 0 ? 3 : 2;
        break;
    default:
        break;
    }

    // the routine(3), the routine(EDI, dt) -- the sound mixer mode
    // and tick. EDI is 0 on every path here (0x00573ec8 / 0x00573f59, no poll writes it): no
    // camera, the manager keeps its listener. dt is the uncapped argument.
    SetClientAudioMode(3);
    TickClientSoundMixer(nullptr, DeltaSeconds);

    if (bDidEnter)
    {
        return TransitionCode; // TEST BL,BL / JNZ 0x00574340
    }
    const bool DrawIntroScene = GetIntroConfigState()->bDrawScripts;
    if (DrawIntroScene)
    {
        // the frame time as a double, capped at 0.2 (the field): FCOMP /
        // TEST AH,0x41 / JNZ keeps it when not greater (or unordered); the fall-through stores 0.2.
        double Dt = static_cast<double>(DeltaSeconds);
        if (Dt > 0.2)
            Dt = 0.2;
        // +0x230 (float) + Dt; the sum rounded to float is kept
        // ([ESP+0x18]), floor(sum * 1000.0) (the field) are the whole milliseconds, +0x230 =
        // float(sum_f - ms * 0.001) (the field, FSUBP ST1,ST0), and ftol(ms) goes to the routine
        // on the field.
        double const Sum = static_cast<double>(Nav->Field230) + Dt;
        float const SumAsFloat = static_cast<float>(Sum);
        double const Milliseconds = floor(Sum * 1000.0);
        Nav->Field230 = static_cast<float>(static_cast<double>(SumAsFloat) - Milliseconds * 0.001);
        G_GfScriptManager.Update(static_cast<int>(Milliseconds));

        // the capped time as a float (EBX) to the routine on the field.
        float const DtFloat = static_cast<float>(Dt);
        TickClientLocationController(DtFloat);

        // every RoomObject (the routine's list) whose +0x04 is clear gets
        // slot 5 (+0x14) with the capped double.
        std::list<FRoomObject*>& Objects = GetRoomObjectList();
        for (std::list<FRoomObject*>::iterator It = Objects.begin(); It != Objects.end(); ++It)
        {
            FRoomObject* const Object = *It;
            if (Object->bNoUpdate == 0)
                Object->Update(Dt);
        }

        // the routine(the routine(), DtFloat) -- the room camera's
        // letterbox; the routine(DtFloat) -- the fade; the routine with the uncapped argument
        // ([ESP+0x434]).
        UpdateRoomLetterbox(static_cast<FRoomCamera*>(static_cast<ICamera*>(GetClientRoomCamera())),
                            DtFloat);
        UpdateFade(DtFloat);
        TickMaterialAnimationPlayback(DeltaSeconds);
    }

    // the frame, only when the routine begins one (JZ 0x0057431b).
    if (BeginClientRenderFrame())
    {
        LoadingScreenReset();                                   // (0x00574294)
        Win3DSetViewport(nullptr, kIntroCpp, 0x1ac7);           // (0x005742a5)
        Win3DSetPipelineClearColor(0.0f, 0.0f, 0.0f, 1.0f);     // (0x005742b5)
        ClearWin3DBuffers(kWin3DClearColour | kWin3DClearDepth); // (0x4100)
        if (DrawIntroScene)
            DrawClientLocation();
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
        // Port-only: the logo sits in the top left of its 4:3 picture and goes with the main
        // menu column to the left edge (hud_widescreen.cpp).
        ImageSetWidescreenAlign(-1);
        Nav->LogoImage.Draw();                                  // +0x210 slot 2 (0x005742dd)
        ImageSetWidescreenAlign(0);
#else
        Nav->LogoImage.Draw();                                  // +0x210 slot 2 (0x005742dd)
#endif
        ClearWin3DBuffers(kWin3DClearDepth);                    // (0x100)
        Win3DDrawAll();                                         // (0x005742ed)
        // the routine clear -> the routine(the routine()).
        if (DrawIntroScene && !IsClientServerTickPaused())
            DrawFade(static_cast<ICamera*>(GetClientRoomCamera()));
        Sleep(0);
        EndClientRenderScene();
        PresentClientRenderFrame();
    }

    // once (+0x22c), the loading screen (the field) gets its
    // progress bar (the routine), drops the startup Image (the routine) and stops clearing
    // (the routine(0)).
    if (Nav->Flag22c == 0)
    {
        LoadingScreenCreateProgressBar();
        LoadingScreenReleaseImage();
        LoadingScreenSetClear(0);
        Nav->Flag22c = 1;
    }

    // [ESP+0x18].
    return TransitionCode;
}

int MenuNavGetIntroModeLatch()
{
    return GIntroModeLatch;
}

void MenuNavSetIntroModeRequest(int Mode)
{
    // — consumed into the field at menu Init / next latch read.
    GIntroModeRequest = Mode;
}

int MenuNavGetIntroModeRequest()
{
    return GIntroModeRequest;
}

bool MenuNavIsServerSelectActive()
{
    // MOV AL,[the field]; RET. Read by WaitForServerResponse (005a8ba1).
    return GServerSelectActive != 0;
}

void MenuNavConsumeIntroModeRequest()
{
    GIntroModeLatch = GIntroModeRequest;
    GIntroModeRequest = 0;
}

// The flat-shell bridge that stood here (MenuEnterStateCase1HitTest / Click / Draw /
// SetHoverFromPoint and the focus stubs) is gone: the column's TextMsgButtons get the mouse the
// baseline way -- MouseDispatchEvents (the routine) -> KeyMapRouteMouseEvent -> commands 4..13 ->
// FWin3DInputHandler::Handle (the routine) -> Win3DPick -> slot 19, the hover from
// Win3DUpdateAll (the routine), the draw from Win3DDrawAll (the routine, 0x005742ed).
