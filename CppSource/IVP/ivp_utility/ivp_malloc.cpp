// Author: Giperion.

// IVP aligned allocator pair. Moved verbatim from src/Common/common_stubs.cpp
// (via common_stubs_a3.cpp) on 2026-10-14 (claude-code:cmn-a3); renamed from
// the raw extern "C" the routine / the routine. No logic changed.

#include "ivp_malloc.h"

#include <cstdint>
#include <cstdlib>

// (module storage-06343fea) -- generic aligned allocator, paired with the
// free half below. Mallocs `Size + Align + 8` bytes (the routine = malloc), rounds
// raw + Align + 7 down to a multiple of Align (`NEG ESI; AND EAX,ESI`, only correct when
// Align is a power of two -- true at every call site, e.g. 0627fc4b pushes 0x10), writes
// the 0x65981234 marker dword at the raw block's start (nothing reads it back) and keeps
// the raw malloc pointer at Aligned[-1] for ivp_free_aligned. baseline does not test the
// malloc result, so neither does this.
void* __cdecl ivp_malloc_aligned(unsigned Size, unsigned Align) // formerly extern "C" the routine
{
    unsigned char* Raw = (unsigned char*)malloc(Size + Align + 8);
    unsigned char* Aligned = (unsigned char*)(((uintptr_t)Raw + Align + 7) & (0 - (uintptr_t)Align));
    *(unsigned*)Raw = 0x65981234;
    ((void**)Aligned)[-1] = Raw;
    return Aligned;
}

// (module storage-06343ffe) -- the free half: recover the raw malloc pointer
// kept one dword before the aligned block and free it (the routine = free). No null
// test -- every caller tests first (e.g. PhyArch::Part::~Part 0627f9fc, Part::clear
// ).
void __cdecl ivp_free_aligned(void* AlignedPtr) // formerly extern "C" the routine
{
    void* RawPtr = ((void**)AlignedPtr)[-1];
    free(RawPtr);
}
