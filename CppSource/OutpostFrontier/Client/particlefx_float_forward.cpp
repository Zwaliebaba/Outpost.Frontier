// Author: Giperion.

#include "particlefx_float_forward.h"
#include "gf_object_ship.h"

// Per-file SSE2 is used only for argument copies, without arithmetic or /RTC.
// Per-file optimization permits native typed calls to copy the
// original argument DWORD. Setters and ordered comparisons stay /fp:precise.
#pragma float_control(precise, off, push)
__declspec(noinline) void ForwardParticleFxEffectFloat(
    IFxEffect* Effect, GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float), const float& Value)
{
    (Effect->*Method)(Value);
}

// each live engine effect takes the original argument through slot 6.
__declspec(noinline) void FObjectShip::SetEngineSparam(float Sparam)
{
    int Index = 0;
    for (Index = 0; Index < Visuals.Count; ++Index)
    {
        IFxEffect* const Effect = EngineFx[Index].Particles.Effect;
        if (Effect != nullptr)
            Effect->Slot6(Sparam);
    }
}
#pragma float_control(pop)
