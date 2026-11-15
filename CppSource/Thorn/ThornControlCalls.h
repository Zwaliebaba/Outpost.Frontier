// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

#include "thorn_controls.h"
#include "thorn_iface.h"

// 06f493a1..06f493ac: publish the captured four-word packet through slot 3.
void ThornSetReverbProperties(IReverbControl* Control,
                              const FThornReverb4* Properties);

// 06f49b45..06f49b4d: raw stored-height publication through slot 5.
void ThornSetFloorHeight(ICompoundControl* Control, const float* Height);
