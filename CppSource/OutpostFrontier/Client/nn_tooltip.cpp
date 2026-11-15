// Author: Giperion.

#include "nn_tooltip.h"

#include "font_atlas.h"
#include "font_imp.h"
#include "game_options.h"
#include "globals.h"
#include "ids_strings.h"
#include "input_manager.h"
#include "menu_fonts_load.h"
#include "menu_keymap_config.h"
#include "nn_rollover.h"   // NnRolloverFeedHoverGlobal (the routine)
#include "nn_ui_pulse.h"
#include "perf_options.h"
#include "space_camera.h"
#include "space_letterbox.h"
#include "win3d.h"              // Win3DGetMouseRay / Win3DScreenToNearPlane / Win3DTransformByMatrix
#include "win3d_engine_submit.h"

#include "common_csys.h"        // Acquire3DMathEngine (the routine)
#include "x86math.h"            // I3DMathEngine, FVector, FMatrix
#include "win3d_immediate.h"
#include "win3d_renderables.h"

#include <cmath>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

namespace
{
    NnToolTipControl* G_ToolTipSingleton = nullptr;
    int G_TipCursorX = 0;
    int G_TipCursorY = 0;

    // this file's 3DMathEngine, the routine on first use (0x004bdab6).
    I3DMathEngine* G_ToolTipMath = nullptr;

    struct FProjectedPoint
    {
        float X;
        float Y;
    };

    FProjectedPoint ProjectUiPoint(float const Position[3])
    {
        FProjectedPoint Point;
        float Depth;
        int const Result = AsCamera(GetWin3DSceneCamera())->Project(
            &Point.X, &Point.Y, &Depth, Position);
        if (static_cast<unsigned char>(Result) == 0)
        {
            Point.X = 0.0f;
            Point.Y = 0.0f;
        }
        return Point;
    }
}

NnToolTipControl::NnToolTipControl()
    : bBuilt_(0)
    , bHasText_(0)
    , bFedThisFrame_(0)
    , DelayRemain_(0.0f)
    , DelayAccum_(0.0f)
    , DelayThreshold_(0.25f)
    , OffsetX_(4.0f)
    , OffsetY_(-42.0f)
{
    // the TextWin part runs in TextWin() (Font::String +0x7c, colours zeroed).
    // , the +0xf4 record: caption {0, 0} and the Font::String (Hover_'s
    // ctor), then +0xf8 = 0, +0xf4 = -1, +0x138 = 0, the routine(null, -1) on the caption (a
    // clear of an empty buffer) and +0x154 = 0 -- the body of the routine, inlined. The world
    // position and half extents are left as operator new returned them; the port zeroes them.
    ToolTipHoverReset(&Hover_);
    InitToolTipDefaults();
}

NnToolTipControl::~NnToolTipControl()
{
    // 0x004bd6b2..0x004bd6c2 drops the singleton when it is this tooltip; then
    // the +0xf4 record (its Font::String at +0x104, then the caption at +0xfc -- Hover_'s
    // dtor, ToolTipHoverDestroy) and the TextWin dtor the routine (the base).
    if (G_ToolTipSingleton == this)
        G_ToolTipSingleton = nullptr;
}

void NnToolTipControl::InitToolTipDefaults()
{
    // (0x004bd770..0x004bd82e). The flags &= 0xfd after it belongs to the
    // factory the routine (0x004be51c), not to this function.
    DrawLayer = 10;          // +0x70
    DelayRemain_ = 0.0f;     // +0x15c
    DelayAccum_ = 0.0f;      // +0x160
    bBuilt_ = 0;             // +0x158
    bHasText_ = 0;           // +0x159
    bFedThisFrame_ = 0;      // +0x164
    DelayThreshold_ = 0.25f; // +0x168 = 0x3e800000
    OffsetX_ = 4.0f;         // +0xec = 0x40800000
    OffsetY_ = -42.0f;       // +0xf0 = 0xc2280000

    // the routine("ToolTip") into the Font::String's +0x1c (+0x98) only when it
    // differs, then LayoutGen (+0xa4) = 0. The child notify loop (+0x8c..+0x90, slot 2 of
    // each child) walks an empty vector here: the Font::String was just constructed.
    IFont* const TipFont = FindMenuFontImp("ToolTip");
    if (Text.FontIface != TipFont)
    {
        Text.FontIface = TipFont;
        Text.LayoutGen = 0; // +0xa4
    }
    if (Text.bMultiLine != 1) // +0xac
    {
        Text.bMultiLine = 1;
        Text.LayoutGen = 0; // +0xa4
    }
}

