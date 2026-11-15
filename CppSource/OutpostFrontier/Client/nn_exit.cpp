// Author: Giperion.

#include "nn_exit.h"

#include "globals.h"            // g_client.window (the field)
#include "hud_command_dispatch.h"
#include "input_manager.h"      // MouseSetCursor, Get/SetClientMouseCursorDrawFlag
#include "nav_bar.h"            // NavBarCloseService (the routine)
#include "nn_pause.h"           // NnPause::CloseNow (the routine)
#include "nn_main_button.h"     // G_bNnKeepPanelOpen (the field)
#include "nn_modal.h"
#include "nn_story_star.h"      // StoryStarOnMissionMessage (the routine)
#include "win3d.h"
#include "win3d_renderables.h"  // Win3DFlushBeforeOverlayDraw (the routine)
#include "win_factory.h"

#include "common_text.h"        // FmtStr

#include <stdio.h>
#include <windows.h>

// WINFACTORY<NN_Exit> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnExit> G_NnExitFactory("NN_Exit");

namespace
{
    char const kExitFrameMesh[] = "Interface\\NeuroNet\\Options\\alt-f4window.cmp";
    char const kExitTextFrameMesh[] = "Interface\\NeuroNet\\Options\\alt-f4textplain.3db";
}

NnExit::NnExit()
    : ResultFn(nullptr)
    , ResultArg(nullptr)
    , bImmediate(0)
{
    // 00472058..0047208c: the routine, the three vptrs, +0x330 = 0, +0x334 = 0,
    // byte +0x338 = 0. Nothing else is written before Create.
}

NnExit::~NnExit()
{
    // vptrs back to NN_Exit's, the routine(+0x32c); then the Event::Handler base
    // dtor the routine and WinControl's the routine run as the base-class destructors.
    HudPopInputFocus(AsHudCmdHandler());
}

bool NnExit::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).

    // `if (!the field) the routine()` -- close NN_MainButton's panel
    // unless something holds it open (G_bNnKeepPanelOpen, nn_main_button.h).
    if (G_bNnKeepPanelOpen == 0)
        NnMainButtonOnModalOpen();
    // the routine(1) -- the NavBar's open service tab closes.
    NavBarCloseService(true);

    // the routine("NN_Pause"), and the routine on it when found.
    NnPause* const Pause = static_cast<NnPause*>(Win3DFindByName("NN_Pause"));
    if (Pause != nullptr)
        Pause->CloseNow();

    // the routine("arrow", 0) -- the Mouse.cpp cursor shape (lower-case in baseline;
    // the lookup is stricmp).
    MouseSetCursor("arrow", false);

    float const Zero[3] = {0.0f, 0.0f, 0.0f}; // local_134 / local_128

    // the routine(&local_118, 0, 0, -1), then the routine.
    float FramePosition[3] = {0.0f, 0.0f, -1.0f};
    CreateStaticControl("ExitFrame", kExitFrameMesh, FramePosition, nullptr, Zero, 0, 0, 0, 1, 1,
                        0, 1, 0, 0, 0, Zero);

    // (0, 0x3cf5c288, -1).
    float TextFramePosition[3] = {0.0f, NnFloatBits(0x3cf5c288), -1.0f};
    CreateStaticControl("ExitTextFrame", kExitTextFrameMesh, TextFramePosition, nullptr, Zero, 0,
                        0, 1, 0, 1, 0, 1, 0, 0, 0, Zero);

    // (0, 0x3d0b4394, -1) -> +0x33c.
    float TextPosition[3] = {0.0f, NnFloatBits(0x3d0b4394), -1.0f};
    ExitText = CreateTextControl("ExitText", "PageHeader", TextPosition, nullptr,
                                 NnFloatBits(0x3eb851ec), NnFloatBits(0x3cf5c28f),
                                 NnFloatBits(0x3fc00000), 0, 1, 0, 0);

    // two buttons side by side. The row coordinate is
    // i * - the field (FILD / FMUL / FSUB, FSTP float); the box sits at
    // + the field and the caption at + the field, both as the X component (the FSTP lands
    // in the first dword of the position; Y is the immediate that follows).
    float const kRowStep = NnFloatBits(0x3e4ccccd);     // 0.2
    float const kRowOrigin = NnFloatBits(0x3e6b851e);   // 0.23
    float const kBoxOffset = NnFloatBits(0x3e051eb8);   // 0.13
    float const kCaptionOffset = NnFloatBits(0x3e051eb9);
    for (int Index = 0; Index < 2; ++Index)
    {
        float const Row = static_cast<float>(Index) * kRowStep - kRowOrigin;
        char Name[0x100]; // local_100

        sprintf(Name, "ExitButtonBox%d", Index);
        float BoxPosition[3] = {Row + kBoxOffset, NnFloatBits(0xbd99999a), -1.0f};
        float BoxExtents[3] = {NnFloatBits(0x3db645a2), NnFloatBits(0x3cac0831), 0.0f};
        ButtonBox[Index] = CreateWireButtonControl(Name, "ui_select_item", BoxPosition,
                                                   BoxExtents, 0, 0, 0, 0, 0);

        sprintf(Name, "ExitButtonText%d", Index);
        float CaptionPosition[3] = {Row + kCaptionOffset, NnFloatBits(0xbd916873), -1.0f};
        ButtonText[Index] = CreateTextControl(Name, "ButtonText", CaptionPosition, nullptr,
                                              NnFloatBits(0x3e8a3d71), NnFloatBits(0x3cf5c28f),
                                              NnFloatBits(0x3fc00000), 0, 1, 0, 0);
    }

    // slot 42 (0x1e, ids, 0) on the three captions, no null test.
    ExitText->SetProperty(0x1e, 0x726, 0);
    ButtonText[0]->SetProperty(0x1e, 0x727, 0);
    ButtonText[1]->SetProperty(0x1e, 0x728, 0);

    // / 00471ae5
    DrawLayer = 0x6e;
    DrawSubLayer = 6;

    bCursorDrawFlag = NnModalPauseGame();
    HudPushInputFocus(AsHudCmdHandler());

    RegisterEvent(0x13);
    RegisterEvent(0x12);
    RegisterEvent(0xf);
    RegisterEvent(0x11);

    // the routine, SETNZ.
    return Win3D::Create(NewParent, Data);
}

