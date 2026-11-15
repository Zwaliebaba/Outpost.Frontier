// Author: Giperion.

#include "nn_button_control.h"
#include "hud_widescreen.h"   // port-only (OUTPOST_FRONTIER_WIDESCREEN_SUPPORT)

#include "game_options.h"         // GetGameOption (the routine)
#include "globals.h"              // GetClientFrameTimeCapped (the routine)
#include "hud_command_dispatch.h" // HudPostCommand (the routine)
#include "input_manager.h"        // GetClientSoftwareCursor (the field / the field)
#include "menu_sound_arch.h"      // LookupClientSoundArch / PlayClientSoundArchIfIdle
#include "menu_ui_init.h"         // FindUiSoundSlot / PlayOrRewindUiSoundSlot / PlayUiRolloverSound
#include "nn_montage_draw.h"      // ButtonMontage tiles, NnMontageDrawTileAt (the routine)
#include "nn_tooltip.h"           // NnToolTipHideGlobal (the routine)
#include "nn_ui_pulse.h"          // NnUiPulseReset / NnUiPulseBlendArgb / palette
#include "space_camera.h"         // FSpaceCamera (the field)
#include "space_camera_third.h"   // CockpitMulMatrix (the routine)
#include "tooltip_hover.h"        // FToolTipHoverInfo / NnToolTipFeedHover (the routine)
#include "win3d.h"
#include "win3d_engine_submit.h"  // SubmitWin3DEngineInstance / ..TintedEngineInstanceWrapped
#include "win3d_immediate.h"      // Win3DDisable (the routine)
#include "win_factory.h"

#include "common_csys.h"          // RotateMatrix, Acquire3DMathEngine (the routine)
#include "common_id.h"            // CreateID
#include "common_ini.h"           // INI_Reader
#include "x86math.h"

#include "NeuronRuntime.h"

#include <math.h>
#include <string.h>

// WINFACTORY<ButtonControl> (static init the branch: the routine
// "ButtonControl" + atexit the branch -> the routine); Create the routine.
WINFACTORY<NnButtonControl> G_ButtonControlFactory("ButtonControl");

// WINFACTORY<ToggleButtonControl> (static init the branch:
// "ToggleButtonControl" + atexit the branch -> the routine); Create
// .
WINFACTORY<NnToggleButtonControl> G_ToggleButtonControlFactory("ToggleButtonControl");

// WINFACTORY<RichTextButtonControl> (static init the branch:
// "RichTextButtonControl" + atexit the branch -> the routine); Create
// .
WINFACTORY<NnRichTextButtonControl> G_RichTextButtonControlFactory("RichTextButtonControl");

namespace
{
    // the 3DMathEngine this class acquires on first use (the routine), shared
    // by SetOrientation (0x0055b5df) and MouseEvent (0x0055b9e6).
    I3DMathEngine* G_ButtonMathEngine = nullptr;

    I3DMathEngine* ButtonMathEngine()
    {
        if (G_ButtonMathEngine == nullptr)
            G_ButtonMathEngine = Acquire3DMathEngine();
        return G_ButtonMathEngine;
    }


    float const kOne = 1.0f;
    // 1/255 (0x3b808081)
    float const kInv255 = 1.0f / 255.0f;
    // the routine's icon size without a tile scale
    float const kDefaultIconSize = 0.022f;
    // 0x3fb504f3, sqrt(2)
    float const kSqrt2 = 1.41421354f;

