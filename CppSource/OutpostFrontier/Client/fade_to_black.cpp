// Author: Giperion.

// FadeToBlack (baseline `Client\cmn\FadeToBlack.cpp`):
// .. the routine setters and queries, the routine the tick.

#include "fade_to_black.h"

#include "globals.h"
#include "space_camera.h"
#include "win3d_immediate.h"

namespace
{
    // . One dword in .data, and it is NOT zero there: `module storage 0000803f`, i.e.
    // 1.0f. The client boots with the screen covered.
    float G_FadeLevel = 1.0f;

    // is NOT zero in .data either: `module storage 01`. With the level at 1.0 and the
    // target at 0 the client boots already animating, so the first ticks fade the covered
    // screen in on their own. 0x00616510 / 0x00616514 are zero.
    bool  G_bFadeAnimating = true;
    float G_FadeTarget = 0.0f;
    // is zero in the PE image, but the next static initializer the branch (_initterm
    // slot 0x0060f060, right after the branch) stores 1.0 (double .rdata 0x005c89b8) divided by
    // the freshly seeded duration: FLD 0x0061650c / FDIVR / FSTP 0x00616508 = 1.0f / 1.0f.
    float G_FadeSpeed = 1.0f;
    // is zero in the PE image, but the static initializer the branch (_initterm
    // slot 0x0060f05c) copies .rdata 0x005c8b64 = 1.0f into it before WinMain runs.
    float G_FadeDuration = 1.0f;
    bool  G_bFadeSkipStep = false;

    // the live colour, 0x006164fc the default. Nothing in the exe ever writes the
    // default, so it stays the black it is in .data -- but the setters still copy it every time,
    // which is what clears a colour a previous FadeInColoured left behind.
    float G_FadeColour[3] = {0.0f, 0.0f, 0.0f};
    const float G_FadeDefaultColour[3] = {0.0f, 0.0f, 0.0f};

    // the per-tick clamp is compared as the .rdata double 0.1 (0x005c7720) and stored
    // as the float 0x3dcccccd (0x0041238c).
    const double kMaxFadeStepCompare = 0.1;
    const float kMaxFadeStepSeconds = 0.1f;

    // IsFadeOpaque's threshold is the .rdata double 0.999 (0x005c8b68); DrawFade's
    // blend test at 0x0041246d compares against the float 0.999f (0x005c8b70).
    const double kFadeOpaqueCompare = 0.999;
    const float kFadeOpaqueThreshold = 0.999f;

    // / 0x0041245d: the source file and line baseline hands the viewport call.
    const char kFadeSource[] = "Client\\cmn\\FadeToBlack.cpp";
    const int kFadeDrawLine = 0xb4;

    void ResetColourToDefault()
    {
        G_FadeColour[0] = G_FadeDefaultColour[0];
        G_FadeColour[1] = G_FadeDefaultColour[1];
        G_FadeColour[2] = G_FadeDefaultColour[2];
    }

    // / 0x004122cd: both coloured fades read the three floats with no null test.
    void CopyColour(const float* Colour3)
    {
        G_FadeColour[0] = Colour3[0];
        G_FadeColour[1] = Colour3[1];
        G_FadeColour[2] = Colour3[2];
    }
}

bool IsFadeOpaque()
{
    // FCOMP double 0.999, TEST AH,1 / JNZ -> 0 on "below" and on unordered.
    return static_cast<double>(G_FadeLevel) >= kFadeOpaqueCompare;
}

float GetFadeLevel()
{
    return G_FadeLevel;
}

float GetFadeTarget()
{
    return G_FadeTarget;
}

float GetFadeDuration()
{
    return G_FadeDuration;
}

const float* GetFadeColour()
{
    return G_FadeColour;
}

void SetFadeTarget(float Target, float Speed)
{
    // . Note it does NOT touch the level or the colour -- it only redirects a fade
    // that is already in whatever state it is in.
    G_FadeTarget = Target;
    G_bFadeAnimating = true;
    G_FadeSpeed = Speed;
    G_bFadeSkipStep = true;
}

