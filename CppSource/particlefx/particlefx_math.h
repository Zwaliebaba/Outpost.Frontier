// Author: Giperion.

#pragma once

#include <cstdint>

namespace ParticleFxImpl
{
    double ComputeParticleFxFpuSine(const double& Angle);
    double ComputeParticleFxFpuCosine(const double& Angle);
    double ComputeParticleFxFpuCosineAndClear(const double& Angle, float& ZeroOut);
    double ComputeParticleFxFpuSquareRoot(const double& Value);
    double ComputeParticleFxFpuWideInteger(const std::int64_t& Value);
    void StoreParticleFxFpuFloat(float& Out, const double& Value);
    std::int64_t ConvertParticleFxFpuInteger(const double& Value);
}
