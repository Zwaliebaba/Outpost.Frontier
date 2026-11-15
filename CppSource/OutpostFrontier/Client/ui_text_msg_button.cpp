// Author: Giperion.

#include "ui_text_msg_button.h"

#include "client_mesh_load.h"
#include "ids_rdl.h"
#include "menu_ui_init.h"   // PlayUiRolloverSound (the routine)
#include "nn_ui_pulse.h"
#include "ui_page.h"      // UiWidgetMathEngine (the field)
#include "ui_page_desc.h"
#include "win_factory.h"

#include "NeuronRuntime.h"
#include "common_csys.h"
#include "engine/sceneruntime.h"
#include "x86math.h"

#include <string.h>

namespace
{
    // , built by the static initializer the branch: byte +0 = 8, the dword at +1 =
    // (old & 0xff000000) | 0x00e0c387 over zeroed .bss -- bytes 08 87 C3 E0. The idle caption
    // attributes (the hover colour is computed per frame).
    TextRenderAttributes const G_UiTextMsgButtonIdleAttrs = {0xe0c38708ul};

    // , the branch: bytes 08 80 80 80 -- the caption of a non-interactive button.
    TextRenderAttributes const G_UiTextMsgButtonInactiveAttrs = {0x80808008ul};

    // .rdata 0x005d4f58, the "<default>" style mesh.
    char const kUiTextMsgButtonDefaultStyle[] = "Interface\\NeuroNet\\Inventory\\FRONT_button.cmp";

}

UiTextMsgButton::UiTextMsgButton()
    : bInteractive(0)
    , LocalWidth(0.0f)
    , LocalHeight(0.0f)
    , MessageArg(0)
    , Field4c4(0)
{
    // inline: RichTextWin ctor the routine, then the empty RDLs at +0x4a8 and
    // +0x4b4 and the two vftables. +0x498..+0x4a4 and +0x4c0 are left to Create; the zeros here
    // are only C++ initialisation.
    CaptionAttrs.Packed = 0;
    Pad499[0] = 0;
    Pad499[1] = 0;
    Pad499[2] = 0;
}

UiTextMsgButton::~UiTextMsgButton()
{
    // the +0x4b4 and +0x4a8 lists (the members' own destructors), then
    // RichTextWin's (the routine).
}

void UiTextMsgButton::SetPosition(float const* NewPosition)
{
    // (RET 4). The style mesh sits at the button's centre: half the payload size
    // along the control's axes (the field = 0.5f, the field = -0.5f), scaled onto the
    // z = -1 plane (the field = -1.0f) and written straight into the WinControl offset
    // +0xa8..+0xb0. Then BaseRichTextWin::SetPosition (the routine).
    // the field -- the UI_PAGE widgets' shared cache
    // (UiWidgetMathEngine), tested and filled from the routine.
    I3DMathEngine* const Math = UiWidgetMathEngine();
    FVector Half;
    Half.X = LocalWidth * 0.5f;
    Half.Y = LocalHeight * -0.5f;
    Half.Z = 0.0f;
    FVector Centre;
    Math->TransformMatrix(Centre, *reinterpret_cast<FMatrix const*>(Orientation), Half);
    double const SumX = static_cast<double>(Centre.X) + NewPosition[0];
    double const SumY = static_cast<double>(Centre.Y) + NewPosition[1];
    double const Scale = -1.0 / NewPosition[2];
    Offset[0] = static_cast<float>(SumX * Scale - NewPosition[0]);
    Offset[1] = static_cast<float>(SumY * Scale - NewPosition[1]);
    Offset[2] = -1.0f - NewPosition[2];
    BaseRichTextWin::SetPosition(NewPosition);
}

void UiTextMsgButton::SetOrientation(float const* NewOrientation)
{
    // (RET 4): WinControl's SetOrientation (the routine), then, with a style mesh
    // bound (the routine), the instance is reset to the identity orientation (Engine +0x94) and
    // updated (Engine +0x6c).
    WinControl::SetOrientation(NewOrientation);
    if (!ClientMeshHolderHasInstance(&MeshHolder))
        return;
    float Identity[9];
    Identity[0] = 1.0f;
    Identity[1] = 0.0f;
    Identity[2] = 0.0f;
    Identity[3] = 0.0f;
    Identity[4] = 1.0f;
    Identity[5] = 0.0f;
    Identity[6] = 0.0f;
    Identity[7] = 0.0f;
    Identity[8] = 1.0f;
    NeuronRuntime::Engine->SetInstanceOrientation(MeshHolder.EngineInstance, Identity);
    NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
}

void UiTextMsgButton::ApplyCaptionLayout()
{
    // the routine clear, then the routine (clone + push) of each node.
    ClearContent();

    JustifyNode Justify(TextJustifyCenter);                          // 1
    AppendNode(Justify);

    RDLRefNode CaptionRef(CaptionBodyList);                          // +0x4a8
    AppendNode(CaptionRef);

    StyleNode Style(0x828b);
    AppendNode(Style);

    TRANode Tra(CaptionAttrs, 0xffffff00u, 0u);                      // +0x4c0
    AppendNode(Tra);

    RDLRefNode TextRef(TextBodyList);                                // +0x4b4
    AppendNode(TextRef);
}