bool NnToolTipControl::HitTest(float const* /*Ray*/)
{
    // slot 4 the routine: XOR AL,AL / RET 4.
    return false;
}

bool NnToolTipControl::Create(Win3D* NewParent, void* /*Data*/)
{
    // (RET 8).
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);
    InitTextDefaults();
    Width = 0.1f;
    Height = 0.1f;
    Flags = static_cast<unsigned char>(Flags & 0xfd);
    DrawSubLayer = 7;   // +0x78
    return true;
}

void NnToolTipControl::RepositionAfterCaption(FToolTipHoverInfo const* Info)
{
    // (thiscall, RET 4). Every camera read is off the Win3D scene camera
    // (+0x04 orientation, +0x68/+0x6c centre, +0x78/+0x7c scale, +0xa4 near,
    // +0xac..+0xb8 viewport).
    FSpaceCamera* const Cam = GetWin3DSceneCamera();

    // the viewport's top-left corner on the near plane (the routine,
    // with FILD'd left/top rounded to float).
    FVector TopLeftLocal;
    Win3DScreenToNearPlane(Cam, TopLeftLocal, static_cast<float>(Cam->Viewport[0]),
                           static_cast<float>(Cam->Viewport[1]));

    // the orientation copied to a local, then this file's own engine
    // (the field) TransformMatrix (+0x2c).
    FMatrix Orientation;
    memcpy(&Orientation, Cam->World, sizeof(Orientation));
    if (G_ToolTipMath == nullptr)
        G_ToolTipMath = Acquire3DMathEngine();
    FVector TopLeft;
    G_ToolTipMath->TransformMatrix(TopLeft, Orientation, TopLeftLocal);

    // the bottom-right corner, inlined with the other operation order
    // ((v - centre) / scale * near), then the routine on a fresh copy of the orientation.
    FVector BottomRightLocal;
    BottomRightLocal.X = static_cast<float>(
        (static_cast<double>(Cam->Viewport[2]) - Cam->ScreenCenterX) / Cam->XScale * Cam->ZNear);
    BottomRightLocal.Y = static_cast<float>(
        (static_cast<double>(Cam->Viewport[3]) - Cam->ScreenCenterY) / Cam->YScale * Cam->ZNear);
    BottomRightLocal.Z = -Cam->ZNear;
    memcpy(&Orientation, Cam->World, sizeof(Orientation));
    FVector BottomRight;
    Win3DTransformByMatrix(BottomRight, Orientation, BottomRightLocal);

    // the mouse ray cached by the Win3D input handler (the routine).
    float Ray[3];
    Win3DGetMouseRay(Ray);

    // the ray scaled onto z = -1. The reciprocal stays unrounded on the
    // FPU for all three products.
    double const RayScale = -1.0 / static_cast<double>(Ray[2]);
    float PosX = static_cast<float>(static_cast<double>(Ray[0]) * RayScale);
    float PosY = static_cast<float>(static_cast<double>(Ray[1]) * RayScale);
    float PosZ = static_cast<float>(RayScale * static_cast<double>(Ray[2]));

    // only the left edge of the top-left corner and the right / bottom
    // of the bottom-right corner are projected.
    float const BoundLeft =
        static_cast<float>(static_cast<double>(TopLeft.X) * (-1.0 / static_cast<double>(TopLeft.Z)));
    double const BoundScale = -1.0 / static_cast<double>(BottomRight.Z);
    float const BoundRight = static_cast<float>(static_cast<double>(BottomRight.X) * BoundScale);
    float const BoundVert = static_cast<float>(static_cast<double>(BottomRight.Y) * BoundScale);

    // a live hover anchors at its world box's corner instead. The
    // reciprocal is FST'd to float and that float scales Y and Z; X uses the unrounded one.
    if (Info->bLive != 0)
    {
        double const Scale = -1.0 / Info->WorldPos[2];
        float const StoredScale = static_cast<float>(Scale);
        PosX = static_cast<float>((static_cast<double>(Info->WorldPos[0]) - Info->HalfExtents[0]) * Scale);
        PosY = static_cast<float>((static_cast<double>(Info->WorldPos[1]) - Info->HalfExtents[1]) * StoredScale);
        PosZ = static_cast<float>(static_cast<double>(Info->WorldPos[2]) * StoredScale);
        PosX = static_cast<float>(static_cast<double>(PosX) + 0.004f);
        PosY = static_cast<float>(static_cast<double>(PosY) - 0.03f);
    }

    // the caption's font scale at the CURRENT depth (+0x68), then its
    // extent.
    FontStringSetScale(&Text, 0.35f, Position[2]);
    int Extent[2] = {0, 0};
    FontStringGetExtent(&Text, Extent);

    // (width + 4) as an unsigned 64-bit FILD, over (near/near)*xscale.
    double const ExtentW = static_cast<unsigned>(Extent[0]) + 4u;
    double const Width = ExtentW / ((static_cast<double>(Cam->ZNear) / Cam->ZNear) * Cam->XScale);
    // TEST AH,0x41 / JNZ -- clamped only when strictly greater.
    if (PosX + Width > BoundRight)
        PosX = static_cast<float>(BoundRight - Width);
    // TEST AH,5 / JP -- only on an ordered "less".
    if (PosX < BoundLeft)
        PosX = static_cast<float>(static_cast<double>(BoundLeft) + 0.004f);

    // +0xf0 * +0xb0; TEST AH,5 / JP, ordered "less" only.
    double const VertBias = static_cast<double>(OffsetY_) * TextScale;
    if (PosY + VertBias < BoundVert)
        PosY = static_cast<float>(PosY - VertBias * 1.5f);

    // virtual slot 5 (+0x14).
    float const NewPosition[3] = {PosX, PosY, PosZ};
    SetPosition(NewPosition);
}

