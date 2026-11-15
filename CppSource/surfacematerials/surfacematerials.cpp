// Author: Giperion.

#include "surfacematerials.h"

#include <cstdio>
#include <cstring>
#include <new>

#pragma comment(lib, "neuroncomponents.lib")

unsigned long G_VisibleCounterA = 0;
unsigned long G_VisibleCounterB = 0;
unsigned long G_VisibleCounterC = 0;

void FlmVisibleCounterReset()
{
    G_VisibleCounterA = 0;
    G_VisibleCounterB = 0;
    G_VisibleCounterC = 0;
}

void FlmVisibleCounterCopy(unsigned long* Out3)
{
    Out3[0] = G_VisibleCounterA;
    Out3[1] = G_VisibleCounterB;
    Out3[2] = G_VisibleCounterC;
}

bool FlmIidEqual(const char* Left, const char* Right)
{
    if (Left == nullptr || Right == nullptr)
        return false;
    return std::strcmp(Left, Right) == 0;
}

bool FlmNameEqual(const char* Left, const char* Right)
{
    if (Left == nullptr || Right == nullptr)
        return false;
    return std::strcmp(Left, Right) == 0;
}

void FlmFdump(int Flags, const char* File, int Line, const char* Kind, const char* Message)
{
    const char* Format = "%s(%d) : ERROR:General:%s";
    if (Flags == kFdumpTrace1)
        Format = "%s(%d) : TRACE_1:General:%s";
    else if (Flags == kFdumpNotice)
        Format = "%s(%d) : NOTICE:General:%s";
    (void)Kind;
    if (FDUMP != nullptr)
        FDUMP(Flags, Format, File != nullptr ? File : "surfacematerials.cpp", Line,
              Message != nullptr ? Message : "");
}

namespace
{
    const FMaterialSpec G_MaterialSpecs[] = {
        {G_NameVeilbornMaterial, KindVeilborn},
        {G_NameVeilbornNoBendy, KindVeilborn},
        {G_NameHudIconMaterial, KindHudIcon},
        {G_NameHudAnimMaterial, KindHudAnim},
        {G_NameColorizeMaterial, KindColorize},
        {G_NameCloakMaterial, KindCloak},
        {G_NameWireMaterial, KindWire},
        {G_NameWireMaterial2, KindWire2},
        {G_NameExclusionZoneMaterial, KindExclusion},
        {G_NameGlassMaterial, KindGlass},
        {G_NameHighGlassMaterial, KindGlass},
        {G_NameGfGlassMaterial, KindGlass},
        {G_NameEnvMapMaterial, KindEnvMap},
        {G_NameBackdropMaterial, KindBackdrop},
        {G_NameStarsphereMaterial, KindBackdrop},
        {G_NameShadowMaterial, KindShadow},
        {G_NameEngineGlowMaterial, KindEngineGlow},
        {G_NameVisibleCounterMaterial, KindVisibleCounter},
        {G_NamePlanetWaterMaterial, KindPlanetWater},
    };

    const int kMaterialCount = (int)(sizeof(G_MaterialSpecs) / sizeof(G_MaterialSpecs[0]));

    class FMaterialFactory final : public IComponentFactory
    {
    public:
        explicit FMaterialFactory(const FMaterialSpec& Spec) : RefCount_(1), Spec_(Spec) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (Out == nullptr)
                return GR_GENERIC;
            *Out = nullptr;
            if (!FlmIidEqual(Iid, Iids::ComponentFactory))
                return GR_INTERFACE_UNSUPPORTED;
            *Out = (IComponentFactory*)this;
            AddRef();
            return GR_OK;
        }

        unsigned long NEURONCOMPONENTS_API AddRef() override
        {
            RefCount_ += 1;
            return RefCount_;
        }

