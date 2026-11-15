// Author: Giperion.

#include "particlefx_material.h"

#include "particlefx_update.h"
#include "particlefx_common.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// Out-of-line method bodies for FALMaterial / FALBatchedMaterialFacet.
// See particlefx_material.h for the full byte-verified baseline trace.
namespace ParticleFxImpl
{
    FALMaterial G_DefaultALMaterial;

    // ---- FALBatchedMaterialFacet ("1.11_IBatchedMaterial", object+4) -----

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::QueryInterface(const char* Iid, void** Out)
    {
        // `this -= 4` then tail-jump into the branch, i.e.
        // the outer object's own QueryInterface handles both IIDs.
        return Outer_->QueryInterface(Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API FALBatchedMaterialFacet::AddRef()
    {
        // `this -= 4` then tail-jump into the branch.
        return Outer_->AddRef();
    }

    unsigned long NEURONCOMPONENTS_API FALBatchedMaterialFacet::Release()
    {
        // `this -= 4` then tail-jump into the branch.
        return Outer_->Release();
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot3(void* /*A*/)
    {
        // OR EAX,0xFFFFFFFF; RET 8.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot4(void* /*A*/)
    {
        // OR EAX,0xFFFFFFFF; RET 8.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot5(unsigned long* Out)
    {
        // MOV EAX,[ESP+8]; MOV dword ptr [EAX],1; XOR EAX,EAX; RET 8.
        *Out = 1;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot6(void* /*A*/)
    {
        // CALL the routine (see
        // FALMaterial::ApplyDefaultRenderPipelineState); XOR EAX,EAX; RET 8.
        return FALMaterial::ApplyDefaultRenderPipelineState();
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot7(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALBatchedMaterialFacet::Slot8(void* /*A*/, void* /*B*/, void* /*C*/,
                                                       void* /*D*/, void* /*E*/, void* /*F*/,
                                                       void* /*G*/)
    {
        // XOR EAX,EAX; RET 0x20 (7 args).
        return GR_OK;
    }

    // ---- FALMaterial ("1.11_IMaterial", object+0) -------------------------

    GENRESULT NEURONCOMPONENTS_API FALMaterial::QueryInterface(const char* Iid, void** Out)
    {
        // baseline's real QI is a generic byte-compared 2-entry
        // IID table ("1.11_IBatchedMaterial" -> offset 4, "1.11_IMaterial"
        // -> offset 0); expressed here the same way this port expresses
        // every other multi-IID QueryInterface (see FFxRuntime::
        // QueryInterface). Falls through to GR_INTERFACE_UNSUPPORTED
        // (-3, *Out=0) exactly like baseline's table-exhausted path.
        if (IidEqual(Iid, Iids::BatchedMaterial))
        {
            Facet_.AddRef();
            *Out = static_cast<IALBatchedMaterial*>(&Facet_);
            return GR_OK;
        }
        if (IidEqual(Iid, Iids::Material))
        {
            AddRef();
            *Out = static_cast<IALMaterial*>(this);
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FALMaterial::Release()
    {
        // guards the decrement against underflow, then on
        // hitting 0 re-arms the refcount to 1 (baseline's re-entrancy guard,
        // already fixed the same way across a dozen other particlefx classes)
        // before freeing via the routine (this port's `delete this`).
        if (RefCount_ != 0)
            --RefCount_;
        const unsigned long Next = RefCount_;
        if (Next == 0)
        {
            RefCount_ = 1;
            delete this;
            return 0;
        }
        return Next;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot3(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot4(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot5(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot6(void* /*A*/, void* /*B*/)
    {
        // XOR EAX,EAX; RET 0xC.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot7(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot8(void* /*A*/)
    {
        // CALL the routine; XOR EAX,EAX; RET 8.
        return ApplyDefaultRenderPipelineState();
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot9(void* /*A*/)
    {
        // XOR EAX,EAX; RET 8.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot10(void* /*A*/, void* /*B*/, void* /*C*/, void* /*D*/,
                                            void* /*E*/, void* /*F*/, void* /*G*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0x20 (7 args).
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot11(void* /*A*/)
    {
        // OR EAX,0xFFFFFFFF; RET 8.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot12(void* /*A*/, void* /*B*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0xC.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot13(void* /*A*/, void* /*B*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0xC.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot14(unsigned long* Out)
    {
        // MOV EAX,[ESP+8]; MOV dword ptr [EAX],1; XOR EAX,EAX; RET 8.
        *Out = 1;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot15(unsigned long* Out)
    {
        // MOV EAX,[ESP+8]; MOV dword ptr [EAX],2; XOR EAX,EAX; RET 8.
        *Out = 2;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot16(unsigned long* Out)
    {
        // MOV EAX,[ESP+8]; MOV dword ptr [EAX],0; XOR EAX,EAX; RET 8.
        *Out = 0;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot17(void* /*A*/, void* /*B*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0xC.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot18(void* /*A*/, void* /*B*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0xC.
        return -1;
    }

    GENRESULT NEURONCOMPONENTS_API FALMaterial::Slot19(void* /*A*/, void* /*B*/)
    {
        // OR EAX,0xFFFFFFFF; RET 0xC.
        return -1;
    }

    const char* __thiscall FALMaterial::GetClassName()
    {
        // returns the class string without accessing the receiver.
        return "ALMaterial";
    }

    GENRESULT FALMaterial::ApplyDefaultRenderPipelineState()
    {
        // == ParticleFxImpl::ApplyParticleDrawRenderState, already
        // ported (particlefx_update.cpp) against G_RenderPipeline (this
        // port's name for the field, see particlefx_common.h/.cpp) -- the
        // same global FParticleFx::BeginDraw drives at its own call site
        // (0x06201b5e). Both baseline call sites of THIS slot (the branch,
        // ) discard the return value and unconditionally return
        // 0 regardless of what the callee did, so this wrapper does the
        // same after invoking the real routine.
        ApplyParticleDrawRenderState();
        return GR_OK;
    }

} // namespace ParticleFxImpl
