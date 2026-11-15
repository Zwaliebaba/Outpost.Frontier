// Author: Giperion.

// baseline ServerFilterDialog -- the server browser's filter box. See server_filter_dialog.h.

#include "server_filter_dialog.h"
#include "msvcrt_compat.h" // MsvcrtWtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "client_state.h"     // CallClientStateUpdateSlot (the field vftable[14])
#include "globals.h"          // PumpMessages (the routine), GetClientFrameTimeCapped (the routine)
#include "ids_rdl.h"          // LoadIdsIntoRdl (the routine), RdlAppendNode
#include "nn_ui_pulse.h"      // G_HudChromeColorRgba (the field)
#include "rich_text_win.h"
#include "tooltip_hover.h"    // FToolTipHoverInfo
#include "win3d.h"
#include "win_factory.h"

#include "common_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// WINFACTORY<ServerFilterDialog> (the branch -> the routine; Create
// ; atexit the branch -> the routine).
WINFACTORY<ServerFilterDialog> G_ServerFilterDialogFactory("ServerFilterDialog");

namespace
{
    // the branch: FLD -0.2f (the field), FADD 0.03f (the field), FSTP.
    // The exact sum rounded once, as the x87 does.
    float const G_FilterLabelX =
        static_cast<float>(static_cast<double>(-0.200000003f) + static_cast<double>(0.0299999993f));

    // the branch: FLD 0.28f (the field), FADD the field, FADD 0.03f,
    // FSTP. Both sums are exact in double, so the one rounding to float matches the x87's.
    float const G_FilterDataX = static_cast<float>(static_cast<double>(0.280000001f) +
                                                   static_cast<double>(G_FilterLabelX) +
                                                   static_cast<double>(0.0299999993f));

    // the five row descriptions. Row 0's Y (0x00614828 = 0.12f) is linked
    // data; every other field is a the branch store (0x00562750..0x005628c6): X = -0.2f
    // (the field), Z = 0 (the field), LabelX = the field, DataX = the field except
    // row 0's 0.12f (0x00614838).
    FServerFilterRowDesc const G_ServerFilterRows[5] = {
        {0.119999997f, 0.0f, -0.200000003f, G_FilterLabelX, 0.119999997f, 1, 0x60078, 0x1b58},
        {0.0599999987f, 0.0f, -0.200000003f, G_FilterLabelX, G_FilterDataX, 0, 0x60074, 0x1b59},
        {0.0f, 0.0f, -0.200000003f, G_FilterLabelX, G_FilterDataX, 0, 0x60073, 0x1b5a},
        {-0.0599999987f, 0.0f, -0.200000003f, G_FilterLabelX, G_FilterDataX, 0, 0x60077, 0x1b5c},
        {-0.119999997f, 0.0f, -0.200000003f, G_FilterLabelX, G_FilterDataX, 0, 0x60075, 0x1b5d},
    };

    // the number in the rows' names ("FilterCheck%d" ...), linked as 1 and
    // raised by every BuildRow; nothing resets it, so each dialog gets fresh names.
    int G_ServerFilterRowCounter = 1;

    // PTR_s_ServerFilterDialog_006148cc -> 0x005e205c.
    char const* const kServerFilterDialogClass = "ServerFilterDialog";

    char const kCheckMesh[] = "Interface\\NeuroNet\\Inventory\\check.3db";
    char const kCheckAltMesh[] = "Interface\\NeuroNet\\Inventory\\repairblackbox.3db";
    char const kCheckSound[] = "ui_shipview_select";
    char const kBackPlaneMesh[] = "Interface\\NeuroNet\\MISC_bigblackplane.3db";
    char const kBackgroundMesh[] = "Interface\\intro\\objects\\MULTI_attentionreq'd.cmp";
    char const kCancelMesh[] = "Interface\\NeuroNet\\Inventory\\X.3db";
    char const kSelectSound[] = "ui_select_item";