    bool HasInstance(WinControl const* Control)
    {
        // The `CMP [ESI+0x84],-1` every slot below inlines.
        return Control->MeshHolder.EngineInstance !=
               reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    // The parent notification the handlers spell out: the parent as a WinControl (slot 2),
    // and if it is one, its slot 42 with (Msg, this as a WinControl, 0).
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

    // The tile search Init (0x0055ac5b..0x0055acf2) and Create (0x0055ab6d..0x0055ac04) inline:
    // load ButtonMontage on first use (the routine), then the first tile whose name stricmp-
    // equals Path, else null.
    FButtonMontageTile* FindMontageTileForPath(char const* Path)
    {
        if (!G_bButtonMontageLoaded)
            LoadButtonMontage();
        int const Count = static_cast<int>(G_ButtonMontageTiles.size());
        for (int Index = 0; Index < Count; ++Index)
        {
            if (_stricmp(G_ButtonMontageTiles[Index].Name.c_str(), Path) == 0)
                return &G_ButtonMontageTiles[Index];
        }
        return nullptr;
    }

    // The mouse ray the branch builds inline from the software cursor through the Win3D
    // camera (0x0055b67a..0x0055b760): near-plane point, the routine with a copy of the camera
    // orientation, normalised by 1 / sqrt.
    void CursorRayForUpdate(float* Ray)
    {
        FSpaceCamera* const Cam = Win3DGetCamera();
        int CursorX = 0;
        int CursorY = 0;
        GetClientSoftwareCursor(&CursorX, &CursorY, nullptr);
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
        Ray[0] = static_cast<float>(World.X * Inv);
        Ray[1] = static_cast<float>(World.Y * Inv);
        Ray[2] = static_cast<float>(World.Z * Inv);
    }

    // The holder tint the routine (0x00559b9b..0x00559bff) and Draw (0x0055b36d..0x0055b3d6)
    // write before the routine: bytes 0..2 into +0x14 / +0x18 / +0x1c, byte 3 into +0x24,
    // each times 1/255.
    void SetHolderTint(FClientMeshHolder* Holder, unsigned char const* Rgba)
    {
        Holder->UserFloats[0] = static_cast<float>(static_cast<double>(Rgba[0]) * kInv255);
        Holder->UserFloats[1] = static_cast<float>(static_cast<double>(Rgba[1]) * kInv255);
        Holder->UserFloats[2] = static_cast<float>(static_cast<double>(Rgba[2]) * kInv255);
        Holder->UserFloats[4] = static_cast<float>(static_cast<double>(Rgba[3]) * kInv255);
    }

    // (cdecl Button, Rgba; Draw pushes +0x335 as a third argument that the
    // function never reads) -- the button body: the ButtonMontage icon at the instance's
    // position (or +0x60 + +0xa8) through the routine, sized by the tile's scale, else by the
    // instance's extent (Engine vt+0x88), else 0.022, times sqrt(2); when no icon was drawn
    // and an instance is bound, the instance tinted by Rgba through the routine.
    void ButtonDrawBody(NnButtonControl* Button, unsigned char const* Rgba)
    {
        float Size = kDefaultIconSize;
        FButtonMontageTile* const Tile = Button->MontageTile;
        if (Tile != nullptr)
        {
            Size = Tile->Scale;
            if (Size == 0.0f)
            {
                // GetInstanceExtent(+0x84, 1, &Size, &Centre) -- no instance test.
                float Centre[3];
                NeuronRuntime::Engine->GetInstanceExtent(Button->MeshHolder.EngineInstance, 1, &Size,
                                                 Centre);
            }
        }
        Size = Size * kSqrt2;

        float Where[3];
        if (HasInstance(Button))
        {
            float const* const Placed = ClientMeshHolderGetPosition(&Button->MeshHolder);
            Where[0] = Placed[0];
            Where[1] = Placed[1];
            Where[2] = Placed[2];
        }
        else
        {
            Where[0] = Button->Offset[0] + Button->Position[0];
            Where[1] = Button->Position[1] + Button->Offset[1];
            Where[2] = Button->Position[2] + Button->Offset[2];
        }

        if (NnMontageDrawTileAt(Tile, Where, Rgba, Size, 0, 0.0f))
            return;
        if (!HasInstance(Button))
            return;
        SetHolderTint(&Button->MeshHolder, Rgba);
        SubmitWin3DEngineInstance(&Button->MeshHolder, Win3DGetCamera(), 0, 1.0f, 0);
    }

    void SetRgba(unsigned char* Out, unsigned char R, unsigned char G, unsigned char B,
                 unsigned char A)
    {
        Out[0] = R;
        Out[1] = G;
        Out[2] = B;
        Out[3] = A;
    }
}

void ButtonKeyCommandLookup(char const* KeyName, int* OutCommandId, int* OutToolTipIds)
{
    // .
    *OutCommandId = 0;
    *OutToolTipIds = 0;
    if (_stricmp(KeyName, "tractor") == 0)
    {
        *OutCommandId = 0x8b;
        *OutToolTipIds = 0xbc2;
        return;
    }
    if (_stricmp(KeyName, "next") == 0)
    {
        *OutCommandId = 0x1d;
        *OutToolTipIds = 0xbb9;
        return;
    }
    if (_stricmp(KeyName, "prev") == 0)
    {
        *OutCommandId = 0x1e;
        *OutToolTipIds = 0xbba;
        return;
    }
    if (_stricmp(KeyName, "next_subtarget") == 0)
    {
        *OutCommandId = 0x21;
        *OutToolTipIds = 0x41f;
        return;
    }
    if (_stricmp(KeyName, "prev_subtarget") == 0)
    {
        *OutCommandId = 0x22;
        *OutToolTipIds = 0x420;
    }
}

NnButtonControl::NnButtonControl()
    : ToolTip2Ids(0)
    , ButtonBits(0)
    , ButtonBits2(0)
    , bSpin(0)
    , bBlink(0)
    , bBlinkPhase(0)
    , MontageMode(0)
    , bUseColour(0)
    , bGlow(0)
    , MontageTile(nullptr)
    , SpinAngle(0.0f)
    , SpinSpeed(180.0f)
    , BlinkTimer(0.0f)
    , GlowArgb(0)
    , CommandId(0)
    , UiSoundSlot(0)
{
    // after the routine. Two empty strings, the colour +0x370 = 0, then
    // +0x330 &= 0x80 and +0x331 = (+0x331 & 0xee) | 0xe; the bits the masks keep are whatever
    // the allocation held in baseline and 0 here. +0x338 and +0x348 are not written by the ctor
    // (Init / Create and Draw write them); the port starts them at null / 0.
    Colour[0] = 0;
    Colour[1] = 0;
    Colour[2] = 0;
    Colour[3] = 0;
    ButtonBits = static_cast<unsigned char>(ButtonBits & 0x80);
    ButtonBits2 = static_cast<unsigned char>((ButtonBits2 & 0xee) | 0x0e);
}

NnButtonControl::~NnButtonControl()
{
    // a button still holding the capture leaves the capture stack
    // (the routine); then the two strings (+0x360 first) and WinControl's the routine.
    if ((ButtonBits & 0x10) != 0)
        Win3DRemoveCapture(this);
}

void NnButtonControl::SetPosition(float const* NewPosition)
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

    // the instance is placed again, at +0x60 (+ +0xa8 under PositionFlags bit 0).
    if (!HasInstance(this))
        return;
    float Instance[3];
    if ((PositionFlags & 1) != 0)
    {
        Instance[0] = Offset[0] + Position[0];
        Instance[1] = Offset[1] + Position[1];
        Instance[2] = Offset[2] + Position[2];
    }
    else
    {
        Instance[0] = Position[0];
        Instance[1] = Position[1];
        Instance[2] = Position[2];
    }
    NeuronRuntime::Engine->SetInstancePosition(MeshHolder.EngineInstance, Instance);
    NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
}

void NnButtonControl::SetOrientation(float const* NewOrientation)
{
    // (RET 4): the new orientation times the attach orientation (slot 40) through
    // the 3DMathEngine's MulMatrix (+0x20) into Win3D's +0x3c, then onto the instance. Unlike
    // WinControl's the routine it does not recurse into children.
    bAttached = 1;
    FMatrix Attach;
    GetAttachOrientation(&Attach.D[0][0]);
    FMatrix Result;
    ButtonMathEngine()->MulMatrix(Result, *reinterpret_cast<FMatrix const*>(NewOrientation),
                                  Attach);
    memcpy(Orientation, &Result, sizeof(Orientation));
    if (!HasInstance(this))
        return;
    NeuronRuntime::Engine->SetInstanceOrientation(MeshHolder.EngineInstance, Orientation);
    NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
}

