// Author: Giperion.

#include "nn_error.h"

#include "client_frame_tick.h"  // BeginClientRenderFrame / EndClientRenderFrame
#include "common_misc.h"        // WaitForClientRunPermission / GiveServerRunPermission
#include "common_server.h"      // IServer
#include "font_string.h"        // FFontString (+0x28 LayoutGen, +0x30 bMultiLine)
#include "globals.h"            // PumpMessages, GetClientFrameTimeCapped, timing pause
#include "hud_command_dispatch.h"
#include "input_manager.h"      // MouseFrameUpdate (the routine)
#include "login_mgr.h"          // G_Server (the field)
#include "nn_modal.h"
#include "nn_text_control.h"    // NnTextControl::GetFontString (+0x334)
#include "win3d.h"
#include "win3d_renderables.h"  // Win3DFlushBeforeOverlayDraw (the routine)
#include "win_factory.h"

#include <stdlib.h>
#include <windows.h>

// WINFACTORY<NN_Error> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnError> G_NnErrorFactory("NN_Error");

namespace
{
    // zeroed by every NN_Error dismiss path (0047280a, 004728d4, 0047295a) and
    // read by the routine (00473f59). No instruction writes anything else to it, so
    // 's result is always true.
    int G_NnErrorPending = 0;

    char const kErrorFrameMesh[] = "Interface\\NeuroNet\\errorexplanation.cmp";
}

NnError::NnError()
{
    // 00474929..0047495b: the routine, vptrs, the routine, INC the field.
    PushClientTimingPause();
    G_ModalCaptureCount = G_ModalCaptureCount + 1;
}

NnError::~NnError()
{
    // DEC the field, the routine(0), the routine(+0x32c), the routine; then
    // and the routine as the base-class destructors.
    G_ModalCaptureCount = G_ModalCaptureCount - 1;
    Win3DSetKeyFocus(nullptr);
    HudPopInputFocus(AsHudCmdHandler());
    PopClientTimingPause();
}

bool NnError::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    // the routine("Arrow", 0) -- the Mouse.cpp cursor shape.
    MouseSetCursor("Arrow", false);

    float const Zero[3] = {0.0f, 0.0f, 0.0f}; // local_24 / local_18

    // the routine "Frame" at (0, 0, 0xbfd9999a).
    float FramePosition[3] = {0.0f, 0.0f, NnFloatBits(0xbfd9999a)};
    CreateStaticControl("Frame", kErrorFrameMesh, FramePosition, nullptr, Zero, 0, 0, 0, 1, 1, 0,
                        1, 0, 0, 0, Zero);

    // "Text" (the field) at (0, 0, -1), no ids -> +0x334.
    float TextPosition[3] = {0.0f, 0.0f, -1.0f};
    Text = CreateTextControl("Text", "SubHeader", TextPosition, nullptr, NnFloatBits(0x3f07ae14),
                             NnFloatBits(0x3e08b439), NnFloatBits(0x3fa00000), 0, 1, 0, 0);
    // Text's Font::String (+0x334) goes multi-line (+0x30 = 1, and +0x28 = 0
    // to re-lay it out) unless it already is. No null test.
    FFontString* const TextFont = static_cast<NnTextControl*>(Text)->GetFontString();
    if (TextFont->bMultiLine != 1)
    {
        TextFont->bMultiLine = 1;
        TextFont->LayoutGen = 0;
    }

    // the OK box -> +0x338.
    float ButtonPosition[3] = {NnFloatBits(0xbc449ba6), NnFloatBits(0xbd99999a), -1.0f};
    float ButtonExtents[3] = {NnFloatBits(0x3e083127), NnFloatBits(0x3cd4fdf4), 0.0f};
    OkayButton = CreateWireButtonControl("ErrorOkayButton", "ui_select_item", ButtonPosition,
                                         ButtonExtents, 0, 0, 0, 0, 0);

    // the OK caption, ids 0x411 -> +0x33c.
    float OkayPosition[3] = {NnFloatBits(0xbcbc6a7f), NnFloatBits(0xbdb851ec), -1.0f};
    OkayText = CreateTextControl("ErrorOkayText", "ButtonText", OkayPosition, nullptr,
                                 NnFloatBits(0x3e21cac1), NnFloatBits(0x3d75c28f),
                                 NnFloatBits(0x3fa00000), 0x411, 1, 0, 0);

    // the title, ids 0x3e8, last argument 1 -> +0x340.
    float TitlePosition[3] = {NnFloatBits(0xbe6c8b44), NnFloatBits(0x3def9db2), -1.0f};
    Title = CreateTextControl("ErrorDialogTitle", "PageHeader", TitlePosition, nullptr,
                              NnFloatBits(0x3ee66666), NnFloatBits(0x3d75c28f),
                              NnFloatBits(0x3f99999a), 0x3e8, 1, 0, 1);
    // the same Font::String poke on the title.
    FFontString* const TitleFont = static_cast<NnTextControl*>(Title)->GetFontString();
    if (TitleFont->bMultiLine != 1)
    {
        TitleFont->bMultiLine = 1;
        TitleFont->LayoutGen = 0;
    }

    // slot 42 (0x1d, L"OutpostFrontier Encountered an Error", 0), no null test.
    Title->SetProperty(0x1d, reinterpret_cast<uintptr_t>(L"OutpostFrontier Encountered an Error"), 0);

    // / 0047278d
    DrawLayer = 0x6e;
    DrawSubLayer = 6;

    bCursorDrawFlag = NnModalPauseGame();
    Win3DSetKeyFocus(this);
    HudPushInputFocus(AsHudCmdHandler());

    RegisterEvent(0xf);
    RegisterEvent(0x11);

    // the routine, SETNZ.
    return Win3D::Create(NewParent, Data);
}