void NnToolTipControl::Draw()
{
    // — listing 004bdfe0..004be279.
    if (GetGameOption(kGameOptInterfaceGroup) == 0)
    {
        return;
    }
    if (GetGameOption(kGameOptDrawTextControls) == 0)
    {
        return;
    }
    if ((Flags & 0x02) == 0)
    {
        return;
    }
    // — skip while letterbox amount > 0.
    if (IsLetterboxVisible())
    {
        return;
    }

    // — flush batches before overlay geometry.
    Win3DFlushBeforeOverlayDraw();

    // (0) + the routine(0xb71) + the routine(0xb44).
    Win3DSetDepthWrite(0);
    Win3DDisable(kWin3DDepthTest);
    Win3DDisable(kWin3DCullFace);

    FontStringSetScale(&Text, 0.35f, Position[2]);
    Bounds[3] = -Height;
    Bounds[2] = Width;
    Bounds[0] = 0.0f;
    Bounds[1] = 0.0f;
    int Extent[2];
    FontStringGetExtent(&Text, Extent);
    Width = static_cast<float>(static_cast<unsigned>(Extent[0]));
    Height = static_cast<float>(static_cast<unsigned>(Extent[1]));

    FProjectedPoint const Screen = ProjectUiPoint(Position);
    if (FlagE1 != 0)
    {
        float const PadW = Width + 3.0f;
        float const PadH = Height + 2.0f;
        float const X0 = static_cast<float>(std::floor(static_cast<double>(Screen.X)));
        float const Y0 = static_cast<float>(std::floor(static_cast<double>(Screen.Y)));
        float const X1 = X0 + PadW;
        float const Y1 = Y0 + PadH;

        Win3DPushProjection();
        Win3DSetOrtho2D();

        unsigned char FillRgba[4] = {
            static_cast<unsigned char>(ColorD8 & 0xff),
            static_cast<unsigned char>((ColorD8 >> 8) & 0xff),
            static_cast<unsigned char>((ColorD8 >> 16) & 0xff),
            static_cast<unsigned char>((ColorD8 >> 24) & 0xff)};
        Win3DColor4ubv(FillRgba);
        Win3DDisable(kWin3DTexture2D);
        Win3DBegin(kWin3DQuads);
        Win3DVertex3f(X0, Y0, 0.0f);
        Win3DVertex3f(X1, Y0, 0.0f);
        Win3DVertex3f(X1, Y1, 0.0f);
        Win3DVertex3f(X0, Y1, 0.0f);
        Win3DEnd();

        // Second pass: listing PUSH the field → the routine; Begin(7)=LineLoop.
        Win3DColor4ubv(G_HudChromeColorRgba);
        Win3DBegin(kWin3DLineLoop);
        Win3DVertex3f(X0, Y0, 0.0f);
        Win3DVertex3f(X1, Y0, 0.0f);
        Win3DVertex3f(X1, Y1, 0.0f);
        Win3DVertex3f(X0, Y1, 0.0f);
        Win3DEnd();

        Win3DPopProjection();
    }

    FontAtlasSetDrawColor(ColorIdle);

    FFontStringDrawOrigin Origin;
    Origin.X = Screen.X + 1.0f;
    Origin.Y = Screen.Y;
    Origin.Scale = Width;
    Origin.Height = Height;
    Origin.Field10 = 1;
    Origin.Field14 = 0;
    FontStringDrawChildren(&Origin, &Text, nullptr, nullptr);

    // (0xb71) + the routine(1).
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
}