bool NnButtonControl::Create(Win3D* NewParent, void* Data)
{
    // (RET 8).
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | 0x03);

    INI_Reader* const Ini = static_cast<INI_Reader*>(Data);
    if (Ini != nullptr)
    {
        while (Ini->read_value())
        {
            if (Ini->is_value("mesh"))
            {
                SetMesh(Ini->get_value_string(), 0); // slot 38
                continue;
            }
            if (Ini->is_value("mesh2"))
            {
                Mesh2.assign(Ini->get_value_string());
                continue;
            }
            if (Ini->is_value("button_sound"))
            {
                char const* const Value = Ini->get_value_string();
                if (Value[0] == '\0')
                    continue;
                SoundName.assign(Value);
                UiSoundSlot = FindUiSoundSlot(SoundName.c_str());
                continue;
            }
            if (Ini->is_value("event"))
            {
                // a null value still reaches the lookup; only an empty one is
                // skipped.
                char const* const Value = Ini->get_value_string();
                if (Value != nullptr && Value[0] == '\0')
                    continue;
                ButtonKeyCommandLookup(Value, &CommandId, &ToolTipIds);
                if (CommandId != 0 || ToolTipIds != 0)
                    RefreshAttachment(); // slot 35
                continue;
            }
            if (Ini->is_value("offset"))
            {
                float const Y = Ini->get_value_float(1);
                float const X = Ini->get_value_float(0);
                float const Z = Ini->get_value_float(2);
                Offset[0] = X;
                Offset[1] = Y;
                Offset[2] = Z;
                if (HasInstance(this))
                {
                    // the routine builds {+0x60 + offset} and the instance moves.
                    float Placed[3];
                    Placed[2] = Offset[2] + Position[2];
                    Placed[1] = Offset[1] + Position[1];
                    Placed[0] = Position[0] + Offset[0];
                    NeuronRuntime::Engine->SetInstancePosition(MeshHolder.EngineInstance, Placed);
                    NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
                }
                continue;
            }
            if (Ini->is_value("hardpoint"))
            {
                // the attach transform is reset, then filled from the NEW parent's
                // hardpoint of that name. The name itself is not kept (+0x1d0 is untouched).
                AttachOrientation[0] = 1.0f;
                AttachOrientation[1] = 0.0f;
                AttachOrientation[2] = 0.0f;
                AttachOrientation[3] = 0.0f;
                AttachOrientation[4] = 1.0f;
                AttachOrientation[5] = 0.0f;
                AttachOrientation[6] = 0.0f;
                AttachOrientation[7] = 0.0f;
                AttachOrientation[8] = 1.0f;
                AttachPosition[0] = 0.0f;
                AttachPosition[1] = 0.0f;
                AttachPosition[2] = 0.0f;
                if (NewParent == nullptr)
                    continue;
                WinControl* const ParentControl = NewParent->AsWinControl();
                if (ParentControl == nullptr)
                    continue;
                ParentControl->GetHardpointTransform(Ini->get_value_string(), AttachOrientation);
                continue;
            }
            if (Ini->is_value("mouse_size"))
            {
                float const Y = Ini->get_value_float(1);
                float const X = Ini->get_value_float(0);
                float const Z = Ini->get_value_float(2);
                HalfExtents[0] = X;
                HalfExtents[1] = Y;
                HalfExtents[2] = Z;
                continue;
            }
            if (Ini->is_value("no_mesh_render"))
            {
                unsigned char const Bit = (Ini->get_value_float(0) != 0.0f) ? 1 : 0;
                ButtonBits2 = static_cast<unsigned char>((ButtonBits2 & 0xfe) | Bit);
                continue;
            }
            if (Ini->is_value("action_button"))
            {
                unsigned char const Bit = (Ini->get_value_float(0) != 0.0f) ? 0x08 : 0;
                ButtonBits = static_cast<unsigned char>((ButtonBits & 0xf7) | Bit);
                continue;
            }
            if (Ini->is_value("no_color_change"))
            {
                unsigned char const Bit = (Ini->get_value_float(0) != 0.0f) ? 0x20 : 0;
                ButtonBits = static_cast<unsigned char>((ButtonBits & 0xdf) | Bit);
                continue;
            }
            if (Ini->is_value("no_z_enable"))
            {
                bNoZEnable = (Ini->get_value_float(0) != 0.0f) ? 1 : 0;
                continue;
            }
            if (Ini->is_value("tooltip"))
            {
                ToolTipIds = static_cast<int>(Ini->get_value_float(0)); // ftol
                continue;
            }
            if (Ini->is_value("tooltip2"))
                ToolTip2Ids = static_cast<int>(Ini->get_value_float(0)); // ftol
        }

        // the ButtonMontage tile of the (possibly just loaded) mesh path.
        MontageTile = FindMontageTileForPath(MeshPath);
    }

    // the parent is re-placed at its own position (slot 5), which positions this
    // control with it.
    if (Parent != nullptr)
        Parent->SetPosition(Parent->Position);
    return true;
}

