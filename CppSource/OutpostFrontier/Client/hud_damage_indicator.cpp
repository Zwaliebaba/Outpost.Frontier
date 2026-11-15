// Author: Giperion.

#include "hud_damage_indicator.h"

#include "common_csys.h"
#include "common_iface.h"
#include "common_ini.h"
#include "game_options.h"
#include "globals.h"
#include "input_manager.h"
#include "nn_ui_pulse.h"
#include "player_behavior.h"
#include "win3d.h"

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "render/rendcomp.h"
#include "x86math.h"

#include <math.h>
#include <string.h>

static_assert(sizeof(FHudDamageIndicatorPart) == 0x2c, "baseline damage-indicator part is 0x2c");
static_assert(sizeof(HUD_DamageIndicator) == 0x414, "baseline HUD_DamageIndicator is 0x414 bytes");

namespace
{
    // the live indicator (ctor 0x004d63c9, dtor 0x004d6507).
    HUD_DamageIndicator* G_HudDamageIndicator = nullptr;

    // HUD_DamageIndicator.cpp's 3DMathEngine, the routine on first use (every
    // use site repeats the test: 0x004d6d5d, 0x004d6d99, 0x004d6f68, 0x004d6fc4, 0x004d710a).
    I3DMathEngine* G_DamageIndicatorMath = nullptr;

    I3DMathEngine* DamageIndicatorMath()
    {
        if (G_DamageIndicatorMath == nullptr)
        {
            G_DamageIndicatorMath = Acquire3DMathEngine();
        }
        return G_DamageIndicatorMath;
    }

    char const kDamageIndicatorCpp[] =
        "Client\\Space\\HUD_DamageIndicator.cpp"; // .rdata 0x005d8198
    char const kDamageBottomMesh[] = "interface\\hud\\HUD_damagebottom.3db"; // .rdata 0x005d8130

    float const kFlashSeconds = 0.7f;   // the hit amount and Draw's divisor
    float const kFlashAhead = 0.35f;    // FlashTowards' straight-ahead bonus
    float const kFlashMax = 1.0f;
    float const kAlphaMax = 0.8f;
    double const kQuadrantTurn = 0.39269909262657166; // , pi/8
    double const kInv255 = 0.00392156862745098;
    double const kByteScale = 255.0;

    // (cdecl) -- clamp to 0..255.
    int ClampByte(int Value)
    {
        if (Value < 0)
        {
            return 0;
        }
        if (Value > 0xff)
        {
            return 0xff;
        }
        return Value;
    }

    // FLD K / FADD Flash / FST Flash / FCOM 1.0 / TEST AH,5 / JNP: the unrounded sum is kept
    // when it is below 1.0, anything else (NaN included) stores 1.0.
    void AddFlash(float& Flash, float Amount)
    {
        double const Sum = static_cast<double>(Amount) + static_cast<double>(Flash);
        Flash = Sum < static_cast<double>(kFlashMax) ? static_cast<float>(Sum) : kFlashMax;
    }

    // The fade step of the routine / the routine: above zero it loses the frame time,
    // otherwise (NaN included, TEST AH,0x41) it is set to 0.
    void FadeFlash(float& Flash, double FrameTime)
    {
        if (Flash > 0.0f)
        {
            Flash = static_cast<float>(static_cast<double>(Flash) - FrameTime);
        }
        else
        {
            Flash = 0.0f;
        }
    }

    // The rotation of kQuadrantTurn about Z both FlashFromPosition (0x004d6d81..0x004d6deb)
    // and FlashTowards (0x004d7104..0x004d7155) build on the stack.
    FMatrix QuadrantMatrix()
    {
        float const Cos = static_cast<float>(cos(kQuadrantTurn));
        float const Sin = static_cast<float>(sin(kQuadrantTurn));
        FMatrix M;
        M.D[0][0] = Cos;
        M.D[0][1] = -Sin;
        M.D[0][2] = 0.0f;
        M.D[1][0] = Sin;
        M.D[1][1] = Cos;
        M.D[1][2] = 0.0f;
        M.D[2][0] = 0.0f;
        M.D[2][1] = 0.0f;
        M.D[2][2] = 1.0f;
        return M;
    }