void NnToolTipControl::ApplyHover(FToolTipHoverInfo const* Info)
{
    // (thiscall, RET 4). No test of Info: 0x004bd896 reads it at once.
    // the record's live fields only (Ids, IdsAlt, WorldPos,
    // HalfExtents, bLive); the caption, Font::String and AuxField of Hover_ stay untouched.
    Hover_.Ids = Info->Ids;
    Hover_.IdsAlt = Info->IdsAlt;
    Hover_.WorldPos[0] = Info->WorldPos[0];
    Hover_.WorldPos[1] = Info->WorldPos[1];
    Hover_.WorldPos[2] = Info->WorldPos[2];
    Hover_.HalfExtents[0] = Info->HalfExtents[0];
    Hover_.HalfExtents[1] = Info->HalfExtents[1];
    Hover_.HalfExtents[2] = Info->HalfExtents[2];
    Hover_.bLive = Info->bLive;

    // three byte-built colour dwords into +0xd8 / +0xd4 / +0xcc, the
    // two flag bytes +0xe1 / +0xe0 = 1 and the text scale +0xb0.
    ColorD8 = 0xFF706143u; // bytes 43 61 70 ff
    ColorD4 = 0x19e0c387u; // bytes 87 c3 e0 19
    ColorIdle = 0xFFFFF7B9u; // bytes b9 f7 ff ff
    TextScale = 0.0011f;
    FlagE0 = 1;
    FlagE1 = 1;

    // Two stack buffers: the ids / key-name scratch at ESP+0x1c (0x200 bytes) and the caption
    // at ESP+0x21c (0x1000 bytes).
    wchar_t Scratch[0x100];
    wchar_t Resolved[0x800];
    if (Info->CaptionW == nullptr)
    {
        // the routine on the stored +0xf8 (IdsAlt) with a capacity
        // of 0x80; on a miss (0) the scratch is emptied -- not a blank. Then wcscpy.
        unsigned const N =
            GetIdsStringWWithRawFallback(static_cast<unsigned>(Hover_.IdsAlt), Scratch, 0x80);
        if (N == 0)
            Scratch[0] = L'\0';
        wcscpy(Resolved, Scratch);
    }
    else
    {
        // wcscpy of the record's caption.
        wcscpy(Resolved, Info->CaptionW);
    }

    // a caption of exactly " " (the field) hides the tip.
    if (_wcsicmp(Resolved, L" ") == 0)
    {
        bHasText_ = 0;
        Flags = static_cast<unsigned char>(Flags & 0xfd);
        return;
    }

    // AuxField = KeyCmd NicknameId -- the routine finds the binding;
    // with its second dword non-zero, the routine formats it into the scratch (capacity
    // 0x100) and " (" / name / ")" (the field / the field) are appended unconditionally.
    if (Info->AuxField != 0)
    {
        FKeyBindingLookup Lookup = {};
        if (KeyMapFindActiveBinding(static_cast<unsigned>(Info->AuxField), &Lookup)
            && Lookup.VirtualKey != 0)
        {
            KeyMapFormatBindingName(&Lookup, Scratch, 0x100);
            wcscat(Resolved, L" (");
            wcscat(Resolved, Scratch);
            wcscat(Resolved, L")");
        }
    }

    SetText(Resolved);            // vt+0x70 (TextWin slot 28)
    RepositionAfterCaption(Info); // the routine
}

