// Author: Giperion.

#include "particle_effect.h"

#include "particlefx_float_forward.h"

#include "cli_projectile_fx.h"
#include "client_gameobj.h"
#include "effect_base.h"
#include "effect_type_info.h"
#include "effect_type_list.h"
#include "effect_vertex_batch.h" // G_EffectMathEngine (the field)
#include "engine_light.h"
#include "fade_to_black.h"       // FadeOutColoured / FadeInColoured / IsFadeOpaque (CameraParticles)
#include "game_options.h"
#include "globals.h"             // ShouldSkipLocalServerTick (the routine)
#include "menu_sound_arch.h"
#include "perf_options.h"
#include "space_camera.h"
#include "space_scene_obj_draw.h" // the field controller the field / 0067524c / 00675248

#include "NeuronRuntime.h"
#include "common_cobject.h"
#include "common_csys.h"          // Acquire3DMathEngine (the routine)
#include "neuroncomponents.h"
#include "soundmanager.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <new>

namespace
{


    void CallKillSlot(FParticleEffect* This)
    {
        This->Kill();
    }

    // , filled by the routine on first use at each site (0x004f88e1, ...).
    I3DMathEngine* ParticleMath()
    {
        if (G_EffectMathEngine == nullptr)
            G_EffectMathEngine = Acquire3DMathEngine();
        return G_EffectMathEngine;
    }

    FEffectBase* AsEffectBase(FParticleEffect* This)
    {
        return This;
    }

    // IFxEffect slots take their float argument as the raw dword.
    void* FloatArg(float Value)
    {
        void* Arg = nullptr;
        std::memcpy(&Arg, &Value, sizeof(float));
        return Arg;
    }

    // These setters retain the original DWORD after their ordered comparisons.
    constexpr float GEffectFloatZero = 0.0f;
    constexpr float GEffectFloatOne = 1.0f;

    const char kEffectCpp[] = "Client\\space\\Effect.cpp";
} // namespace

unsigned char FJumpParticleEffect::UpdateVisibility(float /*Dt*/, FSpaceCamera* /*Cam*/)
{
    FJumpParticleEffect* const Self = this;
    // (RET 8): +0x7c |= 1, and that bit is the result. Neither argument is read.
    Self->Flags7C = static_cast<unsigned char>(Self->Flags7C | 1u);
    return static_cast<unsigned char>(Self->Flags7C & 1u);
}

unsigned char FParticleTunnel::UpdateVisibility(float /*Dt*/, FSpaceCamera* Cam)
{
    FParticleTunnel* const Self = this;
    // (RET 8): bit 0 of +0x7c = Cam is one of the in-flight controller's
    // first-person (the routine), third-person (the routine) or turret (the routine)
    // cameras, tested in that order; that bit is the result. Dt is not read.
    unsigned char bCockpitView = 0;
    if (Cam == GetFirstPersonSpaceCamera() || Cam == GetThirdPersonSpaceCamera() ||
        Cam == GetTurretSpaceCamera())
    {
        bCockpitView = 1;
    }
    Self->Flags7C = static_cast<unsigned char>(((Self->Flags7C ^ bCockpitView) & 1u) ^
                                               Self->Flags7C);
    return static_cast<unsigned char>(Self->Flags7C & 1u);
}

unsigned char FParticleTunnel::TickDraw(float Dt, FSpaceCamera* Cam)
{
    FParticleTunnel* const Self = this;
    // (RET 8): the world transform (the routine), its orientation kept and its
    // translation replaced by the camera's position (vt+0x0c, stdcall), copied into the
    // local transform +0x10; then the particle's own tick the routine(Dt, Cam), whose
    // answer is returned.
    FTransform World;
    EffectGetWorldTransform(reinterpret_cast<FEffectBase const*>(Self), &World);
    typedef float const*(__stdcall * FCamGetPosFn)(FSpaceCamera* CameraSelf);
    float const* const CamPos = AsCamera(Cam)->GetPosition();
    FTransform Local;
    Local.Rotation = World.Rotation;
    Local.Translation.X = CamPos[0];
    Local.Translation.Y = CamPos[1];
    Local.Translation.Z = CamPos[2];
    Self->LocalXform = Local;
    return ParticleEffectTickDraw(Self, Dt, Cam);
}

FParticleEffect::FParticleEffect(unsigned TypeId) : FEffectBase(TypeId)
{
    FxEffect = nullptr;
    FxNode = nullptr;
    Flags7C = 0;
    std::memset(Pad7D, 0, sizeof(Pad7D));
    RunTime78 = -1.0f;
    Float80 = 1.0f;
}

void ParticleEffectConstruct(FParticleEffect* This, unsigned TypeId)
{
    ::new (This) FParticleEffect(TypeId);
}

FParticleEffectCruise::FParticleEffectCruise() : FParticleEffect(0)
{
    CruiseScale = 1.0f;
}

float FParticleEffect::GetWorldPos(FVector* Out)
{
    FTransform World;
    EffectGetWorldTransform(this, &World);
    *Out = World.Translation;
    return Radius74;
}


void ParticleEffectCleanupParticleFx(FParticleEffect* This)
{
    // IFxEffect vt+0x34 (7), Release; then IParticleFx vt+0x58 (DetachNode) with
    // the node -- no test of NeuronRuntime::ParticleFx -- and the node's Release.
    if (This->FxEffect != nullptr)
    {
        This->FxEffect->Slot13(reinterpret_cast<void*>(7));
        static_cast<INeuronComponent*>(This->FxEffect)->Release();
        This->FxEffect = nullptr;
    }
    if (This->FxNode != nullptr)
    {
        NeuronRuntime::ParticleFx->DetachNode(This->FxNode);
        This->FxNode->Release();
        This->FxNode = nullptr;
    }
}

