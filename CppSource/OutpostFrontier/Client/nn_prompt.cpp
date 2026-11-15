// Author: Giperion.

#include "nn_prompt.h"

#include "client_state.h"       // CallClientStateUpdateSlot (the field vftable[14])
#include "common_misc.h"        // WaitForClientRunPermission / GiveServerRunPermission
#include "common_server.h"      // IServer
#include "common_text.h"        // RenderDisplayList, StyleNode, JustifyNode
#include "globals.h"            // PumpMessages, UpdateClientTiming, GetClientFrameTimeCapped
#include "ids_rdl.h"            // LoadIdsIntoRdl (the routine), RdlAppendNode
#include "input_manager.h"      // MouseSetCursor (the routine)
#include "login_mgr.h"          // G_Server (the field)
#include "nn_modal.h"           // NnFloatBits
#include "rich_text_win.h"
#include "win3d.h"
#include "win3d_renderables.h"  // Win3DFlushBeforeOverlayDraw (the routine)
#include "win_factory.h"
#include "wire_controls.h"

#include <wchar.h>
#include <windows.h>

// WINFACTORY<NN_Prompt> static (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<NnPrompt> G_NnPromptFactory("NN_Prompt");

namespace
{
    char const kPromptFrameMesh[] = "Interface\\NeuroNet\\errorexplanation.cmp";
    char const kPromptExitMesh[] = "Interface\\NeuroNet\\Inventory\\X.3db";
    wchar_t const kPromptEditSeed[] = L" ";
}

NnPrompt::NnPrompt()
    : Byte330(0)
    , Byte331(0)
    , Result(nullptr)
    , EditText(nullptr)
    , Mode(0)
    , PromptText(nullptr)
    , PromptEdit(nullptr)
    , PromptExit(nullptr)
    , PromptNo(nullptr)
    , PromptYes(nullptr)
{
    // 00474a68..00474ac6: the routine, the vptrs, +0x338 / +0x340 / +0x344 /
    // +0x348 / +0x334 / +0x34c / +0x350 = 0, byte +0x331 = 0, +0x33c = 0, INC the field.
    // +0x330 is not written (zero here only as C++ initialisation).
    Pad332[0] = 0;
    Pad332[1] = 0;
    G_ModalCaptureCount = G_ModalCaptureCount + 1;
}

NnPrompt::~NnPrompt()
{
    // DEC the field, the routine(0); then the routine (Event::Handler) and
    // (WinControl) as the base-class destructors.
    G_ModalCaptureCount = G_ModalCaptureCount - 1;
    Win3DSetKeyFocus(nullptr);
}

