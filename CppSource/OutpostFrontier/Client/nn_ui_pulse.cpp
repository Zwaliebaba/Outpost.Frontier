// Author: Giperion.

#include "nn_ui_pulse.h"

// .
unsigned char G_HudChromeColorRgba[4] = {0x87u, 0xC3u, 0xE0u, 0xC0u};

// .. the branch (bytes R,G,B,A in memory = little-endian 0xAABBGGRR).
unsigned long G_NnPaletteArgb_679bb0 = 0xFFE0C387u;
unsigned long G_NnPaletteArgb_679bac = 0xFF1DBF3Bu;
unsigned long G_NnPaletteArgb_679ba8 = 0xFF1D1DBFu;
unsigned long G_NnPaletteArgb_679ba4 = 0xFF52EAF5u;
unsigned long G_NnPaletteArgb_679b9c = 0xFFC20088u;
unsigned long G_NnPaletteArgb_679b98 = 0xFFE04848u;
unsigned long G_NnPaletteArgb_679b94 = 0xFF808080u;
unsigned long G_NnPaletteArgb_679b90 = 0xFFC5FBA8u;
unsigned long G_NnPaletteArgb_679b88 = 0xFFFD7EFFu;
unsigned long G_NnPaletteArgb_679b84 = 0xFFFFFFFFu;
unsigned long G_NnPaletteArgb_679b80 = 0xFFFF5ACEu;

namespace
{
    // / the field -- both .bss (listing shows `??`), so zero until the first
    // reset seeds the phase to 0.3f * 0.5f.
    float GPulsePhase = 0.0f;
    unsigned char GPulseDir = 0;
    // bit 0 / the field: the routine's function-local static, .bss (zero) until
    // the first call's latch stores 0xFFB9B9B9 (0x00561dcb..0x00561de0). Every access is inside
    // , after the latch.
    bool GPulseMidInited = false;
    unsigned long GPulseMidArgb = 0; // lazy init

    unsigned char BlendByte(unsigned char From, unsigned char To, float T)
    {
        // Listing 00561e6d..: result = To + ftol((From - To) * T). FILD / FMUL ST1 run in x87
        // extended precision, where the product is exact; double keeps it exact too.
        int const Diff = static_cast<int>(From) - static_cast<int>(To);
        int const Delta = static_cast<int>(static_cast<double>(Diff) * static_cast<double>(T));
        return static_cast<unsigned char>(To + Delta);
    }

    unsigned long BlendArgb(unsigned long From, unsigned long To, float T)
    {
        unsigned char const Fr = static_cast<unsigned char>(From);
        unsigned char const Fg = static_cast<unsigned char>(From >> 8);
        unsigned char const Fb = static_cast<unsigned char>(From >> 16);
        unsigned char const Fa = static_cast<unsigned char>(From >> 24);
        unsigned char const Tr = static_cast<unsigned char>(To);
        unsigned char const Tg = static_cast<unsigned char>(To >> 8);
        unsigned char const Tb = static_cast<unsigned char>(To >> 16);
        unsigned char const Ta = static_cast<unsigned char>(To >> 24);
        unsigned long const R = BlendByte(Fr, Tr, T);
        unsigned long const G = BlendByte(Fg, Tg, T);
        unsigned long const B = BlendByte(Fb, Tb, T);
        unsigned long const A = BlendByte(Fa, Ta, T);
        return R | (G << 8) | (B << 16) | (A << 24);
    }
}

void NnUiPulseReset()
{
    // FLD 0.3f (the field); the field = 0; FMUL 0.5f (the field);
    // FSTP dword the field. Halving is exact, so the stored float is 0.3f * 0.5f.
    GPulseDir = 0;
    GPulsePhase = static_cast<float>(static_cast<double>(0.3f) * static_cast<double>(0.5f));
}

void NnUiPulseAdvance(double DeltaSeconds)
{
    // head, 0059e080..0059e0c9. FLD float phase / FADD double dt; the sum is
    // compared against 0.3f (the field) BEFORE it is rounded, then FST stores the float.
    // While it is still above 0.3f the loop keeps the unrounded value on the FPU stack,
    // subtracts 0.3f and flips the direction byte once per wrap (TEST CL,CL / SETZ CL); the
    // result is stored once at the end (FSTP 0059e0bb). TEST AH,0x41 / JNZ: "<= or unordered"
    // leaves the loop.
    double Phase = static_cast<double>(GPulsePhase) + DeltaSeconds;
    GPulsePhase = static_cast<float>(Phase);
    if (!(Phase > static_cast<double>(0.3f)))
        return;
    unsigned char Dir = GPulseDir;
    do
    {
        Phase -= static_cast<double>(0.3f);
        Dir = (Dir == 0) ? 1 : 0;
    } while (Phase > static_cast<double>(0.3f));
    GPulsePhase = static_cast<float>(Phase);
    GPulseDir = Dir;
}

unsigned long NnUiPulseIdleArgb()
{
    return G_NnPaletteArgb_679bb0;
}

float NnUiPulsePhaseNorm()
{
    // FLD the field / FDIV float 0.3f; every caller stores it as a float.
    return GPulsePhase / 0.3f;
}

unsigned char NnUiPulseDirection()
{
    // MOV AL, the field.
    return GPulseDir;
}

