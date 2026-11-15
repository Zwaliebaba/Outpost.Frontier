// Author: Giperion.

#include "text_win.h"

#include "common_misc.h"
#include "font_atlas.h"
#include "font_imp.h"
#include "game_options.h"
#include "nn_ui_pulse.h"
#include "space_camera.h"

#include <math.h>
#include <string.h>

TextWin::TextWin()
{
    // The inlined TextWin part of every derived ctor (0x0059c448..0x0059c4a6): the Font::String
    // at +0x7c (the routine) is the member's own ctor, then the colours.
    ColorIdle = 0;
    ColorPulse = 0;
    ColorD4 = 0;
    ColorD8 = 0;
    ColorInactive = 0;
}

TextWin::~TextWin()
{
    // clear the text, the Win3D Destroy body (flags, children, unregister),
    // then the Font::String member dtor (inlined at 0x005a1b58..0x005a1bb3, ~FFontString); the
    // Win3D dtor runs as the base.
    FontStringClearContents(&Text);
    Flags = static_cast<unsigned char>(Flags & 0xfc);
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Destroy();
    Win3DUnregister(this);
}

bool TextWin::HitTest(float const* Ray)
{
    // the ray's point at this object's depth (t = z / ray.z) must sit inside
    // [x, x + width] and [y - height, y]. the field is 0.0f.
    double const T = static_cast<double>(Position[2]) / Ray[2];
    float const PointY = static_cast<float>(T * Ray[1]);
    double const DeltaX = T * Ray[0] - Position[0];
    if (DeltaX < 0.0)
        return false;
    if (!(DeltaX <= Width))
        return false;
    double const DeltaY = static_cast<double>(Position[1]) - PointY;
    if (DeltaY < 0.0)
        return false;
    if (!(DeltaY <= Height))
        return false;
    return true;
}

bool TextWin::Create(Win3D* NewParent, void* /*Data*/)
{
    // (RET 8).
    Win3DReparent(this, NewParent);
    Win3DRegister(this);
    Flags = static_cast<unsigned char>((Flags & 0xfb) | kWin3DActive | kWin3DVisible);
    InitTextDefaults();
    Width = 0.1f;
    Height = 0.1f;
    return true;
}

void TextWin::Destroy()
{
    // .
    FontStringClearContents(&Text);
    Flags = static_cast<unsigned char>(Flags & 0xfc);
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Destroy();
    Win3DUnregister(this);
}

void TextWin::Draw()
{
    // .
    if (GetGameOption(kGameOptInterfaceGroup) == 0)
        return;
    if ((Flags & kWin3DVisible) == 0)
        return;

    Bounds[3] = -Height;
    Bounds[0] = 0.0f;
    Bounds[2] = Width;
    Bounds[1] = 0.0f;

    // under the mouse the pulse built around +0xd0 is used; otherwise the idle
    // colour, or the inactive colour when +0xe6 is clear.
    bool const bHover = Win3DGetHover() == this;
    unsigned long Pulsed = 0;
    NnUiPulseBlendFromArgb(&Pulsed, ColorPulse);
    unsigned long const* Color = &ColorInactive;
    if (bUsePalette != 0)
        Color = bHover ? &Pulsed : &ColorIdle;
    FontAtlasSetDrawColor(static_cast<unsigned>(*Color));

    float ScreenWidth = 0.0f;
    float ScreenHeight = 0.0f;
    TextWinProjectExtent(Width, Height, Position[2], &ScreenWidth, &ScreenHeight);
    float ScreenX = 0.0f;
    float ScreenY = 0.0f;
    TextWinProjectPoint(Position, &ScreenX, &ScreenY);

    // the draw origin {x, y, width, height, +0xe8, 0} into the routine.
    FFontStringDrawOrigin Origin;
    Origin.X = ScreenX;
    Origin.Y = ScreenY;
    Origin.Scale = ScreenWidth;
    Origin.Height = ScreenHeight;
    Origin.Field10 = FieldE8;
    Origin.Field14 = 0;
    FontStringDrawChildren(&Origin, &Text, nullptr, nullptr);
}

void TextWin::SetText(wchar_t const* NewText)
{
    // ADD ECX,0x7c / JMP the routine.
    FontStringSetText(&Text, NewText);
}

void TextWin::InitTextDefaults()
{
    // .
    FlagE0 = 0;
    FlagE3 = 0;
    FlagE1 = 0;
    FlagE2 = 1;
    bUsePalette = 1;
    FieldE8 = 0;
    TextScale = 0.002f; // = 0x3b03126f
    ColorIdle = G_NnPaletteArgb_679bb0;
    ColorPulse = G_NnPaletteArgb_679ba4;
    ColorInactive = G_NnPaletteArgb_679b94;
}

void TextWin::SetFont(char const* FontName, float Scale)
{
    // .
    IFont* const Font = FindMenuFontImp(FontName);
    if (Text.FontIface != Font)
    {
        // store at +0x98, then slot +8 on every child of the vector at
        // +0x8c..+0x90 (8-byte slots, the child pointer first). The call does not look at the
        // slot's kind byte: a FontString child binds the font (the branch), a NumberString
        // child takes it as its face key (the routine) -- both are slot +8.
        Text.FontIface = Font;
        for (unsigned Index = 0;
             Text.ChildBegin != nullptr &&
             Index < static_cast<unsigned>(Text.ChildEnd - Text.ChildBegin);
             ++Index)
        {
            Text.ChildBegin[Index].Child->BindFont(Text.FontIface);
        }
        Text.LayoutGen = 0; // +0xa4
    }
    TextScale = Scale; // +0xb0
}

void TextWinProjectExtent(float Width, float Height, float Z, float* OutWidth,
                          float* OutHeight)
{
    // . A depth closer than 0.001 (the field) is taken as 1.0.
    FSpaceCamera const* const Cam = Win3DGetCamera();
    double Depth = fabs(static_cast<double>(Z));
    if (Depth < 0.001)
        Depth = 1.0;
    double const NearOverDepth = static_cast<double>(Cam->ZNear) / Depth;
    double const InvNear = 1.0 / static_cast<double>(Cam->ZNear);
    *OutWidth = static_cast<float>(static_cast<double>(Cam->XScale) * InvNear * NearOverDepth *
                                   Width);
    *OutHeight = static_cast<float>(-(InvNear * Cam->YScale * NearOverDepth * Height));
}

void TextWinProjectPoint(float const* World, float* OutX, float* OutY)
{
    // .
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    if (AsCamera(Win3DGetCamera())->Project(&X, &Y, &Z, World) == 0)
    {
        X = 0.0f;
        Y = 0.0f;
    }
    *OutX = X;
    *OutY = Y;
}
