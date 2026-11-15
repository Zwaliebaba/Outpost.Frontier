// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// Aggregate that loads and initializes the components in the System profile section.
struct ISystemContainer : IAggregateComponent
{
    // +0x10 is also ISystemComponent::Shutdown on the shared vtable (src/system/system.cpp).
    virtual GENRESULT NEURONCOMPONENTS_API Shutdown() = 0;                  // +0x10
    virtual GENRESULT NEURONCOMPONENTS_API LoadSystemComponents() = 0;      // +0x14
    virtual GENRESULT NEURONCOMPONENTS_API UnloadSystemComponents() = 0;    // +0x18
    virtual GENRESULT NEURONCOMPONENTS_API AddComponent(AGGDESC* Desc) = 0; // +0x1C
};
