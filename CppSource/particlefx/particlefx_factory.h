// Author: Giperion.

#pragma once

#include "particlefx_types.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// FComponentFactory: the IComponentFactory shim shared by every registered
// component name, plus the G_Specs[] registration table and DllMain's
// InitializeParticleFxModule helper.
namespace ParticleFxImpl
{
    enum EComponentKind
    {
        KindParticleFx,
        KindFxRuntime,
        KindFxEffectLibrary,
        KindFxEffect,
        KindFxEffectArchetype,
        // One kind per keyed-ramp class (ALC-2): each baseline name has its own factory
        // CreateInstance and its own object vtable (particlefx_ramp.h).
        KindFxRampSingle,
        KindFxRampColor,
        KindFxRampSingleRamp,
        KindFxRampColorRamp,
        KindFxAnimatedSingleRamp,
        KindFxAnimatedSingle,
        KindFxAnimatedTransform,
        KindFxNodeLibrary,
        // "FxNodePropertySet" (RegisterComponent the routine, CreateInstance
        // ): FxNode's own property set, not a node.
        KindFxNodePropertySet,
        KindGeneric,
    };

    struct FComponentSpec
    {
        const char* Name;
        unsigned long RequiredSize;
        EComponentKind Kind;
        const char* PrimaryIid;
    };

    INeuronComponent* CreateComponent(const FComponentSpec& Spec);

    class FComponentFactory final : public IComponentFactory
    {
    public:
        explicit FComponentFactory(const FComponentSpec& Spec) : RefCount_(1), Spec_(Spec) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;

        const char* Name() const { return Spec_.Name; }

    private:
        unsigned long RefCount_;
        FComponentSpec Spec_;
    };

    extern const FComponentSpec G_Specs[];

    void InitializeParticleFxModule();

} // namespace ParticleFxImpl