    // The idiom BuildRow spells for the edit box's two lists: the node cloned (slot +0x1c), the
    // clone pushed (the list cell clones it again), the first clone deleted.
    void AppendNodeClone(RenderDisplayList& Rdl, RDLNode const& Node)
    {
        RDLNode* const Temp = Node.Clone();
        RdlAppendNode(Rdl, Temp->Clone());
        delete Temp;
    }
}

ServerFilterDialog::ServerFilterDialog()
    : OkButton(nullptr)
    , CancelButton(nullptr)
    , Title(nullptr)
    , Target(nullptr)
{
    // 0x0056393c..0x005639f2: WinControl ctor, the rows (the branch), the
    // ServerFilter ctor at +0x378 (the routine inlined), +0x374 / +0x32c / +0x330 / +0x334 = 0.
}

ServerFilterDialog::~ServerFilterDialog()
{
    // the vptrs, the rows through `eh vector destructor iterator` (the branch
    // stores zero into each row of the object being destroyed), then WinControl's dtor
    // . The ServerFilter at +0x378 has no destructor.
}

void FServerFilterDialogRow::Build(WinControl* Dialog, FServerFilterRowDesc const& Desc)
{
    // . The names go through the routine (_vsnprintf into a 0x2000-byte buffer).
    char Name[0x2000];
    float const* const DialogOffset = Dialog->Offset; // +0xa8

    // the toggle at {X, Y, Z}, half extents {0.012, 0.012, 0}, the row's
    // tool tip twice, then property 0x35 = 1.
    float const CheckOffset[3] = {Desc.X, Desc.Y, Desc.Z};
    float const CheckHalfExtents[3] = {0.0120000001f, 0.0120000001f, 0.0f};
    _snprintf(Name, 0x2000, "FilterCheck%d", G_ServerFilterRowCounter);
    Check = Dialog->CreateToggleButtonControl(Name, kCheckMesh, kCheckAltMesh, kCheckSound,
                                              nullptr, CheckOffset, nullptr, CheckHalfExtents,
                                              0, 0, 0, 0, Desc.ToolTipIds,
                                              Desc.ToolTipIds);
    Check->SetProperty(0x35, 1, 0);

    // the label -- the row's IDS text behind StyleNode 0x5724.
    _snprintf(Name, 0x2000, "FilterLabel%d", G_ServerFilterRowCounter);
    Label = static_cast<RichTextWin*>(WinFactoryCreate(Name, "RichTextWin", Dialog, nullptr));
    {
        RenderDisplayList Text;
        LoadIdsIntoRdl(Desc.LabelIds, Text); // , result ignored
        Label->ClearContent();
        {
            StyleNode const Style(0x5724);
            Label->AppendNode(Style);
        }
        Label->AppendContent(Text);
    }

    // {LabelX, Y + 0.03 * 0.5, Z} plus the dialog's +0xa8, summed on
    // the FPU in that order and stored as floats.
    float const LabelOffset[3] = {
        static_cast<float>(static_cast<double>(Desc.LabelX) +
                           static_cast<double>(DialogOffset[0])),
        static_cast<float>((0.0299999993f * 0.5 + static_cast<double>(Desc.Y)) +
                           static_cast<double>(DialogOffset[1])),
        static_cast<float>(static_cast<double>(Desc.Z) + static_cast<double>(DialogOffset[2]))};
    Label->SetAttachOffset(LabelOffset);

    // 0.28 wide beside an edit box, else 0.38; 0.03 high.
    Label->SetNormSize(Desc.bHasData != 0 ? 0.280000001f : 0.379999995f, 0.0299999993f);

    // slot 46's record +4 = the row's tool tip, the feed on, align 2.
    Label->GetToolTipHoverInfo()->IdsAlt = Desc.ToolTipIds;
    Label->SetToolTipEnabled(true); // (1)
    Label->SetAlignMode(2);         // (2)

    if (Desc.bHasData == 0)
    {
        Data = nullptr;
    }
    else
    {
        // the edit box -- {DataX, Y + 0.015, Z} plus +0xa8, 0.1 x 0.03,
        // fixed style 0x9ca4, no IME, a centred one-space prefix, four characters, the chrome
        // frame, align 2.
        _snprintf(Name, 0x2000, "FilterData%d", G_ServerFilterRowCounter);
        Data = static_cast<RichTextEditWin*>(
            WinFactoryCreate(Name, "RichTextEditWin", Dialog, nullptr));
        float const DataOffset[3] = {
            static_cast<float>(static_cast<double>(Desc.DataX) +
                               static_cast<double>(DialogOffset[0])),
            static_cast<float>((0.0299999993f * 0.5 + static_cast<double>(Desc.Y)) +
                               static_cast<double>(DialogOffset[1])),
            static_cast<float>(static_cast<double>(Desc.Z) +
                               static_cast<double>(DialogOffset[2]))};
        Data->SetAttachOffset(DataOffset);
        Data->SetNormSize(0.100000001f, 0.0299999993f);
        Data->SetFixedStyle(true);                            // (1)
        Data->SetImeEnabled(false);                           // (0)
        {
            StyleNode const Style(0x9ca4);
            AppendNodeClone(Data->FixedStyle, Style);         // +0x4fc, the routine
        }
        {
            JustifyNode const Justify(TextJustifyCenter);     // JustifyNode(1)
            AppendNodeClone(Data->Prefix, Justify);           // +0x4dc
        }
        {
            TextNode const Space(L" ", -1);
            AppendNodeClone(Data->Prefix, Space);
        }
        Data->SetMaxLength(4);                                // (4)
        Data->SetDrawFrame(true);                             // (1)
        Data->SetFrameColor(
            reinterpret_cast<unsigned long const*>(G_HudChromeColorRgba));
        Data->SetAlignMode(2);                                // (2)
    }

    // .
    G_ServerFilterRowCounter = G_ServerFilterRowCounter + 1;
}

