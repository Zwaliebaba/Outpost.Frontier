// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

// Retail DAT_0066dc60 / DAT_0066fc60 / DAT_006119f8 -- see nn_text_scratch.h.

#include "nn_text_scratch.h"

wchar_t G_NnTextScratchA[0x1000];
wchar_t G_NnTextScratchB[0x1000];
int G_NnTextScratchCapacity = 0x1000;