void NnButtonControl::Update()
{
    // Win3D's child walk (the routine) first.
    Win3D::Update();

    // while the button holds the capture, follow the cursor and light / unlight
    // bit 2 as it moves on and off.
    if ((ButtonBits & 0x10) != 0)
    {
        float Ray[3];
        CursorRayForUpdate(Ray);
        if (HitTest(Ray)) // slot 4
        {
            if ((ButtonBits & 0x04) == 0)
            {
                ButtonBits = static_cast<unsigned char>(ButtonBits | 0x04);
                TellParent(this, 0x2b);
                if (Parent != nullptr)
                {
                    NnUiPulseReset();
                    TellParent(this, 0x2c);
                }
            }
        }
        else if ((ButtonBits & 0x04) != 0)
        {
            ButtonBits = static_cast<unsigned char>(ButtonBits & 0xfb);
            TellParent(this, 0x2a);
            TellParent(this, 0x2d);
        }
    }

    // the blink. The timer gains the frame time; every whole second past 1.0
    // flips +0x334 (FCOM 1.0 / TEST AH,0x41: "not above" stops).
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

void NnButtonControl::Draw()
{
#ifdef OUTPOST_FRONTIER_WIDESCREEN_SUPPORT
    // Port-only: owners that walk their children themselves (HudTargetBase, NN_MainButton)
    // reach this without the Win3D draw walk, so the control sets its own HUD shift.
    FWidescreenHudShiftScope const WidescreenScope(WidescreenHudShiftPixelsFor(this), true);
#endif
    // .
    GlowArgb = 0;
    if (GetGameOption(kGameOptInterfaceGroup) == 0) // (6)
        return;
    if (GetGameOption(kGameOptNeuronet) == 0) // (9)
        return;
    if (GetGameOption(kGameOptDrawButtonControls) == 0) // (0x14)
        return;
    if ((Flags & kWin3DVisible) == 0)
        return;
    if ((ButtonBits2 & 0x10) == 0 && (Flags & kWin3DActive) == 0)
        return;
    if (bBlink != 0 && bBlinkPhase != 0)
        return;

    Win3DDrawChildren(this);
    unsigned char const SavedSelected = static_cast<unsigned char>((ButtonBits >> 6) & 1);

    if (bSpin != 0)
    {
        // the spin. With a positive speed the angle runs forward while bit 0 is
        // set, otherwise it winds back to 0 the short way (up through 359, down through 1).
        double const Frame = GetClientFrameTimeCapped();
        if (SpinSpeed > 0.0f) // FCOMP 0 / TEST AH,0x41 / JNZ: not above skips
        {
            double const Step = Frame * SpinSpeed;
            if ((ButtonBits & 0x01) != 0)
            {
                SpinAngle = static_cast<float>(Step + SpinAngle);
            }
            else if (SpinAngle > 180.0f)
            {
                double const Angle = Step + Step + SpinAngle;
                SpinAngle = static_cast<float>(Angle);
                if (Angle > 359.0)
                    SpinAngle = 0.0f;
            }
            else
            {
                double const Angle = SpinAngle - (Step + Step);
                SpinAngle = static_cast<float>(Angle);
                if (Angle < kOne) // TEST AH,5 / JP: not below skips
                    SpinAngle = 0.0f;
            }
        }
        // wrap into (-360, 360) -- the field / the field.
        if (SpinAngle >= 360.0f)
            SpinAngle = SpinAngle - 360.0f;
        else if (SpinAngle <= -360.0f)
            SpinAngle = SpinAngle + 360.0f;

        // RotateMatrix({0, angle, 0}) onto the instance (Engine vt+0x94, vt+0x6c).
        // baseline tests no instance here.
        Vector Angles;
        Angles.x = 0.0f;
        Angles.y = SpinAngle;
        Angles.z = 0.0f;
        Matrix Spun = RotateMatrix(Angles);
        NeuronRuntime::Engine->SetInstanceOrientation(MeshHolder.EngineInstance, &Spun);
        NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
    }

    // the colours. Glow (local_3c) starts black; both +0x330 bit-3 arms of the
    // listing (0x0055b18d / 0x0055b240) produce the same pair, so the test is not repeated.
    unsigned char Glow[4] = {0, 0, 0, 0};
    unsigned char Body[4] = {0, 0, 0, 0};
    if ((Flags & kWin3DActive) == 0)
    {
        // Inactive: grey, and the selected / lit bits are dropped.
        ButtonBits = static_cast<unsigned char>(ButtonBits & 0xbb);
        SetRgba(Glow, 0x80, 0x80, 0x80, 0xff);
        SetRgba(Body, 0x80, 0x80, 0x80, 0xff);
    }
    else if ((ButtonBits & 0x40) != 0 || ((ButtonBits2 & 0x04) != 0 && (ButtonBits & 0x04) != 0))
    {
        // Selected, or lit while pressed: yellow.
        SetRgba(Glow, 0xff, 0xff, 0x00, 0xff);
        SetRgba(Body, 0xff, 0xff, 0x00, 0xff);
    }
    else if ((ButtonBits & 0x02) != 0 || ((ButtonBits & 0x01) != 0 && (ButtonBits2 & 0x08) != 0))
    {
        // Focused, or hovered with the hover pulse on: two the routine samples.
        unsigned long Pulse = 0;
        NnUiPulseBlendArgb(&Pulse);
        memcpy(Body, &Pulse, sizeof(Body));
        NnUiPulseBlendArgb(&Pulse);
        memcpy(Glow, &Pulse, sizeof(Glow));
    }
    else
    {
        // Idle: the field.
        memcpy(Glow, &G_NnPaletteArgb_679bb0, sizeof(Glow));
        memcpy(Body, &G_NnPaletteArgb_679bb0, sizeof(Body));
    }
    if ((ButtonBits & 0x20) != 0)
        SetRgba(Body, 0xff, 0xff, 0xff, 0xff); // no_color_change
    if (bUseColour != 0)
    {
        memcpy(Body, Colour, sizeof(Body));
        memcpy(Glow, Colour, sizeof(Glow));
    }

    // the body, unless no_mesh_render.
    if ((ButtonBits2 & 0x01) == 0)
    {
        if (bSpin != 0 && (ButtonBits & 0x01) != 0)
        {
            if (HasInstance(this))
            {
                SetHolderTint(&MeshHolder, Body);
                SubmitWin3DEngineInstance(&MeshHolder, Win3DGetCamera(), 0, 1.0f, 0);
            }
        }
        else
        {
            ButtonDrawBody(this, Body); // (this, Body, +0x335)
        }
    }

    Win3DDisable(kWin3DDepthTest); // (0xb71)
    if ((ButtonBits2 & 0x01) != 0 || bGlow != 0)
    {
        if (HasInstance(this))
        {
            GlowArgb = (static_cast<unsigned long>(Glow[3]) << 24) |
                       (static_cast<unsigned long>(Glow[0]) << 16) |
                       (static_cast<unsigned long>(Glow[1]) << 8) |
                       static_cast<unsigned long>(Glow[2]);
            SubmitWin3DTintedEngineInstanceWrapped(Win3DGetCamera(), MeshHolder.EngineInstance,
                                                  1, Glow, 1, 1.0f, 0);
        }
    }
    Win3DDisable(kWin3DDepthTest); // (0xb71)

    // bit 6 goes back to what it was before the inactive arm could clear it.
    ButtonBits = static_cast<unsigned char>((ButtonBits & 0xbf) | (SavedSelected << 6));
    Slot39(); // vt+0x9c
}

void NnButtonControl::MouseEnter(float const* /*Ray*/)
{
    // (RET 4).
    if (Parent != nullptr)
    {
        NnUiPulseReset();
        TellParent(this, 0x2c);
    }
    ButtonBits = static_cast<unsigned char>(ButtonBits | 0x01);
    if ((ButtonBits2 & 0x02) != 0)
        PlayUiRolloverSound();
}

void NnButtonControl::MouseLeave(float const* /*Ray*/)
{
    // (RET 4).
    if (ToolTipIds != 0)
        NnToolTipHideGlobal();
    TellParent(this, 0x2d);
    ButtonBits = static_cast<unsigned char>(ButtonBits & 0xfe);
}

bool NnButtonControl::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4).
    if ((ButtonBits2 & 0x02) == 0)
        return false;
    // while some control holds the mouse, a button whose WinControl parent does
    // not hold it ignores the event.
    if (Parent != nullptr && Win3DAnyCaptured())
    {
        WinControl* const ParentControl = Parent->AsWinControl();
        if (ParentControl != nullptr && ParentControl->bCaptured == 0)
            return false;
    }

    if ((ButtonBits & 0x10) != 0)
    {
        // the release of a press this button captured.
        if (Event->Field0 != -1 || Event->Field1 != 1)
            return false;
        ButtonBits = static_cast<unsigned char>(ButtonBits & 0xeb);
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
        ButtonMathEngine()->TransformMatrix(World, CamOrientation, Local);
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
            ButtonBits = static_cast<unsigned char>(ButtonBits & 0xfe);
            return true;
        }

        // the click sound -- the UI sound slot, else the sound archetype named by
        // a non-empty +0x360.
        if (UiSoundSlot != -1)
        {
            PlayOrRewindUiSoundSlot(static_cast<unsigned>(UiSoundSlot));
        }
        else if (!SoundName.empty())
        {
            ID_String SoundId;
            SoundId.Id = CreateID(SoundName.c_str());
            void* const Arch = LookupClientSoundArch(&SoundId);
            PlayClientSoundArchIfIdle(Arch, nullptr, nullptr);
        }

        if ((ButtonBits & 0x80) != 0)
        {
            // a right click only tells the parent 0x27 (its answer is ignored).
            TellParent(this, 0x27);
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
        ButtonBits = static_cast<unsigned char>(ButtonBits | 0x80);
    else
        ButtonBits = static_cast<unsigned char>(ButtonBits & 0x7f);
    TellParent(this, 0x28);
    ButtonBits = static_cast<unsigned char>(ButtonBits | 0x14);
    if (Parent != nullptr)
    {
        NnUiPulseReset();
        TellParent(this, 0x2c);
    }
    Win3DPushCapture(this);
    return true;
}