void NnUiPulseBlendArgb(unsigned long* OutArgb)
{
    // — listing 00561da0..00561f06.
    if (!GPulseMidInited)
    {
        // bit0 latch + the field = 0xFFB9B9B9 (atexit omitted).
        GPulseMidInited = true;
        GPulseMidArgb = 0xFFB9B9B9u;
    }

    // phase / 0.3
    float const Norm = GPulsePhase / 0.3f;
    unsigned long From = 0;
    unsigned long To = 0;
    float T = 0.0f;

    if (GPulseDir == 0)
    {
        if (Norm < 0.5f)
        {
            // A=b78, B=bb0, t=2*norm
            From = GPulseMidArgb;
            To = G_NnPaletteArgb_679bb0;
            T = Norm + Norm;
        }
        else
        {
            // A=ba4, B=b78, t=2*(norm-0.5)
            From = G_NnPaletteArgb_679ba4;
            To = GPulseMidArgb;
            T = (Norm - 0.5f) + (Norm - 0.5f);
        }
    }
    else
    {
        if (Norm < 0.5f)
        {
            // A=b78, B=ba4, t=2*norm
            From = GPulseMidArgb;
            To = G_NnPaletteArgb_679ba4;
            T = Norm + Norm;
        }
        else
        {
            // A=bb0, B=b78, t=2*(norm-0.5)
            From = G_NnPaletteArgb_679bb0;
            To = GPulseMidArgb;
            T = (Norm - 0.5f) + (Norm - 0.5f);
        }
    }

    *OutArgb = BlendArgb(From, To, T);
}

void NnUiPulseBlendFromArgb(unsigned long* OutArgb, unsigned long BaseArgb)
{
    // — listing 00561f20..005620f5.
    // phase / 0.3, stored to a float local (00561f29); the routine direction.
    float const Norm = GPulsePhase / 0.3f;
    unsigned char const Dir = GPulseDir;

    // per channel 0..2, Mid = Base - ftol((Hot - Base) * -0.5) (byte
    // arithmetic, the field = -0.5); Grey = (Mid0 + Mid1 + Mid2) / 3 (IMUL 0x55555556);
    // Dim = (Grey, Grey, Grey, 0xff).
    int MidSum = 0;
    for (int Channel = 0; Channel < 3; ++Channel)
    {
        int const Shift = Channel * 8;
        int const BaseByte = static_cast<int>((BaseArgb >> Shift) & 0xffu);
        int const HotByte = static_cast<int>((G_NnPaletteArgb_679ba4 >> Shift) & 0xffu);
        int const Half = static_cast<int>(static_cast<double>(HotByte - BaseByte) * -0.5);
        MidSum += static_cast<unsigned char>(BaseByte - Half);
    }
    unsigned long const Grey = static_cast<unsigned long>(MidSum / 3) & 0xffu;
    unsigned long const DimArgb = Grey | (Grey << 8) | (Grey << 16) | 0xff000000u;

    // FCOMP 0.5 (double the field), TEST AH,5 / JP: the first case is Norm < 0.5.
    unsigned long From = 0;
    unsigned long To = 0;
    float T = 0.0f;
    if (Dir == 0)
    {
        if (Norm < 0.5f)
        {
            // base = BaseArgb, other = Dim, t = 2 * norm.
            From = DimArgb;
            To = BaseArgb;
            T = Norm + Norm;
        }
        else
        {
            // base = Dim, other = amber the field, t = 2 * (norm - 0.5).
            From = G_NnPaletteArgb_679ba4;
            To = DimArgb;
            T = (Norm - 0.5f) + (Norm - 0.5f);
        }
    }
    else
    {
        if (Norm < 0.5f)
        {
            // base = amber, other = Dim, t = 2 * norm.
            From = DimArgb;
            To = G_NnPaletteArgb_679ba4;
            T = Norm + Norm;
        }
        else
        {
            // base = Dim, other = BaseArgb, t = 2 * (norm - 0.5).
            From = BaseArgb;
            To = DimArgb;
            T = (Norm - 0.5f) + (Norm - 0.5f);
        }
    }

    // out byte = base + ftol((other - base) * t), per channel.
    *OutArgb = BlendArgb(From, To, T);
}

void NnReputationColorArgb(unsigned long* OutArgb, float Attitude)
{
    // . FCOMP the field (-0.6f), TEST AH,0x41 + JP: red unless greater (or
    // unordered).
    if (Attitude <= -0.6f)
    {
        *OutArgb = G_NnPaletteArgb_679ba8;
        return;
    }
    // FCOMP the field (-0.2f), the same test: white unless greater.
    if (Attitude <= -0.2f)
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }
    // FCOMP the field (0.6f), TEST AH,1 + JNZ: green unless less (or unordered).
    if (Attitude >= 0.6f)
    {
        *OutArgb = G_NnPaletteArgb_679bac;
        return;
    }
    // FCOMP the field (0.2f), TEST AH,1 + JNZ: both ways white (0x0045a6bb / 0x0045a6c8).
    if (Attitude >= 0.2f)
    {
        *OutArgb = G_NnPaletteArgb_679b84;
        return;
    }
    *OutArgb = G_NnPaletteArgb_679b84;
}