void NnExit::Close(int Result)
{
    NnModalResumeGame(bCursorDrawFlag);
    if (ResultFn != nullptr)
        ResultFn(ResultArg, Result);
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

void NnExit::Update()
{
    // no WinControl Update call. With +0x338 set the dialog confirms at once:
    // the same sequence as the confirm button, DestroyWindow included (00471e6a).
    if (bImmediate == 0)
        return;
    Close(0);
    DestroyWindow(g_client.window);
}

void NnExit::Draw()
{
    // .
    if (GetClientMouseCursorDrawFlag() == 0)
        SetClientMouseCursorDrawFlag(1);
    Win3DFlushBeforeOverlayDraw();
    WinControl::Draw();            // JMP the routine
}

intptr_t NnExit::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc). Nothing reaches WinControl's switch: every path returns 0 here.
    (void)B;
    if (Msg == 0x26)
    {
        // which button box sent the click.
        int Index = 0;
        while (Index < 2)
        {
            if (A == reinterpret_cast<uintptr_t>(ButtonBox[Index]))
                break;
            Index = Index + 1;
        }
        if (Index == 2)
            return 0;
        // `CMP EAX,-1` cannot hold here; 00471d21: box 0 confirms and closes the game
        // window, 00471d95: box 1 cancels.
        if (Index == 0)
        {
            Close(0);
            DestroyWindow(g_client.window);
            return 0;
        }
        Close(1);
        return 0;
    }
    if (Msg == 0x42)
    {
        // slot 31 SetOffset with the three floats at A, then slot 5
        // SetPosition(&+0x60).
        float const* const Offset3 = reinterpret_cast<float const*>(A);
        SetOffset(Offset3[0], Offset3[1], Offset3[2]);
        SetPosition(Position);
        return 0;
    }
    return 0;
}

bool NnExit::OnEvent(unsigned MsgId)
{
    // jump table over MsgId - 0xf (00471c94):
    //   0xf / 0x12 -> caseD_0 00471c25: cancel (callback 1), no DestroyWindow
    //   0x11 / 0x13 -> caseD_2 00471ba9: confirm (callback 0), DestroyWindow
    //   0x10 and everything outside 0xf..0x13 -> false
    switch (MsgId)
    {
    case 0xf:
    case 0x12:
        Close(1);
        return true;
    case 0x11:
    case 0x13:
        Close(0);
        DestroyWindow(g_client.window);
        return true;
    default:
        return false;
    }
}

void NnExitPostShipLostMessage()
{
    // . 0x00471e98..0x00471ed6: the FmtStr zeroed (+0, +4 and the fourteen index
    // bytes); begin_mad_lib(0x723), end_mad_lib, the routine(&text, 5), ~FmtStr.
    FmtStr Text;
    Text.begin_mad_lib(0x723);
    Text.end_mad_lib();
    StoryStarOnMissionMessage(&Text, 5);
}

void NnExitOpen(bool bImmediate)
{
    // .
    if (Win3DFindByClassName("NN_Exit") != nullptr)
        return;
    Win3D* const Created = WinFactoryCreate("NN_Exit", "NN_Exit", nullptr, nullptr);
    if (Created == nullptr)
        return;
    NnExit* const Exit = static_cast<NnExit*>(Created);
    float const Zero[3] = {0.0f, 0.0f, 0.0f};
    Exit->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
    Exit->bImmediate = bImmediate ? 1 : 0;
    Exit->CaptureMouse();
}