void NnButtonControl::ShowToolTip()
{
    // key -1 and no alt id first, the empty caption (the routine), then both ids
    // from +0xcc, the command +0x34c as the aux dword, the instance's position (Engine vt+0xa0
    // [9..11]) or +0x60 + +0xa8, the half extents +0xb4 and the live byte; the routine; then
    // the record's teardown (~FToolTipHoverInfo). The caption +0x08 / +0x0c zeroing and
    // on +0x10 are the record's constructor.
    FToolTipHoverInfo Info;
    Info.IdsAlt = 0;                  // +0x04
    Info.Ids = -1;                    // +0x00
    Info.bLive = 0;                   // +0x60
    Info.Ids = ToolTipIds;            // +0x00 <- +0xcc
    Info.IdsAlt = ToolTipIds;         // +0x04 <- +0xcc
    Info.AuxField = CommandId;        // +0x44 <- +0x34c
    if (HasInstance(this))
    {
        float Transform[12];
        memcpy(Transform, NeuronRuntime::Engine->GetInstanceTransform(MeshHolder.EngineInstance),
               sizeof(Transform));
        Info.WorldPos[0] = Transform[9];
        Info.WorldPos[1] = Transform[10];
        Info.WorldPos[2] = Transform[11];
    }
    else
    {
        Info.WorldPos[0] = Position[0] + Offset[0];
        Info.WorldPos[1] = Offset[1] + Position[1];
        Info.WorldPos[2] = Offset[2] + Position[2];
    }
    Info.HalfExtents[0] = HalfExtents[0]; // +0x54 <- +0xb4
    Info.HalfExtents[1] = HalfExtents[1];
    Info.HalfExtents[2] = HalfExtents[2];
    Info.bLive = 1;
    NnToolTipFeedHover(&Info);
}

intptr_t NnButtonControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): jump table 0x0055bd6c indexed by 0x0055bd98[Msg - 0xc]; every
    // own case answers 0, the rest tail-jump into WinControl's the routine.
    switch (Msg)
    {
    case 0x0c:
        ButtonBits = static_cast<unsigned char>((ButtonBits & 0xbf) | (A != 0 ? 0x40 : 0));
        return 0;
    case 0x2c:
        ButtonBits = static_cast<unsigned char>(ButtonBits | 0x02);
        return 0;
    case 0x2d:
        ButtonBits = static_cast<unsigned char>(ButtonBits & 0xfd);
        return 0;
    case 0x2e:
        *reinterpret_cast<int*>(A) = CommandId;
        return 0;
    case 0x32:
        ButtonBits2 = static_cast<unsigned char>((ButtonBits2 & 0xfe) | (A != 0 ? 0x01 : 0));
        return 0;
    case 0x33:
        memcpy(Colour, reinterpret_cast<void const*>(A), sizeof(Colour));
        bUseColour = (B == 1) ? 1 : 0;
        return 0;
    case 0x34:
        ButtonBits2 = static_cast<unsigned char>((ButtonBits2 & 0xef) | (A != 0 ? 0x10 : 0));
        return 0;
    case 0x35:
        bGlow = (A != 0) ? 1 : 0;
        return 0;
    case 0x36:
        bSpin = (A == 1) ? 1 : 0;
        return 0;
    case 0x51:
        bBlink = (A != 0) ? 1 : 0;
        return 0;
    default:
        return WinControl::SetProperty(Msg, A, B); // JMP the routine
    }
}

void NnButtonControl::Activate()
{
    // .
    if (CommandId != 0)
    {
        HudPostCommand(static_cast<unsigned>(CommandId), 0);
        return;
    }
    TellParent(this, 0x26);
}

