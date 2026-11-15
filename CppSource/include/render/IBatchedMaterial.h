// Author: Giperion.

#pragma once

#include "neuroncomponents.h"

// IBatchedMaterial ("1.11_IBatchedMaterial") -- the batched face a material can be asked for.

// It lives in its own header rather than inside render/shading.h because surfacematerials queries
// for it too: every Candidate dance in surfacematerials/MaterialImpl.cpp QIs the material out of
// Context+0x1C for this IID and, on success, binds/tears down/draws through slots 6/7/8
// instead of the material's own 8/9/10. Those sites used to call the slots through raw
// vtable indices, which is how they came to pass one argument too few.

struct IBatchedMaterial : INeuronComponent
{
    // +0x0C. the branch RET 8: face at object byte 4, inner IMaterial at byte 0x10.
    virtual GENRESULT NEURONCOMPONENTS_API CreateInnerMaterial(void* Name) = 0;
    virtual GENRESULT NEURONCOMPONENTS_API SetBatchBin(void* Value) = 0; // +0x10 [this+4]=arg (object+8)
    virtual GENRESULT NEURONCOMPONENTS_API GetBatchBin(void** Out) = 0;  // +0x14 *out=[this+4]
    virtual GENRESULT NEURONCOMPONENTS_API Bind(void* Context) = 0; // +0x18: JMP inner slot 8 (bind)
    virtual GENRESULT NEURONCOMPONENTS_API Teardown(void* Context) = 0; // +0x1C: JMP inner slot 9 (teardown)
    virtual GENRESULT NEURONCOMPONENTS_API Submit(void* Context, unsigned long PrimType,
                                      unsigned long Start, unsigned long Count,
                                      unsigned long IbStart, unsigned long IndexSpan,
                                      unsigned long Flags) = 0; // +0x20: JMP inner slot 10 (submit)
};
