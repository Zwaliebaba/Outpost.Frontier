// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "engine/sceneruntime.h"

// Instance values, lighting, override materials and appearance drawing.
struct IFLAppearance : INeuronComponent
{
    virtual GENRESULT NEURONCOMPONENTS_API SetInstanceValue(int Id, void* Value) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetInstanceValue(int Id, void** Out) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API RemoveInstanceValue(int Key) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API SetDirectionalLight(const float* First, const float* Second) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API SetDirectionalLightEnabled(unsigned char Flag) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API GetOverrideMaterial(unsigned long Index, void** Out) = 0;
    // +0x24/+0x28: Camera and engine instance passed to Renderer2 with the
    // Backdrop (066fe270) / Starsphere (066fe570) override material.
    virtual GENRESULT NEURONCOMPONENTS_API DrawBackdrop(void* A, void* Extra) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API DrawStarsphere(void* A, void* Extra) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API DrawFadingShadows(void* A, void* B, void* C, void* D) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API DrawFixedShadows(void* A, void* B, void* C, void* D) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API CountVisibleGeometry(void* A, void* B, void* C) = 0;
};

struct FAggDesc18
{
    unsigned long Size;
    const char* NameAt4;
    void* OuterAt8;
    void** InnerOutAtC;
    const char* ValueAt10;
    INeuronComponent* SystemAt14;
};