unsigned char ParticleEffectCreateParticleFxNodes(FParticleEffect* This)
{
    // — listing 004f8360..004f84c7. No null tests on `this`, the NeuronRuntime globals
    // or the VisEffect: baseline reads them straight.
    // already created -> 1; not started (bit 4 clear) -> 0.
    if ((This->Flags7C & 4u) != 0)
        return 1;
    if ((This->Flags7C & 0x10u) == 0)
        return 0;

    // NeuronComponents CreateInstance({8, "FxNode"}) straight into +0x70.
    AGGDESC Desc = {8, "FxNode"};
    GENRESULT const NodeHr = NeuronRuntime::NeuronComponents->CreateInstance(
        &Desc, reinterpret_cast<void**>(&This->FxNode));
    if (NodeHr != GR_OK)
    {
        // WARNING with the VisEffect nickname, bit 4 cleared.
        FDUMP(0x100002,
              "%s(%d) : *** WARNING: ParticleEffect::start_effect: couldnt create ale.tree "
              "for %s",
              "Client\\space\\Effect.cpp", 0x3dc, This->Vis->Nickname);
        This->Flags7C = static_cast<unsigned char>(This->Flags7C & 0xefu);
        return 0;
    }

    // the routine on the VisEffect; its answer is not read. Bit 3 records that
    // this effect holds a reference (released by the destructor, 0x004f7fe5).
    VisEffectArchAddRef(This->Vis);
    This->Flags7C = static_cast<unsigned char>(This->Flags7C | 8u);

    // FxEffectLibrary vt+0x18 CreateEffect(FxNode, effect_crc,
    // &+0x6c) -- the out pointer is the member itself.
    unsigned long const Key = This->Vis->EffectCrc;
    GENRESULT const Hr = NeuronRuntime::FxEffectLibrary->CreateEffect(
        This->FxNode, reinterpret_cast<void*>(Key), &This->FxEffect);
    if (Hr != GR_OK)
    {
        if (Hr == static_cast<GENRESULT>(-8))
        {
            // no flag change.
            FDUMP(0x100001, "%s(%d) : *** ERROR: could not find particlefx effect (%s) in file (%s)",
                  "Client\\space\\Effect.cpp", 0x3eb, This->Vis->Nickname,
                  This->Vis->ParticleFxPath);
            return 0;
        }
        // bit 4 cleared.
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: ParticleEffect::start_effect: couldnt create ale.eff "
              "(%s) in file (%s) ... Effect is most likely corrupted",
              "Client\\space\\Effect.cpp", 0x3f0,
              This->Vis->Nickname, This->Vis->ParticleFxPath);
        This->Flags7C = static_cast<unsigned char>(This->Flags7C & 0xefu);
        return 0;
    }

    // ParticleFx vt+0x64 GetRootNode, vt+0x50 AttachNode(root, FxNode),
    // root vt+0x8 Release -- no null test on the root.
    void* RootRaw = nullptr;
    NeuronRuntime::ParticleFx->GetRootNode(&RootRaw);
    NeuronRuntime::ParticleFx->AttachNode(RootRaw, This->FxNode);
    reinterpret_cast<INeuronComponent*>(RootRaw)->Release();

    // bit 2 set, and the answer is that bit.
    This->Flags7C = static_cast<unsigned char>(This->Flags7C | 4u);
    return static_cast<unsigned char>((This->Flags7C >> 2) & 1u);
}

unsigned char FParticleEffect::Start(FEffectArch* EffectArch)
{
    FParticleEffect* const This = this;
    // — listing 004f8160..004f8351 (the routine is only the stack probe for the
    // 0x2000-byte format buffer). No null test on `this`. Every failure calls the object's own
    // slot +4 (`MOV EAX,[ESI] / CALL [EAX+4]`), not a fixed function.
    This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
    This->Flags7C = static_cast<unsigned char>(This->Flags7C & 0xefu);

    if (EffectArch == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kEffectCpp, 0x37c,
              "ParticleEffect::start_effect: archetype invalid (null archetype passed "
              "specified)");
        CallKillSlot(This);
        return 0;
    }

    // the routine(0x26).
    if (GetGameOption(kGameOptParticles) == 0)
    {
        CallKillSlot(This);
        return 0;
    }
    // .
    if (NeuronRuntime::ParticleFx == nullptr)
    {
        CallKillSlot(This);
        return 0;
    }

    // JG -- a positive id is kept, zero or negative takes the arch's.
    if (static_cast<int>(This->EffectTypeId) <= 0)
        This->EffectTypeId = static_cast<unsigned>(EffectArch->EffectType);

    // and 0x004f8201: the routine is called twice; the first result only seeds
    // the priority.
    FEffectTypeInfo const* const FirstInfo =
        EffectTypeInfoByIndex(static_cast<int>(This->EffectTypeId));
    if (FirstInfo != nullptr)
        This->Priority0C = FirstInfo->Priority;

    FEffectTypeInfo const* const TypeInfo =
        EffectTypeInfoByIndex(static_cast<int>(This->EffectTypeId));
    if (TypeInfo == nullptr)
    {
        // the name comes from the VisEffect already at +0x04 (its first field),
        // not from the arch -- baseline formats it with the routine into the stack buffer and
        // passes that through "%s".
        FDUMP(0x100002, "%s(%d) : *** WARNING: no type info specified for %s", kEffectCpp,
              0x395, This->Vis->Nickname);
        CallKillSlot(This);
        return 0;
    }

    // FCOMP Priority vs the field, TEST AH,5 / JP -> the full effect unless the
    // priority is strictly (ordered) below the perf level; then FCOMP GenericPriority,
    // TEST AH,1 / JNZ -> the full effect when that is below or unordered; then the generic one
    // only if the arch has one (+0x10).
    float const Perf = GetPerfGenericEffectPerf();
    if (!(TypeInfo->Priority < Perf) || !(TypeInfo->GenericPriority >= Perf) ||
        EffectArch->VisGeneric == nullptr)
    {
        This->Vis = EffectArch->VisEffect;                 // +0x08
        This->Priority0C = TypeInfo->Priority;
    }
    else
    {
        This->Vis = EffectArch->VisGeneric;                // +0x10
        This->Priority0C = TypeInfo->GenericPriority;
    }

    if (This->Vis == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kEffectCpp, 0x3b0,
              "ParticleEffect::start_effect: archetype invalid (no visarch specified)");
        CallKillSlot(This);
        return 0;
    }

    This->Flags7C = static_cast<unsigned char>(This->Flags7C | 0x10u);
    This->Flags08 = static_cast<unsigned char>(This->Flags08 & 0xfdu);
    This->RunTime78 = TypeInfo->RunTime;                   // +0x18
    This->Radius74 = TypeInfo->Radius;                     // +0x10

    // the object's slot +0x10; when it says yes, the routine inlined (which is
    // where the one RefCount increment happens).
    if (This->IsAlive() != 0)
        EffectTypeListLink(This);

    return static_cast<unsigned char>((This->Flags7C >> 4) & 1u);
}

unsigned char ParticleEffectStart(FParticleEffect* This, FEffectArch* EffectArch)
{
    return This->FParticleEffect::Start(EffectArch);
}

void FParticleEffect::Kill()
{
    FParticleEffect* const This = this;
    // slot +4 of both particle tables (.rdata 0x005d9ad8, 0x005dd148).
    FEffectBase* const Base = reinterpret_cast<FEffectBase*>(This);

    if (This->FxEffect == nullptr)
    {
        // no running particlefx effect -- link if needed, the routine, dead.
        EffectTypeListLink(This);
        EffectFreezeToWorld(Base);
        This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
        return;
    }

    // link if needed.
    EffectTypeListLink(This);

    if (This->EngineIndex != -1)
    {
        if (EffectWatchedObject(Base) == nullptr)
        {
            // the watched object is gone -- link (the routine + RefCount++, a
            // no-op now), the routine, dead; then on to the fade-out below.
            EffectTypeListLink(This);
            EffectFreezeToWorld(Base);
            This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
        }
        else
        {
            // the routine's body inlined (velocity, world transform,
            // index = -1). The effect is NOT marked dead: it keeps drawing while the particlefx
            // effect winds down.
            EffectFreezeToWorld(Base);
        }
    }

    // the remaining run time is pinned to 1.0 when it is below 0.0 (TEST AH,5 /
    // JNP: strictly less, ordered) or above 1.0 (TEST AH,0x41 / JNZ skips on <=, = and
    // unordered); a NaN is left alone.
    if (This->RunTime78 < 0.0f || This->RunTime78 > 1.0f)
        This->RunTime78 = 1.0f;

    // IFxEffect vt+0x34 with 1 -- stop emitting, let the live particles finish.
    This->FxEffect->Slot13(reinterpret_cast<void*>(1));
}