    // Draw's alpha for one arrow: Flash / 0.7 when above zero, capped at 0.8. False when the
    // arrow is not drawn (TEST AH,0x41 / JNZ: zero, negative or NaN).
    bool FlashAlpha(float Flash, float* OutAlpha)
    {
        double const Ratio = static_cast<double>(Flash) / static_cast<double>(kFlashSeconds);
        if (!(Ratio > 0.0))
        {
            return false;
        }
        // TEST AH,5 / JP: 0.8 or more takes the cap.
        *OutAlpha = Ratio < static_cast<double>(kAlphaMax) ? static_cast<float>(Ratio) : kAlphaMax;
        return true;
    }

    // The tint and draw tail shared by all four arrows: the routine with the byte quad, then
    // IRenderer2 slot 7 (&the field, instance, 1.0f, 0, 0, 0).
    void DrawPart(FHudDamageIndicatorPart& Part, unsigned char const* Colour)
    {
        ClientMeshHolderSetColor(&Part.Holder, Colour);
        NeuronRuntime::Renderer->RenderInstance(Win3DGetCamera(), Part.Holder.EngineInstance, 1.0f, 0, nullptr,
                               nullptr);
    }

    // The colour the first two arrows build inline (0x004d666c..0x004d6727 and
    // ): from the field's bytes 0, 2, 1 and the alpha. Bytes 1 and 2
    // pass through a float (FSTP float) before the x255; byte 0 stays on the FPU stack, so its
    // /255*255 round trip is taken at the FPU's precision (under 53-bit precision it lands one
    // below for 24 byte values -- the same truncation the double arithmetic here gives).
    void InlineColour(float Alpha, unsigned char* OutColour)
    {
        unsigned long const Palette = G_NnPaletteArgb_679ba8;
        int const Byte0 = static_cast<int>(Palette & 0xffu);
        int const Byte1 = static_cast<int>((Palette >> 8) & 0xffu);
        int const Byte2 = static_cast<int>((Palette >> 16) & 0xffu);
        float const Byte1F = static_cast<float>(static_cast<double>(Byte1) * kInv255);
        float const Byte2F = static_cast<float>(static_cast<double>(Byte2) * kInv255);
        OutColour[0] = 0;
        OutColour[1] = 0;
        OutColour[2] = 0;
        OutColour[3] = 0;
        OutColour[0] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(Byte0) * kInv255 * kByteScale)));
        OutColour[1] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(Byte2F) * kByteScale)));
        OutColour[2] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(Byte1F) * kByteScale)));
        OutColour[3] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(Alpha) * kByteScale)));
    }

    // (thiscall on a byte quad, RET 0x10) -- each float x255.0, ftol, clamped to
    // 0..255 (inline, not through the routine), into bytes 0..3.
    void ColourFromFloats(unsigned char* OutColour, float R, float G, float B, float A)
    {
        OutColour[0] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(R) * kByteScale)));
        OutColour[1] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(G) * kByteScale)));
        OutColour[2] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(B) * kByteScale)));
        OutColour[3] = static_cast<unsigned char>(
            ClampByte(static_cast<int>(static_cast<double>(A) * kByteScale)));
    }

    // The colour the last two arrows build through the routine (0x004d68bc..0x004d6926 and
    // ): the same bytes 0, 2, 1, each rounded to a float first.
    void CalledColour(float Alpha, unsigned char* OutColour)
    {
        unsigned long const Palette = G_NnPaletteArgb_679ba8;
        int const Byte0 = static_cast<int>(Palette & 0xffu);
        int const Byte1 = static_cast<int>((Palette >> 8) & 0xffu);
        int const Byte2 = static_cast<int>((Palette >> 16) & 0xffu);
        ColourFromFloats(OutColour, static_cast<float>(static_cast<double>(Byte0) * kInv255),
                         static_cast<float>(static_cast<double>(Byte2) * kInv255),
                         static_cast<float>(static_cast<double>(Byte1) * kInv255), Alpha);
    }
}

HUD_DamageIndicator* GetHudDamageIndicator()
{
    return G_HudDamageIndicator;
}

// ============================================================================================
// FHudDamageIndicatorPart
// ============================================================================================

