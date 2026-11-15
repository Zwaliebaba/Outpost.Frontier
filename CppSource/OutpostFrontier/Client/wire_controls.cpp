// Author: Giperion.

#include "wire_controls.h"

#include "game_options.h"        // GetGameOption (the routine)
#include "globals.h"             // GetClientFrameTimeCapped (the routine)
#include "hud_command_dispatch.h" // HudPostCommand (the routine)
#include "ids_rdl.h"             // LoadIdsIntoRdl (the routine), RdlAppendNode (the routine)
#include "input_manager.h"       // GetClientSoftwareCursor (the field / the field)
#include "menu_sound_arch.h"     // LookupClientSoundArch / PlayClientSoundArchIfIdle
#include "menu_ui_init.h"        // FindUiSoundSlot / PlayOrRewindUiSoundSlot / PlayUiRolloverSound
#include "nn_tooltip.h"          // NnToolTipHideGlobal (the routine)
#include "nn_ui_pulse.h"         // NnUiPulseReset / NnUiPulseBlendArgb / idle palette
#include "space_camera.h"        // FSpaceCamera (the field)
#include "tooltip_hover.h"       // FToolTipHoverInfo / NnToolTipFeedHover (the routine)
#include "win3d.h"
#include "win3d_renderables.h"   // Win3DImmedBatchFlushAll / Win3DImmedBatchReset
#include "win_factory.h"

#include "common_csys.h"         // Acquire3DMathEngine (the routine)
#include "common_id.h"           // CreateID
#include "common_text.h"
#include "x86math.h"

#include <math.h>
#include <string.h>
#include <windows.h>

// / the field / the field -- the three WINFACTORY<T> statics
// (the branch / the branch / the branch; their atexit halves the branch / 005a2880 /
// are WinFactory's destructor).
WINFACTORY<WireStaticControl> G_WireStaticControlFactory("WireStaticControl");
WINFACTORY<WireButtonControl> G_WireButtonControlFactory("WireButtonControl");
WINFACTORY<RichTextWireButtonControl> G_RichTextWireButtonControlFactory(
    "RichTextWireButtonControl");

namespace
{
    // the 3DMathEngine this file acquires on first use (the routine) and shares
    // between SetOrientation (0x005a3a50) and MouseEvent (0x005a4425).
    I3DMathEngine* G_WireMathEngine = nullptr;

    I3DMathEngine* WireMathEngine()
    {
        if (G_WireMathEngine == nullptr)
            G_WireMathEngine = Acquire3DMathEngine();
        return G_WireMathEngine;
    }


    float const kOne = 1.0f;
    // the line width every Draw edge passes to the routine.
    float const kWireLineWidth = 0.003f;

    // (thiscall on the batcher the field, RET 0x14): append one thick line
    // From -> To with the ARGB colour and its alpha-less RGB to the batcher's vertex / index
    // arrays, flushing both batchers first when either is full (0x1800 indices / 0xc00
    // vertices) -- ImmedBatchAddLine (win3d_renderables.cpp).
    void WireBatchAddLine(float const* From, float const* To, unsigned long Argb,
                          unsigned long Rgb, float Width)
    {
        ImmedBatchAddLine(&G_ImmedBatchA, From, To, Argb, Rgb, Width);
    }

    // and 0 -- a 3x3 identity.
    void SetIdentity9(float* Out)
    {
        Out[0] = 1.0f;
        Out[1] = 0.0f;
        Out[2] = 0.0f;
        Out[3] = 0.0f;
        Out[4] = 1.0f;
        Out[5] = 0.0f;
        Out[6] = 0.0f;
        Out[7] = 0.0f;
        Out[8] = 1.0f;
    }

    // The parent notification every WireButtonControl handler spells out: the parent as a
    // WinControl (slot 2), and if it is one, its slot 42 with (Msg, this as a WinControl, 0).
    // Returns the parent's answer, or 0 when there is no WinControl parent.
    intptr_t TellParent(WinControl* Self, int Msg)
    {
        Win3D* const Parent = Self->GetParent();
        if (Parent == nullptr)
            return 0;
        WinControl* const ParentControl = Parent->AsWinControl();
        if (ParentControl == nullptr)
            return 0;
        return ParentControl->SetProperty(Msg, reinterpret_cast<uintptr_t>(Self->AsWinControl()),
                                          0);
    }

