// Author: Giperion.

#pragma once

// Redirect -- IMEInputBox is declared once, in src/include/imeui.h (cross-DLL rule:
// OutpostFrontier.exe derives FLImeInputBox from it). The implementation TU also needs CImeUiFont.
#include "imeui.h"
#include "CImeUiFont.h"
