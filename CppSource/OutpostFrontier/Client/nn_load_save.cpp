// Author: Giperion.

// baseline NN_LoadSave (NN_LoadSave.cpp). See nn_load_save.h.

#include "nn_load_save.h"

#include "client_state.h"          // QueueLoadGameCharacter (the routine)
#include "fade_to_black.h"         // FadeIn (the routine)
#include "hud_command_dispatch.h"  // HudPostCommand (the routine)
#include "ids_strings.h"           // GetIdsStringWWithRawFallback (the routine)
#include "input_manager.h"         // Get/SetClientMouseCursorDrawFlag (the routine / the routine)
#include "login_mgr.h"             // G_LoginMgr (the field), G_Server (the field)
#include "nn_button_control.h"     // NnButtonControl::SetMontageMode (the routine)
#include "nn_confirm.h"            // NnConfirmOpen (the routine)
#include "nn_error.h"              // NnErrorOpen (the routine)
#include "nn_modal.h"              // NnModalPauseGame / NnModalResumeGame, NnFloatBits
#include "nn_text_control.h"       // NnTextControl (+0x381 / +0x385)
#include "nn_ui_pulse.h"           // G_HudChromeColorRgba (the field), G_NnPaletteArgb_679ba4
#include "player_spawn.h"          // GetPlayerConnectionId (the field)
#include "rich_text_win.h"         // RichTextEditWin
#include "ui_scroll.h"             // UiScroll::SetRange (the routine)
#include "win3d.h"
#include "win3d_renderables.h"     // Win3DFlushBeforeOverlayDraw (the routine)
#include "win_factory.h"

#include "common_archetype.h"      // Archetype::GetShip
#include "common_misc.h"           // filetime_to_wstring
#include "common_server.h"         // IServer::SaveGame
#include "common_universe.h"       // Universe::get_system
#include "neuroncomponents.h"                 // FDUMP

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

// WINFACTORY<NN_LoadSave> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnLoadSave> G_NnLoadSaveFactory("NN_LoadSave");

wchar_t G_NnLoadSaveText[0x100];

namespace
{
    char const kNnLoadSaveCpp[] = "Client\\nn\\NN_LoadSave.cpp";
    char const kBlackPlaneMesh[] = "Interface\\NeuroNet\\MISC_bigblackplane.3db";
    char const kFrameMesh[] = "Interface\\LoadSave\\LS_background.cmp";
    char const kButtonMesh[] = "Interface\\LoadSave\\FRONT_button.cmp";
    char const kInfoFrameMesh[] = "Interface\\LoadSave\\FRONT_infowindow.cmp";
    char const kDescripButtonMesh[] = "Interface\\LoadSave\\LS_bottomframe.3db";
    char const kCloseMesh[] = "Interface\\NeuroNet\\Inventory\\x.3db";
    // an empty wide string.
    wchar_t const kEmptyText[] = L"";

    // Win3D +0x6c bits 0..1 (the routine's pair): set = shown.
    void ShowControl(Win3D* Control)
    {
        Control->Flags = static_cast<unsigned char>(Control->Flags | 3);
    }

    void HideControl(Win3D* Control)
    {
        Control->Flags = static_cast<unsigned char>(Control->Flags & 0xfc);
    }
}

NnLoadSave::NnLoadSave()
    : bCursorDrawFlag(0)
    , Mode(0)
    , ConfirmMode(0)
    , bExitToMainMenu(0)
    , bSaveAllowed(1)
    , bListRefreshed(1)
    , Pad33f(0)
    , Byte340(0)
    , SaveNamePrefixLength(0)
    , NextSaveNumber(0)
    , Title(nullptr)
    , VisibleRows(0)
    , Scroll(nullptr)
    , DescripText(nullptr)
    , DescripButton(nullptr)
    , DescripEdit(nullptr)
    , CloseButton(nullptr)
{
    // WinControl ctor the routine; +0x340 (allocator byte), the vector, +0x350
    // (allocator byte) and the string; +0x368 = 0 (the CHARACTER_ID ctor); ListBox ctor
    // at +0x5dc and +0xe98; +0x33d = +0x33e = 1, +0x334 = 0, +0x338 = 0,
    // +0x33c = 0; the three vptrs. Everything else is written by Create before it is read;
    // the port zeroes it here.
    Pad331[0] = 0;
    Pad331[1] = 0;
    Pad331[2] = 0;
    SaveName[0] = 0;
    for (int Index = 0; Index < 5; ++Index)
    {
        Buttons[Index] = nullptr;
        ButtonTexts[Index] = nullptr;
        bButtonEnabled[Index] = 0;
    }
    Pad5d9[0] = 0;
    Pad5d9[1] = 0;
    Pad5d9[2] = 0;
    for (int Index = 0; Index < 3; ++Index)
    {
        NameListOffset[Index] = 0.0f;
        TimeListOffset[Index] = 0.0f;
    }
    for (int Index = 0; Index < 4; ++Index)
    {
        InfoTitles[Index] = nullptr;
        InfoValues[Index] = nullptr;
    }
}

NnLoadSave::~NnLoadSave()
{
    // vptrs back; ListBox dtor the routine on +0xe98 then +0x5dc, the string at
    // +0x350, the vector at +0x340 (the routine per entry), then the routine (Event::Handler)
    // and the routine (WinControl) -- the member / base destructors, in that order.
}