    // The ToolTip record the routine / the routine build: key -1 and no alt id first, the
    // empty caption (the routine), then both ids from +0xcc, the aux dword, the wire centre,
    // the half extents and the live byte; the routine; then the Font::String teardown that
    // WinControl::ShowToolTip spells out too.
    void FeedWireToolTip(WireStaticControl* Self, int Aux)
    {
        FToolTipHoverInfo Info;           // caption +0x08 / +0x0c null, the routine on +0x10
        Info.IdsAlt = 0;                  // +0x04
        Info.Ids = -1;                    // +0x00
        Info.AuxField = 0;                // +0x44
        Info.Ids = Self->ToolTipIds;      // +0x00 <- +0xcc
        Info.IdsAlt = Self->ToolTipIds;   // +0x04 <- +0xcc
        Info.AuxField = Aux;              // +0x44
        Info.WorldPos[0] = Self->WireCenter[0];   // +0x48 <- +0x350
        Info.WorldPos[1] = Self->WireCenter[1];
        Info.WorldPos[2] = Self->WireCenter[2];
        Info.HalfExtents[0] = Self->HalfExtents[0]; // +0x54 <- +0xb4
        Info.HalfExtents[1] = Self->HalfExtents[1];
        Info.HalfExtents[2] = Self->HalfExtents[2];
        Info.bLive = 1;                   // +0x60
        NnToolTipFeedHover(&Info);
        // The record's teardown: ~FToolTipHoverInfo.
    }

    // The sound-name store of the routine / the routine: only a non-empty name replaces
    // +0x368, and then the routine looks it up in the UI sound table.
    void StoreSoundName(WireStaticControl* Self, char const* NewSoundName)
    {
        if (NewSoundName == nullptr || NewSoundName[0] == '\0')
            return;
        Self->SoundName.assign(NewSoundName);
        Self->SoundSlot = FindUiSoundSlot(Self->SoundName.c_str());
    }
}

// ---------------------------------------------------------------------------------------
// WireStaticControl
// ---------------------------------------------------------------------------------------

WireStaticControl::WireStaticControl()
{
    // after the routine.
    SetIdentity9(WireOrientation);
    WireCenter[0] = 0.0f;
    WireCenter[1] = 0.0f;
    WireCenter[2] = 0.0f;
    Colour[0] = 0;
    Colour[1] = 0;
    Colour[2] = 0;
    Colour[3] = 0;
    SoundSlot = 0;
    bUseColour = 0;
    // / 005a32d7: only bit 0 of +0x35d is cleared; the other bits keep whatever the
    // allocation held. Only bit 0 is ever read (Draw 005a38bc).
    WireState2 = static_cast<unsigned char>(WireState2 & 0xfe);
    bBlink = 0;
    bBlinkPhase = 0;
    BlinkTimer = 0.0f;
    WireState = 0x60;
    // +0x364 (ToolTipAux) and +0x384 (EdgeMask) are not written here; Init writes the mask.
}

WireStaticControl::~WireStaticControl()
{
    // the +0x368 string is freed, then the routine (WinControl's dtor).
}

bool WireStaticControl::HitTest(float const* Ray)
{
    // .
    return Win3D::HitTest(Ray);
}

void WireStaticControl::SetPosition(float const* NewPosition)
{
    // (RET 4).
    Field2e0[0] = NewPosition[0];
    Field2e0[1] = NewPosition[1];
    Field2e0[2] = NewPosition[2];
    bAttached = 1;

    float Placed[3];
    if ((PositionFlags & 1) != 0)
    {
        float Attach[3];
        GetAttachPosition(Attach); // slot 41
        Placed[0] = NewPosition[0] + Attach[0];
        Placed[1] = Attach[1] + NewPosition[1];
        Placed[2] = Attach[2] + NewPosition[2];
    }
    else
    {
        Placed[0] = NewPosition[0];
        Placed[1] = NewPosition[1];
        Placed[2] = NewPosition[2];
    }
    WinControl::SetPosition(Placed);

    if ((PositionFlags & 1) != 0)
    {
        WireCenter[0] = Offset[0] + Position[0];
        WireCenter[1] = Offset[1] + Position[1];
        WireCenter[2] = Offset[2] + Position[2];
    }
    else
    {
        WireCenter[0] = Position[0];
        WireCenter[1] = Position[1];
        WireCenter[2] = Position[2];
    }
}

