// Author: Giperion.

// SpaceDust -- baseline Client\Space\SpaceDust.cpp, OutpostFrontier.exe 0x0054bda0..0x0054c361.
// Listing: the branch / the branch (static init), the routine, the routine, the routine,
// , the routine.

#include "space_dust.h"

#include "effect_type_list.h"
#include "game_options.h"
#include "particle_effect.h"
#include "perf_options.h"
#include "star_sphere.h"
#include "vis_effect_arch.h"

#include "common_csys.h"
#include "common_id.h"
#include "common_ini.h"
#include "common_misc.h"
#include "neuroncomponents.h"
#include "x86math.h"

namespace
{
    char const kSpaceDustCppPath[] = "Client\\Space\\SpaceDust.cpp";

    // baseline constants.
    float const kDustDetailThreshold = 0.75f;
    float const kDustIntensityPerParticle = 0.002f;
    float const kOneF = 1.0f;
    double const kFadeCutoff = 1e-5;
    double const kOneD = 1.0;
    double const kSystemDustIntensityScale = 0.002;
    int const kDefaultZoneDustParticles = 0x32;
    unsigned const kPropDustZoneMask = 0x200000;        // , System slot +0x30 mask

    // (.data, initial 0xffffffc4, read only at 0054bf1c): the dust frame sits
    // this far along the camera's Z axis.
    int const kSpaceDustOffsetZ = -60;

    CacheString MakeClearedCacheString()
    {
        CacheString Result;
        Result.clear();
        return Result;
    }

    // / the branch -- static init: CacheString::clear on each name.
    CacheString G_SystemDust = MakeClearedCacheString();
    CacheString G_ZoneDust = MakeClearedCacheString();

    I3DMathEngine* G_SpaceDustMath = nullptr; // the routine's lazy engine

    int G_SystemDustAtFullDetail = 0;         // (0x50 after [Dust])
    int G_SystemDustAtZeroDetail = 0;         // (0x1e after [Dust])
    int G_ZoneDustParticles = 0;
    int G_FadingDustParticles = 0;
    FParticleEffect* G_SystemDustEffect = nullptr;
    FParticleEffect* G_ZoneDustEffect = nullptr;
    FParticleEffect* G_FadingDustEffect = nullptr; // previous zone's, fading

    // ParticleEffect vtable slots called here (the routine): +0x00 Start, +0x04 the
    // death arm (the branch), +0x0c SetIntensity (the branch). baseline calls them
    // virtually; so does the port.

    // Camera vt+0x10: __stdcall(this), returns the camera transform.

    void DustArmDeath(FParticleEffect* Effect)
    {
        Effect->Kill();
    }

    // (cdecl): death-arm and release *Slot, then null it.
    void ReleaseDustEffect(FParticleEffect** Slot)
    {
        if (*Slot != nullptr)
        {
            DustArmDeath(*Slot);
            ParticleEffectRelease(*Slot);
            *Slot = nullptr;
        }
    }

    // (thiscall on the camera transform, RET 8): Out = CamXform * Offset through
    // the file's own lazily acquired 3DMathEngine (the routine), slot +0x1c.
    void ComposeDustXform(FTransform const* CamXform, FTransform* Out, FTransform const* Offset)
    {
        if (G_SpaceDustMath == nullptr)
            G_SpaceDustMath = Acquire3DMathEngine();
        FTransform Local;
        G_SpaceDustMath->MulTransform(Local, *CamXform, *Offset);
        *Out = Local;
    }

    // / 0054c299..0054c311: allocate (the routine(0x84) + the routine(0)),
    // then start it on the archetype named Name (the routine(&CreateID(Name))), or FDUMP.
    void CreateDustEffect(FParticleEffect** Slot, char const* Name)
    {
        *Slot = ParticleEffectCreate();
        unsigned const NicknameId = CreateID(Name);
        FEffectArch* const Arch = FindEffectArch(&NicknameId);
        if (Arch != nullptr)
        {
            (*Slot)->Start(Arch);
        }
        else
        {
            FDUMP(0x100001,
                  "%s(%d) : *** ERROR: no archetype found for effect named \"%s\" in space dust",
                  kSpaceDustCppPath, 0x49, Name);
        }
    }

    // / 0054c186..0054c1c8 / 0054c22b..0054c283: place the effect on the
    // dust frame, set its intensity, mark it (+0x08 |= 1) and link it (the routine).
    void PlaceDustEffect(FParticleEffect* Effect, FTransform const& DustXform, float Intensity)
    {
        Effect->LocalXform = DustXform;
        Effect->SetIntensity(Intensity);
        Effect->Flags08 = static_cast<unsigned char>(Effect->Flags08 | 1);
        EffectTypeListLink(Effect);
    }
}

void SpaceDustSetSystemDust(CacheString const* Dust)
{
    // .
    G_SystemDust.Value = Dust->Value;
    G_SystemDustAtFullDetail = 0x50;
    G_SystemDustAtZeroDetail = 0x1e;
    G_ZoneDust.clear();
    G_ZoneDustParticles = 0;
    G_FadingDustParticles = 0;
}

