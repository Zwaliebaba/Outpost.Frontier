// OutpostFrontier is a reimplementation of the original 2003 game client and engine,
// faithful to the behaviour of the original game.
// This file was reconstructed by reverse engineering the original game's version 1.1 binaries,
// developed by Digital Anvil.
// Author: Giperion.

#pragma once

#include "common_exports.h"

// ICliObjFactory — the cross-DLL contract between OutpostFrontier.exe and Common.dll.
// Common.dll's DataConfig::startup / Archetype::Load* receive a factory pointer and call
// vtable[0](TypeId, ICliObj** Out) for every archetype that gets a client companion; the
// server passes null. The factory itself, its ICliObj companions and the MaterialLib cache
// are OutpostFrontier.exe code in retail (DAT_006164a0 / FUN_0040ca50, Client\cmn\MaterialLib.cpp)
// and live in src/OutpostFrontier/Client/client_cliobj.{h,cpp}; Common.dll exports none of them.

#include "common_archetype.h"

namespace Archetype
{
    // Retail: thiscall vtable[0](TypeId, ICliObj** Out). Server passes null.
    struct ICliObjFactory
    {
        virtual void Create(int TypeId, ICliObj** Out) = 0;
    };
}