void WireStaticControl::SetOrientation(float const* NewOrientation)
{
    // (RET 4): the new orientation times the attach orientation (slot 40) through
    // the 3DMathEngine's MulMatrix (+0x20), copied to Win3D's +0x3c and to +0x32c. Unlike
    // WinControl's the routine it neither moves an engine instance nor recurses into children.
    bAttached = 1;
    FMatrix Attach;
    GetAttachOrientation(&Attach.D[0][0]);
    FMatrix Result;
    WireMathEngine()->MulMatrix(Result, *reinterpret_cast<FMatrix const*>(NewOrientation), Attach);
    memcpy(Orientation, &Result, sizeof(Orientation));
    memcpy(WireOrientation, &Result, sizeof(WireOrientation));
}

bool WireStaticControl::Create(Win3D* NewParent, void* /*Data*/)
{
    // (RET 8): reparent, register, flags = (flags & 0xfb) | 3, then the parent is
    // re-placed at its own position (its slot 5), which positions this control with it.
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | 0x03);
    if (Parent != nullptr)
        Parent->SetPosition(Parent->Position);
    return true;
}

void WireStaticControl::UpdateBlink()
{
    // while +0x35e is set the timer runs; every whole second past 1.0
    // flips +0x35f. The comparisons are FCOM against 1.0 with TEST AH,0x41: "not above" stops.
    if (bBlink == 0)
        return;
    double Timer = GetClientFrameTimeCapped() + static_cast<double>(BlinkTimer);
    BlinkTimer = static_cast<float>(Timer);
    if (!(Timer > kOne))
        return;
    unsigned char Phase = bBlinkPhase;
    do
    {
        Timer = Timer - kOne;
        Phase = (Phase == 0) ? 1 : 0;
    } while (Timer > kOne);
    BlinkTimer = static_cast<float>(Timer);
    bBlinkPhase = Phase;
}

void WireStaticControl::Update()
{
    // Win3D's child walk (the routine), then the blink.
    Win3D::Update();
    UpdateBlink();
}

