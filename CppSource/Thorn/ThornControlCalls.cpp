// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#include "ThornControlCalls.h"

void ThornSetReverbProperties(IReverbControl* Control,
                              const FThornReverb4* Properties)
{
    Control->SetReverb(Properties->Environment, Properties->Volume,
                       Properties->Decay, Properties->Damping);
}

void ThornSetFloorHeight(ICompoundControl* Control, const float* Height)
{
    Control->SetFloorHeight(*Height);
}