bool NnLoadSave::Create(Win3D* NewParent, void* Data)
{
    // (RET 8, 0x26c-byte frame). No null test on Data (0x00485c07).
    Mode = *static_cast<int const*>(Data);

    float const Zero[3] = {0.0f, 0.0f, 0.0f};
    float const ZFar = NnFloatBits(0xbf866666); // -1.05

    // the routine("LoadSave_BlackPlane", mesh, {0, 0, -1}, 0, {0,0,0},
    // 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, {0,0,0}).
    float const PlanePosition[3] = {0.0f, 0.0f, -1.0f};
    CreateStaticControl("LoadSave_BlackPlane", kBlackPlaneMesh, PlanePosition, nullptr, Zero, 0, 0,
                        0, 1, 1, 0, 1, 0, 0, 0, Zero);

    // "LoadSave_Frame" at {0.14, 0, -1.05}, the same flags.
    float const FramePosition[3] = {NnFloatBits(0x3e0f5c29), 0.0f, ZFar};
    CreateStaticControl("LoadSave_Frame", kFrameMesh, FramePosition, nullptr, Zero, 0, 0, 0, 1, 1,
                        0, 1, 0, 0, 0, Zero);

    // the routine("LoadSave_Title", "PageHeader", {0.145, 0.25, -1.05},
    // 0, 0.27, 0.03, 1.0, 0, 1, 0, 0) -> +0x5a8, then slot 42 (0x1e, 0x586, 0).
    float const TitlePosition[3] = {NnFloatBits(0x3e147ae1), NnFloatBits(0x3e800000), ZFar};
    Title = CreateTextControl("LoadSave_Title", "PageHeader", TitlePosition, nullptr,
                              NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f), 1.0f, 0, 1, 0, 0);
    Title->SetProperty(0x1e, 0x586, 0);

    // the five buttons. Save and Load share {-0.33, 0.27, -1.05}; Delete,
    // Main Menu and Resume step down by 0.0916 / 0.1832 / 0.2748 (FSUB float). Half extents
    // {0.089, 0.021, 0}; the routine(name, FRONT_button.cmp, 0, 0, 0, pos, 0, half, 0, 0, 1, 0,
    // tooltip ids 0x58c..0x590, 0, 0) -> +0x5ac..+0x5bc.
    float const ButtonX = NnFloatBits(0xbea8f5c2);
    float const ButtonY = NnFloatBits(0x3e8a3d71);
    float const ButtonHalf[3] = {NnFloatBits(0x3db645a2), NnFloatBits(0x3cac0831), 0.0f};
    float const SavePosition[3] = {ButtonX, ButtonY, ZFar};
    Buttons[0] = CreateButtonControl("LoadSave_SaveGame", kButtonMesh, nullptr, nullptr, nullptr,
                                     SavePosition, nullptr, ButtonHalf, 0, 0, 1, 0, 0x58c, 0, 0);
    Buttons[1] = CreateButtonControl("LoadSave_LoadGame", kButtonMesh, nullptr, nullptr, nullptr,
                                     SavePosition, nullptr, ButtonHalf, 0, 0, 1, 0, 0x58d, 0, 0);
    float const DeletePosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3dbb98c8), ZFar};
    Buttons[2] = CreateButtonControl("LoadSave_Delete", kButtonMesh, nullptr, nullptr, nullptr,
                                     DeletePosition, nullptr, ButtonHalf, 0, 0, 1, 0, 0x58e, 0, 0);
    float const MainMenuPosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3e3b98c8), ZFar};
    Buttons[3] = CreateButtonControl("LoadSave_MainMenu", kButtonMesh, nullptr, nullptr, nullptr,
                                     MainMenuPosition, nullptr, ButtonHalf, 0, 0, 1, 0, 0x58f, 0,
                                     0);
    float const ResumePosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3e8cb296), ZFar};
    Buttons[4] = CreateButtonControl("LoadSave_ResumeGame", kButtonMesh, nullptr, nullptr, nullptr,
                                     ResumePosition, nullptr, ButtonHalf, 0, 0, 1, 0, 0x590, 0, 0);

    // the routine(1) on each (no null test).
    for (int Index = 0; Index < 5; ++Index)
        static_cast<NnButtonControl*>(Buttons[Index])->SetMontageMode(1);

    // the captions, the routine(name, "ButtonText", pos, 0, 0.27, 0.03,
    // 1.0, 0, 1, 0, 0) -> +0x5c0..+0x5d0. Y = the button row's Y + 0.0018 (Save, Load), - 0.0898,
    // - 0.1814, - 0.273.
    float const SaveTextPosition[3] = {ButtonX, ButtonY + NnFloatBits(0x3aebedfa), ZFar};
    ButtonTexts[0] = CreateTextControl("LoadSave_SaveGameText", "ButtonText", SaveTextPosition,
                                       nullptr, NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f),
                                       1.0f, 0, 1, 0, 0);
    ButtonTexts[1] = CreateTextControl("LoadSave_LoadGameText", "ButtonText", SaveTextPosition,
                                       nullptr, NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f),
                                       1.0f, 0, 1, 0, 0);
    float const DeleteTextPosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3db7e910), ZFar};
    ButtonTexts[2] = CreateTextControl("LoadSave_DeleteText", "ButtonText", DeleteTextPosition,
                                       nullptr, NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f),
                                       1.0f, 0, 1, 0, 0);
    float const MainMenuTextPosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3e39c0ec), ZFar};
    ButtonTexts[3] = CreateTextControl("LoadSave_MainMenuText", "ButtonText",
                                       MainMenuTextPosition, nullptr, NnFloatBits(0x3e8a3d71),
                                       NnFloatBits(0x3cf5c28f), 1.0f, 0, 1, 0, 0);
    float const ResumeTextPosition[3] = {ButtonX, ButtonY - NnFloatBits(0x3e8bc6a8), ZFar};
    ButtonTexts[4] = CreateTextControl("LoadSave_ResumeGameText", "ButtonText",
                                       ResumeTextPosition, nullptr, NnFloatBits(0x3e8a3d71),
                                       NnFloatBits(0x3cf5c28f), 1.0f, 0, 1, 0, 0);

    // slot 42 (0x1e, 0x587..0x58b, 0) on the captions.
    for (int Index = 0; Index < 5; ++Index)
        ButtonTexts[Index]->SetProperty(0x1e, static_cast<uintptr_t>(0x587 + Index), 0);

    // +0x5d4..+0x5d8 = 0; each caption's +0x381 = the field and
    // +0x385 = the field (two unaligned dwords).
    for (int Index = 0; Index < 5; ++Index)
        bButtonEnabled[Index] = 0;
    for (int Index = 0; Index < 5; ++Index)
    {
        NnTextControl* const Text = static_cast<NnTextControl*>(ButtonTexts[Index]);
        memcpy(Text->BlinkColourOn, G_HudChromeColorRgba, 4);
        memcpy(Text->BlinkColourOff, &G_NnPaletteArgb_679ba4, 4);
    }

    // "LoadSave_InfoFrame" at {-0.33, -0.2, -1.05}, the static flags.
    float const InfoFramePosition[3] = {ButtonX, NnFloatBits(0xbe4ccccd), ZFar};
    CreateStaticControl("LoadSave_InfoFrame", kInfoFrameMesh, InfoFramePosition, nullptr, Zero, 0,
                        0, 0, 1, 1, 0, 1, 0, 0, 0, Zero);

    // the info texts, the routine(name, "Normal", pos, 0, 0.25, 0.02,
    // 1.0, 0, 0, 0, 0), from {-0.32, -0.125, -1.05} down by 0, 0.02, 0.0425, 0.0625, 0.085, 0.105,
    // 0.1275, 0.1475 (FSUB float). Created title, data, title, data...; stored at +0x1768
    // (location title), +0x1778 (location data), +0x176c, +0x177c, +0x1770, +0x1780, +0x1774,
    // +0x1784.
    float const InfoX = NnFloatBits(0xbea3d70a);
    float const InfoY = NnFloatBits(0xbe000000);
    float const InfoWidth = NnFloatBits(0x3e800000);
    float const InfoHeight = NnFloatBits(0x3ca3d70a);
    char const* const InfoTitleNames[4] = {"LoadSave_InfoLocationTitle",
                                           "LoadSave_InfoCreditsTitle", "LoadSave_InfoRankTitle",
                                           "LoadSave_InfoShipTitle"};
    char const* const InfoDataNames[4] = {"LoadSave_InfoLocationData", "LoadSave_InfoCreditsData",
                                          "LoadSave_InfoRankData", "LoadSave_InfoShipData"};
    {
        // Spelled out per text so every subtraction is the listing's single FSUB.
        float const LocationTitlePosition[3] = {InfoX, InfoY, ZFar};
        InfoTitles[0] = CreateTextControl(InfoTitleNames[0], "Normal", LocationTitlePosition,
                                          nullptr, InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const LocationDataPosition[3] = {InfoX, InfoY - NnFloatBits(0x3ca3d70a), ZFar};
        InfoValues[0] = CreateTextControl(InfoDataNames[0], "Normal", LocationDataPosition,
                                          nullptr, InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const CreditsTitlePosition[3] = {InfoX, InfoY - NnFloatBits(0x3d2e147b), ZFar};
        InfoTitles[1] = CreateTextControl(InfoTitleNames[1], "Normal", CreditsTitlePosition,
                                          nullptr, InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const CreditsDataPosition[3] = {InfoX, InfoY - NnFloatBits(0x3d800000), ZFar};
        InfoValues[1] = CreateTextControl(InfoDataNames[1], "Normal", CreditsDataPosition,
                                          nullptr, InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const RankTitlePosition[3] = {InfoX, InfoY - NnFloatBits(0x3dae147b), ZFar};
        InfoTitles[2] = CreateTextControl(InfoTitleNames[2], "Normal", RankTitlePosition, nullptr,
                                          InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const RankDataPosition[3] = {InfoX, InfoY - NnFloatBits(0x3dd70a3e), ZFar};
        InfoValues[2] = CreateTextControl(InfoDataNames[2], "Normal", RankDataPosition, nullptr,
                                          InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const ShipTitlePosition[3] = {InfoX, InfoY - NnFloatBits(0x3e028f5c), ZFar};
        InfoTitles[3] = CreateTextControl(InfoTitleNames[3], "Normal", ShipTitlePosition, nullptr,
                                          InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
        float const ShipDataPosition[3] = {InfoX, InfoY - NnFloatBits(0x3e170a3e), ZFar};
        InfoValues[3] = CreateTextControl(InfoDataNames[3], "Normal", ShipDataPosition, nullptr,
                                          InfoWidth, InfoHeight, 1.0f, 0, 0, 0, 0);
    }

    // titles slot 42 (0x1e, 0x593 / 0x594 / 0x5ec / 0x591, 0); values
    // (0x1d, L"", 0).
    InfoTitles[0]->SetProperty(0x1e, 0x593, 0);
    InfoTitles[1]->SetProperty(0x1e, 0x594, 0);
    InfoTitles[2]->SetProperty(0x1e, 0x5ec, 0);
    InfoTitles[3]->SetProperty(0x1e, 0x591, 0);
    for (int Index = 0; Index < 4; ++Index)
        InfoValues[Index]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(kEmptyText), 0);

    // 0x10 visible rows in save mode, else 0x14. Row height
    // (double)0.032538 * rows (the field) and text scale (double)0.01773 / rows
    // (the field), both stored as float; the lists' Y is 0.11 in save mode, else 0.21.
    VisibleRows = Mode == kNnLoadSaveModeSave ? 0x10 : 0x14;
    float const ListHeight = static_cast<float>(0.032538 * static_cast<double>(VisibleRows));
    float const ListTextScale = static_cast<float>(0.01773 / static_cast<double>(VisibleRows));
    float ListY = NnFloatBits(0x3de147ae);
    if (Mode != kNnLoadSaveModeSave)
        ListY = NnFloatBits(0x3e570a3d);

    // the name list -- slot 9 Create(this, 0); height, visible rows, no
    // frame, row rules, width 0.4, text scale; its offset {-0.142, Y, -1.05} at +0xe8c.
    NameList.Create(this, nullptr);
    NameList.Height = ListHeight;
    NameList.VisibleRows = VisibleRows;
    NameList.bDrawFrame = 0;
    NameList.bDrawRowRules = 1;
    NameList.Width = NnFloatBits(0x3ecccccd);
    NameList.TextScale = ListTextScale;
    NameListOffset[0] = NnFloatBits(0xbe116873);
    NameListOffset[1] = ListY;
    NameListOffset[2] = ZFar;

    // the date list, the same with width 0.14 and offset
    // {0.258, Y, -1.05} at +0x1748.
    TimeList.Create(this, nullptr);
    TimeList.Height = ListHeight;
    TimeList.bDrawFrame = 0;
    TimeList.bDrawRowRules = 1;
    TimeList.Width = NnFloatBits(0x3e0f5c2a);
    TimeList.VisibleRows = VisibleRows;
    TimeList.TextScale = ListTextScale;
    TimeListOffset[0] = NnFloatBits(0x3e841894);
    TimeListOffset[1] = ListY;
    TimeListOffset[2] = ZFar;

    if (Mode == kNnLoadSaveModeSave)
    {
        // the routine("LoadSave_Edit2", {0.125 - 0.26, 0.158 + 0.015,
        // -1.05}, 0.52, 0.03, L"") -> +0x1764; the routine(0x1e); the routine (no null test).
        float const DescripX = NnFloatBits(0x3e000000);
        float const DescripY = NnFloatBits(0x3e21cac1);
        float const EditPosition[3] = {DescripX - NnFloatBits(0x3e851eb8),
                                       DescripY + NnFloatBits(0x3c75c28f), ZFar};
        DescripEdit = CreateUiTextEdit("LoadSave_Edit2", EditPosition, NnFloatBits(0x3f051eb8),
                                       NnFloatBits(0x3cf5c28f), const_cast<wchar_t*>(kEmptyText));
        DescripEdit->SetMaxLength(0x1e);
        DescripEdit->Activate();

        // the routine("LoadSave_DescripButton", LS_bottomframe.3db, 0,
        // 0, 0, {0.125, 0.158, -1.05}, 0, {0.27, 0.029, 0}, 0, 0, 1, 0, 0x596, 0, 0) -> +0x1760;
        // (1); slot 42 (0x32, 1, 0).
        float const DescripPosition[3] = {DescripX, DescripY, ZFar};
        float const DescripHalf[3] = {NnFloatBits(0x3e8a3d71), NnFloatBits(0x3ced9168), 0.0f};
        DescripButton = CreateButtonControl("LoadSave_DescripButton", kDescripButtonMesh, nullptr,
                                            nullptr, nullptr, DescripPosition, nullptr,
                                            DescripHalf, 0, 0, 1, 0, 0x596, 0, 0);
        static_cast<NnButtonControl*>(DescripButton)->SetMontageMode(1);
        DescripButton->SetProperty(0x32, 1, 0);

        // the routine("LoadSave_DescripText", "ListText",
        // {-0.009, 0.204, -1.05}, 0, 0.27, 0.03, 1.0, 0, 0, 0, 0) -> +0x175c; slot 42 (0x1e,
        // 0x592, 0).
        float const DescripTextPosition[3] = {NnFloatBits(0xbc1374bc), NnFloatBits(0x3e50e560),
                                              ZFar};
        DescripText = CreateTextControl("LoadSave_DescripText", "ListText", DescripTextPosition,
                                        nullptr, NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f),
                                        1.0f, 0, 0, 0, 0);
        DescripText->SetProperty(0x1e, 0x592, 0);
    }
    else
    {

        DescripEdit = nullptr;
        DescripButton = nullptr;
        DescripText = nullptr;
    }

    // the routine("LoadSave_Scroll", {0.406, Y, -1.05}, 0.02, H, 0,
    // 100, rows, 0) -> +0x1758; Y / H 0.107 / 0.37 in save mode, else 0.21 / 0.465.
    float ScrollY = NnFloatBits(0x3e570a3d);
    float ScrollHeight = NnFloatBits(0x3eee147b);
    if (Mode == kNnLoadSaveModeSave)
    {
        ScrollY = NnFloatBits(0x3ddb22d1);
        ScrollHeight = NnFloatBits(0x3ebd70a4);
    }
    float const ScrollPosition[3] = {NnFloatBits(0x3ecfdf3c), ScrollY, ZFar};
    Scroll = CreateUIScroll("LoadSave_Scroll", ScrollPosition, NnFloatBits(0x3ca3d70a),
                            ScrollHeight, 0, 100, VisibleRows, 0);

    // the routine("LoadSave_Close", x.3db, 0, 0, 0,
    // {0.415, 0.26, -1.05}, 0, {0.02, 0.02, 0}, 0, 1, 0, 0, 0x409, 0, 0) -> +0x1788;
    // (1).
    float const ClosePosition[3] = {NnFloatBits(0x3ed47ae2), NnFloatBits(0x3e851eb8), ZFar};
    float const CloseHalf[3] = {NnFloatBits(0x3ca3d70a), NnFloatBits(0x3ca3d70a), 0.0f};
    CloseButton = CreateButtonControl("LoadSave_Close", kCloseMesh, nullptr, nullptr, nullptr,
                                      ClosePosition, nullptr, CloseHalf, 0, 1, 0, 0, 0x409, 0, 0);
    static_cast<NnButtonControl*>(CloseButton)->SetMontageMode(1);

    // the routine(ids 0x65d, buffer, 0x100) -- FDUMP at line 0x18e on a
    // miss, the buffer used either way -- into the +0x350 string; +0x360 = its length.
    wchar_t PrefixText[0x100];
    if (GetIdsStringWWithRawFallback(0x65d, PrefixText, 0x100) == 0)
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Failed to find string resource %d\n", kNnLoadSaveCpp,
              0x18e, 0x65d);
    }
    SaveNamePrefix.assign(PrefixText, wcslen(PrefixText));
    SaveNamePrefixLength = static_cast<int>(wcslen(SaveNamePrefix.c_str()));

    NextSaveNumber = 1;

    // the title and which buttons show, by mode.
    if (Mode == kNnLoadSaveModeLoad)
    {
        Title->SetProperty(0x1e, 0x588, 0);
        HideControl(Buttons[0]);
        ShowControl(Buttons[1]);
        HideControl(ButtonTexts[0]);
        ShowControl(ButtonTexts[1]);
    }
    else if (Mode == kNnLoadSaveModeSave)
    {
        Title->SetProperty(0x1e, 0x587, 0);
        ShowControl(Buttons[0]);
        HideControl(Buttons[1]);
        ShowControl(ButtonTexts[0]);
        HideControl(ButtonTexts[1]);
    }
    else if (Mode == kNnLoadSaveModeStoryStar)
    {
        Title->SetProperty(0x1e, 0x588, 0);
        HideControl(Buttons[0]);
        ShowControl(Buttons[1]);
        HideControl(ButtonTexts[0]);
        ShowControl(ButtonTexts[1]);
        HideControl(Buttons[2]);
        HideControl(Buttons[4]);
        HideControl(ButtonTexts[2]);
        HideControl(ButtonTexts[4]);
        HideControl(CloseButton);
    }
    else
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid load/save mode %d\n", kNnLoadSaveCpp, 0x1bc,
              Mode);
    }


    DrawLayer = 0x6e;
    DrawSubLayer = 6;
    // the routine.
    bCursorDrawFlag = NnModalPauseGame();
    // the routine(0xf) on the +0x32c face.
    RegisterEvent(0xf);

    RefreshSaveList();
    SelectSave(-1);
    // the routine, SETNZ.
    return Win3D::Create(NewParent, Data);
}