bool NnPrompt::Create(Win3D* NewParent, void* Data)
{
    // (RET 8). 00472ace..00472aed: the routine(Parent, 0) first; false -> false.
    if (!Win3D::Create(NewParent, nullptr))
        return false;

    // the routine("Arrow", 0).
    MouseSetCursor("Arrow", false);

    FNnPromptPayload const* const Payload = static_cast<FNnPromptPayload const*>(Data);
    // +0x33c = payload[2]; the payload is not null-tested.
    Mode = Payload->Mode;

    // the routine "PromptFrame" at (0, 0, 0xbfd9999a), the NN_Error frame
    // mesh and argument list.
    float const Zero[3] = {0.0f, 0.0f, 0.0f}; // local_48 / local_3c
    float FramePosition[3] = {0.0f, 0.0f, NnFloatBits(0xbfd9999a)};
    CreateStaticControl("PromptFrame", kPromptFrameMesh, FramePosition, nullptr, Zero, 0, 0, 0, 1,
                        1, 0, 1, 0, 0, 0, Zero);

    // the routine("PromptText", 0.7f, 0.15f, {-0.35, 0.12, -1}, 1).
    float TextOffset[3] = {NnFloatBits(0xbeb33333), NnFloatBits(0x3df5c28f), -1.0f};
    PromptText = CreateRichTextWin("PromptText", NnFloatBits(0x3f333333),
                                   NnFloatBits(0x3e19999a), TextOffset, true);

    // only mode 0 gets the edit box.
    if (Mode == 0)
        PromptEdit = static_cast<RichTextEditWin*>(
            WinFactoryCreate("PromptEdit", "RichTextEditWin", this, nullptr));
    else
        PromptEdit = nullptr;

    if (PromptEdit != nullptr)
    {
        // the routine, the routine {-0.3, -0.015, -1}, the routine(0).
        PromptEdit->SetNormSize(NnFloatBits(0x3f19999a), NnFloatBits(0x3cf5c28f));
        float EditOffset[3] = {NnFloatBits(0xbe99999a), NnFloatBits(0xbc75c28f), -1.0f};
        PromptEdit->SetAttachOffset(EditOffset);
        PromptEdit->SetDrawFrame(false);
        // password mode on, fixed style on, IME off (EBP = 0).
        PromptEdit->SetPasswordMode(true);   // (1)
        PromptEdit->SetFixedStyle(true);     // (1)
        PromptEdit->SetImeEnabled(false);    // (0)

        // StyleNode(0x9ca4) -- a clone (slot +0x1c) pushed onto the +0x4fc
        // fixed-style list by the routine, whose cell clones it again; the first clone deleted.
        {
            StyleNode const Style(0x9ca4);
            RDLNode* const Temp = Style.Clone();
            RdlAppendNode(PromptEdit->FixedStyle, Temp->Clone());
            delete Temp;
        }

        // the +0x4dc prefix list emptied (the routine(begin, end)).
        RdlClear(PromptEdit->Prefix);

        // JustifyNode(1) the same way onto the prefix list.
        {
            JustifyNode const Justify(TextJustifyCenter);
            RDLNode* const Temp = Justify.Clone();
            RdlAppendNode(PromptEdit->Prefix, Temp->Clone());
            delete Temp;
        }

        // TextPtrNode(L" " the field, -1) the same way.
        {
            TextPtrNode const Space(kPromptEditSeed, -1);
            RDLNode* const Temp = Space.Clone();
            RdlAppendNode(PromptEdit->Prefix, Temp->Clone());
            delete Temp;
        }

        // relayout, then activate.
        PromptEdit->RebuildDisplay();
        PromptEdit->Activate();
    }

    // the OK / Yes tool tip defaults to 0xd48 in mode 0 and to 0 otherwise;
    // a non-zero payload[0] replaces it. The No tool tip is payload[1].
    int YesToolTip = (Mode == 0) ? 0xd48 : 0;
    if (Payload->YesToolTipIds != 0)
        YesToolTip = Payload->YesToolTipIds;
    int const NoToolTip = Payload->NoToolTipIds;

    // The boxes: the routine(name, "ui_select_item", pos, {0.05, 0.017, 0}, 0, 0, tip,
    // caption, 0, 0).
    float const ButtonExtents[3] = {NnFloatBits(0x3d4ccccd), NnFloatBits(0x3c8b4396), 0.0f};
    if (Mode == 0)
    {
        // "PromptOk" centred, caption 0x411 -> +0x350.
        float OkPosition[3] = {0.0f, NnFloatBits(0xbd851eb8), -1.0f};
        PromptYes = static_cast<RichTextWireButtonControl*>(CreateRichTextWireButtonControl(
            "PromptOk", "ui_select_item", OkPosition, ButtonExtents, 0, 0, YesToolTip, 0x411, 0,
            0));
    }
    else if (Mode == 1 || Mode == 2)
    {
        // / 00472ef5..00472f50: "PromptYes" on the left -> +0x350, caption
        // 0x4dc in mode 1 and none in mode 2.
        float YesPosition[3] = {NnFloatBits(0xbd8f5c29), NnFloatBits(0xbd851eb8), -1.0f};
        unsigned const YesCaption = (Mode == 1) ? 0x4dcu : 0u;
        PromptYes = static_cast<RichTextWireButtonControl*>(CreateRichTextWireButtonControl(
            "PromptYes", "ui_select_item", YesPosition, ButtonExtents, 0, 0, YesToolTip,
            YesCaption, 0, 0));

        // "PromptNo" on the right -> +0x34c, caption 0x4dd in mode 1 and
        // none in mode 2.
        float NoPosition[3] = {NnFloatBits(0x3d8f5c29), NnFloatBits(0xbd851eb8), -1.0f};
        unsigned const NoCaption = (Mode == 1) ? 0x4ddu : 0u;
        PromptNo = static_cast<RichTextWireButtonControl*>(CreateRichTextWireButtonControl(
            "PromptNo", "ui_select_item", NoPosition, ButtonExtents, 0, 0, NoToolTip, NoCaption,
            0, 0));
    }

    // the routine "PromptExit", the X mesh, sound "ui_select_item", at
    // (0.345, 0.105, -1), half extents (0.02, 0.02, 0), every flag 0 -> +0x348.
    float ExitPosition[3] = {NnFloatBits(0x3eb0a3d7), NnFloatBits(0x3dd70a3d), -1.0f};
    float ExitExtents[3] = {NnFloatBits(0x3ca3d70a), NnFloatBits(0x3ca3d70a), 0.0f};
    PromptExit = CreateButtonControl("PromptExit", kPromptExitMesh, nullptr, "ui_select_item",
                                     nullptr, ExitPosition, nullptr, ExitExtents, 0, 0, 0, 0, 0,
                                     0, 0);


    DrawLayer = 0x6e;
    DrawSubLayer = 6;

    // mode 1 listens for 0x13, 0x12 and 0xf; mode 2 for 0xf only.
    if (Mode == 1)
    {
        RegisterEvent(0x13);
        RegisterEvent(0x12);
        RegisterEvent(0xf);
    }
    else if (Mode == 2)
    {
        RegisterEvent(0xf);
    }

    // the routine(this); AL = 1.
    Win3DSetKeyFocus(this);
    return true;
}

