// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Redirect -- IMEInputBox is declared once, in src/include/imeui.h (cross-DLL rule:
// OutpostFrontier.exe derives FLImeInputBox from it). The implementation TU also needs CImeUiFont.
#include "imeui.h"
#include "CImeUiFont.h"
