// Author: Giperion.

#pragma once

#include "particlefx_types.h"
#include "particlefx_common.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// FFxRuntime / FFxRuntimeAggregate: the IFxRuntime component and its
// IAggregateComponent inner-object facet.
namespace ParticleFxImpl
{
    class FFxRuntime;

    class FFxRuntimeAggregate final : public IAggregateComponent
    {
    public:
        explicit FFxRuntimeAggregate(FFxRuntime* Outer) : Outer_(Outer) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
        GENRESULT NEURONCOMPONENTS_API Initialize() override
        {
            return GR_OK;
        }

    private:
        FFxRuntime* Outer_;
    };

    class FFxRuntime final : public IFxRuntime
    {
    public:
        FFxRuntime()
            : Aggregate_(this), Inner_(this), Controller_(&Inner_)
        {
        }

        FFxRuntimeAggregate* Aggregate() { return &Aggregate_; }

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;

        void SetController(INeuronComponent* OuterUnknown)
        {
            Controller_ = OuterUnknown != nullptr ? OuterUnknown : &Inner_;
        }
        INeuronComponent* InnerUnknown() { return &Inner_; }
        GENRESULT InitializeFromFactory(AGGDESC*) { return GR_OK; }

        GENRESULT NEURONCOMPONENTS_API BindParticleFx(void* ParticleFx) override;
        GENRESULT NEURONCOMPONENTS_API BindUnknown(void* Value) override;
        GENRESULT NEURONCOMPONENTS_API BindEffectLibrary(void* Library) override;
        GENRESULT NEURONCOMPONENTS_API GetParticleFx(void** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetUnknown(void** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetEffectLibrary(void** Out) override;
        GENRESULT NEURONCOMPONENTS_API BindMeshFactory(void* Factory) override;
        GENRESULT NEURONCOMPONENTS_API GetMeshFactory(void** Out) override;

    private:
        class FInnerUnknown final : public INeuronComponent
        {
        public:
            explicit FInnerUnknown(FFxRuntime* Owner) : RefCount_(1), Owner_(Owner) {}

            GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
            unsigned long NEURONCOMPONENTS_API AddRef() override;
            unsigned long NEURONCOMPONENTS_API Release() override;

        private:
            unsigned long RefCount_;
            FFxRuntime* Owner_;
        };

        FFxRuntimeAggregate Aggregate_;
        FInnerUnknown Inner_;
        INeuronComponent* Controller_;
        // The three bound pointers are module globals in baseline (the field / b94 /
        // b90), read directly by the particle appearance; see G_RuntimeParticleFx.
    };

} // namespace ParticleFxImpl
