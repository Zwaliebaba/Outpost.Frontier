// Author: Giperion.

// IVP_U_BigVector growth helpers. Moved from src/Common/common_stubs.cpp (via
// common_stubs_a3.cpp) on 2026-10-14 (claude-code:cmn-a3); renamed from the
// raw extern "C" the routine / the routine and typed on the vector header
// (the former int* view: [0] capacity, [1] count, [2] elems). Same
// statements, same order.

#include "ivp_u_bigvector.h"

#include "../ivp_physics/ivp_surface_manager.h"

#include <cstdlib>

static_assert(sizeof(IVP_SurfaceManager_OutList) == 0xc, "the inline buffer starts right after the 12-byte header");

// Commond3e0-0635d43d (XREF 0634aadb, 0634ab2e, 0634e357,
// , 0635abd2, 06362445, 0636247c), thiscall on the vector. Mallocs a
// new buffer sized OldCapacity*8+8 bytes (`LEA ECX,[EAX*8+8]`), stores the
// new capacity OldCap*2+1, copies the Count live elements across, frees the
// old buffer unless it is the inline buffer immediately following the header
// (`LEA ECX,[ESI+0xc]; CMP`) or null -- nulling the field on that path --
// then installs the new buffer.
void __fastcall IVP_U_BigVector_Grow(IVP_SurfaceManager_OutList* Vector) // formerly extern "C" the routine
{
    void* newBuffer = malloc(static_cast<size_t>(Vector->Capacity) * 8 + 8);
    Vector->Capacity = Vector->Capacity * 2 + 1;
    int count = Vector->Count;
    void** newElems = (void**)newBuffer;
    void** oldElems = Vector->Elems;
    for (int i = 0; i < count; ++i)
    {
        newElems[i] = oldElems[i];
    }
    void** oldBuffer = Vector->Elems;
    if (oldBuffer != (void**)(Vector + 1) && oldBuffer != nullptr)
    {
        free(oldBuffer);
        Vector->Elems = nullptr;
    }
    Vector->Elems = newElems;
}

// Commonc (XREF 0634e223): `MOV EAX,[ECX+4];
// CMP EAX,[ECX]; JL ret; JMP the routine` -- grow only when Count >=
// Capacity, otherwise there is room and this is a no-op.
void __fastcall IVP_U_BigVector_EnsureRoom(IVP_SurfaceManager_OutList* Vector) // formerly extern "C" the routine
{
    if (Vector->Count >= Vector->Capacity)
    {
        IVP_U_BigVector_Grow(Vector);
    }
}