void NnLoadSave::ResumeAndDelete()
{
    NnModalResumeGame(bCursorDrawFlag);
    NameList.Destroy();
    TimeList.Destroy();
    ReleaseMouse();
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();
    delete this;
}

void NnLoadSave::ClearListsAndSelection()
{
    NameList.ItemCount = 0;
    NameList.SelectedIndex = -1;
    TimeList.ItemCount = 0;
    TimeList.SelectedIndex = -1;
    SelectSave(-1);
}

WinControl* NnLoadSave::ButtonTextFor(uintptr_t Button) const
{
    // / 0x004870f0..0x00487143
    for (int Index = 0; Index < 5; ++Index)
    {
        if (Button == reinterpret_cast<uintptr_t>(Buttons[Index]))
            return ButtonTexts[Index];
    }
    return nullptr;
}

bool NnLoadSave::OnEvent(unsigned MsgId)
{
    // (RET 4): only USER_CANCEL (0xf), which is consumed either way; mode 3 keeps
    // the window.
    if (MsgId != 0xf)
        return false;
    if (Mode != kNnLoadSaveModeStoryStar)
        ResumeAndDelete();
    return true;
}

void NnLoadSave::Draw()
{
    // the software cursor forced on, the routine, WinControl's draw, then +0x33d
    // follows +0x33e.
    if (GetClientMouseCursorDrawFlag() == 0)
        SetClientMouseCursorDrawFlag(1);
    Win3DFlushBeforeOverlayDraw();
    WinControl::Draw();
    if (bListRefreshed != 0)
        bSaveAllowed = 1;
}

