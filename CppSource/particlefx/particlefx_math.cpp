// Author: Giperion.

#include "particlefx_math.h"

#include <cmath>
#include <float.h>

// These private numeric kernels use /O2 /Oi /fp:fast in every configuration.
// The actual MSVC output must inherit the caller's x87 control word: the three
// math instructions, FILD qword, and one FSTP dword to the final destination.
// The CRT math alternatives change precision or select SSE; unoptimized float
// assignments can reload an intermediate float and signal a denormal operand.
#pragma intrinsic(sin, cos, sqrt)

namespace ParticleFxImpl
{
    __declspec(noinline) double ComputeParticleFxFpuSine(const double& Angle)
    {
        return std::sin(Angle);
    }

    __declspec(noinline) double ComputeParticleFxFpuCosine(const double& Angle)
    {
        return std::cos(Angle);
    }

    // publish the raw zero after FCOS, before a waiting
    // store of its result can deliver a pending x87 exception.
    __declspec(noinline) double ComputeParticleFxFpuCosineAndClear(const double& Angle, float& ZeroOut)
    {
        const double Result = std::cos(Angle);
        volatile unsigned long* const ZeroBits = reinterpret_cast<volatile unsigned long*>(&ZeroOut);
        *ZeroBits = 0;
        return Result;
    }

    __declspec(noinline) double ComputeParticleFxFpuSquareRoot(const double& Value)
    {
        return std::sqrt(Value);
    }

    __declspec(noinline) double ComputeParticleFxFpuWideInteger(const std::int64_t& Value)
    {
        return static_cast<double>(Value);
    }

    __declspec(noinline) void StoreParticleFxFpuFloat(float& Out, const double& Value)
    {
        Out = Value;
    }

    // baseline _ftol converts to int64 with x87 RC_CHOP and restores RC only after
    // successful conversion. A null SSE output leaves MXCSR untouched. Native
    // __ftol2 alone inherits the old RC on its unmasked exception path.
    __declspec(noinline) std::int64_t ConvertParticleFxFpuInteger(const double& Value)
    {
        unsigned int SavedControl;
        unsigned int CurrentControl;
        __control87_2(0, 0, &SavedControl, nullptr);
        __control87_2(_RC_CHOP, _MCW_RC, &CurrentControl, nullptr);
        const std::int64_t Converted = static_cast<std::int64_t>(Value);
        __control87_2(SavedControl, _MCW_RC, &CurrentControl, nullptr);
        return Converted;
    }
}