void NnButtonControl::Init(char const* NewMeshPath, char const* NewMesh2,
                           char const* NewSoundName, char const* KeyCommandName,
                           float const* NewOffset, char const* Hardpoint,
                           float const* NewHalfExtents, unsigned char bNoMeshRender,
                           unsigned char bActionButton, unsigned char bNoColorChange,
                           unsigned char bNewNoZEnable, int NewToolTipIds, int NewToolTip2Ids,
                           int NewCommandId)
{
    // (RET 0x38).
    if (NewMeshPath != nullptr)
    {
        strcpy(MeshPath, NewMeshPath);
        MontageTile = FindMontageTileForPath(MeshPath);
        // without a tile, or with a tile of scale 0 (FCOMP 0 / TEST AH,0x44 / JP:
        // a non-zero scale skips), the mesh itself is loaded (slot 38).
        if (MontageTile == nullptr || MontageTile->Scale == 0.0f)
            SetMesh(NewMeshPath, 0);
    }
    if (NewMesh2 != nullptr)
        Mesh2.assign(NewMesh2);
    if (NewSoundName != nullptr && NewSoundName[0] != '\0')
    {
        SoundName.assign(NewSoundName);
        UiSoundSlot = FindUiSoundSlot(SoundName.c_str());
    }
    if (KeyCommandName != nullptr && KeyCommandName[0] != '\0')
    {
        ButtonKeyCommandLookup(KeyCommandName, &CommandId, &ToolTipIds);
        if (CommandId != 0 || ToolTipIds != 0)
            RefreshAttachment(); // slot 35
    }

    Offset[0] = NewOffset[0];
    Offset[1] = NewOffset[1];
    Offset[2] = NewOffset[2];
    if (HasInstance(this))
    {
        float Placed[3];
        Placed[0] = Offset[0] + Position[0];
        Placed[1] = Offset[1] + Position[1];
        Placed[2] = Offset[2] + Position[2];
        NeuronRuntime::Engine->SetInstancePosition(MeshHolder.EngineInstance, Placed);
        NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
    }

    if (Hardpoint != nullptr)
    {
        // the name is kept, the attach transform reset and filled from the
        // parent's hardpoint of that name (slot 36; its answer is not used).
        strcpy(HardpointName, Hardpoint);
        AttachOrientation[0] = 1.0f;
        AttachOrientation[1] = 0.0f;
        AttachOrientation[2] = 0.0f;
        AttachOrientation[3] = 0.0f;
        AttachOrientation[4] = 1.0f;
        AttachOrientation[5] = 0.0f;
        AttachOrientation[6] = 0.0f;
        AttachOrientation[7] = 0.0f;
        AttachOrientation[8] = 1.0f;
        AttachPosition[0] = 0.0f;
        AttachPosition[1] = 0.0f;
        AttachPosition[2] = 0.0f;
        if (Parent != nullptr)
        {
            WinControl* const ParentControl = Parent->AsWinControl();
            if (ParentControl != nullptr)
                ParentControl->GetHardpointTransform(Hardpoint, AttachOrientation);
        }
    }

    HalfExtents[0] = NewHalfExtents[0];
    HalfExtents[1] = NewHalfExtents[1];
    HalfExtents[2] = NewHalfExtents[2];
    ButtonBits2 = static_cast<unsigned char>((ButtonBits2 & 0xfe) | (bNoMeshRender & 0x01));
    ButtonBits = static_cast<unsigned char>((ButtonBits & 0xf7) | ((bActionButton << 3) & 0x08));
    ButtonBits = static_cast<unsigned char>((ButtonBits & 0xdf) | ((bNoColorChange << 5) & 0x20));
    bNoZEnable = bNewNoZEnable;
    ToolTipIds = NewToolTipIds;
    ToolTip2Ids = NewToolTip2Ids;
    CommandId = NewCommandId;
}

// ---------------------------------------------------------------------------------------
// ToggleButtonControl (nn_button_control.h)
// ---------------------------------------------------------------------------------------

namespace
{
    // the double 0x400921fb51000000 (pi rounded to float): the Draw glow pass
    // turns the instance half a turn about y.
    double const kToggleGlowAngle = 3.1415927410125732;

    bool HasToggleInstance(NnToggleButtonControl const* Control)
    {
        // The `CMP [reg+0x37c],-1` the slots inline.
        return Control->ToggleMesh.EngineInstance !=
               reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }

    // The parent notification with a B argument (the routine's 0x30 carries the state).
    void TellParentWithValue(WinControl* Self, int Msg, uintptr_t Value)
    {
        Win3D* const Parent = Self->GetParent();
        if (Parent == nullptr)
            return;
        WinControl* const ParentControl = Parent->AsWinControl();
        if (ParentControl == nullptr)
            return;
        ParentControl->SetProperty(Msg, reinterpret_cast<uintptr_t>(Self->AsWinControl()), Value);
    }

    // The hover record the branch packs inline (0x0055c835..0x0055ca82): the ids twice, no
    // aux dword (unlike the routine it leaves +0x44 at 0), the main instance's position
    // (Engine vt+0xa0 [9..11], no instance test), the half extents +0xb4 and the live byte;
    // ; then the record's teardown (~FToolTipHoverInfo).
    void FeedToggleToolTip(NnToggleButtonControl* Control, int Ids)
    {
        // The caption +0x08 / +0x0c and the routine on +0x10: the record's constructor;
        // (0, -1) on the null caption does nothing.
        FToolTipHoverInfo Info;
        Info.IdsAlt = 0;                 // +0x04
        Info.Ids = -1;                   // +0x00
        Info.AuxField = 0;               // +0x44
        Info.bLive = 0;                  // +0x60
        Info.Ids = Ids;
        Info.IdsAlt = Ids;
        float Transform[12];
        memcpy(Transform, NeuronRuntime::Engine->GetInstanceTransform(Control->MeshHolder.EngineInstance),
               sizeof(Transform));
        Info.WorldPos[0] = Transform[9];
        Info.WorldPos[1] = Transform[10];
        Info.WorldPos[2] = Transform[11];
        Info.HalfExtents[0] = Control->HalfExtents[0];
        Info.HalfExtents[1] = Control->HalfExtents[1];
        Info.HalfExtents[2] = Control->HalfExtents[2];
        Info.bLive = 1;
        NnToolTipFeedHover(&Info);
    }
}

NnToggleButtonControl::NnToggleButtonControl()
    : ToggleState(0)
    , UiSoundSlotState0(0)
    , UiSoundSlotState1(0)
{
    // The ctor the routine inlines after the routine: the +0x378 holder (the routine, the
    // five floats 1.0f, +0x10 = -1 -- FClientMeshHolder's defaults) and two empty strings.
    // +0x3a0 / +0x3c4 / +0x3c8 are not written (Create / Init set them); the port starts them
    // at 0.
}

NnToggleButtonControl::~NnToggleButtonControl()
{
    // +0x3b4, then +0x3a4, then the holder (the routine), then ButtonControl's
    // body inlined -- all by the member / base destructors in that order.
}

void NnToggleButtonControl::PlaceToggleMesh()
{
    if (HasToggleInstance(this))
    {
        // (+0xa8 + +0x60, +0xac + +0x64, +0xb0 + +0x68).
        float Placed[3];
        Placed[0] = Offset[0] + Position[0];
        Placed[1] = Offset[1] + Position[1];
        Placed[2] = Offset[2] + Position[2];
        NeuronRuntime::Engine->SetInstancePosition(ToggleMesh.EngineInstance, Placed);
        NeuronRuntime::Engine->UpdateInstance(ToggleMesh.EngineInstance, 0, nullptr);
        if (HasInstance(this))
        {
            float Transform[12];
            memcpy(Transform, NeuronRuntime::Engine->GetInstanceTransform(MeshHolder.EngineInstance),
                   sizeof(Transform));
            NeuronRuntime::Engine->SetInstanceOrientation(ToggleMesh.EngineInstance, Transform);
            NeuronRuntime::Engine->UpdateInstance(ToggleMesh.EngineInstance, 0, nullptr);
        }
    }
    RefreshAttachment(); // slot 35
}