FHudDamageIndicatorPart::FHudDamageIndicatorPart(char const* Path, float Degrees)
    : Flash(0.0f)
{
    // .
    ClientMeshHolderConstruct(&Holder);
    Holder.UserFloats[3] = 1.0f;          // +0x20
    Holder.UserFloats[2] = 1.0f;          // +0x1c
    Holder.UserFloats[1] = 1.0f;          // +0x18
    Holder.UserFloats[0] = 1.0f;          // +0x14
    Holder.UserFloats[4] = 1.0f;          // +0x24
    Holder.AnimOrArchIndex = -1;          // +0x10
    ClientMeshLoadFromPath(&Holder, Path, 0, true); // (Path, 0, 1)
    if (!ClientMeshHolderHasInstance(&Holder))
    {
        FDUMP(0x100001, "%s(%d) : *** ERROR: HUD_DamageIndicator is missing: %s.",
              kDamageIndicatorCpp, 0xf7, Path);
    }

    // the angle is FST'd as a float before FSIN reloads it; FCOS
    // takes the unrounded product.
    double const Angle = static_cast<double>(Degrees) * static_cast<double>(0.017453292f);
    float const AngleF = static_cast<float>(Angle);
    float const Cos = static_cast<float>(cos(Angle));
    float const Sin = static_cast<float>(sin(static_cast<double>(AngleF)));
    FTransform Xform;
    Xform.Rotation.D[0][0] = Cos;
    Xform.Rotation.D[0][1] = -Sin;
    Xform.Rotation.D[0][2] = 0.0f;
    Xform.Rotation.D[1][0] = Sin;
    Xform.Rotation.D[1][1] = Cos;
    Xform.Rotation.D[1][2] = 0.0f;
    Xform.Rotation.D[2][0] = 0.0f;
    Xform.Rotation.D[2][1] = 0.0f;
    Xform.Rotation.D[2][2] = 1.0f;
    Xform.Translation.X = 0.0f;
    Xform.Translation.Y = 0.0f;
    Xform.Translation.Z = -1.0f;
    // The instance is used as it is, -1 included, as in baseline.
    NeuronRuntime::Engine->SetInstanceMatrix(Holder.EngineInstance, &Xform);   // +0x9c
    NeuronRuntime::Engine->UpdateInstance(Holder.EngineInstance, 0, nullptr);  // +0x6c
}

FHudDamageIndicatorPart::~FHudDamageIndicatorPart()
{
    // the routine with an instance bound, then the holder's destructor
    // (~FClientMeshHolder: the same release again, which finds nothing).
    if (ClientMeshHolderHasInstance(&Holder))
    {
        ClientUiMeshHolderRelease(&Holder);
    }
}

// ============================================================================================
// HUD_DamageIndicator
// ============================================================================================

HUD_DamageIndicator::HUD_DamageIndicator(INI_Reader& /*Reader*/)
    : PartDeg180(kDamageBottomMesh, 180.0f)     // , 0x43340000
    , PartDeg0(kDamageBottomMesh, 0.0f)
    , PartDegMinus90(kDamageBottomMesh, -90.0f) // , 0xc2b40000
    , PartDeg90(kDamageBottomMesh, 90.0f)       // , 0x42b40000
{
    // HUDItem inlined, WinControl the routine, the four arrows, the vptrs,
    // = this, HUDItem's name, then Create's body inlined (SetOffset slot 31,
    // SetPosition slot 5, Win3D::Create the routine(0, 0) called directly), the Win3D name and
    // the class name. Flags are not touched.
    G_HudDamageIndicator = this;
    HUDItem::Name = "DamageIndicator";
    SetOffset(0.0f, 0.0f, -1.0f);
    SetPosition(Position);
    Win3D::Create(nullptr, nullptr);
    Win3D::SetName("HUD_DamageIndicator");
    ClassName = "HUD_DamageIndicator";
}

HUD_DamageIndicator::~HUD_DamageIndicator()
{
    // the field = 0, then the arrows in reverse (+0x3e8, +0x3bc, +0x390,
    // +0x364), ~WinControl, HUDItem's watcher -- the member and base destructors.
    G_HudDamageIndicator = nullptr;
}

bool HUD_DamageIndicator::Create(Win3D* NewParent, void* Data)
{
    // (this = the WinControl face).
    SetOffset(0.0f, 0.0f, -1.0f);   // slot 31 (+0x7c)
    SetPosition(Position);          // slot 5 (+0x14)
    return Win3D::Create(NewParent, Data);
}