void WireStaticControl::Draw()
{
    // .
    if (GetGameOption(kGameOptInterfaceGroup) == 0) // (6)
        return;
    if (GetGameOption(kGameOptNeuronet) == 0)       // (9)
        return;
    if ((Flags & kWin3DVisible) == 0)
        return;
    if ((WireState & 0x80) == 0 && (Flags & kWin3DActive) == 0)
        return;
    if (bBlink != 0 && bBlinkPhase != 0)
        return;

    Win3DDrawChildren(this);

    // the colour.
    unsigned char const SavedLit = static_cast<unsigned char>((WireState >> 4) & 1);
    unsigned char Rgba[4];
    if ((Flags & kWin3DActive) == 0)
    {
        WireState = static_cast<unsigned char>(WireState & 0xeb);
        Rgba[0] = 0x80;
        Rgba[1] = 0x80;
        Rgba[2] = 0x80;
        Rgba[3] = 0xff;
    }
    else if (SavedLit != 0 || ((WireState & 0x20) != 0 && (WireState & 0x04) != 0))
    {
        Rgba[0] = 0xff;
        Rgba[1] = 0xff;
        Rgba[2] = 0x00;
        Rgba[3] = 0xff;
    }
    else if ((WireState & 0x02) != 0 || ((WireState & 0x01) != 0 && (WireState & 0x40) != 0))
    {
        // twice: the first result is the colour, the second lands in a scratch
        // dword nobody reads.
        unsigned long Pulse = 0;
        NnUiPulseBlendArgb(&Pulse);
        unsigned long Scratch = 0;
        NnUiPulseBlendArgb(&Scratch);
        memcpy(Rgba, &Pulse, sizeof(Rgba));
    }
    else
    {
        memcpy(Rgba, &G_NnPaletteArgb_679bb0, sizeof(Rgba));
    }
    if ((WireState & 0x08) != 0)
    {
        Rgba[0] = 0xff;
        Rgba[1] = 0xff;
        Rgba[2] = 0xff;
        Rgba[3] = 0xff;
    }
    if (bUseColour != 0)
        memcpy(Rgba, Colour, sizeof(Rgba));

    // the centre (position + offset) goes to +0x350; the corner vectors are
    // (-hx, -hy, -hz) and (hx, -hy, -hz).
    float const Center[3] = {Position[0] + Offset[0], Position[1] + Offset[1],
                             Position[2] + Offset[2]};
    WireCenter[0] = Center[0];
    WireCenter[1] = Center[1];
    WireCenter[2] = Center[2];
    float CornerA[3] = {-HalfExtents[0], -HalfExtents[1], -HalfExtents[2]};
    float CornerB[3] = {-CornerA[0], CornerA[1], CornerA[2]};
    unsigned long const Argb = (static_cast<unsigned long>(Rgba[3]) << 24) |
                               (static_cast<unsigned long>(Rgba[0]) << 16) |
                               (static_cast<unsigned long>(Rgba[1]) << 8) |
                               static_cast<unsigned long>(Rgba[2]);
    unsigned long const Rgb = (static_cast<unsigned long>(Rgba[0]) << 16) |
                              (static_cast<unsigned long>(Rgba[1]) << 8) |
                              static_cast<unsigned long>(Rgba[2]);

    // mask bit 2, the bottom edge, (-hx, -hy) -> (hx, -hy).
    if ((EdgeMask & 0x4) != 0)
    {
        float const To[3] = {Center[0] + CornerB[0], Center[1] + CornerB[1],
                             Center[2] + CornerB[2]};
        float const From[3] = {Center[0] + CornerA[0], Center[1] + CornerA[1],
                               Center[2] + CornerA[2]};
        WireBatchAddLine(From, To, Argb, Rgb, kWireLineWidth);
    }
    // CornerA becomes (hx, hy, -hz). Bit 1, the right edge, (hx, -hy) -> (hx, hy).
    CornerA[0] = -CornerA[0];
    CornerA[1] = -CornerA[1];
    if ((EdgeMask & 0x2) != 0)
    {
        float const To[3] = {Center[0] + CornerA[0], Center[1] + CornerA[1],
                             Center[2] + CornerA[2]};
        float const From[3] = {Center[0] + CornerB[0], Center[1] + CornerB[1],
                               Center[2] + CornerB[2]};
        WireBatchAddLine(From, To, Argb, Rgb, kWireLineWidth);
    }
    // CornerB becomes (-hx, hy, -hz). Bit 0, the top edge, (hx, hy) -> (-hx, hy).
    CornerB[0] = -CornerB[0];
    CornerB[1] = -CornerB[1];
    if ((EdgeMask & 0x1) != 0)
    {
        float const To[3] = {Center[0] + CornerB[0], Center[1] + CornerB[1],
                             Center[2] + CornerB[2]};
        float const From[3] = {Center[0] + CornerA[0], Center[1] + CornerA[1],
                               CornerA[2] + Center[2]};
        WireBatchAddLine(From, To, Argb, Rgb, kWireLineWidth);
    }
    // CornerA is rebuilt as (-hx, -hy, -hz). Bit 3, the left edge, (-hx, hy) ->
    // (-hx, -hy).
    CornerA[0] = -HalfExtents[0];
    CornerA[1] = -HalfExtents[1];
    CornerA[2] = -HalfExtents[2];
    if ((EdgeMask & 0x8) != 0)
    {
        float const To[3] = {CornerA[0] + Center[0], CornerA[1] + Center[1],
                             CornerA[2] + Center[2]};
        float const From[3] = {Center[0] + CornerB[0], Center[1] + CornerB[1],
                               Center[2] + CornerB[2]};
        WireBatchAddLine(From, To, Argb, Rgb, kWireLineWidth);
    }

    // bit 4 goes back to what it was before the inactive branch could clear it.
    WireState = static_cast<unsigned char>((WireState & 0xef) | (SavedLit << 4));
    Slot39(); // vt+0x9c

    // with +0x35d bit 0, both batchers are flushed (the routine on the field,
    // then the field) and their dirty state reset -- the the routine body, inlined.
    if ((WireState2 & 0x01) != 0)
    {
        Win3DImmedBatchFlushAll();
        Win3DImmedBatchReset();
    }
}