void NnPrompt::Draw()
{
    // .
    Win3DFlushBeforeOverlayDraw();
    WinControl::Draw();            // JMP the routine
}

void NnPrompt::CopyEditText()
{
    // e.g. 004730a4..00473115: PromptEdit and EditText both non-null -> the routine(buffer,
    // 0x400), wcslen, then the string's _Grow / copy / _Eos (or the routine + _Eos, or
    // assign(ptr, len) -- the three spellings at the six sites all make *EditText the buffer).
    if (PromptEdit == nullptr || EditText == nullptr)
        return;
    wchar_t Buffer[0x400];
    PromptEdit->GetText(Buffer, 0x400);
    EditText->assign(Buffer, wcslen(Buffer));
}

void NnPrompt::Close()
{
    ReleaseMouse(); // slot 33 (+0x84)
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();   // slot 10 (+0x28)
    delete this; // slot 0 (1)
}

bool NnPrompt::OnEvent(unsigned MsgId)
{
    // (RET 4): 0x13 answers 1; 0xf and 0x12 answer 0; anything else is not
    // handled.
    int Answer = 0;
    if (MsgId == 0x13)
        Answer = 1;
    else if (MsgId != 0xf && MsgId != 0x12)
        return false;

    CopyEditText();
    if (Result != nullptr)
        *Result = Answer;
    Close();
    return true;
}

bool NnPrompt::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // (RET 0xc): 0xf003 answers 0, 0xf004 answers 1, both close and return true;
    // anything else goes to WinControl's the routine.
    int Answer = 0;
    if (Msg == 0xf004)
        Answer = 1;
    else if (Msg != 0xf003)
        return WinControl::Slot23(Msg, A, B);

    CopyEditText();
    if (Result != nullptr)
        *Result = Answer;
    Close();
    return true;
}