void NnError::Dismiss()
{
    G_NnErrorPending = 0;
    if (Parent != nullptr)
        Parent->Slot23(0x3006, 0, 0);
    NnModalResumeGame(bCursorDrawFlag);
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

void NnError::Draw()
{
    // .
    Win3DFlushBeforeOverlayDraw();
    WinControl::Draw();            // JMP the routine
}

bool NnError::KeyEvent(unsigned Key, unsigned char* Modifiers)
{
    // (RET 8): only with modifier bit 1 set (00472947 TEST [EAX],2 / JZ), Escape
    // (0x1b) or Enter (0xd) dismisses.
    if ((*Modifiers & 0x02) == 0)
        return false;
    if (Key != 0x1b && Key != 0xd)
        return false;
    Dismiss();
    return true;
}

intptr_t NnError::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): every message returns 0 without reaching WinControl's switch.
    (void)B;
    if (Msg == 0x26)
    {
        // a click on the OK box dismisses.
        if (A == reinterpret_cast<uintptr_t>(OkayButton))
            Dismiss();
        return 0;
    }
    if (Msg == 0x42)
    {
        float const* const Offset3 = reinterpret_cast<float const*>(A);
        SetOffset(Offset3[0], Offset3[1], Offset3[2]);
        SetPosition(Position);
    }
    return 0;
}

bool NnError::OnEvent(unsigned MsgId)
{
    // 0xf or 0x11 dismisses; anything else is not handled.
    if (MsgId != 0xf && MsgId != 0x11)
        return false;
    Dismiss();
    return true;
}

namespace
{
    // The head the routine and the routine share (00473c04..00473c8b / 00473cbb..00473d40).
    void RaiseWithIds(unsigned Ids, Win3D* Parent)
    {
        if (Win3DFindByClassName("NN_Error") != nullptr)
            return;
        Win3D* const Created = WinFactoryCreate("NN_Error", "NN_Error", Parent, nullptr);
        if (Created == nullptr)
            return;
        NnError* const Dialog = static_cast<NnError*>(Created);
        float const Zero[3] = {0.0f, 0.0f, 0.0f};
        Dialog->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
        if (Dialog->Title != nullptr)
            Dialog->Title->Flags = static_cast<unsigned char>(Dialog->Title->Flags & 0xfc);
        if (Dialog->Text != nullptr)
            Dialog->Text->SetProperty(0x1e, Ids, 0);
        Dialog->CaptureMouse();
    }
}

void NnErrorOpen(unsigned Ids, Win3D* Parent)
{
    // .
    RaiseWithIds(Ids, Parent);
}

Win3D* NnErrorFind()
{
    // .
    return Win3DFindByClassName("NN_Error");
}

void NnErrorRunModal(unsigned Ids, bool bUpdateServer, Win3D* Parent)
{
    // .
    RaiseWithIds(Ids, Parent);

    // .
    int ExitCode = 0; // local_1c
    if (Win3DFindByClassName("NN_Error") == nullptr)
        return;
    do
    {
        Sleep(10);
        if (!PumpMessages(&ExitCode))
            return;
        if (bUpdateServer)
        {
            if (G_Server != nullptr)
                G_Server->Update(); // +0x2c
        }
        else
        {
            WaitForClientRunPermission();
        }
        Win3DUpdateAll(GetClientFrameTimeCapped()); // (the routine())
        if (BeginClientRenderFrame())
        {
            Win3DDrawAll();
            EndClientRenderFrame(); // + the routine
        }
        if (!bUpdateServer)
            GiveServerRunPermission();
    } while (Win3DFindByClassName("NN_Error") != nullptr);
}

bool NnErrorRunModalMessage(char const* Message)
{
    // .
    if (Win3DFindByClassName("NN_Error") != nullptr)
        return true;

    // the second lookup is in the listing too.
    if (Win3DFindByClassName("NN_Error") == nullptr)
    {
        Win3D* const Created = WinFactoryCreate("NN_Error", "NN_Error", nullptr, nullptr);
        if (Created != nullptr)
        {
            NnError* const Dialog = static_cast<NnError*>(Created);
            float const Zero[3] = {0.0f, 0.0f, 0.0f};
            Dialog->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
            if (Dialog->Title != nullptr)
                Dialog->Title->Flags = static_cast<unsigned char>(Dialog->Title->Flags | 0x03);
            wchar_t Wide[0x400]; // local_818 region
            mbstowcs(Wide, Message, 0x400);
            if (Dialog->Text != nullptr)
                Dialog->Text->SetProperty(0x1d, reinterpret_cast<uintptr_t>(Wide), 0);
            Dialog->CaptureMouse();
        }
    }

    // .
    int ExitCode = 0; // local_81c
    Sleep(10);
    if (PumpMessages(&ExitCode))
    {
        do
        {
            Win3DUpdateAll(GetClientFrameTimeCapped());
            Win3D* const Live = Win3DFindByClassName("NN_Error");
            if (Live == nullptr)
                break;
            if (BeginClientRenderFrame())
            {
                Win3DBeginDraw();
                Live->Draw();                  // slot 13
                Win3DFlushBeforeOverlayDraw();
                MouseFrameUpdate();
                EndClientRenderFrame();        // + the routine
            }
            Sleep(10);
        } while (PumpMessages(&ExitCode));
    }

    // SETZ on the field.
    return G_NnErrorPending == 0;
}