void WireStaticControl::ShowToolTip()
{
    // the record's aux dword is +0x364.
    FeedWireToolTip(this, ToolTipAux);
}

intptr_t WireStaticControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): byte table 0x005a3be0 over Msg - 0xc; the rest is the routine.
    switch (Msg)
    {
    case 0x0c:
        WireState = static_cast<unsigned char>((WireState & 0xef) | ((A != 0) ? 0x10 : 0));
        return 0;
    case 0x2c:
        WireState = static_cast<unsigned char>(WireState | 0x02);
        return 0;
    case 0x2d:
        WireState = static_cast<unsigned char>(WireState & 0xfd);
        return 0;
    case 0x33:
        memcpy(Colour, reinterpret_cast<void const*>(A), sizeof(Colour));
        bUseColour = (B == 1) ? 1 : 0;
        return 0;
    case 0x34:
        WireState = static_cast<unsigned char>(WireState & 0x7f);
        if (A != 0)
            WireState = static_cast<unsigned char>(WireState | 0x80);
        return 0;
    case 0x51:
        bBlink = 0;
        if (A != 0)
            bBlink = 1;
        return 0;
    default:
        return WinControl::SetProperty(Msg, A, B);
    }
}

void WireStaticControl::Init(float const* NewOffset, float const* NewHalfExtents,
                             unsigned char bAlwaysWhite, unsigned char bNewNoZEnable,
                             int NewToolTipIds, unsigned NewEdgeMask)
{
    // (RET 0x18).
    EdgeMask = (NewEdgeMask != 0) ? NewEdgeMask : 0xf;
    Offset[0] = NewOffset[0];
    Offset[1] = NewOffset[1];
    Offset[2] = NewOffset[2];
    WireCenter[0] = Offset[0] + Position[0];
    WireCenter[1] = Offset[1] + Position[1];
    WireCenter[2] = Offset[2] + Position[2];
    HalfExtents[0] = NewHalfExtents[0];
    HalfExtents[1] = NewHalfExtents[1];
    HalfExtents[2] = NewHalfExtents[2];
    WireState = static_cast<unsigned char>((WireState & 0xf7) | ((bAlwaysWhite << 3) & 0x08));
    bNoZEnable = bNewNoZEnable;
    ToolTipIds = NewToolTipIds;
}

// ---------------------------------------------------------------------------------------
// WireButtonControl
// ---------------------------------------------------------------------------------------

WireButtonControl::WireButtonControl()
{
    // 005a4e50..005a4e68: bit 0 cleared and bit 2 set; bits 1 and 3..7 keep what
    // the allocation held (bit 1 is written by every press before it is read).
    ButtonState = static_cast<unsigned char>((ButtonState & 0xfe) | 0x04);
    CommandId = 0;
}

WireButtonControl::~WireButtonControl()
{
    // a button still holding the capture leaves the capture stack
    // (the routine) before the string and WinControl are torn down.
    if ((ButtonState & 0x01) != 0)
        Win3DRemoveCapture(this);
}

bool WireButtonControl::HitTest(float const* Ray)
{
    // JMP the routine.
    return WinControl::HitTest(Ray);
}