void HUD_DamageIndicator::Update()
{
    // (this = the WinControl face).
    if (GetClientMouseCursorDrawFlag() != 0 && GetPlayerObjRW() != nullptr)
    {
        SetHiddenWithChildren(false);             // (0)
        Flags = static_cast<unsigned char>(Flags | kWin3DActive);
    }
    else
    {
        SetHiddenWithChildren(true);              // (1)
    }
    double const FrameTime = GetClientFrameTimeCapped();
    FadeFlash(PartDeg0.Flash, FrameTime);         // +0x3b8
    FadeFlash(PartDeg180.Flash, FrameTime);       // +0x38c
    FadeFlash(PartDegMinus90.Flash, FrameTime);   // +0x3e4
    FadeFlash(PartDeg90.Flash, FrameTime);        // +0x410
    Win3D::Update();                              // tail jump the routine
}

void HUD_DamageIndicator::UpdateInactive()
{
    // (this = the WinControl face).
    if (GetClientMouseCursorDrawFlag() != 0 && GetPlayerObjRW() != nullptr)
    {
        SetHiddenWithChildren(false);             // (0)
    }
    double const FrameTime = GetClientFrameTimeCapped();
    FadeFlash(PartDeg0.Flash, FrameTime);
    FadeFlash(PartDeg180.Flash, FrameTime);
    FadeFlash(PartDegMinus90.Flash, FrameTime);
    FadeFlash(PartDeg90.Flash, FrameTime);
    // the branch inlined.
    Win3D::UpdateInactive();
}

void HUD_DamageIndicator::Draw()
{
    // (this = the WinControl face).
    if (GetGameOption(kGameOptInterfaceGroup) == 0)     // (6)
    {
        return;
    }
    if (GetGameOption(kGameOptHud) == 0)                // (0xa)
    {
        return;
    }
    if (GetPlayerObjRW() == nullptr)
    {
        return;
    }

    unsigned char Colour[4];
    float Alpha = 0.0f;
    if (FlashAlpha(PartDeg0.Flash, &Alpha))             // +0x3b8, 0x004d6629
    {
        InlineColour(Alpha, Colour);
        DrawPart(PartDeg0, Colour);
    }
    if (FlashAlpha(PartDeg180.Flash, &Alpha))           // +0x38c, 0x004d6753
    {
        InlineColour(Alpha, Colour);
        DrawPart(PartDeg180, Colour);
    }
    if (FlashAlpha(PartDegMinus90.Flash, &Alpha))       // +0x3e4, 0x004d687b
    {
        CalledColour(Alpha, Colour);
        DrawPart(PartDegMinus90, Colour);
    }
    if (FlashAlpha(PartDeg90.Flash, &Alpha))            // +0x410, 0x004d6955
    {
        // the routine zeroes the quad before the routine fills it.
        memset(Colour, 0, sizeof(Colour));
        CalledColour(Alpha, Colour);
        DrawPart(PartDeg90, Colour);
    }
}

void HUD_DamageIndicator::FlashFromPosition(Vector const* WorldPos)
{
    // .
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
    {
        return;
    }
    // Slot 4 (+0x10); its body is the one of slot 3 (0x5F1F0 in Common, ICF), so the pointer
    // is the whole transform the math engine reads.
    FTransform const& ShipXform = reinterpret_cast<FTransform const&>(Player->get_transform());
    FVector Local;
    DamageIndicatorMath()->InverseTransform(Local, ShipXform,
                                            reinterpret_cast<FVector const&>(*WorldPos));
    FVector const Source = Local;
    FMatrix const Turn = QuadrantMatrix();
    FVector Turned;
    DamageIndicatorMath()->TransposeTransform(Turned, Turn, Source);

    // TEST AH,0x41 on y, then on x (<= 0 and NaN go the "no" way).
    if (Turned.Y > 0.0f)
    {
        if (Turned.X > 0.0f)
        {
            AddFlash(PartDeg180.Flash, kFlashSeconds);     // +0x38c
        }
        else
        {
            AddFlash(PartDegMinus90.Flash, kFlashSeconds); // +0x3e4
        }
    }
    else
    {
        if (Turned.X > 0.0f)
        {
            AddFlash(PartDeg90.Flash, kFlashSeconds);      // +0x410
        }
        else
        {
            AddFlash(PartDeg0.Flash, kFlashSeconds);       // +0x3b8
        }
    }
}