bool UiTextMsgButton::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    bool const bResult = RichTextWin::Create(NewParent, nullptr);
    bInteractive = 1;                                                 // +0x498
    CaptionAttrs = G_UiTextMsgButtonIdleAttrs;                        // +0x4c0

    FUiPayloadTextMsgButton const* const Payload =
        static_cast<FUiPayloadTextMsgButton const*>(Data);
    if (Payload != nullptr)
    {
        LocalWidth = Payload->Right;                                  // +0x49c
        NormWidth = Payload->Right;                                   // +0x330
        LocalHeight = Payload->Bottom;                                // +0x4a0
        NormHeight = Payload->Bottom;                                 // +0x334

        if (Payload->ExtraId != 0)
        {
            // (id, this+0x4b4) -- appends, no trailing-paragraph trim here.
            LoadIdsIntoRdl(static_cast<unsigned>(Payload->ExtraId), TextBodyList);
        }
        else
        {
            // TextNode(caption, -1) (import 0x005c62b0); its vt+0x1c clone goes into the list.
            TextNode Text(Payload->CaptionUtf16, -1);
            RdlAppendNode(TextBodyList, Text.Clone());
        }
        MessageArg = Payload->ExtraInt;                               // +0x4a4
    }

    SetAlignMode(2);

    // MOV EAX,[EDI+0x14] runs on both paths -- baseline reads the payload here even
    // when it was null above.
    char const* const StyleName = Payload->StyleName;
    if (StyleName != nullptr && strcmp(StyleName, "<none>") != 0)
    {
        if (strcmp(StyleName, "<default>") == 0)
            SetMesh(kUiTextMsgButtonDefaultStyle, 0);                 // vt+0x98
        else
            SetMesh(StyleName, 0);
        Field2d2 = 1;                                                 // +0x2d2
    }

    ApplyCaptionLayout();
    return bResult;
}

void UiTextMsgButton::Draw()
{
    // . The caption follows three states: hovered (the pulse around the amber
    // , the routine), idle (the field) and non-interactive (the field). Only
    // the colour bytes +0x4c1..+0x4c3 are compared; a change rebuilds the content.
    bool const bHovered = Win3DGetHover() == this;
    unsigned long PulseArgb = 0;
    NnUiPulseBlendFromArgb(&PulseArgb, G_NnPaletteArgb_679ba4);
    unsigned char* const Attrs = reinterpret_cast<unsigned char*>(&CaptionAttrs.Packed);

    bool bRelayout = false;
    if (bInteractive != 0)
    {
        if (bHovered)
        {
            unsigned char const R = static_cast<unsigned char>(PulseArgb & 0xff);
            unsigned char const G = static_cast<unsigned char>((PulseArgb >> 8) & 0xff);
            unsigned char const B = static_cast<unsigned char>((PulseArgb >> 16) & 0xff);
            if (Attrs[1] != R || Attrs[2] != G || Attrs[3] != B)
            {
                Attrs[1] = R;
                Attrs[2] = G;
                Attrs[3] = B;
                bRelayout = true;
            }
        }
        else if (((CaptionAttrs.Packed ^ G_UiTextMsgButtonIdleAttrs.Packed) & 0xffffff00u) != 0)
        {
            CaptionAttrs = G_UiTextMsgButtonIdleAttrs;
            bRelayout = true;
        }
    }
    else if (((CaptionAttrs.Packed ^ G_UiTextMsgButtonInactiveAttrs.Packed) & 0xffffff00u) != 0)
    {
        CaptionAttrs = G_UiTextMsgButtonInactiveAttrs;
        bRelayout = true;
    }
    if (bRelayout)
        ApplyCaptionLayout();

    // a red debug outline gated on the field, which has four readers
    // and no writer anywhere in the PE -- never drawn.

    BaseRichTextWin::Draw();
}

void UiTextMsgButton::MouseEnter(float const* /*Ray*/)
{
    // (RET 4): 0059a240..0059a24a -- byte +0x498 set -> the routine.
    if (bInteractive != 0)
        PlayUiRolloverSound();
}

bool UiTextMsgButton::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4): a release (Field1 == 2) of button -1 on an interactive button tells
    // the parent (slot 23: 0x3004, +0x4a4, 0) and is consumed.
    if (Event->Field1 != 2)
        return false;
    if (Event->Field0 != -1)
        return false;
    if (bInteractive == 0)
        return false;
    if (Parent != nullptr)
        Parent->Slot23(0x3004u, static_cast<unsigned>(MessageArg), 0u);
    return true;
}

// WINFACTORY<UITextMsgButton> static (the branch -> the routine;
// Create the routine); atexit dtor the routine.
WINFACTORY<UiTextMsgButton> G_UITextMsgButtonFactory("UITextMsgButton");