bool NnLoadSave::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // (RET 0xc): the list clicks (0xf001 / 0xf002), NN_Confirm's answer (0x3006)
    // and USER_CANCEL / USER_ENTER relays (0xf003 / 0xf004) go on to slot 42; always false.
    if (Msg == 0xf001 || Msg == 0xf002 || Msg == 0x3006 || Msg == 0xf003 || Msg == 0xf004)
        SetProperty(static_cast<int>(Msg), A, B);
    return false;
}

intptr_t NnLoadSave::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc, 0x1218-byte frame: two 0x400-character buffers). Every path
    // returns 0; nothing reaches WinControl's switch.
    wchar_t Format[0x400];
    wchar_t Text[0x400];

    if (static_cast<unsigned>(Msg) > 0x3006)
    {
        // 0xf001..0xf004 (jump table 00487534).
        int const Row = static_cast<int>(B);
        switch (static_cast<unsigned>(Msg))
        {
        case 0xf001:
        {
            // a list click -- that row selected in the list(s) it fits,
            // the date list follows the name list, then SelectSave.
            if (Row >= -1)
            {
                if (Row < NameList.ItemCount)
                    NameList.SelectedIndex = Row;
                if (Row >= -1 && Row < TimeList.ItemCount)
                    TimeList.SelectedIndex = Row;
            }
            int const Selected = NameList.SelectedIndex;
            if (Selected >= -1 && Selected < TimeList.ItemCount)
                TimeList.SelectedIndex = Selected;
            SelectSave(Selected);
            return 0;
        }
        case 0xf002:
        {
            // a double click loads, in modes 1 and 3.
            if (Mode != kNnLoadSaveModeLoad && Mode != kNnLoadSaveModeStoryStar)
                return 0;
            if (Row >= -1)
            {
                if (Row < NameList.ItemCount)
                    NameList.SelectedIndex = Row;
                if (Row >= -1 && Row < TimeList.ItemCount)
                    TimeList.SelectedIndex = Row;
            }
            int const Selected = NameList.SelectedIndex;
            if (Selected >= -1 && Selected < TimeList.ItemCount)
                TimeList.SelectedIndex = Selected;
            SelectSave(Selected);
            if (Selected == -1)
                return 0;
            LoadSave(Selected);
            return 0;
        }
        case 0xf003:
            // the Event::Handler face's slot 0 with USER_CANCEL.
            OnEvent(0xf);
            return 0;
        case 0xf004:
            // save, when allowed and in save mode.
            if (bSaveAllowed != 0 && Mode == kNnLoadSaveModeSave)
                SaveGame();
            return 0;
        default:
            return 0;
        }
    }

    if (Msg == 0x3006)
    {
        // NN_Confirm answered (A == 1 yes) the question +0x338 names.
        switch (ConfirmMode)
        {
        case 1:
            // delete the selected save -- its CHARACTER_ID copied
            // (REP MOVSD 0x80), the list erased (the routine), the lists cleared,
            // on the field, the list refilled.
            if (A == 1 && NameList.SelectedIndex != -1)
            {
                CHARACTER_ID Id;
                memcpy(&Id, &Entries[NameList.SelectedIndex].Info->Id, sizeof(CHARACTER_ID));
                Entries.clear();
                ClearListsAndSelection();
                if (G_LoginMgr.DestroyCharacter(Id) != 1)
                {
                    FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kNnLoadSaveCpp, 0x3ab,
                          "Failed to delete character\n");
                }
                RefreshSaveList();
            }
            break;
        case 2:
            // back to the main menu at the next Update.
            if (A == 1)
                bExitToMainMenu = 1;
            break;
        case 3:
            break;
        case 4:
            // overwrite -- the save the routine would have made, with
            // +0x368 as the character.
            if (A == 1)
            {
                CHARACTER_ID Id;
                memcpy(&Id, &OverwriteId, sizeof(CHARACTER_ID));
                Entries.clear();
                ClearListsAndSelection();
                bSaveAllowed = 0;
                bListRefreshed = 0;
                if (G_Server != nullptr)
                {
                    G_Server->SaveGame(Id, SaveName, GetPlayerConnectionId()); // +0xe8
                }
                else
                {
                    FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kNnLoadSaveCpp, 0x366,
                          "No server, cannot save\n");
                }
                RefreshSaveList();
                bListRefreshed = 1;
            }
            break;
        default:
            FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid confirmation mode %d\n", kNnLoadSaveCpp,
                  0x2dc, ConfirmMode);
            break;
        }
        // the question is over -- the edit box back, both lists selectable.
        ConfirmMode = 0;
        if (DescripEdit != nullptr)
            DescripEdit->Activate();
        NameList.bSelectable = 1;
        TimeList.bSelectable = 1;
        return 0;
    }

    switch (Msg)
    {
    case 0x42:
    {
        // caseD_42: slot 31 SetOffset(*A), slot 5 SetPosition(+0x60), then each list's slot 5
        // with the window position plus its offset (FADD float per component).
        float const* const Offset3 = reinterpret_cast<float const*>(A);
        SetOffset(Offset3[0], Offset3[1], Offset3[2]);
        SetPosition(Position);
        float const NamePosition[3] = {Position[0] + NameListOffset[0],
                                       Position[1] + NameListOffset[1],
                                       Position[2] + NameListOffset[2]};
        NameList.SetPosition(NamePosition);
        float const TimePosition[3] = {Position[0] + TimeListOffset[0],
                                       Position[1] + TimeListOffset[1],
                                       Position[2] + TimeListOffset[2]};
        TimeList.SetPosition(TimePosition);
        return 0;
    }
    case 0x26:
    {
        // caseD_26: a button was pressed (A = the sender).
        if (A == reinterpret_cast<uintptr_t>(Buttons[0]))
        {
            // -> the branch
            if (bButtonEnabled[0] != 0)
                SaveGame();
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(Buttons[1]))
        {

            if (bButtonEnabled[1] == 0 || NameList.SelectedIndex == -1)
                return 0;
            LoadSave(NameList.SelectedIndex);
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(Buttons[2]))
        {
            // "delete <name>?" -- ids 0x75c formatted with the row's
            // name (+0x14 string of the entry), NN_Confirm style "ConfirmPlayerText".
            if (bButtonEnabled[2] == 0 || NameList.SelectedIndex == -1)
                return 0;
            wchar_t const* const Name = Entries[NameList.SelectedIndex].Name.c_str();
            ConfirmMode = 1;
            if (DescripEdit != nullptr)
                DescripEdit->Deactivate(false); // (0)
            GetIdsStringWWithRawFallback(0x75c, Format, 0x400);
            // MSVCRT swprintf has no count in baseline; the port bounds it by the buffer.
            swprintf(Text, 0x400, Format, Name);
            NnConfirmOpen(this, Text, "ConfirmPlayerText");
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(Buttons[3]))
        {
            // "quit to the main menu?" (ids 0x75b); both lists stop
            // taking clicks while it is up.
            if (bButtonEnabled[3] == 0)
                return 0;
            GetIdsStringWWithRawFallback(0x75b, Text, 0x400);
            ConfirmMode = 2;
            if (DescripEdit != nullptr)
                DescripEdit->Deactivate(false);
            NameList.bSelectable = 0;
            TimeList.bSelectable = 0;
            NnConfirmOpen(this, Text, nullptr);
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(Buttons[4]))
        {
            // resume -- the routine, slot 10 on both lists, slot 33
            // ReleaseMouse, the routine(this).
            if (bButtonEnabled[4] == 0)
                return 0;
            NnModalResumeGame(bCursorDrawFlag);
            NameList.Destroy();
            TimeList.Destroy();
            ReleaseMouse();
            Win3DDeleteTree(this);
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(DescripButton))
            return 0;
        if (A == reinterpret_cast<uintptr_t>(CloseButton))
        {

            ResumeAndDelete();
            return 0;
        }
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kNnLoadSaveCpp, 0x260, "Unknown button\n");
        return 0;
    }
    case 0x2c:
    {
        // caseD_2c: the pointer entered a button -- its caption, when shown, gets property
        // (0x51, 1, 0).
        WinControl* const Caption = ButtonTextFor(A);
        if (Caption == nullptr || (Caption->Flags & 1) == 0)
            return 0;
        Caption->SetProperty(0x51, 1, 0);
        return 0;
    }
    case 0x2d:
    {
        // caseD_2d: it left -- (0x51, 0, 0), then the colour back to the field (0x33).
        WinControl* const Caption = ButtonTextFor(A);
        if (Caption == nullptr || (Caption->Flags & 1) == 0)
            return 0;
        Caption->SetProperty(0x51, 0, 0);
        Caption->SetProperty(0x33, reinterpret_cast<uintptr_t>(G_HudChromeColorRgba), 0);
        return 0;
    }
    case 0x3e:
        // caseD_3e: the scroll bar moved -- B is the first visible row of both lists.
        NameList.FirstVisible = static_cast<int>(B);
        TimeList.FirstVisible = static_cast<int>(B);
        return 0;
    default:
        return 0;
    }
}

