// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

// The IVP object hierarchy is public Common.dll ABI since 2026-10-14 (Server.dll and the
// client read IVP_Real_Object fields): its single declaration is src/include/common_ivp_real_object.h.
// This path stays only so the existing src/IVP and src/Common includers keep resolving.
#include "../../include/common_ivp_real_object.h"
