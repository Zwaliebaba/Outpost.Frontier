// Author: Giperion.

#pragma once

#include "common_exports.h"

// DataConfig -- defined in common_dataconfig.cpp.
namespace Archetype
{
    struct ICliObjFactory;
}

// Common DataConfig::startup @ 0xa8e60 / shutdown @ 0xa9850
// ?startup@DataConfig@@YAHPBDPAUICliObjFactory@Archetype@@PAUProgressCB@1@@Z
// ?shutdown@DataConfig@@YAXXZ
// ProgressCB is nested in DataConfig (mangle @1). ICliObjFactory / ICliObj nested in Archetype.
namespace DataConfig
{
    // RTTI .?AUProgressCB@DataConfig@@. startup (Common.dll 0x06308f38..0x06308f52) calls
    // slot 0, thiscall, with each [Data] value string when the pointer is non-null; a nonzero
    // EAX aborts the scan and startup returns 1. The only baseline implementation is
    // OutpostFrontier.exe's DataConfigPump (vftable 0x005e6ab8 = {the branch}, one slot).
    struct ProgressCB
    {
        virtual int Slot00(char const* ValueString) = 0; // +0x00
    };

    COMMON_API int __cdecl startup(char const* IniPath, Archetype::ICliObjFactory* Factory,
                        ProgressCB* Progress);
    COMMON_API void __cdecl shutdown();
}
