// Author: Giperion.

#pragma once

#include "particlefx_types.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.

// FALMaterial: a previously-undocumented component (sonnet:particlefx-wave73-lane2,
// 2026-09-11) discovered by a broad-range survey of baseline's 0x062031b0-
// byte range. Full baseline evidence:

//   - the branch returns the literal string "ALMaterial" (a GetClassName-
//     style getter). The "ALMaterial" string itself has exactly one XREF in
//     the whole module (that getter) -- it is NOT one of the 33 doc-
//     documented RegisterComponent names, so this component is never handed
//     to ICOManager::RegisterComponent the way the rest of particlefx's classes
//     are.
//   - the branch is a real byte-verified QueryInterface: a generic 2-entry
//     IID table (string-ptr + offset pairs at 0x0624b3ac/0x0624b3b4) that
//     recognizes "1.11_IBatchedMaterial" (offset 4 -> the facet below) and
//     "1.11_IMaterial" (offset 0 -> the object itself), matching the
//     standard MSVC multiple-inheritance QI-table codegen (adjustor thunks
// /50/60 do `this -= 4` then tail-jump into the very same
//     QueryInterface/AddRef/Release used for offset 0).
//   - the branch/the branch are a completely ordinary AddRef/Release
//     pair: refcount lives at this+8, Release frees via the routine (this
//     port's usual `delete this`) once it hits 0, with the same
//     re-entrancy guard (refcount re-armed to 1 immediately before the free
//     call) already fixed across a dozen other particlefx classes this
//     marathon.
//   - Both "1.11_IMaterial" and "1.11_IBatchedMaterial" are real, widely
//     used NEURONCOMPONENTS interface names -- surfacematerials.dll implements the exact
//     same two IID strings dozens of times over for its own FL rendering
//     materials (src/surfacematerials/surfacematerials.h, render/IMaterial.h). That
//     header is explicitly a surfacematerials-local, 29-slot shape though (see
//     its own top-of-file comment: "this fix is local to surfacematerials.dll
//     and cannot affect Shading/RendComp's own, differently-shaped IMaterial
//     interfaces"). ParticleFx's own vtable1 here is a *smaller*, 21-slot
//     shape (QI/AddRef/Release + 17 unnamed slots + GetClassName) and
//     vtable2 a 9-slot shape (QI/AddRef/Release + 6 unnamed slots) -- a
//     third, particlefx-local shape of the same conceptual interface. No
//     semantic slot names can be derived without a real header for this
//     exact shape (not present anywhere in NeuronRuntime.h/neuroncomponents.h/render/
//     IMaterial.h), so every unnamed slot below keeps its baseline byte
//     behavior with a plain SlotN name, matching this port's existing
//     convention for unresolved NEURONCOMPONENTS interfaces (see e.g. IFxRuntime).

// baseline wires this object up unconditionally at DLL load: the branch is
// one of ~20 entries in the CRT's C++ static-object-constructor table
// (the field..0625504c, iterated by MSVCRT's `_initterm` from the DLL
// entry point) -- NOT a lazily-triggered singleton the way a prior pass's
// notes described it. It unconditionally stores this object's two vtables
// into the two static fields below and leaves refcount at 1. No other code
// anywhere in particlefx.dll ever reads those two vtable fields again (XREF-
// confirmed -- each has exactly one XREF, the constructor's own write), and
// "ALMaterial" is absent from the 33 documented RegisterComponent names, so
// the real call site that ever queries this instance for "1.11_IMaterial"/
// "1.11_IBatchedMaterial" was not found this wave either. Ported here as a
// real, byte-faithful global singleton (matching baseline's unconditional
// construction) so a future wave that does locate the consumer (most likely
// something in OutpostFrontier.exe's renderer or the ParticleFx.useMaterialBatcher
// material-batcher path -- G_Defaults.bUseMaterialBatcher in
// particlefx_update.h/.cpp) has a working target to wire in immediately,
// rather than leaving the gap undiscoverable.
namespace ParticleFxImpl
{
    class FALMaterial;

