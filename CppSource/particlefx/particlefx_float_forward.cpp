// Author: Giperion.

#include "particlefx_effectobject.h"
#include "particlefx_appearances.h"
#include "particlefx_emitters.h"
#include "particlefx_common.h"

// Per-file SSE2 is used only for argument copies, without arithmetic or /RTC.
// This translation unit has per-file optimization. baseline forwards
// these float arguments as DWORDs; arithmetic and primitive field stores stay in
// their /fp:precise translation units.
#pragma float_control(precise, off, push)
namespace ParticleFxImpl
{
    // forwards the original Dt word and reloads the library and handle
    // after AdvanceTime before fetching the current frame.
    void FFxTextureWrapper::Advance(const float& Dt)
    {
        if (Handle == -1)
            return;
        static_cast<ITextureLibrary2*>(G_TextureLibrary)->AdvanceTime(Handle, Dt);
        static_cast<ITextureLibrary2*>(G_TextureLibrary)->GetFrame(
            Handle, -1, reinterpret_cast<unsigned long*>(Frame));
    }

    __declspec(noinline) double ForwardAppearanceSingleEval(
        INestedSingleRamp* Ramp, const float& OuterTime, const float& InnerTime)
    {
        return Ramp->Eval(OuterTime, InnerTime);
    }

    __declspec(noinline) unsigned long ForwardAppearanceColorEval(
        IRampColorRamp* Ramp, const float& OuterTime, const float& InnerTime)
    {
        return Ramp->EvalPacked(OuterTime, InnerTime);
    }

    __declspec(noinline) float* ForwardAppearanceRgbEval(
        IRampColorRamp* Ramp, float* Out, const float& OuterTime, const float& InnerTime)
    {
        return Ramp->EvalRgb(Out, OuterTime, InnerTime);
    }

    __declspec(noinline) void ForwardAppearanceSinCos(const float& Angle, float* Output)
    {
        G_ParticleFxSinCos(Angle, Output);
    }

    __declspec(noinline) GENRESULT ForwardEmitterPad46(
        IGenericFx* Appearance, const float* Six, const float& AgeOffset, const float& LifeSpan)
    {
        return Appearance->Pad46(Six, AgeOffset, LifeSpan);
    }

    __declspec(noinline) GENRESULT ForwardEmitterPad47(
        IGenericFx* Appearance, const float* Six, const float& AgeOffset, const float& LifeSpan)
    {
        return Appearance->Pad47(Six, AgeOffset, LifeSpan);
    }

    // node+0x64, with the original argument.
    __declspec(noinline) GENRESULT NEURONCOMPONENTS_API FFxEffectObject::Slot5(float Age)
    {
        return WalkNodes(&IGenericFx::SetNodeAge, Age);
    }

    // node+0x5c, with the original argument.
    __declspec(noinline) GENRESULT NEURONCOMPONENTS_API FFxEffectObject::Slot6(float OuterTime)
    {
        return WalkNodes(&IGenericFx::SetNodeOuterTime, OuterTime);
    }

    // node+0x88, with the original argument.
    __declspec(noinline) GENRESULT NEURONCOMPONENTS_API FFxEffectObject::Slot14(float LodInput)
    {
        return WalkNodes(&IGenericFx::SetNodeLodInput, LodInput);
    }

    __declspec(noinline) GENRESULT FFxEffectObject::WalkNodes(
        GENRESULT (NEURONCOMPONENTS_API IGenericFx::*Method)(float), float Value)
    {
        const unsigned long Count = NodeCount_;
        FFxEffectNodeRec* Record = Nodes_;
        for (unsigned long Index = 0; Index < Count; ++Index, ++Record)
        {
            (static_cast<IGenericFx*>(Record->Node)->*Method)(Value);
        }
        return GR_OK;
    }

    // /the routine compare a field through x87, then forward its raw bits.
    __declspec(noinline) void FxParticleAppearance::ForwardEffectFloat(
        IFxEffect* Effect, GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float),
        const float& Value)
    {
        (Effect->*Method)(Value);
    }

    // /the routine first store through x87 in their original TU,
    // then pass the original parameter by reference to this pure forwarding walk.
    __declspec(noinline) void FxParticleAppearance::ForwardChildFloat(
        GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float), const float& Value)
    {
        FPooledParticle* Record = InstanceListHead_;
        while (Record != nullptr)
        {
            FParticleAppRecord* const R = reinterpret_cast<FParticleAppRecord*>(Record);
            if (R->LifeEffect != nullptr)
                (R->LifeEffect->*Method)(Value);
            if (R->DeathEffect != nullptr)
                (R->DeathEffect->*Method)(Value);
            Record = Record->Next;
        }
    }
}
#pragma float_control(pop)