void ParticleEffectArmDeath(FParticleEffect* This)
{
    This->FParticleEffect::Kill();
}

void FParticleEffect::SetIntensity(float Intensity)
{
    FParticleEffect* const This = this;
    // (__thiscall, RET 4). Nothing without the IFxEffect at +0x6c; otherwise the
    // value is clamped to [0, 1] -- below 0.0f (the field, TEST AH,5 / JP) becomes 0, above
    // 1.0f (the field, TEST AH,0x41 / JNZ) becomes 1 -- and handed to its vt+0x18 as the
    // raw float dword.
    IFxEffect* const Fx = This->FxEffect;
    if (Fx == nullptr)
        return;
    float Value;
    std::memcpy(&Value, &Intensity, sizeof(Value));
    if (Value < 0.0f)
        std::memcpy(&Value, &GEffectFloatZero, sizeof(Value));
    else if (Value > 1.0f)
        std::memcpy(&Value, &GEffectFloatOne, sizeof(Value));
    ForwardParticleFxEffectFloat(Fx, &IFxEffect::Slot6, Value);
}

void ParticleEffectSetIntensity(FParticleEffect* This, float Intensity)
{
    This->FParticleEffect::SetIntensity(Intensity);
}

unsigned char FParticleEffect::UpdateVisibility(float Dt, FSpaceCamera* Cam)
{
    FParticleEffect* const This = this;
    // (__thiscall, RET 8; EBX = this, EBP = Cam). Dt is not read.
    // the routine(0x26).
    if (GetGameOption(kGameOptParticles) == 0)
        return 0;

    FEffectBase* const Base = AsEffectBase(This);
    ICamera* const CamIface = AsCamera(Cam);

    // a watched gameobj ([+0x44] and that minus 8 both non-null) of
    // ManagerKind 1 or 2.
    FClientGameobj* const Watched = static_cast<FClientGameobj*>(EffectWatchedObject(Base));
    if (Watched != nullptr && (Watched->ManagerKind == 1 || Watched->ManagerKind == 2))
    {
        // IObjInspect::cobject() (vt+0x150); an equipped object
        // (class & 0x103 == 0x103) whose CEqObj +0x140 is at least 1.0 (FCOMP 1.0,
        // TEST AH,1 / JNZ goes on when below or unordered) hides the effect.
        CObject const* const Obj = Watched->cobject();
        if (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x103u) == 0x103u &&
            static_cast<CEqObj const*>(Obj)->CloakPercent >= 1.0f)
            return 0;

        // only with a field controller (the field).
        void* const Field = GetSpaceFieldController();
        if (Field != nullptr)
        {
            // vt+0x18c with a zeroed out-dword.
            unsigned DrawStateB = 0;
            unsigned const DrawStateA = Watched->Slot18c(&DrawStateB);
            if (DrawStateA == 0)
            {
                // outside every field while the camera is fully inside one.
                if (GetSpaceFieldEnclosure() == 2)
                    return 0;
            }
            else
            {
                // inside another field than the camera's.
                if (DrawStateA != static_cast<unsigned>(reinterpret_cast<uintptr_t>(Field)))
                    return 0;

                // the world transform (the routine inlined; the
                // no-op the routine on the stack local is dropped), then camera slot +0x0c.
                FTransform World;
                EffectGetWorldTransform(Base, &World);
                float const* const CamPos = CamIface->GetPosition();

                // each difference is rounded to a float (FSTP into
                // 's arguments); 0x004f8873..0x004f888e: (z*z + y*y) + x*x on the
                // FPU against the field; TEST AH,0x41 / JNZ goes on when below, equal or
                // unordered, so only strictly beyond the radius hides the effect.
                float const Dx = World.Translation.X - CamPos[0];
                float const Dy = World.Translation.Y - CamPos[1];
                float const Dz = World.Translation.Z - CamPos[2];
                double const DistSq = (static_cast<double>(Dz) * Dz + static_cast<double>(Dy) * Dy) +
                                      static_cast<double>(Dx) * Dx;
                if (DistSq > static_cast<double>(GetSpaceFieldRadiusSq()))
                    return 0;
            }
        }
    }

    // FCOMP Priority0C vs the field, TEST AH,1 / JNZ -- below the perf level
    // (or unordered) returns the visibility bit untouched.
    if (!(This->Priority0C >= GetPerfGenericEffectPerf()))
        return static_cast<unsigned char>(This->Flags7C & 1u);

    // own slot +8 -> position, radius in ST0 (FSTP float).
    FVector WorldPos;
    float const Radius = This->GetWorldPos(&WorldPos);

    // camera slot +0x40 (view transform), I3DMathEngine +0x28.
    FTransform const* const ViewXform =
        reinterpret_cast<FTransform const*>(CamIface->GetViewTransform());
    FVector ViewPos;
    ParticleMath()->TransformXform(ViewPos, *ViewXform, WorldPos);

    // NegZ = -z (FST float), far edge = NegZ + Radius (FSTP float);
    // FCOMP ZNear vs far edge, TEST AH,5 / JP -- on only when ZNear is strictly below it.
    float const NegZ = -ViewPos.Z;
    float const FarEdge = NegZ + Radius;
    if (!(CamIface->GetZNear() < FarEdge))
        return static_cast<unsigned char>(This->Flags7C & 1u);

    // the routine (no null test here). Near distance NegZ - Radius
    // against pbubble near + (far - near) * (1.0 (double the field) - perf), all on the FPU;
    // FCOMPP + TEST AH,0x41 / JNZ goes on only when the threshold is strictly greater.
    FEffectTypeInfo const* const TypeInfo =
        EffectTypeInfoByIndex(static_cast<int>(This->EffectTypeId));
    double const NearDistance = static_cast<double>(NegZ) - Radius;
    double const Threshold =
        (static_cast<double>(TypeInfo->PbubbleFar) - TypeInfo->PbubbleNear) *
            (1.0 - static_cast<double>(GetPerfGenericEffectPerf())) +
        TypeInfo->PbubbleNear;
    if (!(Threshold > NearDistance))
        return static_cast<unsigned char>(This->Flags7C & 1u);

    // PARTICLEFX_LOD (option 0x40) -> the routine fills +0x80; a false
    // return zeroes +0x80 and leaves the visibility bit as it is.
    if (GetGameOption(kGameOptParticleFxLod) != 0)
    {
        if (!EffectTypeLodBlend(static_cast<int>(This->EffectTypeId), CamIface, NegZ,
                                &This->Float80))
        {
            This->Float80 = 0.0f;
            return static_cast<unsigned char>(This->Flags7C & 1u);
        }
    }

    // the two side planes (camera +0x80/+0x88, +0x90/+0x94) against
    // the radius; TEST AH,0x41 / JP -- inside only when the distance is below or equal.
    double const DistX = std::fabs(static_cast<double>(ViewPos.X)) * Cam->PlaneX[0] +
                         static_cast<double>(ViewPos.Z) * Cam->PlaneX[2];
    unsigned char Inside = 0;
    if (DistX <= Radius)
    {
        double const DistY = std::fabs(static_cast<double>(ViewPos.Y)) * Cam->PlaneY[1] +
                             static_cast<double>(ViewPos.Z) * Cam->PlaneY[2];
        if (DistY <= Radius)
            Inside = 1;
    }
    This->Flags7C = static_cast<unsigned char>((This->Flags7C & 0xfeu) | Inside);
    return static_cast<unsigned char>(This->Flags7C & 1u);
}