    // baseline vtable2 (the branch, "1.11_IBatchedMaterial"): a facet
    // embedded at object+4, exactly the FFxRuntimeAggregate /
    // IAggregateComponent composition pattern already used elsewhere in
    // this port (see particlefx_fxruntime.h) -- QueryInterface/AddRef/Release
    // all delegate to the outer object, matching baseline's adjustor thunks
    // (the branch/50/60: `this -= 4` then jump into the outer's own
    // QI/AddRef/Release).
    struct IALBatchedMaterial : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* A) = 0;                    // +0x0C the branch RET 8, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot4(void* A) = 0;                    // +0x10 the branch RET 8, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot5(unsigned long* Out) = 0;         // +0x14 the branch RET 8, *Out=1, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot6(void* A) = 0;                    // +0x18 the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot7(void* A) = 0;                    // +0x1C the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* A, void* B, void* C, void* D,
                                          void* E, void* F, void* G) = 0;  // +0x20 the branch RET 0x20, returns 0
    };

    class FALBatchedMaterialFacet final : public IALBatchedMaterial
    {
    public:
        explicit FALBatchedMaterialFacet(FALMaterial* Outer) : Outer_(Outer) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;

        GENRESULT NEURONCOMPONENTS_API Slot3(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot4(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot5(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API Slot6(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot7(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot8(void* A, void* B, void* C, void* D, void* E, void* F,
                                  void* G) override;

    private:
        FALMaterial* Outer_;
    };

    // baseline vtable1 (the branch, "1.11_IMaterial").
    struct IALMaterial : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Slot3(void* A) = 0;                    // +0x0C the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot4(void* A) = 0;                    // +0x10 the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot5(void* A) = 0;                    // +0x14 the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot6(void* A, void* B) = 0;           // +0x18 the branch RET 0xC, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot7(void* A) = 0;                    // +0x1C the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot8(void* A) = 0;                    // +0x20 the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot9(void* A) = 0;                    // +0x24 the branch RET 8, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot10(void* A, void* B, void* C, void* D,
                                           void* E, void* F, void* G) = 0; // +0x28 the branch RET 0x20, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot11(void* A) = 0;                   // +0x2C the branch RET 8, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot12(void* A, void* B) = 0;          // +0x30 the branch RET 0xC, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot13(void* A, void* B) = 0;          // +0x34 the branch RET 0xC, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot14(unsigned long* Out) = 0;        // +0x38 the branch RET 8, *Out=1, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot15(unsigned long* Out) = 0;        // +0x3C the branch RET 8, *Out=2, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot16(unsigned long* Out) = 0;        // +0x40 the branch RET 8, *Out=0, returns 0
        virtual GENRESULT NEURONCOMPONENTS_API Slot17(void* A, void* B) = 0;          // +0x44 the branch RET 0xC, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot18(void* A, void* B) = 0;          // +0x48 the branch RET 0xC, returns -1
        virtual GENRESULT NEURONCOMPONENTS_API Slot19(void* A, void* B) = 0;          // +0x4C the branch RET 0xC, returns -1
        // +0x50 the branch -> "ALMaterial": `MOV EAX,str; RET` -- no stack argument popped,
        // so not a NEURONCOMPONENTS_API (stdcall, `this` on the stack, RET 4) slot. __thiscall, as the
        // shared IMaterial::ClassName (render/IMaterial.h) declares the same slot.
        virtual const char* __thiscall GetClassName() = 0;
    };

    class FALMaterial final : public IALMaterial
    {
    public:
        FALMaterial() : Facet_(this), RefCount_(1) {}

        FALBatchedMaterialFacet* Facet() { return &Facet_; }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        GENRESULT NEURONCOMPONENTS_API Slot3(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot4(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot5(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot6(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot7(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot8(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot9(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot10(void* A, void* B, void* C, void* D, void* E, void* F,
                                   void* G) override;
        GENRESULT NEURONCOMPONENTS_API Slot11(void* A) override;
        GENRESULT NEURONCOMPONENTS_API Slot12(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot13(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot14(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API Slot15(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API Slot16(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API Slot17(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot18(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot19(void* A, void* B) override;
        const char* __thiscall GetClassName() override;

        // / the branch call the same render/texture-stage
        // state block as BeginDraw (the routine), then return success.
        static GENRESULT ApplyDefaultRenderPipelineState();

    private:
        // Both interface identities share RefCount_; the modern facet keeps
        // its outer pointer instead of requiring the baseline binary layout.
        FALBatchedMaterialFacet Facet_;
        unsigned long RefCount_;
    };

    // The one global instance baseline's CRT static-initializer table
    // (the branch) unconditionally constructs at DLL load. Never queried
    // by any other code in this module (byte-verified: the field/24/28
    // each carry exactly one XREF, the constructor's own write) -- kept
    // here, constructed the same way, so a future wave that finds the real
    // consumer can wire it in directly instead of re-deriving this class
    // from scratch.
    extern FALMaterial G_DefaultALMaterial;

} // namespace ParticleFxImpl