bool NnToggleButtonControl::Create(Win3D* NewParent, void* Data)
{
    // (RET 8): montage mode first, then ButtonControl's INI parser.
    MontageMode = 1; // +0x335
    bool const bResult = NnButtonControl::Create(NewParent, Data);

    // a non-empty "mesh2" goes into the +0x378 holder (the routine(path, 0, 0)),
    // whose instance takes the main holder's floats +0x94 as user data (Engine vt+0x7c).
    if (Mesh2.c_str()[0] != '\0')
    {
        ClientMeshLoadFromPath(&ToggleMesh, Mesh2.c_str(), 0, false);
        NeuronRuntime::Engine->SetInstanceUserData(ToggleMesh.EngineInstance, &MeshHolder.UserFloats[0]);
        PlaceToggleMesh();
    }

    // state 0, both state sounds from "button_sound", and their UI slots.
    ToggleState = 0;
    SoundState0 = SoundName;
    SoundState1 = SoundName;
    UiSoundSlotState0 = FindUiSoundSlot(SoundState0.c_str());
    UiSoundSlotState1 = FindUiSoundSlot(SoundState1.c_str());
    return bResult;
}

void NnToggleButtonControl::Init(char const* NewMeshPath, char const* NewMesh2,
                                 char const* NewSoundName, char const* KeyCommandName,
                                 float const* NewOffset, char const* Hardpoint,
                                 float const* NewHalfExtents, unsigned char bNoMeshRender,
                                 unsigned char bActionButton, unsigned char bNoColorChange,
                                 unsigned char bNewNoZEnable, int NewToolTipIds,
                                 int NewToolTip2Ids)
{
    // (RET 0x34).
    MontageMode = 1; // +0x335
    NnButtonControl::Init(NewMeshPath, NewMesh2, NewSoundName, KeyCommandName, NewOffset,
                          Hardpoint, NewHalfExtents, bNoMeshRender, bActionButton,
                          bNoColorChange, bNewNoZEnable, NewToolTipIds, NewToolTip2Ids,
                          0);

    // a non-null NewMesh2 (only null is tested here) into the +0x378 holder.
    if (NewMesh2 != nullptr)
    {
        ClientMeshLoadFromPath(&ToggleMesh, NewMesh2, 0, false); // (path, 0, 0)
        NeuronRuntime::Engine->SetInstanceUserData(ToggleMesh.EngineInstance, &MeshHolder.UserFloats[0]);
        PlaceToggleMesh();
    }

    // as Create's tail.
    ToggleState = 0;
    SoundState0 = SoundName;
    SoundState1 = SoundName;
    UiSoundSlotState0 = FindUiSoundSlot(SoundState0.c_str());
    UiSoundSlotState1 = FindUiSoundSlot(SoundState1.c_str());
}

void NnToggleButtonControl::SetPosition(float const* NewPosition)
{
    // (RET 4): ButtonControl's, then with both instances bound the main
    // instance's whole transform onto the +0x378 one (Engine vt+0x9c).
    NnButtonControl::SetPosition(NewPosition);
    if (!HasToggleInstance(this))
        return;
    if (!HasInstance(this))
        return;
    float Transform[12];
    memcpy(Transform, NeuronRuntime::Engine->GetInstanceTransform(MeshHolder.EngineInstance),
           sizeof(Transform));
    NeuronRuntime::Engine->SetInstanceMatrix(ToggleMesh.EngineInstance, Transform);
    NeuronRuntime::Engine->UpdateInstance(ToggleMesh.EngineInstance, 0, nullptr);
}

void NnToggleButtonControl::SetOrientation(float const* NewOrientation)
{
    // (RET 4): the new orientation times the attach orientation (slot 40) through
    // into +0x3c, onto the main instance, then the main instance's transform onto
    // the +0x378 one.
    bAttached = 1; // +0x328
    float Attach[9];
    GetAttachOrientation(Attach);
    float Result[9];
    CockpitMulMatrix(Result, NewOrientation, Attach);
    memcpy(Orientation, Result, sizeof(Orientation));
    if (HasInstance(this))
    {
        NeuronRuntime::Engine->SetInstanceOrientation(MeshHolder.EngineInstance, Orientation);
        NeuronRuntime::Engine->UpdateInstance(MeshHolder.EngineInstance, 0, nullptr);
    }
    if (!HasToggleInstance(this))
        return;
    if (!HasInstance(this))
        return;
    float Transform[12];
    memcpy(Transform, NeuronRuntime::Engine->GetInstanceTransform(MeshHolder.EngineInstance),
           sizeof(Transform));
    NeuronRuntime::Engine->SetInstanceOrientation(ToggleMesh.EngineInstance, Transform);
    NeuronRuntime::Engine->UpdateInstance(ToggleMesh.EngineInstance, 0, nullptr);
}

