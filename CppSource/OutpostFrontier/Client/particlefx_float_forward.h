// Author: Giperion.

#pragma once

#include "particlefx.h"

// Private client forwarding boundary; the value stays in its original float object.
void ForwardParticleFxEffectFloat(IFxEffect* Effect,
                              GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float),
                              const float& Value);