void WireButtonControl::Update()
{
    // .
    Win3D::Update();
    UpdateBlink();

    // while the button holds the capture, follow the cursor and light / unlight as
    // it moves on and off.
    if ((ButtonState & 0x01) == 0)
        return;

    FSpaceCamera* const Cam = Win3DGetCamera();
    int CursorX = 0;
    int CursorY = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, nullptr); // / the field
    FVector Local;
    Local.X = static_cast<float>((static_cast<double>(CursorX) - Cam->ScreenCenterX) /
                                 Cam->XScale * Cam->ZNear);
    Local.Y = static_cast<float>((static_cast<double>(CursorY) - Cam->ScreenCenterY) /
                                 Cam->YScale * Cam->ZNear);
    Local.Z = -Cam->ZNear;
    FMatrix CamOrientation;
    memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
    FVector World;
    Win3DTransformByMatrix(World, CamOrientation, Local);
    double const Inv =
        kOne / sqrt(static_cast<double>(World.X) * World.X +
                    static_cast<double>(World.Y) * World.Y +
                    static_cast<double>(World.Z) * World.Z);
    float Ray[3];
    Ray[0] = static_cast<float>(World.X * Inv);
    Ray[1] = static_cast<float>(World.Y * Inv);
    Ray[2] = static_cast<float>(World.Z * Inv);

    if (HitTest(Ray)) // slot 4
    {
        if ((WireState & 0x04) != 0)
            return;
        WireState = static_cast<unsigned char>(WireState | 0x04);
        TellParent(this, 0x2b);
        if (Parent == nullptr)
            return;
        NnUiPulseReset();
        TellParent(this, 0x2c);
        return;
    }
    if ((WireState & 0x04) == 0)
        return;
    WireState = static_cast<unsigned char>(WireState & 0xfb);
    TellParent(this, 0x2a);
    TellParent(this, 0x2d);
}

void WireButtonControl::MouseEnter(float const* /*Ray*/)
{
    // (RET 4).
    if (Parent != nullptr)
    {
        NnUiPulseReset();
        TellParent(this, 0x2c);
    }
    WireState = static_cast<unsigned char>(WireState | 0x01);
    if ((ButtonState & 0x04) != 0)
        PlayUiRolloverSound();
}

void WireButtonControl::MouseLeave(float const* /*Ray*/)
{
    // (RET 4).
    if (ToolTipIds != 0)
        NnToolTipHideGlobal();
    TellParent(this, 0x2d);
    WireState = static_cast<unsigned char>(WireState & 0xfe);
}