void NnToggleButtonControl::Update()
{
    // . 0x0055c6b1: the +0x378 mesh is loaded late when it is still unbound and
    // "mesh2" is set -- without the user data Create / Init give it.
    if (!ClientMeshHolderHasInstance(&ToggleMesh) && Mesh2.c_str()[0] != '\0')
    {
        ClientMeshLoadFromPath(&ToggleMesh, Mesh2.c_str(), 0, false); // (path, 0, 0)
        PlaceToggleMesh();
    }

    Win3D::Update();

    // under the cached mouse ray (the routine, slot 4) the tooltip of the current
    // state: +0x32c in state 1, else +0xcc -- each only when non-zero.
    float MouseRay[3];
    Win3DGetMouseRay(MouseRay);
    if (HitTest(MouseRay))
    {
        if (ToggleState == 1)
        {
            if (ToolTip2Ids != 0)
                FeedToggleToolTip(this, ToolTip2Ids);
        }
        else if (ToolTipIds != 0)
        {
            FeedToggleToolTip(this, ToolTipIds);
        }
    }

    // while the button holds the capture -- the cursor ray (the routine on
    // , the 3DMathEngine's TransformMatrix with a copy of the camera orientation,
    // normalised by 1 / sqrt), then slot 4. Unlike ButtonControl's the branch no +0x330 bit is
    // written: on the button it tests bit 0 (not bit 2), off it tests bit 2.
    if ((ButtonBits & 0x10) != 0)
    {
        FSpaceCamera* const Cam = Win3DGetCamera();
        int CursorX = 0;
        int CursorY = 0;
        GetClientSoftwareCursor(&CursorX, &CursorY, nullptr); // / the field
        FVector Local;
        Win3DScreenToNearPlane(Cam, Local, static_cast<float>(CursorX),
                               static_cast<float>(CursorY));
        FMatrix CamOrientation;
        memcpy(&CamOrientation, Cam->World, sizeof(CamOrientation));
        FVector World;
        ButtonMathEngine()->TransformMatrix(World, CamOrientation, Local); // +0x2c
        double const Inv =
            kOne / sqrt(static_cast<double>(World.X) * World.X +
                        static_cast<double>(World.Y) * World.Y +
                        static_cast<double>(World.Z) * World.Z);
        float Ray[3];
        Ray[0] = static_cast<float>(World.X * Inv);
        Ray[1] = static_cast<float>(World.Y * Inv);
        Ray[2] = static_cast<float>(World.Z * Inv);

        bool const bHit = HitTest(Ray); // slot 4
        unsigned char const Bits = ButtonBits; // read after the call (0x0055cb68)
        if (bHit)
        {
            if ((Bits & 0x01) == 0)
            {
                TellParent(this, 0x2b);
                if (Parent != nullptr)
                {
                    NnUiPulseReset();
                    TellParent(this, 0x2c);
                }
            }
        }
        else if ((Bits & 0x04) != 0)
        {
            TellParent(this, 0x2a);
            TellParent(this, 0x2d);
        }
    }

    // the blink, as ButtonControl's.
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

void NnToggleButtonControl::Draw()
{
    // in state 1 ButtonControl's Draw runs with the +0x378 instance standing in
    // for +0x84 (restored after); the instance drawn is kept for the glow pass below.
    void* Instance = nullptr;
    if (ToggleState == 1)
    {
        Instance = ToggleMesh.EngineInstance;
        void* const Saved = MeshHolder.EngineInstance;
        MeshHolder.EngineInstance = Instance;
        NnButtonControl::Draw();
        MeshHolder.EngineInstance = Saved;
    }
    else
    {
        Instance = MeshHolder.EngineInstance;
        NnButtonControl::Draw();
    }

    Win3DDisable(kWin3DDepthTest); // (0xb71)
    // after a glow pass (+0x348 non-zero) the glow again, half a turn about y --
    // the orientation {C,0,S, 0,1,0, -S,0,C} of the field replaces the instance's (updated),
    // the tinted submit the routine in the glow colour, and the saved orientation goes back
    // without an update.
    if (GlowArgb != 0 && Instance != reinterpret_cast<void*>(static_cast<intptr_t>(-1)))
    {
        float Saved[9];
        memcpy(Saved, NeuronRuntime::Engine->GetInstanceOrientation(Instance), sizeof(Saved)); // vt+0x98
        float const C = static_cast<float>(cos(kToggleGlowAngle));
        float const S = static_cast<float>(sin(kToggleGlowAngle));
        float Turned[9];
        Turned[0] = C;
        Turned[1] = 0.0f;
        Turned[2] = S;
        Turned[3] = 0.0f;
        Turned[4] = 1.0f;
        Turned[5] = 0.0f;
        Turned[6] = -S;
        Turned[7] = 0.0f;
        Turned[8] = C;
        NeuronRuntime::Engine->SetInstanceOrientation(Instance, Turned);
        NeuronRuntime::Engine->UpdateInstance(Instance, 0, nullptr);
        unsigned char Glow[4];
        Glow[0] = static_cast<unsigned char>(GlowArgb >> 16);
        Glow[1] = static_cast<unsigned char>(GlowArgb >> 8);
        Glow[2] = static_cast<unsigned char>(GlowArgb);
        Glow[3] = static_cast<unsigned char>(GlowArgb >> 24);
        SubmitWin3DTintedEngineInstanceWrapped(Win3DGetCamera(), Instance, 1, Glow, 1, 1.0f,
                                               0);
        NeuronRuntime::Engine->SetInstanceOrientation(Instance, Saved);
    }
    Win3DDisable(kWin3DDepthTest); // (0xb71)
}

bool NnToggleButtonControl::MouseEvent(FWin3DMouseEvent const* Event)
{
    // (RET 4): the button sound and its UI slot of the current state, then
    // ButtonControl's handler.
    if (ToggleState == 0)
    {
        SoundName = SoundState0;
        UiSoundSlot = UiSoundSlotState0;
    }
    else
    {
        SoundName = SoundState1;
        UiSoundSlot = UiSoundSlotState1;
    }
    return NnButtonControl::MouseEvent(Event);
}

intptr_t NnToggleButtonControl::SetProperty(int Msg, uintptr_t A, uintptr_t B)
{
    // (RET 0xc): jump table 0x0055d200 indexed by 0x0055d214[Msg - 6].
    switch (Msg)
    {
    case 6:
    {
        // the offset from A; with a MAIN instance bound the +0x378 instance moves to
        // +0x60 (+ +0xa8 under +0x2f4 bit 0). Then ButtonControl's 6 as well.
        float const* const NewOffset = reinterpret_cast<float const*>(A);
        Offset[0] = NewOffset[0];
        Offset[1] = NewOffset[1];
        Offset[2] = NewOffset[2];
        if (HasInstance(this))
        {
            float Placed[3];
            if ((PositionFlags & 1) != 0)
            {
                Placed[0] = Offset[0] + Position[0];
                Placed[1] = Offset[1] + Position[1];
                Placed[2] = Offset[2] + Position[2];
            }
            else
            {
                Placed[0] = Position[0];
                Placed[1] = Position[1];
                Placed[2] = Position[2];
            }
            NeuronRuntime::Engine->SetInstancePosition(ToggleMesh.EngineInstance, Placed);
            NeuronRuntime::Engine->UpdateInstance(ToggleMesh.EngineInstance, 0, nullptr);
        }
        return NnButtonControl::SetProperty(Msg, A, B);
    }
    case 0x2f:
        *reinterpret_cast<int*>(A) = ToggleState;
        return 0;
    case 0x30:
        // SETZ: a zero A selects state 1.
        ToggleState = (A == 0) ? 1 : 0;
        return 0;
    case 0x31:
        // the two state sounds, each only when its pointer is non-null.
        if (A != 0)
        {
            SoundState0.assign(reinterpret_cast<char const*>(A));
            UiSoundSlotState0 = FindUiSoundSlot(SoundState0.c_str());
        }
        if (B != 0)
        {
            SoundState1.assign(reinterpret_cast<char const*>(B));
            UiSoundSlotState1 = FindUiSoundSlot(SoundState1.c_str());
        }
        return 0;
    default:
        return NnButtonControl::SetProperty(Msg, A, B);
    }
}

void NnToggleButtonControl::Activate()
{
    // ButtonControl's the routine body inlined (the command, else the parent's
    // 0x26), then the state flips and the parent hears 0x30 with the new state as B.
    NnButtonControl::Activate();
    ToggleState = (ToggleState == 0) ? 1 : 0;
    TellParentWithValue(this, 0x30, static_cast<uintptr_t>(ToggleState));
}