        unsigned long NEURONCOMPONENTS_API Release() override
        {
            // saturation at zero, reset before freeing the factory.
            if (RefCount_ != 0)
                --RefCount_;
            const unsigned long Remaining = RefCount_;
            if (Remaining == 0)
            {
                RefCount_ = 1;
                delete this;
            }
            return Remaining;
        }

        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            // validate size/name, construct controller, then Slot22.
            if (Desc->size != 0x14)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            FMaterialAggregateDesc* Agg = (FMaterialAggregateDesc*)Desc;
            if (std::strcmp(Agg->NameAt4, Spec_.Name) != 0)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }

            IMaterial* Object = FlmCreateMaterial(Spec_);
            if (Object == nullptr)
            {
                *Instance = nullptr;
                return GR_OUT_OF_MEMORY;
            }
            ((FMaterialImpl*)Object)->ConfigureAggregation(Agg);
            const GENRESULT Result = Object->InitializeFromDescriptor(Agg);
            if (Result != GR_OK)
            {
                if (Agg->InnerOutAtC != nullptr)
                    *Agg->InnerOutAtC = nullptr;
                Object->ScalarDeletingDestructor(1);
                *Instance = nullptr;
                return Result;
            }
            *Instance = Object;
            return Result;
        }

        const char* Name() const { return Spec_.Name; }

    private:
        unsigned long RefCount_;
        FMaterialSpec Spec_;
    };

    class FAppearanceFactory final : public IComponentFactory
    {
    public:
        FAppearanceFactory() : RefCount_(1) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
        {
            if (Out == nullptr)
                return GR_GENERIC;
            *Out = nullptr;
            if (!FlmIidEqual(Iid, Iids::ComponentFactory))
                return GR_INTERFACE_UNSUPPORTED;
            *Out = (IComponentFactory*)this;
            AddRef();
            return GR_OK;
        }

        unsigned long NEURONCOMPONENTS_API AddRef() override
        {
            RefCount_ += 1;
            return RefCount_;
        }

        unsigned long NEURONCOMPONENTS_API Release() override
        {
            // The same ICF-folded factory Release body, the routine.
            if (RefCount_ != 0)
                --RefCount_;
            const unsigned long Remaining = RefCount_;
            if (Remaining == 0)
            {
                RefCount_ = 1;
                delete this;
            }
            return Remaining;
        }

        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override
        {
            // descriptor and result pointer are mandatory.
            if (Desc->size != 0x18)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            FAggDesc18* Agg = (FAggDesc18*)Desc;
            if (std::strcmp(Agg->NameAt4, G_NameFlAppearance) != 0)
            {
                *Instance = nullptr;
                return GR_INTERFACE_UNSUPPORTED;
            }
            return FlmCreateAppearanceInstance(Agg, Instance);
        }

    private:
        unsigned long RefCount_;
    };

    bool RegisterMaterialComponent(const FMaterialSpec& Spec)
    {
        FMaterialFactory* Factory = new (std::nothrow) FMaterialFactory(Spec);
        if (Factory == nullptr)
            return false;
        ICOManager* Manager = NEURONCOMPONENTS_Acquire();
        // Opaque priority token: keep const removal explicit.
        Manager->RegisterComponent(Factory, Factory->Name(), const_cast<void*>(kRegisterPriority));
        Factory->Release();
        return true;
    }

    void RegisterAll()
    {
        // captures this manager before the individual material wrappers.
        ICOManager* AppearanceManager = NEURONCOMPONENTS_Acquire();
        RegisterMaterialComponent(G_MaterialSpecs[0]); // VeilbornMaterial
        RegisterMaterialComponent(G_MaterialSpecs[1]); // VeilbornMaterialNoBendy
        RegisterMaterialComponent(G_MaterialSpecs[2]); // HUDIconMaterial
        RegisterMaterialComponent(G_MaterialSpecs[3]); // HUDAnimMaterial
        RegisterMaterialComponent(G_MaterialSpecs[4]); // ColorizeMaterial
        RegisterMaterialComponent(G_MaterialSpecs[5]); // CloakMaterial
        RegisterMaterialComponent(G_MaterialSpecs[6]); // WireMaterial
        RegisterMaterialComponent(G_MaterialSpecs[7]); // WireMaterial2
        RegisterMaterialComponent(G_MaterialSpecs[8]); // ExclusionZoneMaterial
        RegisterMaterialComponent(G_MaterialSpecs[9]); // GlassMaterial
        RegisterMaterialComponent(G_MaterialSpecs[10]); // HighGlassMaterial
        RegisterMaterialComponent(G_MaterialSpecs[11]); // GFGlassMaterial
        RegisterMaterialComponent(G_MaterialSpecs[12]); // EnvMapMaterial
        RegisterMaterialComponent(G_MaterialSpecs[13]); // BackdropMaterial
        RegisterMaterialComponent(G_MaterialSpecs[14]); // StarsphereMaterial
        RegisterMaterialComponent(G_MaterialSpecs[15]); // ShadowMaterial
        RegisterMaterialComponent(G_MaterialSpecs[16]); // EngineGlowMaterial
        RegisterMaterialComponent(G_MaterialSpecs[17]); // VisibleCounterMaterial
        RegisterMaterialComponent(G_MaterialSpecs[18]); // PlanetWaterMaterial

        if (AppearanceManager != nullptr)
        {
            FAppearanceFactory* Appearance = new (std::nothrow) FAppearanceFactory();
            if (Appearance != nullptr)
            {
                // Opaque priority token: keep const removal explicit.
                AppearanceManager->RegisterComponent(Appearance, G_NameFlAppearance,
                                                     const_cast<void*>(kRegisterPriority));
                Appearance->Release();
            }
        }
    }
}

BOOL WINAPI DllMain(HINSTANCE Instance, DWORD Reason, LPVOID /*Reserved*/)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Instance);
        RegisterAll();
    }
    return TRUE;
}
