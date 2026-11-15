// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// Redirect — public declaration lives in src/include/imeui.h (cross-DLL rule).
#include "imeui.h"

// Implementation TUs still need the private helpers that IMEFocus.cpp uses.
#include "IMEInputBox.h"
#include "CImeUiFont.h"
#include "ImeUiCore.h"