void ServerFilterDialog::ReadControls()
{
    // . Toggle state 0 is "on" (property 0x2f reads it; SETZ).
    if (Target == nullptr)
        return;
    int State = 0;
    Rows[0].Check->SetProperty(0x2f, reinterpret_cast<uintptr_t>(&State), 0);
    Target->bFilterPing = static_cast<unsigned char>(State == 0);
    Rows[1].Check->SetProperty(0x2f, reinterpret_cast<uintptr_t>(&State), 0);
    Target->bFilterKnown = static_cast<unsigned char>(State == 0);
    Rows[2].Check->SetProperty(0x2f, reinterpret_cast<uintptr_t>(&State), 0);
    Target->bFilterVersion = static_cast<unsigned char>(State == 0);
    Rows[3].Check->SetProperty(0x2f, reinterpret_cast<uintptr_t>(&State), 0);
    Target->bFilterPassword = static_cast<unsigned char>(State == 0);
    Rows[4].Check->SetProperty(0x2f, reinterpret_cast<uintptr_t>(&State), 0);
    Target->bFilterField3e = static_cast<unsigned char>(State == 0);

    // the ping box's first 15 characters through _wtoi into the
    // maximum; the minimum back to 0.
    wchar_t Text[0x10];
    Rows[0].Data->GetText(Text, 0x10);
    Target->PingMax = static_cast<unsigned>(MsvcrtWtoi(Text));
    Target->PingMin = 0;
}