unsigned char ParticleEffectUpdateVisibility(FParticleEffect* This, float Dt, FSpaceCamera* Cam)
{
    return This->FParticleEffect::UpdateVisibility(Dt, Cam);
}

unsigned char FParticleEffect::TickDraw(float Dt, FSpaceCamera* Cam)
{
    FParticleEffect* const This = this;
    // (__thiscall, RET 8; EBP = this). Cam is not read.
    // the routine(0x26).
    if (GetGameOption(kGameOptParticles) == 0)
        return 0;

    // the routine.
    if (ShouldSkipLocalServerTick())
        return 1;

    // CL = +0x7c, read once. Without the start bit (0x10) the effect is finished.
    unsigned char const Flags = This->Flags7C;
    bool bFinished = true;
    if ((Flags & 0x10u) != 0)
    {
        bFinished = false;
        // a run time above zero (TEST AH,0x41 / JNZ skips <= 0 and NaN) counts
        // down by Dt; the FPU result (FST float kept on the stack) at or below zero
        // (TEST AH,0x41 / JNP) finishes the effect.
        if (This->RunTime78 > 0.0f)
        {
            double const Left = static_cast<double>(This->RunTime78) - Dt;
            This->RunTime78 = static_cast<float>(Left);
            if (Left <= 0.0)
                bFinished = true;
        }
        // with the ale bit (4), IFxEffect slot +0x2c(4) returning a non-zero
        // byte also finishes it.
        if (!bFinished && (Flags & 4u) != 0 &&
            static_cast<unsigned char>(This->FxEffect->Slot11(reinterpret_cast<void*>(4))) != 0)
            bFinished = true;
    }
    if (bFinished)
    {
        // the routine (link), the routine (ECX still this), dead.
        EffectTypeListLink(This);
        EffectFreezeToWorld(AsEffectBase(This));
        This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
        return 0;
    }

    // the routine and baseline's own null test.
    FEffectTypeInfo const* const TypeInfo =
        EffectTypeInfoByIndex(static_cast<int>(This->EffectTypeId));
    if (TypeInfo == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kEffectCpp, 0x4c8,
              "effect shouldnt have gotten created w/o type info");
        return 0;
    }

    // neither UPDATE_OFFSCREEN (+0x15) nor visible (+0x7c bit 0, re-read) -> 0.
    if (TypeInfo->UpdateOffscreen == 0 && (This->Flags7C & 1u) == 0)
        return 0;

    // the routine; on failure the object's own slot +4 (kill), then 0.
    if (ParticleEffectCreateParticleFxNodes(This) == 0)
    {
        CallKillSlot(This);
        return 0;
    }

    // with an IFxEffect -- the world transform (the routine inlined, copied to a
    // stack local), own slot +0x24 with +0x80 (the LOD blend), then the effect's +0x0c(0,
    // &xform), +0x1c(0) and +0x20(Dt).
    if (This->FxEffect != nullptr)
    {
        FTransform World;
        EffectGetWorldTransform(AsEffectBase(This), &World);
        This->SetLod(This->Float80);
        IFxEffect* const Fx = This->FxEffect;
        Fx->Slot3(nullptr, &World);
        Fx->Slot7(nullptr);
        Fx->Slot8(FloatArg(Dt));
    }
    return 1;
}

unsigned char ParticleEffectTickDraw(FParticleEffect* This, float Dt, FSpaceCamera* Cam)
{
    return This->FParticleEffect::TickDraw(Dt, Cam);
}

unsigned char FParticleEffect::BuildGeometry(FSpaceCamera* Cam)
{
    FParticleEffect* const This = this;
    // (__thiscall, RET 4): the visibility bit is cleared first, then
    // (0x26); with an IFxEffect its slot +0x24(Cam). Returns 1 past the option.
    This->Flags7C = static_cast<unsigned char>(This->Flags7C & 0xfeu);
    if (GetGameOption(kGameOptParticles) == 0)
        return 0;
    if (This->FxEffect != nullptr)
        This->FxEffect->Slot9(Cam);
    return 1;
}

unsigned char ParticleEffectBuildGeometry(FParticleEffect* This, FSpaceCamera* Cam)
{
    return This->FParticleEffect::BuildGeometry(Cam);
}

void FParticleEffect::SetLod(float Lod)
{
    FParticleEffect* const This = this;
    // (__thiscall, RET 4): no IFxEffect -> the warning, nothing else. Otherwise
    // the same [0, 1] clamp as the branch (TEST AH,5 / JP for < 0, TEST AH,0x41 / JNZ for
    // > 1; NaN passes through) into the effect's slot +0x38.
    IFxEffect* const Fx = This->FxEffect;
    if (Fx == nullptr)
    {
        FDUMP(0x100002, "%s(%d) : *** WARNING: %s", kEffectCpp, 0x511,
              "Trying to set LOD on effect that does not exist - ADO");
        return;
    }
    float Value;
    std::memcpy(&Value, &Lod, sizeof(Value));
    if (Value < 0.0f)
        std::memcpy(&Value, &GEffectFloatZero, sizeof(Value));
    else if (Value > 1.0f)
        std::memcpy(&Value, &GEffectFloatOne, sizeof(Value));
    ForwardParticleFxEffectFloat(Fx, &IFxEffect::Slot14, Value);
}

void ParticleEffectSetLod(FParticleEffect* This, float Lod)
{
    This->FParticleEffect::SetLod(Lod);
}