void NnLoadSave::SaveGame()
{
    // (0xa14-byte frame).
    FNnLoadSaveEntry const* Selected = nullptr;
    if (NameList.SelectedIndex != -1)
    {
        // CHARACTER_ID::operator= (COMMON.DLL) from the row's record.
        Selected = &Entries[NameList.SelectedIndex];
        OverwriteId = Selected->Info->Id;
    }

    // the routine(+0x568, 0x1f), or an empty name without the box.
    if (DescripEdit != nullptr)
        DescripEdit->GetText(SaveName, 0x1f);
    else
        SaveName[0] = 0;

    // a different name than the selected row's makes a new save.
    if (Selected != nullptr)
    {
        if (wcscmp(Selected->Name.c_str(), SaveName) != 0)
            OverwriteId.Buffer[0] = 0;
    }

    // no name -- the prefix and the next number (_ltow base 10).
    if (SaveName[0] == 0)
    {
        wcscpy(SaveName, SaveNamePrefix.c_str());
        _ltow(NextSaveNumber, SaveName + SaveNamePrefixLength, 10);
    }

    if (OverwriteId.Buffer[0] != 0)
    {
        // "overwrite?" (ids 0x706), +0x338 = 4, the edit box and the
        // lists put down, then the routine's body inline with style "ConfirmText".
        wchar_t Text[0x400];
        GetIdsStringWWithRawFallback(0x706, Text, 0x400);
        ConfirmMode = 4;
        if (DescripEdit != nullptr)
            DescripEdit->Deactivate(false);
        NameList.bSelectable = 0;
        TimeList.bSelectable = 0;
        NnConfirmOpen(this, Text, "ConfirmText");
        return;
    }

    // the save itself -- the id copied (REP MOVSD 0x80), the list erased,
    // the lists cleared, IServer::SaveGame (+0xe8) or the FDUMP, the list refilled.
    CHARACTER_ID Id;
    memcpy(&Id, &OverwriteId, sizeof(CHARACTER_ID));
    Entries.clear();
    ClearListsAndSelection();
    bSaveAllowed = 0;
    bListRefreshed = 0;
    if (G_Server != nullptr)
    {
        G_Server->SaveGame(Id, SaveName, GetPlayerConnectionId());
    }
    else
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kNnLoadSaveCpp, 0x366,
              "No server, cannot save\n");
    }
    RefreshSaveList();
    bListRefreshed = 1;
}