void ServerFilterDialog::WriteControls()
{
    // . Property 0x30 with the enable byte (the toggle turns a zero into state 1).
    if (Target == nullptr)
        return;
    Rows[0].Check->SetProperty(0x30, Target->bFilterPing, 0);
    Rows[1].Check->SetProperty(0x30, Target->bFilterKnown, 0);
    Rows[2].Check->SetProperty(0x30, Target->bFilterVersion, 0);
    Rows[3].Check->SetProperty(0x30, Target->bFilterPassword, 0);
    Rows[4].Check->SetProperty(0x30, Target->bFilterField3e, 0);

    // the maximum ping in decimal into the box, current attributes.
    wchar_t Text[0x10];
    _ultow(Target->PingMax, Text, 10);
    Rows[0].Data->SetText(Text, nullptr); // (text, 0)
}

void ServerFilterDialog::Close()
{
    ReleaseMouse(); // slot 33
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        Win3DDeleteTree(Child);
        Child = Next;
    }
    Destroy();   // slot 10
    delete this; // slot 0 (1)
}

bool ServerFilterDialog::Create(Win3D* NewParent, void* Data)
{
    // . Win3D's slot 9 (the routine) with no data first.
    if (!Win3D::Create(NewParent, nullptr))
        return false;

    // in front at z = -1, the black backing plane.
    Offset[2] = -1.0f;
    SetMesh(kBackPlaneMesh, nullptr); // slot 38

    // "Filter_BG" 0.01 behind the plane -- not kept.
    float const BackgroundOffset[3] = {Offset[0], Offset[1], Offset[2] + 0.00999999978f};
    float const ZeroHalfExtents[3] = {0.0f, 0.0f, 0.0f};
    float const ZeroVector338[3] = {0.0f, 0.0f, 0.0f};
    CreateStaticControl("Filter_BG", kBackgroundMesh, BackgroundOffset, nullptr, ZeroHalfExtents,
                        0, 0, 0, 1, 1, 0, 1, 0, 0, 0, ZeroVector338);

    // the caller's filter, and a copy to put back on cancel.
    Target = static_cast<ServerFilter*>(Data);
    Backup = *Target;

    // the title -- IDS 0x60079 behind StyleNodes 0x9ca4 and 0x1df5, at
    // {-0.15, 0.19, 0} from this control, 0.3 x 0.04.
    Title = static_cast<RichTextWin*>(WinFactoryCreate("FilterTitle", "RichTextWin", this, nullptr));
    float const TitleOffset[3] = {Offset[0] + -0.150000006f, Offset[1] + 0.189999998f, Offset[2]};
    Title->SetAttachOffset(TitleOffset);
    Title->SetNormSize(0.300000012f, 0.0399999991f);
    {
        RenderDisplayList Text;
        LoadIdsIntoRdl(0x60079, Text);                // , result ignored
        Title->ClearContent();
        {
            StyleNode const Style(0x9ca4);
            Title->AppendNode(Style);
        }
        {
            StyleNode const Style(0x1df5);
            Title->AppendNode(Style);
        }
        Title->AppendContent(Text);
    }                                                 // list dtor inlined (the routine + free)

    // "FilterCancel" -- X.3db at {0.225, 0.175, -1}, half extents
    // {0.02, 0.02, 0}, tool tip 0x409.
    float const CancelOffset[3] = {0.224999994f, 0.174999997f, -1.0f};
    float const CancelHalfExtents[3] = {0.0199999996f, 0.0199999996f, 0.0f};
    CancelButton = CreateButtonControl("FilterCancel", kCancelMesh, nullptr, kSelectSound, nullptr,
                                       CancelOffset, nullptr, CancelHalfExtents, 0, 0, 0, 0,
                                       0x409, 0, 0);

    // "FilterOk" -- a wire button at {0, -0.17, -1}, half extents
    // {0.05, 0.017, 0}, tool tip 0xbdb, caption IDS 0x411.
    float const OkOffset[3] = {0.0f, -0.170000002f, -1.0f};
    float const OkHalfExtents[3] = {0.0500000007f, 0.0170000009f, 0.0f};
    OkButton = CreateRichTextWireButtonControl("FilterOk", kSelectSound, OkOffset, OkHalfExtents,
                                               0, 0, 0xbdb, 0x411, 0, 0);

    // the five rows, the field up to the field.
    for (int Index = 0; Index < 5; ++Index)
        Rows[Index].Build(this, G_ServerFilterRows[Index]);

    WriteControls();
    CaptureMouse();   // slot 32
    if (Rows[0].Data != nullptr)
        Rows[0].Data->Activate();
    return true;
}