intptr_t NnPrompt::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): every path returns 0 and none reaches WinControl's switch.
    (void)B;
    if (Msg == 0x42)
    {
        // slot 31 SetOffset(*A), slot 5 SetPosition(+0x60).
        float const* const Offset3 = reinterpret_cast<float const*>(A);
        SetOffset(Offset3[0], Offset3[1], Offset3[2]);
        SetPosition(Position);
        return 0;
    }
    if (Msg != 0x26)
        return 0;

    if (A == reinterpret_cast<uintptr_t>(PromptExit))
    {
        // the X box answers 0 and closes the whole dialog.
        CopyEditText();
        if (Result != nullptr)
            *Result = 0;
        Close();
        return 0;
    }

    if (A == reinterpret_cast<uintptr_t>(PromptNo))
    {
        // No answers 3 in mode 2 and 0 otherwise.
        CopyEditText();
        if (Result != nullptr)
            *Result = (Mode == 2) ? 3 : 0;
    }
    else if (A == reinterpret_cast<uintptr_t>(PromptYes))
    {
        // OK / Yes answers 2 in mode 2 -- and then closes like the X box --
        // and 1 otherwise.
        CopyEditText();
        if (Mode == 2)
        {
            if (Result != nullptr)
                *Result = 2;
            Close();
            return 0;
        }
        if (Result != nullptr)
            *Result = 1;
    }
    else
    {
        return 0;
    }

    // slot 33 ReleaseMouse, then the routine on the dialog itself.
    ReleaseMouse();
    Win3DDeleteTree(this);
    return 0;
}

void NnPrompt::SetPromptIds(unsigned Ids, unsigned short StyleId)
{
    // (RET 8).
    if (PromptText == nullptr || Ids == 0)
        return;

    // a local list holding a clone of StyleNode(StyleId).
    RenderDisplayList Text;
    StyleNode Style(StyleId);
    RdlAppendNode(Text, Style.Clone());

    // the routine(Ids, &list); only on success the routine and
    // (&list). No trailing-paragraph trim.
    if (LoadIdsIntoRdl(Ids, Text))
    {
        PromptText->ClearContent();
        PromptText->AppendContent(Text);
    }
    // the list's nodes and sentinel go with it (the member dtor).
}

void NnPrompt::SetPromptRdl(RenderDisplayList const* Text, unsigned short StyleId)
{
    // (RET 8): PromptText null -> nothing. Text is not null-tested.
    if (PromptText == nullptr)
        return;
    PromptText->ClearContent();
    StyleNode Style(StyleId);
    PromptText->AppendNode(Style);
    PromptText->AppendContent(*Text);
}

void NnPrompt::SetButtonRdl(int Which, RenderDisplayList const* Caption)
{
    // (RET 8): 0 -> +0x350, 1 -> +0x34c, anything else -> nothing. Neither the
    // box nor its caption window (*(box + 0x394)) is null-tested.
    RichTextWireButtonControl* Button = nullptr;
    if (Which == 0)
        Button = PromptYes;
    else if (Which == 1)
        Button = PromptNo;
    else
        return;

    RichTextWin* const CaptionWin = *Button->TextWinRef;
    CaptionWin->ClearContent();
    StyleNode Style(0x828b);
    CaptionWin->AppendNode(Style);
    JustifyNode Justify(TextJustifyCenter);       // JustifyNode(1)
    CaptionWin->AppendNode(Justify);
    CaptionWin->AppendContent(*Caption);
}

namespace
{
    // The head the three entry points share (e.g. 00473f7c..00473fe4): the routine("NN_Prompt"),
    // and only when that misses the routine("NN_Prompt", "NN_Prompt", 0, &payload). A prompt
    // that is already up is reused as it is -- the payload then goes nowhere.
    NnPrompt* RaisePrompt(FNnPromptPayload* Payload)
    {
        Win3D* Found = Win3DFindByClassName("NN_Prompt");
        if (Found == nullptr)
            Found = WinFactoryCreate("NN_Prompt", "NN_Prompt", nullptr, Payload);
        return static_cast<NnPrompt*>(Found);
    }