void SetFadeImmediate(float Level)
{
    // . The hard cut: level and target both land on the argument and the animation
    // is switched off, so the next tick has nothing to do.
    G_FadeTarget = Level;
    G_FadeLevel = Level;
    G_FadeDuration = 1.0f;
    G_bFadeAnimating = false;
    ResetColourToDefault();
    G_FadeSpeed = 1.0f;
}

void FadeIn(bool bStartOpaque)
{
    // . The flag is the only conditional part; everything below it runs either way.
    if (bStartOpaque)
        G_FadeLevel = 1.0f;

    G_FadeDuration = 1.0f;
    ResetColourToDefault();
    G_FadeTarget = 0.0f;
    G_bFadeAnimating = true;
    G_bFadeSkipStep = true;
    G_FadeSpeed = 1.0f;
}

void FadeOut()
{
    // . Unlike FadeIn this one always forces the starting level, because fading out
    // from a partly covered screen would shorten the fade.
    G_FadeDuration = 1.0f;
    ResetColourToDefault();
    G_FadeLevel = 0.0f;
    G_FadeTarget = 1.0f;
    G_bFadeAnimating = true;
    G_bFadeSkipStep = true;
    G_FadeSpeed = 1.0f;
}

void FadeInColoured(const float* Colour3, float Duration, bool bStartOpaque)
{
    // . The duration default is applied INSIDE the flag test, so a call with
    // bStartOpaque false and a non-positive duration keeps whatever duration was last used --
    // including a zero one, which then divides by zero below. baseline does not guard it and
    // neither does the port; the guard would change which fade speed the next transition runs at.
    if (bStartOpaque)
    {
        G_FadeDuration = 1.0f;
        G_FadeLevel = 1.0f;
    }

    CopyColour(Colour3);

    if (Duration > 0.0f)
        G_FadeDuration = Duration;

    G_FadeTarget = 0.0f;
    G_bFadeAnimating = true;
    G_bFadeSkipStep = true;
    // FLD duration / FDIVR double 1.0 / FSTP float.
    G_FadeSpeed = static_cast<float>(1.0 / static_cast<double>(G_FadeDuration));
}

void FadeOutColoured(const float* Colour3, float Duration)
{
    // . This one always seeds the duration, so it has no such hole.
    G_FadeDuration = 1.0f;
    G_FadeLevel = 0.0f;

    CopyColour(Colour3);

    if (Duration > 0.0f)
        G_FadeDuration = Duration;

    G_FadeTarget = 1.0f;
    G_bFadeAnimating = true;
    G_bFadeSkipStep = true;
    // FLD duration / FDIVR double 1.0 / FSTP float.
    G_FadeSpeed = static_cast<float>(1.0 / static_cast<double>(G_FadeDuration));
}

void UpdateFade(float DeltaSeconds)
{
    // .

    // the skip flag wins outright -- the frame a fade is started on advances it by
    // nothing at all, which is what stops a long load frame from swallowing the whole fade.
    if (G_bFadeSkipStep)
    {
        G_bFadeSkipStep = false;
        DeltaSeconds = 0.0f;
    }
    else if (static_cast<double>(DeltaSeconds) > kMaxFadeStepCompare)
    {
        // TEST AH,0x41 / JNZ skips the clamp on "less", "equal" and unordered, so
        // the clamp fires only on an ordered, strictly greater delta.
        DeltaSeconds = kMaxFadeStepSeconds;
    }

    if (!G_bFadeAnimating)
        return;

    // TEST AH,5 / JP takes "greater", "equal" and unordered to 0x004123e7, so the
    // rising arm runs only on an ordered level < target.
    if (G_FadeLevel < G_FadeTarget)
    {
        // rising. TEST AH,1 / JNZ returns on "below" and on unordered.
        // x87: FLD speed / FMUL dt / FADD level on the stack, one rounding at the FST.
        G_FadeLevel = static_cast<float>(static_cast<double>(G_FadeSpeed) * DeltaSeconds +
                                         static_cast<double>(G_FadeLevel));
        if (!(G_FadeLevel >= G_FadeTarget))
            return;
        G_FadeLevel = G_FadeTarget;
        G_bFadeAnimating = false;
        return;
    }

    // TEST AH,0x41 / JNZ -- equal or unordered stops the animation with the level
    // left as it is.
    if (!(G_FadeLevel > G_FadeTarget))
    {
        G_bFadeAnimating = false;
        return;
    }

    // falling. TEST AH,0x41 / JP returns on "above" and on unordered; "below" and
    // "equal" snap to the target.
    // x87: FLD speed / FMUL dt / FSUBR level, one rounding at the FST.
    G_FadeLevel = static_cast<float>(static_cast<double>(G_FadeLevel) -
                                     static_cast<double>(G_FadeSpeed) * DeltaSeconds);
    if (!(G_FadeLevel <= G_FadeTarget))
        return;
    G_FadeLevel = G_FadeTarget;
    G_bFadeAnimating = false;
}

