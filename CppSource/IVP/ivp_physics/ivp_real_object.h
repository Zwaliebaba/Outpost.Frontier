// Author: Giperion.

#pragma once

// The IVP object hierarchy is public Common.dll ABI since 2026-10-14 (Server.dll and the
// client read IVP_Real_Object fields): its single declaration is src/include/common_ivp_real_object.h.
// This path stays only so the existing src/IVP and src/Common includers keep resolving.
#include "../../include/common_ivp_real_object.h"