void NnLoadSave::RefreshSaveList()
{
    // .
    // erase(begin, end) (the routine + the routine).
    Entries.clear();

    if (G_LoginMgr.RequestCharacterInfo() == 1)
    {
        // count the character list and reserve that many rows.
        int Count = 0;
        MetaNode<CCharacterSet::CharacterInfo>* Node = nullptr;
        while (G_LoginMgr.Characters.traverse(Node, true))
            Count = Count + 1;
        if (Entries.capacity() < static_cast<size_t>(Count))
            Entries.reserve(static_cast<size_t>(Count));

        // one row per record that is not the restart save (+0x200):
        // the listed text (the routine) and the date (filetime_to_wstring(+0x228)), appended
        // (the routine).
        for (int Index = 0; Index < Count; ++Index)
        {
            CCharacterSet::CharacterInfo* const Info = G_LoginMgr.CharacterAt(Index);
            if (Info->bIsRestart != 0)
                continue;
            FNnLoadSaveEntry Entry;
            Entry.Info = Info;
            wchar_t const* const EntryText = NnLoadSaveEntryText(Info);
            Entry.Name.assign(EntryText, wcslen(EntryText));
            wchar_t const* const TimeText =
                filetime_to_wstring(*reinterpret_cast<_FILETIME const*>(&Info->TimestampLow));
            Entry.Time.assign(TimeText, wcslen(TimeText));
            Entries.push_back(Entry);
        }

        // saves failed while away (the field) -- NN_Error ids 0x5cd
        // over this window, +0x338 = 3.
        if (G_LoginMgr.SaveFailureCount != 0)
        {
            ConfirmMode = 3;
            if (DescripEdit != nullptr)
                DescripEdit->Deactivate(false);
            NnErrorOpen(0x5cd, this);
        }
    }
    else
    {

        FDUMP(0x100001, "%s(%d) : *** ERROR: %s", kNnLoadSaveCpp, 0x446,
              "Failed to retrieve available save games\n");
    }

    // the next generated number -- the largest N among rows named
    // <prefix>N (wcsncmp, then wcstol base 10 that consumes the rest, N <= 1000000000), plus 1.
    int Largest = 0;
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        wchar_t const* const Name = Entries[Index].Name.c_str();
        if (wcsncmp(Name, SaveNamePrefix.c_str(), static_cast<size_t>(SaveNamePrefixLength)) != 0)
            continue;
        wchar_t* End = nullptr;
        long const Value = wcstol(Name + SaveNamePrefixLength, &End, 10);
        if (End == nullptr || *End != 0)
            continue;
        if (Value > 1000000000)
            continue;
        if (Value <= Largest)
            continue;
        Largest = static_cast<int>(Value);
    }
    NextSaveNumber = Largest + 1;

    // at most 0x100 rows go into the lists (names, then dates).
    int Count = static_cast<int>(Entries.size());
    if (Count > 0x100)
        Count = 0x100;
    NameList.ItemCount = Count;
    TimeList.ItemCount = Count;
    for (int Index = 0; Index < Count; ++Index)
    {
        NameList.SetItemText(Index, Entries[Index].Name.c_str());
        TimeList.SetItemText(Index, Entries[Index].Time.c_str());
    }

    // the scroll bar -- range 0..1 and hidden when the rows fit
    // (count - rows + 1 <= 1), else range 0..count and shown.
    if (Count - VisibleRows + 1 <= 1)
    {
        Scroll->SetRange(0.0f, 1.0f);
        HideControl(Scroll);
    }
    else
    {
        Scroll->SetRange(0.0f, static_cast<float>(Count));
        ShowControl(Scroll);
    }
}