void DrawFade(ICamera* Cam)
{
    // .

    // the guard is a compare against 0.0 with TEST AH,0x41 / JNZ, so it draws only
    // on a strictly positive level -- a zero or negative level, and a NaN, all draw nothing.
    const float Level = G_FadeLevel;
    if (!(Level > 0.0f))
        return;

    // the 2D projection and the viewport, in that order.
    Win3DSetOrtho2D();
    Win3DSetViewport(nullptr, kFadeSource, kFadeDrawLine);

    // blending goes on only while the fade is see-through. The threshold is the same
    // 0.999 IsFadeOpaque uses -- once the screen is covered there is nothing to blend against.
    if (Level < kFadeOpaqueThreshold)
    {
        Win3DEnable(kWin3DBlend);
        Win3DSetBlendFunc(kWin3DBlendSrcAlpha, kWin3DBlendInvSrcAlpha);
    }
    else
    {
        Win3DDisable(kWin3DBlend);
    }

    // no culling, no depth test, no depth writes -- it is an overlay.
    Win3DDisable(kWin3DCullFace);
    Win3DDisable(kWin3DDepthTest);
    Win3DSetDepthWrite(0);

    // the rectangle. A null camera covers the surface RenderMgr last laid out
    // (0x004124e2 / 0x004124e8: the field height, the field width) -- not the options
    // object's requested mode.
    int Left = 0;
    int Top = 0;
    int Right = static_cast<int>(G_RenderSurfaceWidth);
    int Bottom = static_cast<int>(G_RenderSurfaceHeight);
    if (Cam != nullptr)
    {
        // slot 0x14 (GetViewport) of whatever camera the caller hands
        // in -- the space camera from the branch, the room camera (the routine) from
        // .
        const int* const Rect = Cam->GetViewport();
        Left = Rect[0];
        Top = Rect[1];
        Right = Rect[2];
        Bottom = Rect[3];
    }

    // the colour, with the level as alpha.
    Win3DColor4f(G_FadeColour[0], G_FadeColour[1], G_FadeColour[2], Level);

    // four corners, wound clockwise from the top left, each pushed
    // half a pixel outward.
    Win3DBegin(kWin3DQuads);
    Win3DVertex3f(static_cast<float>(Left) - 0.5f, static_cast<float>(Top) - 0.5f, 0.0f);
    Win3DVertex3f(static_cast<float>(Right) + 0.5f, static_cast<float>(Top) - 0.5f, 0.0f);
    Win3DVertex3f(static_cast<float>(Right) + 0.5f, static_cast<float>(Bottom) + 0.5f, 0.0f);
    Win3DVertex3f(static_cast<float>(Left) - 0.5f, static_cast<float>(Bottom) + 0.5f, 0.0f);
    Win3DEnd();

    // the depth test goes back on, and the depth write is restored to whatever the
    // caller had. Culling and blending are NOT restored -- baseline leaves both as it found them,
    // which is a real asymmetry and not an omission here.
    Win3DEnable(kWin3DDepthTest);
    Win3DSetDepthWrite(1);
}