void NnToolTipControl::UpdateFromHover(FToolTipHoverInfo const* Info)
{
    // . No null test on Info (the rollover's the routine reads it first).
    bFedThisFrame_ = 1;
    if (GetClientMouseCursorDrawFlag() == 0)
    {
        bHasText_ = 0;
        Flags = static_cast<unsigned char>(Flags & 0xfd);
        return;
    }

    // the same hover to the Rollover (the routine).
    NnRolloverFeedHoverGlobal(Info);

    // the routine("enable_tooltips") -> the record's +0x14 (the applied value)
    // against 0.999 (the field, double); TEST AH,5 / JNP bails only below it. No null test.
    FPerfOption* Opt = FindPerfOption("enable_tooltips");
    if (static_cast<double>(Opt->Applied) < 0.999)
        return;

    if (bHasText_ != 0)
    {
        bool const bSameIds = (Hover_.Ids == Info->Ids);
        bool const bSameLive = (Hover_.bLive == Info->bLive);
        bool bSamePos = true;
        if (Hover_.bLive != 0)
            bSamePos = (Hover_.WorldPos[0] == Info->WorldPos[0]
                        && Hover_.WorldPos[1] == Info->WorldPos[1]);
        if (bSameIds && bSameLive && bSamePos)
        {
            Flags = static_cast<unsigned char>(Flags & 0xfd);
            return;
        }
        bHasText_ = 0;
        DelayRemain_ = 0.0f;
        DelayAccum_ = 0.0f;
    }

    bool const bNeedRebuild =
        (Hover_.Ids != Info->Ids) || (Hover_.bLive != Info->bLive)
        || (Hover_.bLive != 0
            && (Hover_.WorldPos[0] != Info->WorldPos[0]
                || Hover_.WorldPos[1] != Info->WorldPos[1]))
        || (bBuilt_ == 0);
    if (bNeedRebuild)
    {
        if (DelayRemain_ <= 0.0f && DelayAccum_ < DelayThreshold_)
            return;
        ApplyHover(Info);
    }
    Flags = static_cast<unsigned char>(Flags | 0x03);
}

void NnToolTipControl::TickToolTip(double DeltaSeconds)
{
    // .
    if (bFedThisFrame_ == 0)
    {
        Flags = static_cast<unsigned char>(Flags & 0xfd);
        bHasText_ = 0;
    }
    bFedThisFrame_ = 0;

    if (GetClientMouseCursorDrawFlag() == 0)
    {
        bHasText_ = 0;
        Flags = static_cast<unsigned char>(Flags & 0xfd);
        return;
    }

    unsigned char FlagsByte = Flags;
    if ((FlagsByte & 0x02) == 0)
    {
        Flags = static_cast<unsigned char>(FlagsByte & 0xfe);
        if (DelayRemain_ > 0.0f)
        {
            DelayRemain_ = static_cast<float>(DelayRemain_ - DeltaSeconds);
            if (DelayRemain_ < 0.0f)
                DelayRemain_ = 0.0f;
        }
    }
    else
    {
        DelayRemain_ = DelayThreshold_;
    }

    bBuilt_ = static_cast<unsigned char>((Flags >> 1) & 1);
    Flags = static_cast<unsigned char>(Flags & 0xfd);

    int CursorX = 0;
    int CursorY = 0;
    int CursorZ = 0;
    GetClientSoftwareCursor(&CursorX, &CursorY, &CursorZ);
    if (G_TipCursorX == CursorX && G_TipCursorY == CursorY)
        DelayAccum_ += DeltaSeconds;
    else
    {
        G_TipCursorX = CursorX;
        G_TipCursorY = CursorY;
        DelayAccum_ = 0.0f;
    }

    // with the left or right button down (the field & 3) the +0x159
    // latch goes up, so the routine keeps the tip hidden for the hover it was clicked on.
    if ((GetClientMouseButtons() & 3u) != 0)
        bHasText_ = 1;
}