bool ServerFilterDialog::MouseEvent(FWin3DMouseEvent const* Event)
{
    // . On a button-down (+4 == 2) the edit boxes are dropped one by one until the
    // mouse is over one of them or its label, which then takes the focus; rows without a box
    // are passed over. Every mouse event is taken.
    if (Event->Field1 == 2)
    {
        bool bFocused = false;
        for (int Index = 0; Index < 5; ++Index)
        {
            if (bFocused)
                continue;
            RichTextEditWin* const Data = Rows[Index].Data;
            if (Data == nullptr)
                continue;
            Data->Deactivate(false); // (0)
            float Ray[3];
            if (Data->HitTest(Win3DGetMouseRay(Ray)) ||
                Rows[Index].Label->HitTest(Win3DGetMouseRay(Ray)))
            {
                Data->Activate();
                bFocused = true;
            }
        }
    }
    return true;
}

bool ServerFilterDialog::KeyEvent(unsigned Key, unsigned char* Modifiers)
{
    // . Escape (without modifier bit 1) cancels: the copy back, then the close.
    if ((*Modifiers & 2) != 0 || Key != 0x1b)
        return false;
    *Target = Backup;
    Close();
    return true;
}

bool ServerFilterDialog::Slot23(unsigned Msg, unsigned A, unsigned B)
{
    // .
    if (Msg == 0xf004)
    {
        // the ping box finished -- dropped, then read.
        RichTextEditWin* const Data = Rows[0].Data;
        if (reinterpret_cast<uintptr_t>(Data) == static_cast<uintptr_t>(A))
        {
            if (Data != nullptr)
                Data->Deactivate(false); // (0)
            ReadControls();
            return true;
        }
    }
    else if (Msg == 0xf003)
    {
        // cancel.
        *Target = Backup;
        Close();
        return true;
    }
    return WinControl::Slot23(Msg, A, B);
}

intptr_t ServerFilterDialog::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // .
    if (Msg == 0x26)
    {
        if (A == reinterpret_cast<uintptr_t>(OkButton))
        {
            // OK -- read the controls, close.
            ReadControls();
            Close();
            return 0;
        }
        if (A == reinterpret_cast<uintptr_t>(CancelButton))
        {
            // cancel -- the copy back, close.
            *Target = Backup;
            Close();
            return 0;
        }
    }
    else if (Msg == 0x30)
    {
        // a toggle changed.
        ReadControls();
        return 0;
    }
    return WinControl::SetProperty(Msg, A, B);
}

bool ServerFilterDialogRun(Win3D* Parent, ServerFilter* Filter)
{
    // .
    Win3D* const Created =
        WinFactoryCreate(kServerFilterDialogClass, kServerFilterDialogClass, Parent, Filter);
    if (Created == nullptr)
        return true;
    ServerFilterDialog* const Dialog = static_cast<ServerFilterDialog*>(Created);
    float const Zero[3] = {0.0f, 0.0f, 0.0f};
    Dialog->SetProperty(0x42, reinterpret_cast<uintptr_t>(Zero), 0);
    Dialog->CaptureMouse(); // slot 32

    // pump while a ServerFilterDialog is registered.
    int ExitCode = 0;
    while (Win3DFindByClassName(kServerFilterDialogClass) != nullptr)
    {
        if (!PumpMessages(&ExitCode))
            return false;
        double const Delta = GetClientFrameTimeCapped();
        Win3DUpdateAll(Delta);
        CallClientStateUpdateSlot(static_cast<float>(Delta));
    }
    return true;
}
