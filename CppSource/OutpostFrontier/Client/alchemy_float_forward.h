// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

#include "alchemy.h"

// Private client forwarding boundary; the value stays in its original float object.
void ForwardAlchemyEffectFloat(IFxEffect* Effect,
                              GENRESULT (DACOM_API IFxEffect::*Method)(float),
                              const float& Value);