float FParticleEffectCruise::GetWorldPos(FVector* Out)
{
    FParticleEffectCruise* const This = this;
    // (__thiscall, RET 4). Radius = +0x84 * +0x74 (FST float); the FPU product
    // strictly below 10.0 (the field, TEST AH,5 / JP) becomes 10.0f.
    double const Product = static_cast<double>(This->CruiseScale) * This->Radius74;
    float Radius = static_cast<float>(Product);
    if (Product < 10.0)
        Radius = 10.0f;

    // the routine inlined; *Out = the translation.
    FTransform World;
    EffectGetWorldTransform(AsEffectBase(This), &World);
    *Out = World.Translation;
    return Radius;
}

float ParticleEffectCruiseGetWorldPos(FParticleEffectCruise* This, FVector* Out)
{
    return This->FParticleEffectCruise::GetWorldPos(Out);
}

void FParticleEffectCruise::SetIntensity(float Scale)
{
    FParticleEffectCruise* const This = this;
    // (__thiscall, RET 4): +0x84 = the raw argument; with an IFxEffect, the
    // clamp into its slot +0x18.
    std::memcpy(&This->CruiseScale, &Scale, sizeof(Scale));
    IFxEffect* const Fx = This->FxEffect;
    if (Fx == nullptr)
        return;
    float Value;
    std::memcpy(&Value, &Scale, sizeof(Value));
    if (Value < 0.0f)
        std::memcpy(&Value, &GEffectFloatZero, sizeof(Value));
    else if (Value > 1.0f)
        std::memcpy(&Value, &GEffectFloatOne, sizeof(Value));
    ForwardParticleFxEffectFloat(Fx, &IFxEffect::Slot6, Value);
}

void ParticleEffectCruiseSetScale(FParticleEffectCruise* This, float Scale)
{
    This->FParticleEffectCruise::SetIntensity(Scale);
}

unsigned char FParticleEffectCruise::UpdateVisibility(float Dt, FSpaceCamera* Cam)
{
    FParticleEffectCruise* const This = this;
    // (__thiscall, RET 8): +0x80 saved first, then a direct call of the routine;
    // its low bit replaces bit 0 of +0x7c. A hidden effect whose saved +0x80 was above zero
    // (TEST AH,0x41 / JNZ) stays visible while +0x80 fades: saved - Dt * 0.5 (the field),
    // strictly negative (TEST AH,5 / JP) clamped to 0.
    float const SavedLod = This->Float80;
    unsigned char const Visible = ParticleEffectUpdateVisibility(This, Dt, Cam);
    This->Flags7C = static_cast<unsigned char>((This->Flags7C & 0xfeu) | (Visible & 1u));
    if ((This->Flags7C & 1u) == 0 && SavedLod > 0.0f)
    {
        double Faded = static_cast<double>(SavedLod) - static_cast<double>(Dt) * 0.5;
        if (Faded < 0.0)
            Faded = 0.0;
        This->Float80 = static_cast<float>(Faded);
        This->Flags7C = static_cast<unsigned char>(This->Flags7C | 1u);
    }
    return static_cast<unsigned char>(This->Flags7C & 1u);
}

unsigned char ParticleEffectCruiseUpdateVisibility(FParticleEffectCruise* This, float Dt, FSpaceCamera* Cam)
{
    return This->FParticleEffectCruise::UpdateVisibility(Dt, Cam);
}

FParticleEffect::~FParticleEffect()
{
    if ((Flags7C & 4u) != 0)
        ParticleEffectCleanupParticleFx(this);
    if ((Flags7C & 8u) != 0 && Vis != nullptr)
        VisEffectArchRelease(Vis);
}

void ParticleEffectDestruct(FParticleEffect* This)
{
    This->FParticleEffect::~FParticleEffect();
}


void ParticleEffectDestroy(FParticleEffect* This, int FreeMemory)
{
    if ((FreeMemory & 1) != 0)
        delete This;
    else
        This->FParticleEffect::~FParticleEffect();
}


void ParticleEffectCruiseDestroy(FParticleEffect* This, int FreeMemory)
{
    if ((FreeMemory & 1) != 0)
        delete This;
    else
        ((FParticleEffectCruise*)This)->FParticleEffectCruise::~FParticleEffectCruise();
}


void ParticleEffectAddRef(FParticleEffect* This)
{
    // .
    This->RefCount = This->RefCount + 1;
}

int ParticleEffectRelease(FParticleEffect* This)
{
    return EffectRelease(This);
}


void ParticleEffectAttach(FParticleEffect* This, void* WatchableOwner, int EngineIndex,
                          FTransform const* LocalXform)
{
    // , the shared Effect attach.
    This->Attach(WatchableOwner, EngineIndex, LocalXform);
}

FParticleEffect* ParticleEffectCreate()
{
    // the routine(0x84) -- the Heap pool -- then the routine(0) when it
    // returned memory.
    void* const Mem = EffectAlloc(0x84u);
    if (Mem == nullptr)
        return nullptr;
    FParticleEffect* const Effect = static_cast<FParticleEffect*>(Mem);
    ParticleEffectConstruct(Effect, 0);
    return Effect;
}

FParticleEffect* ParticleEffectCreateCruise()
{
    // , 0x00528bd1..0x00528bf7: the routine(0x90) (malloc), the routine(0),
    // +0x84 = 1.0f, vptr the routine.
    void* const Mem = EffectAlloc(0x90u);
    if (Mem == nullptr)
        return nullptr;
    FParticleEffectCruise* Effect = static_cast<FParticleEffectCruise*>(Mem);
    ::new (Effect) FParticleEffectCruise();
    return Effect;
}

FParticleEffect* ParticleEffectCreateJump()
{
    // the routine(0x84) (the Heap pool); null returns null. 0x005033c0..
    // the routine(0x25), then the vptr the routine.
    void* const Mem = EffectAlloc(0x84u);
    if (Mem == nullptr)
        return nullptr;
    FParticleEffect* const Effect = static_cast<FParticleEffect*>(Mem);
    ::new (Effect) FJumpParticleEffect();
    return Effect;
}

FParticleEffect* ParticleTunnelCreate()
{
    // the routine(0x84) (the Heap pool); null returns null, else JMP the branch:
    // (0), then the vptr the routine (ParticleTunnel).
    void* const Mem = EffectAlloc(0x84u);
    if (Mem == nullptr)
        return nullptr;
    FParticleEffect* const Effect = static_cast<FParticleEffect*>(Mem);
    ::new (Effect) FParticleTunnel();
    return Effect;
}

namespace
{
    // .rdata 0x005d9a74 (0.4f): the fade duration both halves pass, and the timer's seed twice.
    constexpr float kCameraParticlesFadeTime = 0.4f;
    // .rdata 0x005d9a7c / 0x005d9a78: the near and far distances of Start's fade factor.
    constexpr float kCameraParticlesNearDistance = 2000.0f;
    constexpr float kCameraParticlesFarDistance = 3000.0f;
    // The colour both fades are given: three 0x3f800000 dwords built on the stack.
    float const kCameraParticlesWhite[3] = {1.0f, 1.0f, 1.0f};


} // namespace