void SpaceDustShutdown()
{
    // the same release sequence as the routine, inlined.
    ReleaseDustEffect(&G_FadingDustEffect);
    ReleaseDustEffect(&G_ZoneDustEffect);
    G_ZoneDust.clear();
    G_ZoneDustParticles = 0;
    G_FadingDustParticles = 0;
    ReleaseDustEffect(&G_SystemDustEffect);
    G_SystemDust.clear();
    G_SystemDustAtFullDetail = 0;
}

void SpaceDustUpdate(Universe::ISystem const* System, void* Camera)
{
    // .

    // background option off -- drop every dust effect (no early return).
    if (GetGameOption(kGameOptBackgroundGroup) == 0 &&
        (G_SystemDustEffect != nullptr || G_ZoneDustEffect != nullptr ||
         G_FadingDustEffect != nullptr))
    {
        ReleaseDustEffect(&G_FadingDustEffect);
        ReleaseDustEffect(&G_ZoneDustEffect);
        G_ZoneDust.clear();
        G_ZoneDustParticles = 0;
        G_FadingDustParticles = 0;
        ReleaseDustEffect(&G_SystemDustEffect);
    }

    // DustXform = camera transform * (identity, (0, 0, -60)).
    FTransform Offset;
    Win3DIdentityRotation3x3(&Offset.Rotation.D[0][0]);
    Offset.Translation.X = 0.0f;
    Offset.Translation.Y = 0.0f;
    Offset.Translation.Z = static_cast<float>(kSpaceDustOffsetZ);
    FTransform const* const CamXform =
        (FTransform const*)((ICamera*)Camera)->GetOrientation();
    FTransform DustXform;
    ComposeDustXform(CamXform, &DustXform, &Offset);
    Vector const& DustPos = *reinterpret_cast<Vector const*>(&DustXform.Translation);

    // TEST AH,0x41 / JNZ -- the zone dust runs only when detail > 0.75.
    if (GetPerfSpaceDustDetail() > kDustDetailThreshold)
    {
        CacheString Name;
        Name.clear();
        Name.clear();
        int Particles = 0;
        float Blend = 1.0f;
        if (System != nullptr)
        {
            Universe::IZone const* const Zone = System->GetSmallestZoneWithPropertiesAt(&DustPos, kPropDustZoneMask);
            if (Zone != nullptr)
            {
                if (!Zone->GetSpacedust(Name.Value, Particles))
                {
                    FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kSpaceDustCppPath, 0xa8,
                          "zone flagged as PROP_DUST_ZONE but has no dust");
                }
                if (Particles == 0)
                    Particles = kDefaultZoneDustParticles;
                Blend = Zone->GetImmersion(DustPos);
            }
        }

        // a different zone dust name (pointer compare, as baseline) moves the
        // current zone effect to the fading slot and starts the new one.
        if (Name.Value != G_ZoneDust.Value)
        {
            G_ZoneDust.Value = Name.Value;
            if (G_FadingDustEffect != nullptr)
            {
                DustArmDeath(G_FadingDustEffect);
                ParticleEffectRelease(G_FadingDustEffect);
            }
            G_FadingDustEffect = G_ZoneDustEffect;
            G_FadingDustParticles = G_ZoneDustParticles;
            G_ZoneDustEffect = nullptr;
            if (Name.Value[0] != '\0')
                CreateDustEffect(&G_ZoneDustEffect, Name.Value);
        }

        // fading effect -- dies once its level reaches 1e-5.
        G_ZoneDustParticles = Particles;
        if (G_FadingDustEffect != nullptr)
        {
            double const Fade =
                (static_cast<double>(G_FadingDustParticles) *
                 static_cast<double>(kDustIntensityPerParticle)) *
                (static_cast<double>(kOneF) - static_cast<double>(Blend));
            // TEST AH,0x41 / JP at 0054c12c: the effect stays when Fade > 1e-5 or unordered.
            if (!(Fade <= kFadeCutoff))
            {
                PlaceDustEffect(G_FadingDustEffect, DustXform, static_cast<float>(Fade));
            }
            else
            {
                DustArmDeath(G_FadingDustEffect);
                ParticleEffectRelease(G_FadingDustEffect);
                G_FadingDustEffect = nullptr;
            }
        }

        // current zone effect.
        if (G_ZoneDustEffect != nullptr)
        {
            float const Level = static_cast<float>(
                (static_cast<double>(G_ZoneDustParticles) *
                 static_cast<double>(kDustIntensityPerParticle)) *
                static_cast<double>(Blend));
            PlaceDustEffect(G_ZoneDustEffect, DustXform, Level);
        }
    }
    else
    {
        // .
        ReleaseDustEffect(&G_FadingDustEffect);
        ReleaseDustEffect(&G_ZoneDustEffect);
        G_ZoneDust.clear();
        G_ZoneDustParticles = 0;
        G_FadingDustParticles = 0;
    }

    // the system's own dust.
    if (G_SystemDustEffect != nullptr)
    {
        double const Detail = static_cast<double>(GetPerfSpaceDustDetail());
        float const Level = static_cast<float>(
            ((kOneD - Detail) * static_cast<double>(G_SystemDustAtZeroDetail) +
             static_cast<double>(G_SystemDustAtFullDetail) * Detail) *
            kSystemDustIntensityScale);
        PlaceDustEffect(G_SystemDustEffect, DustXform, Level);
        return;
    }
    if (G_SystemDust.Value[0] != '\0')
        CreateDustEffect(&G_SystemDustEffect, G_SystemDust.Value);
}