bool WireButtonControl::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4).
    if ((ButtonState & 0x04) == 0)
        return false;
    // while some control holds the mouse, a button whose WinControl parent does not
    // hold it ignores the event.
    if (Parent != nullptr && Win3DAnyCaptured())
    {
        WinControl* const ParentControl = Parent->AsWinControl();
        if (ParentControl != nullptr && ParentControl->bCaptured == 0)
            return false;
    }

    if ((ButtonState & 0x01) != 0)
    {
        // the release of a press this button captured.
        if (Event->Field0 != -1 || Event->Field1 != 1)
            return false;
        WireState = static_cast<unsigned char>(WireState & 0xfb);
        ButtonState = static_cast<unsigned char>(ButtonState & 0xfe);
        Win3DRemoveCapture(this);

        // the cursor ray -- the routine on the field, the engine's
        // TransformMatrix (+0x2c) with a copy of the camera orientation, normalised inline.
        FSpaceCamera* const Cam = Win3DGetCamera();
        int CursorX = 0;
        int CursorY = 0;
        GetClientSoftwareCursor(&CursorX, &CursorY, nullptr);
        FVector Local;
        Win3DScreenToNearPlane(Cam, Local, static_cast<float>(CursorX),
                               static_cast<float>(CursorY));
        FMatrix CamOrientation;
        memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
        FVector World;
        WireMathEngine()->TransformMatrix(World, CamOrientation, Local);
        double const Inv =
            kOne / sqrt(static_cast<double>(World.X) * World.X +
                        static_cast<double>(World.Y) * World.Y +
                        static_cast<double>(World.Z) * World.Z);
        float Ray[3];
        Ray[0] = static_cast<float>(World.X * Inv);
        Ray[1] = static_cast<float>(World.Y * Inv);
        Ray[2] = static_cast<float>(World.Z * Inv);

        if (!HitTest(Ray)) // slot 4
        {
            // released off the button.
            TellParent(this, 0x29);
            WireState = static_cast<unsigned char>(WireState & 0xfe);
            return true;
        }

        // the click sound -- the UI sound slot, else the sound archetype by name.
        if (SoundSlot != -1)
        {
            PlayOrRewindUiSoundSlot(static_cast<unsigned>(SoundSlot));
        }
        else
        {
            ID_String SoundId;
            SoundId.Id = CreateID(SoundName.c_str());
            void* const Arch = LookupClientSoundArch(&SoundId);
            PlayClientSoundArchIfIdle(Arch, nullptr, nullptr);
        }

        if ((ButtonState & 0x02) != 0)
        {
            // a right click asks the parent 0x27 first; a zero answer becomes 0x26.
            if (Parent == nullptr)
                return true;
            WinControl* const ParentControl = Parent->AsWinControl();
            if (ParentControl == nullptr)
                return true;
            if (ParentControl->SetProperty(0x27, reinterpret_cast<uintptr_t>(AsWinControl()),
                                           0) != 0)
                return true;
            ParentControl->SetProperty(0x26, reinterpret_cast<uintptr_t>(AsWinControl()), 0);
            return true;
        }
        Activate(); // slot 45
        return true;
    }

    // a press, left (Field1 2) or right (3).
    if (Event->Field0 != -1)
        return false;
    if (Event->Field1 != 2 && Event->Field1 != 3)
        return false;
    if (Event->Field1 == 3)
        ButtonState = static_cast<unsigned char>(ButtonState | 0x02);
    else
        ButtonState = static_cast<unsigned char>(ButtonState & 0xfd);
    TellParent(this, 0x28);
    WireState = static_cast<unsigned char>(WireState | 0x04);
    ButtonState = static_cast<unsigned char>(ButtonState | 0x01);
    if (Parent != nullptr)
    {
        NnUiPulseReset();
        TellParent(this, 0x2c);
    }
    Win3DPushCapture(this);
    return true;
}

void WireButtonControl::ShowToolTip()
{
    // the record's aux dword is the command id +0x38c.
    FeedWireToolTip(this, CommandId);
}

intptr_t WireButtonControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc).
    if (Msg == 0x2e)
    {
        *reinterpret_cast<int*>(A) = CommandId;
        return 0;
    }
    return WireStaticControl::SetProperty(Msg, A, B); // JMP the branch
}

void WireButtonControl::Activate()
{
    // .
    if (CommandId != 0)
    {
        HudPostCommand(static_cast<unsigned>(CommandId), 0);
        return;
    }
    TellParent(this, 0x26);
}

void WireButtonControl::Init(char const* NewSoundName, float const* NewOffset,
                             float const* NewHalfExtents, unsigned char bAlwaysWhite,
                             unsigned char bNewNoZEnable, int NewToolTipIds, int NewCommandId,
                             unsigned NewEdgeMask)
{
    // (RET 0x20): the routine's body inlined, the command id, then the sound.
    WireStaticControl::Init(NewOffset, NewHalfExtents, bAlwaysWhite, bNewNoZEnable,
                            NewToolTipIds, NewEdgeMask);
    CommandId = NewCommandId;
    StoreSoundName(this, NewSoundName);
}

// ---------------------------------------------------------------------------------------
// RichTextWireButtonControl
// ---------------------------------------------------------------------------------------

RichTextWireButtonControl::RichTextWireButtonControl()
{
    // 005a4f90..005a4fb3.
    TextWinRef = &TextWin;
    TextWin = nullptr;
}

RichTextWireButtonControl::~RichTextWireButtonControl()
{
}