unsigned char FCameraParticles::Start(FEffectArch* Arch)
{
    FCameraParticles* const Self = this;
    // (RET 4). 0x004f8e16..0x004f8e84: the world transform -- the routine's
    // body inlined (instance x local through the field when watched and attached, else
    // the local transform).
    FTransform World;
    EffectGetWorldTransform(Self, &World);

    // the routine, stored at +0x74 before the test.
    void* const Camera = GetClientSoundListenerObject();
    Self->Camera = Camera;
    if (Camera != nullptr)
    {
        // the camera's vt+0x0c (stdcall, `this` pushed) -- its position.
        typedef float const*(__stdcall * FCamGetPosFn)(void* CameraSelf);
        float const* const CamPos =
            ((ICamera*)Camera)->GetPosition();
        float const Dx = CamPos[0] - World.Translation.X;
        float const Dy = CamPos[1] - World.Translation.Y;
        float const Dz = CamPos[2] - World.Translation.Z;
        // (Dz*Dz + Dx*Dx) + Dy*Dy, then FSQRT.
        float const Distance = std::sqrt(Dz * Dz + Dx * Dx + Dy * Dy);
        float const Factor = (Distance - kCameraParticlesNearDistance) /
                             (kCameraParticlesFarDistance - kCameraParticlesNearDistance);

        // FCOMP 0.0f, TEST AH,0x41 / JP -> 0: the byte is (Factor <= 0.0f), a
        // NaN giving 0.
        Self->bStartOpaque = static_cast<unsigned char>((Factor <= 0.0f) ? 1 : 0);
        if (Self->bStartOpaque == 0)
        {
            // FCOMP 1.0f, TEST AH,0x41 / JNZ multiplies; the fall-through
            // (strictly greater) returns 0 before anything is linked or faded.
            if (Factor > 1.0f)
                return 0;
            Self->Timer = Factor * Self->Timer;
        }
    }

    // the routine inlined (link unless linked or dead, +0x58 += 1).
    EffectTypeListLink(Self);

    // the routine(white, 0.4f).
    FadeOutColoured(kCameraParticlesWhite, kCameraParticlesFadeTime);

    // the branch's body inlined (a type id at or below zero taken
    // from the arch, the row's priority into +0x0c); returns 1.
    return EffectBaseStart(Self, Arch);
}

float FCameraParticles::GetWorldPos(FVector* Out)
{
    FCameraParticles* const Self = this;
    // (RET 4): three zero dwords into *Out, 100.0f (.rdata 0x005ca230) on ST0.
    Out->X = 0.0f;
    Out->Y = 0.0f;
    Out->Z = 0.0f;
    return 100.0f;
}

unsigned char FCameraParticles::TickDraw(float Dt, FSpaceCamera* Cam)
{
    FCameraParticles* const Self = this;
    // (RET 8). 0x004f8fd6..0x004f8fee: a Start camera that is not the one
    // drawing re-seeds the timer with 0.4f + 0.4f.
    if (Self->Camera != nullptr && Self->Camera != static_cast<void*>(Cam))
        Self->Timer = kCameraParticlesFadeTime + kCameraParticlesFadeTime;

    // the routine, kept in the Dt argument slot; every return is its negation.
    // An opaque fade goes straight to the fade-in and the kill.
    if (!IsFadeOpaque())
    {
        // FCOMP 0.0f, TEST AH,0x41 / JNZ -> return: a timer that is not above
        // zero (or NaN) keeps the effect alive and does nothing.
        if (!(Self->Timer > 0.0f))
            return 1;
        // Timer -= Dt; FCOMP 0.0f, TEST AH,0x41 / JP -> 0x004f9097 (return 1)
        // unless the new timer is at or below zero; NaN also returns 1.
        Self->Timer = Self->Timer - Dt;
        if (!(Self->Timer <= 0.0f))
            return 1;
        // the saved byte becomes 1.
    }

    // the routine(white, 0.4f, +0x70).
    FadeInColoured(kCameraParticlesWhite, kCameraParticlesFadeTime, Self->bStartOpaque != 0);

    // the routine's body inlined -- link unless linked or dead; with
    // an engine index, an empty watcher calls the routine itself and a watched one is baked
    // to the world (velocity +0x4c, transform +0x10, index -1); then +0x08 |= 2.
    EffectKill(Self);
    // SETZ on the saved byte, which is 1 on this path.
    return 0;
}

FCameraParticles::FCameraParticles() : FEffectBase(0x2cu)
{
    Camera = nullptr;
    bStartOpaque = 1;
    Timer = kCameraParticlesFadeTime + kCameraParticlesFadeTime;
}

void CameraParticlesConstruct(FCameraParticles* This)
{
    ::new (This) FCameraParticles();
}


FEffectBase* CameraParticlesCreate()
{
    // the routine(0x78) (malloc); null returns null, else JMP the routine.
    void* const Mem = EffectAlloc(sizeof(FCameraParticles));
    if (Mem == nullptr)
        return nullptr;
    FCameraParticles* const Effect = static_cast<FCameraParticles*>(Mem);
    CameraParticlesConstruct(Effect);
    return Effect;
}

namespace
{
    FGameLight* CreateEffectGameLight()
    {
        // (slot 2 of every factory): the routine(0x12c), then the routine on the
        // memory when there is some.
        void* const Mem = GameLightHeapAlloc(0x12c);
        if (Mem == nullptr)
            return nullptr;
        return GameLightConstruct(static_cast<FGameLight*>(Mem));
    }
}

FEffectBase* FStandardCliEffectFactory::CreateParticle() const { return ParticleEffectCreate(); }
ISoundSource* FStandardCliEffectFactory::CreateSound() const { return (ISoundSource*)CreateClientSoundSource3D(); }
FGameLight* FStandardCliEffectFactory::CreateLight() const { return CreateEffectGameLight(); }
FEffectBase* FJumpCliEffectFactory::CreateParticle() const { return ParticleEffectCreateJump(); }
FEffectBase* FCameraCliEffectFactory::CreateParticle() const { return CameraParticlesCreate(); }
ISoundSource* FCameraCliEffectFactory::CreateSound() const { return (ISoundSource*)CreateClientSoundSource(); }
FEffectBase* FTunnelCliEffectFactory::CreateParticle() const { return ParticleTunnelCreate(); }

ISoundSource* FJumpCliEffectFactory::CreateSound() const { return (ISoundSource*)CreateClientSoundSource3D(); }
FGameLight* FJumpCliEffectFactory::CreateLight() const { return CreateEffectGameLight(); }
FGameLight* FCameraCliEffectFactory::CreateLight() const { return CreateEffectGameLight(); }
ISoundSource* FTunnelCliEffectFactory::CreateSound() const { return (ISoundSource*)CreateClientSoundSource(); }
FGameLight* FTunnelCliEffectFactory::CreateLight() const { return CreateEffectGameLight(); }

