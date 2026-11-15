// Author: Giperion.

#include "particlefx_fxruntime.h"

#include "particlefx_common.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// Out-of-line method bodies for FFxRuntime / FFxRuntimeAggregate.
namespace ParticleFxImpl
{
    GENRESULT NEURONCOMPONENTS_API FFxRuntimeAggregate::QueryInterface(const char* Iid, void** Out)
    {
        return Outer_->QueryInterface(Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API FFxRuntimeAggregate::AddRef() { return Outer_->AddRef(); }

    unsigned long NEURONCOMPONENTS_API FFxRuntimeAggregate::Release() { return Outer_->Release(); }

    GENRESULT NEURONCOMPONENTS_API FFxRuntime::FInnerUnknown::QueryInterface(const char* Iid, void** Out)
    {
        // only IFxRuntime and IAggregateComponent are listed.
        if (IidEqual(Iid, Iids::FxRuntime))
        {
            IFxRuntime* const Interface = static_cast<IFxRuntime*>(Owner_);
            Interface->AddRef();
            *Out = Interface;
            return GR_OK;
        }
        if (IidEqual(Iid, Iids::AggregateComponent))
        {
            IAggregateComponent* const Interface = &Owner_->Aggregate_;
            Interface->AddRef();
            *Out = Interface;
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FFxRuntime::FInnerUnknown::AddRef()
    {
        return ++RefCount_;
    }

    unsigned long NEURONCOMPONENTS_API FFxRuntime::FInnerUnknown::Release()
    {
        if (RefCount_ != 0)
            --RefCount_;
        const unsigned long Next = RefCount_;
        if (Next == 0)
        {
            RefCount_ = 1;
            if (Owner_ != nullptr)
                delete Owner_;
            return 0;
        }
        return Next;
    }

    GENRESULT NEURONCOMPONENTS_API FFxRuntime::QueryInterface(const char* Iid, void** Out)
    {
        return Controller_->QueryInterface(Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API FFxRuntime::AddRef()
    {
        return Controller_->AddRef();
    }

    unsigned long NEURONCOMPONENTS_API FFxRuntime::Release()
    {
        return Controller_->Release();
    }

    GENRESULT NEURONCOMPONENTS_API FFxRuntime::BindParticleFx(void* ParticleFx)
    {
        // (the field): raw store, no AddRef / no Release of the
        // previous value — same pattern as BindMeshFactory below.
        G_RuntimeParticleFx = ParticleFx;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::BindUnknown(void* Value)
    {
        // (the field): raw store, no AddRef / no Release.
        G_RuntimeUnknown = Value;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::BindEffectLibrary(void* Library)
    {
        // (the field): raw store, no AddRef / no Release.
        G_RuntimeEffectLibrary = Library;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::GetParticleFx(void** Out)
    {
        // a miss leaves Out untouched. Reload the global after
        // AddRef, since the bound object's callback may change the binding.
        if (G_RuntimeParticleFx == nullptr)
            return GR_GENERIC;
        static_cast<INeuronComponent*>(G_RuntimeParticleFx)->AddRef();
        *Out = G_RuntimeParticleFx;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::GetUnknown(void** Out)
    {
        // miss does not write Out; hit reloads after AddRef.
        if (G_RuntimeUnknown == nullptr)
            return GR_GENERIC;
        static_cast<INeuronComponent*>(G_RuntimeUnknown)->AddRef();
        *Out = G_RuntimeUnknown;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::GetEffectLibrary(void** Out)
    {
        // miss does not write Out; hit reloads after AddRef.
        if (G_RuntimeEffectLibrary == nullptr)
            return GR_GENERIC;
        static_cast<INeuronComponent*>(G_RuntimeEffectLibrary)->AddRef();
        *Out = G_RuntimeEffectLibrary;
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::BindMeshFactory(void* Factory)
    {
        // raw store, no AddRef / no Release of the previous.
        G_FxMeshFactory = static_cast<INeuronComponent*>(Factory);
        return GR_OK;
    }
    GENRESULT NEURONCOMPONENTS_API FFxRuntime::GetMeshFactory(void** Out)
    {
        // miss does not write Out; hit reloads after AddRef.
        if (G_FxMeshFactory == nullptr)
            return GR_GENERIC;
        G_FxMeshFactory->AddRef();
        *Out = G_FxMeshFactory;
        return GR_OK;
    }

} // namespace ParticleFxImpl
