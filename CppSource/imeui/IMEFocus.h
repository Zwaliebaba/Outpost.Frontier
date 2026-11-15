// Author: Giperion.

#pragma once

// Redirect — public declaration lives in src/include/imeui.h (cross-DLL rule).
#include "imeui.h"

// Implementation TUs still need the private helpers that IMEFocus.cpp uses.
#include "IMEInputBox.h"
#include "CImeUiFont.h"
#include "ImeUiCore.h"