FStandardCliEffectFactory const G_CliEffectFactoryC60;
FJumpCliEffectFactory const G_CliEffectFactoryJump;
FCameraCliEffectFactory const G_CliEffectFactoryCameraParticles;
FTunnelCliEffectFactory const G_CliEffectFactoryTunnel;

unsigned char CreateEffectCompanionAlive(void* CompanionLocal)
{
    // — this = &{arch, sound, particle, light}. No test of `this` (0x00503333
    // reads +8 at once); every caller passes an embedded companion's address.
    unsigned char* Base = static_cast<unsigned char*>(CompanionLocal);
    FEffectBase* Particle = *reinterpret_cast<FEffectBase**>(Base + 8);
    if (Particle != nullptr && (Particle->Flags08 & 2u) == 0)
        return 1;
    ISoundSource* Sound = *reinterpret_cast<ISoundSource**>(Base + 4);
    if (Sound != nullptr && Sound->IsActive() != 0)
        return 1;
    void* Light = *reinterpret_cast<void**>(Base + 0xc);
    if (Light != nullptr)
    {
        // Light+0xd4 == 0 → alive. Gamelit layout not owned here.
        int const FieldD4 = *reinterpret_cast<int*>(static_cast<unsigned char*>(Light) + 0xd4);
        if (FieldD4 == 0)
            return 1;
    }
    return 0;
}

unsigned char CliCreateEffectOneShot(FEffectArch* EffectArch, void* ListOwner,
                                     int EngineIndex, FTransform const* LocalXform,
                                     void* CompanionOut)
{
    return CliCreateEffectOneShotOn(&G_CliEffectFactoryC60, EffectArch, ListOwner, EngineIndex,
                                    LocalXform, CompanionOut);
}

unsigned char CliCreateEffectOneShotOn(FCliEffectFactory const* Factory, FEffectArch* EffectArch,
                                       void* ListOwner, int EngineIndex,
                                       FTransform const* LocalXform, void* CompanionOut)
{
    // — listing 00503070..0050332b (thiscall on the factory).
    if (GetGameOption(kGameOptEffectsGroup) == 0 || EffectArch == nullptr)
        return 0;

    FEffectBase* Particle = nullptr;
    ISoundSource* Sound = nullptr;
    FGameLight* Light = nullptr;

    // / 0x005030ef / 0x0050310a: the factory's slots 0, 1 and 2 (`CALL [EDX]`,
    // `[EAX+4]`, `[EDX+8]`).
    if (GetGameOption(kGameOptParticles) != 0 && EffectArch->VisEffect != nullptr &&
        EffectArch->VisEffect->Nickname[0] != '\0')
    {
        Particle = Factory->CreateParticle();
    }
    if (EffectArch->SndEffect != nullptr)
        Sound = static_cast<ISoundSource*>(Factory->CreateSound());
    if (EffectArch->LgtRadius > 0.0f)
        Light = Factory->CreateLight();

    // baseline Attach(ListOwner, eng, xform) — ListOwner is CliProjectile / Gameobj.
    if (Particle != nullptr)
        EffectAttach(Particle, ListOwner, EngineIndex, LocalXform);
    if (Sound != nullptr && LocalXform != nullptr)
        Sound->SetPose(EngineIndex, reinterpret_cast<float const*>(LocalXform));
    if (Light != nullptr && LocalXform != nullptr)
        CliExplosionAttachLocalXform(Light, EngineIndex, LocalXform);

    // the particle's vtable slot 0 (`CALL [EAX]`); a zero result releases it.
    if (Particle != nullptr)
    {
        if (Particle->Start(EffectArch) == 0)
        {
            EffectRelease(Particle);
            Particle = nullptr;
        }
    }
    if (Sound != nullptr)
    {
        Sound->SetLooping(static_cast<unsigned char>(EffectArch->Flags50 & 1u));
        if (EffectArch->SndEffect == nullptr ||
            Sound->Start(EffectArch->SndEffect, 0) == 0)
        {
            Sound->Release();
            Sound = nullptr;
        }
    }
    if (Light != nullptr)
    {
        // (1,1) + SetColorBytesAlt(arch+0x4c) + SetRadius(arch+0x48) + vt+4 Enable.
        EngineLightSetType((FEngineLight*)Light, 1, true);
        unsigned char const Rgb[3] = {EffectArch->LgtColorR, EffectArch->LgtColorG,
                                      EffectArch->LgtColorB};
        EngineLightSetColorBytesAlt((FEngineLight*)Light, Rgb);
        EngineLightSetRadius((FEngineLight*)Light, EffectArch->LgtRadius);
        if (!GameLightEnable((FEngineLight*)Light))
        {
            GameLightRelease(Light);
            Light = nullptr;
        }
        else if (EffectArch->LgtEffectLen != 0)
        {
            Light::AnimationArch* Anim = GameLightFindAnimByName(EffectArch->LgtEffect);
            if (Anim != nullptr)
                GameLightAttachAnim(Light, Anim, EffectArch->LgtRangeScale,
                                    EffectArch->LgtTimeScale, 0.0f);
        }
    }

    unsigned char LocalCompanion[0x10];
    std::memset(LocalCompanion, 0, sizeof(LocalCompanion));
    *reinterpret_cast<FEffectArch**>(LocalCompanion + 0) = EffectArch;
    *reinterpret_cast<ISoundSource**>(LocalCompanion + 4) = Sound;
    *reinterpret_cast<FEffectBase**>(LocalCompanion + 8) = Particle;
    *reinterpret_cast<FGameLight**>(LocalCompanion + 0xc) = Light;

    if (CreateEffectCompanionAlive(LocalCompanion) == 0)
    {
        if (Particle != nullptr)
            EffectRelease(Particle);
        if (Sound != nullptr)
            Sound->Release();
        if (Light != nullptr)
            GameLightRelease(Light);
        return 0;
    }

    if (CompanionOut != nullptr)
    {
        // clear + store arch/particle/sound/light with AddRef.
        unsigned char* Out = static_cast<unsigned char*>(CompanionOut);
        std::memset(Out, 0, 0x10);
        *reinterpret_cast<FEffectArch**>(Out + 0) = EffectArch;
        *reinterpret_cast<ISoundSource**>(Out + 4) = Sound;
        *reinterpret_cast<FEffectBase**>(Out + 8) = Particle;
        *reinterpret_cast<FGameLight**>(Out + 0xc) = Light;
        if (Particle != nullptr)
            EffectAddRef(Particle);
        if (Sound != nullptr)
            Sound->AddRef();
        if (Light != nullptr)
            GameLightAddRef(Light);
    }

    // Listing 005032c7..005032d7: if ListOwner != 0 → the routine(ListOwner+0x28, &local).
    if (ListOwner != nullptr)
        CliEffectListInsert(static_cast<unsigned char*>(ListOwner) + 0x28, LocalCompanion);

    if (Particle != nullptr)
        EffectRelease(Particle);
    if (Sound != nullptr)
        Sound->Release();
    if (Light != nullptr)
        GameLightRelease(Light);
    return 1;
}