void HUD_DamageIndicator::FlashAlongRay(Vector const* Origin, Vector const* Direction)
{
    // each Direction component x -1000.0f (the field), plus Origin's. The z
    // product is parked in a float local (0x004d6d12) before its add; x and y stay on the FPU.
    float const K = -1000.0f;
    float const ZScaled = static_cast<float>(static_cast<double>(Direction->z) * K);
    Vector Point;
    Point.x = static_cast<float>(static_cast<double>(Direction->x) * K + Origin->x);
    Point.y = static_cast<float>(static_cast<double>(Direction->y) * K + Origin->y);
    Point.z = static_cast<float>(static_cast<double>(ZScaled) + Origin->z);
    FlashTowards(&Point);
}

void HUD_DamageIndicator::FlashTowardsPoint(Vector const* Point, int /*Unused*/)
{
    // .
    FlashTowards(Point);
}

void HUD_DamageIndicator::FlashTowards(Vector const* Point)
{
    // .
    IObjRW* const Player = static_cast<IObjRW*>(GetPlayerObjRW());
    if (Player == nullptr)
    {
        return;
    }
    float Radius = 0.0f;
    Vector Center;
    Player->get_physical_radius(Radius, Center);        // slot 11 (+0x2c)
    FTransform const& ShipXform = reinterpret_cast<FTransform const&>(Player->get_transform());

    // the ship's rotation at its physical centre; D = (0, 0, -Radius).
    FTransform Frame;
    Frame.Rotation = ShipXform.Rotation;
    Frame.Translation.X = Center.x;
    Frame.Translation.Y = Center.y;
    Frame.Translation.Z = Center.z;
    FVector Back;
    Back.X = 0.0f;
    Back.Y = 0.0f;
    Back.Z = -Radius;
    FVector Rotated;
    DamageIndicatorMath()->Rotate(Rotated, Frame, Back); // +0x30
    // (Vector::operator+=): this[i] = other[i] + this[i].
    Frame.Translation.X = Rotated.X + Frame.Translation.X;
    Frame.Translation.Y = Rotated.Y + Frame.Translation.Y;
    Frame.Translation.Z = Rotated.Z + Frame.Translation.Z;

    FVector Local;
    DamageIndicatorMath()->InverseTransform(Local, Frame,
                                            reinterpret_cast<FVector const&>(*Point)); // +0x3c
    FVector const Source = Local;

    // a hit from straight ahead -- |z| above |x| and above |y| (NaN
    // fails both) and z not below zero -- adds 0.35 to all four arrows; z below zero leaves
    // without touching anything.
    float const AbsX = fabsf(Local.X);
    float const AbsY = fabsf(Local.Y);
    float const AbsZ = fabsf(Local.Z);
    if (AbsZ > AbsX && AbsZ > AbsY)
    {
        if (Local.Z < 0.0f)
        {
            return;
        }
        AddFlash(PartDeg180.Flash, kFlashAhead);      // +0x38c
        AddFlash(PartDeg0.Flash, kFlashAhead);        // +0x3b8
        AddFlash(PartDegMinus90.Flash, kFlashAhead);  // +0x3e4
        AddFlash(PartDeg90.Flash, kFlashAhead);       // +0x410
    }

    // the same quadrant pick as FlashFromPosition.
    FMatrix const Turn = QuadrantMatrix();
    FVector Turned;
    DamageIndicatorMath()->TransposeTransform(Turned, Turn, Source); // +0x38
    if (Turned.Y > 0.0f)
    {
        if (Turned.X > 0.0f)
        {
            AddFlash(PartDeg180.Flash, kFlashSeconds);
        }
        else
        {
            AddFlash(PartDegMinus90.Flash, kFlashSeconds);
        }
    }
    else
    {
        if (Turned.X > 0.0f)
        {
            AddFlash(PartDeg90.Flash, kFlashSeconds);
        }
        else
        {
            AddFlash(PartDeg0.Flash, kFlashSeconds);
        }
    }
}

namespace
{
    // The static HUDFACTORY<HUD_DamageIndicator> the field: XC entry the branch runs the
    // ctor the routine ("DamageIndicator") and registers the branch with atexit (->
    // ). Its Create the routine is operator new(0x414) + the routine.
    HUDFACTORY<HUD_DamageIndicator> G_HudDamageIndicatorFactory("DamageIndicator");
}