    // slot 42 (0x42, &{0, 0, 0}, 0): centre the dialog.
    void CentrePrompt(NnPrompt* Dialog)
    {
        float const Zero[3] = {0.0f, 0.0f, 0.0f};
        Dialog->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
    }

    // The loop the three entry points share (e.g. 00474034..004740d2): while an NN_Prompt is
    // registered -- Sleep(10), the routine, the routine (false -> the caller returns false),
    // IServer::Update or WaitForClientRunPermission, the routine(the routine()), the current
    // client state's slot 14 with the same time as a float, GiveServerRunPermission.
    bool PumpWhilePromptUp(bool bUpdateServer)
    {
        int ExitCode = 0; // local_2c
        if (Win3DFindByClassName("NN_Prompt") == nullptr)
            return true;
        do
        {
            Sleep(10);
            UpdateClientTiming();
            if (!PumpMessages(&ExitCode))
                return false;
            if (bUpdateServer)
                G_Server->Update(); // +0x2c, no null test
            else
                WaitForClientRunPermission();
            double const Delta = GetClientFrameTimeCapped();
            Win3DUpdateAll(Delta);
            CallClientStateUpdateSlot(static_cast<float>(Delta));
            if (!bUpdateServer)
                GiveServerRunPermission();
        } while (Win3DFindByClassName("NN_Prompt") != nullptr);
        return true;
    }
}

bool NnPromptRunTextEntry(unsigned Ids, std::wstring* Text, bool bUpdateServer,
                          int OkToolTipIds)
{
    // .
    FNnPromptPayload Payload = {OkToolTipIds, 0, 0};
    NnPrompt* const Dialog = RaisePrompt(&Payload);
    int Result = 0; // local_30
    if (Dialog != nullptr)
    {
        CentrePrompt(Dialog);
        Dialog->SetPromptIds(Ids, 0x5c12);
        // Text->c_str() (not null-tested); with an edit box,
        // (text, 0) seeds it.
        wchar_t const* const Seed = Text->c_str();
        if (Dialog->PromptEdit != nullptr)
            Dialog->PromptEdit->SetText(Seed, nullptr);

        Dialog->EditText = Text;
        Dialog->Result = &Result;
        Dialog->CaptureMouse(); // slot 32
    }

    if (!PumpWhilePromptUp(bUpdateServer))
        return false;
    // SETZ result == 1.
    return Result == 1;
}

bool NnPromptRunYesNo(unsigned Ids, unsigned short StyleId, bool bUpdateServer,
                      int YesToolTipIds, int NoToolTipIds)
{
    // .
    FNnPromptPayload Payload = {YesToolTipIds, NoToolTipIds, 1};
    NnPrompt* const Dialog = RaisePrompt(&Payload);
    int Result = 0; // local_30
    if (Dialog != nullptr)
    {
        CentrePrompt(Dialog);
        Dialog->SetPromptIds(Ids, StyleId);
        Dialog->Result = &Result;
        Dialog->CaptureMouse();
    }

    if (!PumpWhilePromptUp(bUpdateServer))
        return false;

    return Result == 1;
}

bool NnPromptRunChoice(RenderDisplayList const* Text, unsigned short StyleId,
                       RenderDisplayList const* YesCaption, RenderDisplayList const* NoCaption,
                       int* OutChoice, bool bUpdateServer)
{
    // .
    FNnPromptPayload Payload = {0, 0, 2};
    NnPrompt* const Dialog = RaisePrompt(&Payload);
    int Result = 0; // local_30
    if (Dialog != nullptr)
    {
        CentrePrompt(Dialog);
        Dialog->SetPromptRdl(Text, StyleId);
        Dialog->Result = &Result;
        Dialog->SetButtonRdl(0, YesCaption);
        Dialog->SetButtonRdl(1, NoCaption);
        Dialog->CaptureMouse();
    }

    if (!PumpWhilePromptUp(bUpdateServer))
        return false;


    if (Result == 0)
        return false;
    if (Result == 2)
    {
        *OutChoice = 0;
        return true;
    }
    if (Result == 3)
        *OutChoice = 1;
    return true;
}
