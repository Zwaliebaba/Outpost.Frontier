// Author: Giperion.

#pragma once

#include <cstdint>

// The IVP library's CORE: the ten-byte `MOV dword ptr [0x00000000],0`
// (c7 05 00 00 00 00 00 00 00 00) that baseline compiles into paths which must
// never run. It is a real store to address zero, so it traps in every build;
// an assert would vanish under NDEBUG. Execution continues after it only in a
// debugger that skips the faulting instruction, exactly as in baseline.
inline void IVP_CoreTrap()
{
    volatile int32_t* Trap = nullptr;
    *Trap = 0;
}