unsigned char CliCreateEffectWorldOneShot(FEffectArch* EffectArch, FTransform const* LocalXform,
                                          float const* Pos3, void* CompanionOut)
{
    return CliCreateEffectWorldOneShotOn(&G_CliEffectFactoryC60, EffectArch, LocalXform, Pos3,
                                         CompanionOut);
}

unsigned char CliCreateEffectWorldOneShotOn(FCliEffectFactory const* Factory,
                                            FEffectArch* EffectArch, FTransform const* LocalXform,
                                            float const* Pos3, void* CompanionOut)
{
    // — listing 00503f40..005041ad. 0x00503f5c..0x00503f7a: the effects option
    // (the routine(0x25)) and a null EffectArch return 0; neither pointer argument is tested.
    if (GetGameOption(kGameOptEffectsGroup) == 0 || EffectArch == nullptr)
        return 0;

    FEffectBase* Particle = nullptr;
    ISoundSource* Sound = nullptr;
    FGameLight* Light = nullptr;

    // the factory's slot 0 (particles option and a named VisEffect),
    // slot 1 (a SndEffect) and slot 2 (LgtRadius above 0.0f, TEST AH,0x41 / JNZ skips).
    if (GetGameOption(kGameOptParticles) != 0 && EffectArch->VisEffect != nullptr &&
        EffectArch->VisEffect->Nickname[0] != '\0')
    {
        Particle = Factory->CreateParticle();
    }
    if (EffectArch->SndEffect != nullptr)
        Sound = static_cast<ISoundSource*>(Factory->CreateSound());
    if (EffectArch->LgtRadius > 0.0f)
        Light = Factory->CreateLight();

    // Particle: memcpy 0xc dwords of LocalXform into +0x10 (listing 00503fff..00504009).
    if (Particle != nullptr)
        std::memcpy(&Particle->LocalXform, LocalXform, sizeof(FTransform));

    // Sound: 0x0050401f vt+0x8c (slot 35) with xform+0x24 -- the world position (Source3D
    // ; a plain Source from the field's slot folds it onto a RET 4) -- and
    // vt+0x90 (slot 36, SetVelocity) with Pos3.
    if (Sound != nullptr)
    {
        Sound->Slot35(const_cast<float*>(&LocalXform->Translation.X));
        Sound->SetVelocity(Pos3);
    }

    // Light: EngIndex==-1 → the routine(xform); else memcpy into +0xf0 (00504029..00504052).
    if (Light != nullptr)
    {
        if (static_cast<int>(Light->EngineIndex) == -1)
        {
            EngineLightSetTransform((FEngineLight*)Light, &LocalXform->Rotation.D[0][0],
                                    &LocalXform->Translation.X);
        }
        else
        {
            std::memcpy(Light->LocalXform, LocalXform, sizeof(FTransform));
        }
        GameLightSetFreeMotionOffset(Light, Pos3);
    }

    // Pos3 → particle Pad4C / +0x4c (listing 0050405b..0050406f).
    if (Particle != nullptr)
    {
        float* const Pad = reinterpret_cast<float*>(Particle->Pad4C);
        Pad[0] = Pos3[0];
        Pad[1] = Pos3[1];
        Pad[2] = Pos3[2];
    }

    // the particle's own vtable slot 0 (`CALL [EDX]`, the factory's
    // class decides -- CameraParticles for the field); a zero result releases it.
    if (Particle != nullptr)
    {
        if (Particle->Start(EffectArch) == 0)
        {
            EffectRelease(Particle);
            Particle = nullptr;
        }
    }
    if (Sound != nullptr)
    {
        Sound->SetLooping(static_cast<unsigned char>(EffectArch->Flags50 & 1u));
        if (EffectArch->SndEffect == nullptr ||
            Sound->Start(EffectArch->SndEffect, 0) == 0)
        {
            Sound->Release();
            Sound = nullptr;
        }
    }
    if (Light != nullptr)
    {
        EngineLightSetType((FEngineLight*)Light, 1, true);
        unsigned char const Rgb[3] = {EffectArch->LgtColorR, EffectArch->LgtColorG,
                                      EffectArch->LgtColorB};
        EngineLightSetColorBytesAlt((FEngineLight*)Light, Rgb);
        EngineLightSetRadius((FEngineLight*)Light, EffectArch->LgtRadius);
        if (!GameLightEnable((FEngineLight*)Light))
        {
            GameLightRelease(Light);
            Light = nullptr;
        }
        else if (EffectArch->LgtEffectLen != 0)
        {
            Light::AnimationArch* Anim = GameLightFindAnimByName(EffectArch->LgtEffect);
            if (Anim != nullptr)
                GameLightAttachAnim(Light, Anim, EffectArch->LgtRangeScale,
                                    EffectArch->LgtTimeScale, 0.0f);
        }
    }

    unsigned char LocalCompanion[0x10];
    std::memset(LocalCompanion, 0, sizeof(LocalCompanion));
    *reinterpret_cast<FEffectArch**>(LocalCompanion + 0) = EffectArch;
    *reinterpret_cast<ISoundSource**>(LocalCompanion + 4) = Sound;
    *reinterpret_cast<FEffectBase**>(LocalCompanion + 8) = Particle;
    *reinterpret_cast<FGameLight**>(LocalCompanion + 0xc) = Light;

    if (CreateEffectCompanionAlive(LocalCompanion) == 0)
    {
        if (Particle != nullptr)
            EffectRelease(Particle);
        if (Sound != nullptr)
            Sound->Release();
        if (Light != nullptr)
            GameLightRelease(Light);
        return 0;
    }

    if (CompanionOut != nullptr)
    {
        unsigned char* Out = static_cast<unsigned char*>(CompanionOut);
        std::memset(Out, 0, 0x10);
        *reinterpret_cast<FEffectArch**>(Out + 0) = EffectArch;
        *reinterpret_cast<ISoundSource**>(Out + 4) = Sound;
        *reinterpret_cast<FEffectBase**>(Out + 8) = Particle;
        *reinterpret_cast<FGameLight**>(Out + 0xc) = Light;
        if (Particle != nullptr)
            EffectAddRef(Particle);
        if (Sound != nullptr)
            Sound->AddRef();
        if (Light != nullptr)
            GameLightAddRef(Light);
    }

    // No ListOwner → no the routine (unlike 00503070).
    if (Particle != nullptr)
        EffectRelease(Particle);
    if (Sound != nullptr)
        Sound->Release();
    if (Light != nullptr)
        GameLightRelease(Light);
    return 1;
}