void NnLoadSave::SelectSave(int Index)
{
    // (RET 4, 0x800-byte frame).
    if (Index == -1)
    {
        // the values read ids 0x595, the description emptied; Save,
        // Main Menu and Resume on, Load and Delete greyed (caption property 2 = 1).
        for (int Value = 0; Value < 4; ++Value)
            InfoValues[Value]->SetProperty(0x1e, 0x595, 0);
        if (DescripEdit != nullptr)
            DescripEdit->ClearText();
        ButtonTexts[0]->SetProperty(2, 0, 0);
        bButtonEnabled[0] = 1;
        ButtonTexts[1]->SetProperty(2, 1, 0);
        bButtonEnabled[1] = 0;
        ButtonTexts[2]->SetProperty(2, 1, 0);
        bButtonEnabled[2] = 0;
        ButtonTexts[3]->SetProperty(2, 0, 0);
        bButtonEnabled[3] = 1;
        ButtonTexts[4]->SetProperty(2, 0, 0);
        bButtonEnabled[4] = 1;
        return;
    }

    int const Count = static_cast<int>(Entries.size());
    if (Index < 0 || Index >= Count)
    {

        FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid index %d for savegame\n", kNnLoadSaveCpp,
              0x4ea, Index);
        return;
    }

    CCharacterSet::CharacterInfo const* const Info = Entries[Index].Info;

    // the system's name (ISystem slot 3, strid_name), or empty.
    Universe::ISystem const* const System = Universe::get_system(Info->SystemId);
    if (System != nullptr)
        InfoValues[0]->SetProperty(0x1e, (uintptr_t)System->GetStridName(), 0);
    else
        InfoValues[0]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(kEmptyText), 0);

    // credits (+0x234) and rank (+0x270) through L"%d" (005d0ec4).
    wchar_t Number[0x400];
    swprintf(Number, 0x400, L"%d", Info->Money);
    InfoValues[1]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(Number), 0);
    swprintf(Number, 0x400, L"%d", Info->Rank);
    InfoValues[2]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(Number), 0);

    // the ship's name (Archetype::GetShip(+0x230)->ids_name), or empty
    // with no ship.
    if (Info->ShipArchetype != 0)
    {
        Archetype::Ship const* const Ship = Archetype::GetShip(Info->ShipArchetype);
        InfoValues[3]->SetProperty(0x1e, static_cast<uintptr_t>(Ship->IdsName), 0);
    }
    else
    {
        InfoValues[3]->SetProperty(0x1d, reinterpret_cast<uintptr_t>(kEmptyText), 0);
    }

    // the description box -- DescripStrId (+0x224) through the routine
    // into the field (FDUMP at line 0x64d on a miss, the buffer either way), else the
    // Description string; the routine, then the routine(text, 0).
    if (DescripEdit != nullptr)
    {
        wchar_t const* Description = nullptr;
        if (Info->DescripStrId != 0)
        {
            if (GetIdsStringWWithRawFallback(Info->DescripStrId, G_NnLoadSaveText, 0x100) == 0)
            {
                FDUMP(0x100001, "%s(%d) : *** ERROR: Failed to find string resource %d\n",
                      kNnLoadSaveCpp, 0x64d, Info->DescripStrId);
            }
            Description = G_NnLoadSaveText;
        }
        else
        {
            Description = Info->Description.c_str();
        }
        DescripEdit->ClearText();
        DescripEdit->SetText(Description, nullptr);
    }

    // the restart / autosave records (+0x200 / +0x201) cannot be saved
    // over or deleted.
    if (Info->bIsRestart == 0 && Info->bIsAutoSave == 0)
    {
        for (int Button = 0; Button < 5; ++Button)
        {
            ButtonTexts[Button]->SetProperty(2, 0, 0);
            bButtonEnabled[Button] = 1;
        }
        return;
    }
    ButtonTexts[0]->SetProperty(2, 1, 0);
    bButtonEnabled[0] = 0;
    ButtonTexts[1]->SetProperty(2, 0, 0);
    bButtonEnabled[1] = 1;
    ButtonTexts[2]->SetProperty(2, 1, 0);
    bButtonEnabled[2] = 0;
    ButtonTexts[3]->SetProperty(2, 0, 0);
    bButtonEnabled[3] = 1;
    ButtonTexts[4]->SetProperty(2, 0, 0);
    bButtonEnabled[4] = 1;
}