void RichTextWireButtonControl::Init(char const* NewSoundName, float const* NewOffset,
                                     float const* NewHalfExtents, unsigned char bAlwaysWhite,
                                     unsigned char bNewNoZEnable, int NewToolTipIds,
                                     unsigned CaptionIds, int NewCommandId,
                                     unsigned NewEdgeMask)
{
    // (RET 0x24).
    WireStaticControl::Init(NewOffset, NewHalfExtents, bAlwaysWhite, bNewNoZEnable,
                            NewToolTipIds, NewEdgeMask);
    CommandId = NewCommandId;
    StoreSoundName(this, NewSoundName);

    // a RichTextWin child named <this name> + "Text" (the field).
    std::string Nickname(Name);
    Nickname.append("Text");
    TextWin = static_cast<RichTextWin*>(
        WinFactoryCreate(Nickname.c_str(), "RichTextWin", this, nullptr));
    if (TextWin == nullptr)
        return;

    // the caption box sits on the wire box's top-left corner.
    float AttachOffset[3];
    AttachOffset[0] = Offset[0] - HalfExtents[0];
    AttachOffset[1] = Offset[1] + HalfExtents[1];
    AttachOffset[2] = Offset[2];
    TextWin->SetAttachOffset(AttachOffset);
    TextWin->SetNormSize(HalfExtents[0] + HalfExtents[0],
                         HalfExtents[1] + HalfExtents[1]);
    TextWin->SetAutoScrollbar(false);
    TextWin->SetNeverHit(true);
    TextWin->SetAlignMode(2);

    if (CaptionIds == 0)
        return;

    // a local list of StyleNode(0x828b), JustifyNode(1) and the caption
    // ids (the routine, no trailing-paragraph trim), appended to the caption (the routine).
    RenderDisplayList Caption;
    StyleNode Style(0x828b);
    RdlAppendNode(Caption, Style.Clone());
    JustifyNode Justify(TextJustifyCenter);
    RdlAppendNode(Caption, Justify.Clone());
    LoadIdsIntoRdl(CaptionIds, Caption);
    TextWin->AppendContent(Caption);
}

void WireControlsJoinColumn(WinControl* const* Controls, int Count)
{
    // . ESI = the previous control, ECX = this one, EDX = the next (null past the
    // end).
    WireStaticControl* Prev = nullptr;
    for (int Index = 0; Index < Count; ++Index)
    {
        WireStaticControl* const Cur = static_cast<WireStaticControl*>(Controls[Index]);
        WireStaticControl* const Next = (Index + 1 != Count)
                                            ? static_cast<WireStaticControl*>(Controls[Index + 1])
                                            : nullptr;

        // highlighted (state bit 0 hovered or bit 1 focused) with no lit
        // (bit 4) neighbour on either side.
        unsigned char const State = Cur->WireState;
        unsigned char const bFocused = static_cast<unsigned char>((State >> 1) & 1);
        unsigned char bAlone = 0;
        if (bFocused != 0 || (State & 1) != 0)
        {
            bAlone = 1;
            if (Next != nullptr && (Next->WireState & 0x10) != 0)
                bAlone = 0;
            else if (Prev != nullptr && (Prev->WireState & 0x10) != 0)
                bAlone = 0;
        }

        // the full box unless the next control is shown (Flags bits 1 and
        // 0), this one is not lit and not highlighted alone.
        if (Next == nullptr || (Next->Flags & 2) == 0 || (Next->Flags & 1) == 0
            || (Cur->WireState & 0x10) != 0 || bAlone != 0)
        {
            Cur->EdgeMask = 0xf;
        }
        else if (bFocused == 0 && (Cur->WireState & 1) == 0)
        {

            if (Prev != nullptr && (Prev->WireState & 0x13) != 0)
                Cur->EdgeMask = 0xa;
            else
                Cur->EdgeMask = 0xb;
        }
        else
        {

            Cur->EdgeMask = 0xa;
            if (Prev == nullptr || (Prev->WireState & 0x10) == 0)
                Cur->EdgeMask = 0xb;
            if ((Next->WireState & 0x10) == 0)
                Cur->EdgeMask |= 4;
        }

        // a coloured control below an uncoloured one takes over the shared
        // edge: the previous one gets its bottom edge, this one loses its top edge.
        if (Prev != nullptr && (Prev->EdgeMask & 4) == 0 && (Cur->EdgeMask & 1) != 0
            && Prev->bUseColour == 0 && Cur->bUseColour != 0)
        {
            Prev->EdgeMask |= 4;
            Cur->EdgeMask &= ~1u;
        }

        Prev = Cur;
    }
}