void NnToolTipControl::HideNow()
{
    // .
    Flags = static_cast<unsigned char>(Flags & 0xfd);
    bHasText_ = 0;
}

void NnToolTipFeedHover(FToolTipHoverInfo const* Info)
{
    // only the singleton is tested (0x004be376); Info is passed on as is.
    if (G_ToolTipSingleton == nullptr)
        return;
    G_ToolTipSingleton->UpdateFromHover(Info);
}

void NnToolTipControl::ReadHover(FToolTipHoverInfo* Out) const
{
    // 0x004be2f9..0x004be35f.
    Out->Ids = Hover_.Ids;
    Out->IdsAlt = Hover_.IdsAlt;
    Out->WorldPos[0] = Hover_.WorldPos[0];
    Out->WorldPos[1] = Hover_.WorldPos[1];
    Out->WorldPos[2] = Hover_.WorldPos[2];
    Out->HalfExtents[0] = Hover_.HalfExtents[0];
    Out->HalfExtents[1] = Hover_.HalfExtents[1];
    Out->HalfExtents[2] = Hover_.HalfExtents[2];
    Out->bLive = Hover_.bLive;
}

bool NnToolTipReadHover(FToolTipHoverInfo* Out)
{
    // .
    if (G_ToolTipSingleton == nullptr)
        return false;
    G_ToolTipSingleton->ReadHover(Out);
    return true;
}

void ToolTipHoverReset(FToolTipHoverInfo* Info)
{
    // +4 = 0, +0 = -1, +0x44 = 0, then the caption's first wchar cleared only
    // when the buffer exists and is non-empty, then +0x60 = 0. No null test on Info.
    Info->IdsAlt = 0;
    Info->Ids = -1;
    Info->AuxField = 0;
    if (Info->CaptionW != nullptr && Info->CaptionW[0] != L'\0')
        Info->CaptionW[0] = L'\0';
    Info->bLive = 0;
}

void NnToolTipTickGlobal(double DeltaSeconds)
{
    // .
    if (G_ToolTipSingleton == nullptr)
        return;
    G_ToolTipSingleton->TickToolTip(DeltaSeconds);
}

void NnToolTipHideGlobal()
{
    if (G_ToolTipSingleton == nullptr)
        return;
    G_ToolTipSingleton->HideNow();
}

bool NnToolTipWasFedThisFrame()
{
    // the singleton's +0x164 byte, false without one.
    if (G_ToolTipSingleton == nullptr)
        return false;
    return G_ToolTipSingleton->bFedThisFrame() != 0;
}

NnToolTipControl* GetNnToolTipSingleton()
{
    return G_ToolTipSingleton;
}

void SetNnToolTipSingleton(NnToolTipControl* Tip)
{
    G_ToolTipSingleton = Tip;
}

// WINFACTORY<ToolTip>, static init the branch (the routine("ToolTip")).
ToolTipFactory G_ToolTipFactory("ToolTip");

Win3D* ToolTipFactory::Create(Win3D* Parent, void* Payload)
{
    // (RET 8): operator new(0x170), the ctor (the routine inside), +0x6c &= 0xfd
    // (0x004be51c), the singleton the field (0x004be525), +0x10 = the factory's name (+0xc,
    // ), then slot 9 (the routine); on failure slot 0 with 1 (the scalar deleting
    // dtor) and null.
    NnToolTipControl* const Tip = new NnToolTipControl();
    Tip->Flags = static_cast<unsigned char>(Tip->Flags & 0xfd);
    G_ToolTipSingleton = Tip;
    Tip->ClassName = ClassName;
    if (!Tip->Create(Parent, Payload))
    {
        delete Tip;
        return nullptr;
    }
    return Tip;
}