void NnLoadSave::LoadSave(int Index)
{
    // (RET 4).
    int const Count = static_cast<int>(Entries.size());
    if (Index < 0 || Index >= Count)
    {

        FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid index %d for savegame\n", kNnLoadSaveCpp,
              0x502, Index);
        return;
    }
    // the routine(record, record +0x201).
    CCharacterSet::CharacterInfo const* const Info = Entries[Index].Info;
    QueueLoadGameCharacter(&Info->Id, Info->bIsAutoSave != 0);
    // close.
    ResumeAndDelete();
    // the routine(1).
    FadeIn(true);
}

void NnLoadSave::Update()
{
    // WinControl's Update (the routine).
    WinControl::Update();

    // outside the main-menu question, the row under the pointer in
    // either list is drawn highlighted in both (ListBox +0x80).
    if (ConfirmMode != 2)
    {
        int Row = -1;
        if (NameList.HoverIndex != -1)
            Row = NameList.HoverIndex;
        if (TimeList.HoverIndex != -1)
            Row = TimeList.HoverIndex;
        if (Row >= -1)
        {
            if (Row < NameList.ItemCount)
                NameList.Field80 = Row;
            if (Row >= -1 && Row < TimeList.ItemCount)
                TimeList.Field80 = Row;
        }
    }

    // main menu confirmed -- the routine(0xc5, 0) (USER_EXIT_GAME),
    // then close.
    if (bExitToMainMenu == 0)
        return;
    bExitToMainMenu = 0;
    HudPostCommand(0xc5, 0);
    ResumeAndDelete();
}

void OpenNnLoadSave(int Mode)
{
    // .
    if (Win3DFindByClassName("NN_LoadSave") != nullptr)
        return;
    Win3D* const Created = WinFactoryCreate("NN_LoadSave", "NN_LoadSave", nullptr, &Mode);
    if (Created == nullptr)
        return;
    NnLoadSave* const LoadSave = static_cast<NnLoadSave*>(Created);
    float const Zero[3] = {0.0f, 0.0f, 0.0f};
    LoadSave->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
    LoadSave->CaptureMouse();
}

wchar_t const* NnLoadSaveEntryText(CCharacterSet::CharacterInfo const* Info)
{
    // (cdecl).
    if (Info->DescripStrId != 0)
    {
        if (GetIdsStringWWithRawFallback(Info->DescripStrId, G_NnLoadSaveText, 0x100) == 0)
        {
            FDUMP(0x100001, "%s(%d) : *** ERROR: Failed to find string resource %d\n",
                  kNnLoadSaveCpp, 0x64d, Info->DescripStrId);
        }
        return G_NnLoadSaveText;
    }
    return Info->Description.c_str();
}
