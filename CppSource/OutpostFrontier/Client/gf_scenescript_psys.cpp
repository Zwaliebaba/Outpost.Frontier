// Author: Giperion.

#include "gf_scenescript_psys.h"

#include "particlefx_float_forward.h"
#include "particlefx.h"
#include "common_ini.h" // StringAlloc
#include "neuroncomponents.h"
#include "gf_object_ship.h"
#include "vis_effect_arch.h"

#include <cstdint>
#include <cstring>

namespace
{
    char const* const GScriptPsysFile = "Client\\GF\\ScriptPsys.cpp";

    // IFxEffect slots 7 / 8 still use the existing Dt argument adapter.
    void* FloatArg(float Value)
    {
        void* Out = nullptr;
        memcpy(&Out, &Value, sizeof(Value));
        return Out;
    }

}

// ---------------------------------------------------------------------------------------
// SceneScript_Psys
// ---------------------------------------------------------------------------------------

// . Base ctor; +0x6c / +0x70 / +0x74 = 0; vptrs; +0x78 = 0 and +0x84 identity;
// +0xa8 = 0.
FSceneScriptPsys::FSceneScriptPsys(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Vis(nullptr),
      Sparam(0.0f)
{
    Particles.Effect = nullptr;
    Particles.Vis = nullptr;
    Local.Pos.x = 0.0f;
    Local.Pos.y = 0.0f;
    Local.Pos.z = 0.0f;
    MatrixSetIdentity(Local.Orient);
}

// . 0x00453e62..0x00453e7a: StringAlloc(EffectName, 0), the routine on it (the
// VisEffect map lookup); +0x74 = the answer; non-null -> the routine, else FDUMP(0x100001,
// "SceneScript_Psys: Failed to find [VisEffect] %s.", ScriptPsys.cpp, 0x4c, EffectName). The
// StringAlloc'd copy is never freed, as baseline.
void FSceneScriptPsys::BindVisEffect(char const* EffectName)
{
    Vis = FindVisEffectByNickname(StringAlloc(EffectName, false));
    if (Vis != nullptr)
    {
        VisEffectArchAddRef(Vis);
        return;
    }
    FDUMP(0x100001, "%s(%d) : *** ERROR: SceneScript_Psys: Failed to find [VisEffect] %s.",
          GScriptPsysFile, 0x4c, EffectName);
}

// (identity, RET 8). +0x78 = Transform+0x24, +0x84 = the rotation. No frame.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::SetTransform(float const* Transform12)
{
    Local.Pos.x = Transform12[9];
    Local.Pos.y = Transform12[10];
    Local.Pos.z = Transform12[11];
    memcpy(&Local.Orient, Transform12, sizeof(Local.Orient));
    return GR_OK;
}

// (identity, RET 8). {+0x84 rotation, +0x78 position} copied out as 0xc dwords.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::GetTransform(float* OutTransform12)
{
    memcpy(OutTransform12, &Local.Orient, sizeof(Local.Orient));
    OutTransform12[9] = Local.Pos.x;
    OutTransform12[10] = Local.Pos.y;
    OutTransform12[11] = Local.Pos.z;
    return GR_OK;
}

// (geo facet, RET 8). Facet +0x6c = +0x78.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::SetPosition(float* Position)
{
    Local.Pos.x = Position[0];
    Local.Pos.y = Position[1];
    Local.Pos.z = Position[2];
    return GR_OK;
}

// (geo facet, RET 8). REP MOVSD 9 into facet +0x78 (= +0x84).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::SetOrientation(float* Orient9)
{
    memcpy(&Local.Orient, Orient9, sizeof(Local.Orient));
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::GetPosition(float* OutPosition)
{
    OutPosition[0] = Local.Pos.x;
    OutPosition[1] = Local.Pos.y;
    OutPosition[2] = Local.Pos.z;
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::GetOrientation(float* OutOrient9)
{
    memcpy(OutOrient9, &Local.Orient, sizeof(Local.Orient));
    return GR_OK;
}

// (psys facet, RET 4). Facet +0x5c (= +0x74) non-null -> the routine on facet
// +0x54 (= +0x6c) with it. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::Start()
{
    if (Vis != nullptr)
        Particles.Create(Vis);
    return GR_OK;
}

// (psys facet, RET 4). the routine on +0x6c. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::StopEffect()
{
    Particles.Destroy();
    return GR_OK;
}

// (psys facet, RET 8). Dt = DeltaMs * 0.001 (the field, double, stored as a
// float); with an effect: slot 3 (1, &{+0x84 rotation, +0x78 position}), slot 7 (Dt), slot 8
// (Dt). 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::Tick(int DeltaMs)
{
    float const Dt = static_cast<float>(static_cast<double>(DeltaMs) * 0.001);
    if (Particles.Effect != nullptr)
    {
        float Xform[12];
        memcpy(Xform, &Local.Orient, sizeof(float) * 9);
        Xform[9] = Local.Pos.x;
        Xform[10] = Local.Pos.y;
        Xform[11] = Local.Pos.z;
        Particles.Effect->Slot3(reinterpret_cast<void*>(1), Xform);
        Particles.Effect->Slot7(FloatArg(Dt));
        Particles.Effect->Slot8(FloatArg(Dt));
    }
    return GR_OK;
}

// (psys facet, RET 8). Facet +0x90 (= +0xa8) = Value; with an effect, its slot 6
// with the value. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::SetSparam(float Value)
{
    Sparam = Value;
    if (Particles.Effect != nullptr)
        ForwardParticleFxEffectFloat(Particles.Effect, &IFxEffect::Slot6, Value);
    return GR_OK;
}

// (psys facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPsys::GetSparam(float* OutValue)
{
    *OutValue = Sparam;
    return GR_OK;
}

// . The psys facet's slot 4 through its vtable (0x00453e33..0x00453e3a), then
// on +0x74 when set; +0x74 keeps its value.
void FSceneScriptPsys::Stop()
{
    static_cast<IPSysControl*>(this)->StopEffect();
    if (Vis != nullptr)
        VisEffectArchRelease(Vis);
}

// .
void* FSceneScriptPsys::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ADD ECX,0x6c; JMP the routine.
void FSceneScriptPsys::DrawEffects(ICamera* Camera)
{
    Particles.Draw(Camera);
}

// ---------------------------------------------------------------------------------------
// SceneScript_PlayerEnginePsys
// ---------------------------------------------------------------------------------------

// . Base ctor, vptrs, 0x00454117 +0x6c = 0.
FSceneScriptPlayerEnginePsys::FSceneScriptPlayerEnginePsys(Csys const& ScriptFrame, FGfScript* InOwner,
                                               FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Sparam(0.0f)
{
}

// (psys facet, RET 4). the routine; non-null -> the routine on it. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEnginePsys::Start()
{
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->StartEngineEffects();
    return GR_OK;
}

// (psys facet, RET 4). the routine on the ship. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEnginePsys::StopEffect()
{
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->StopEngineEffects();
    return GR_OK;
}

// (psys facet, RET 8). Facet +0x54 (= +0x6c) = Value, then the routine(Value) on
// the ship. 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEnginePsys::SetSparam(float Value)
{
    Sparam = Value;
    FObjectShip* const Ship = GfObjectShip();
    if (Ship != nullptr)
        Ship->SetEngineSparam(Value);
    return GR_OK;
}

// (psys facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptPlayerEnginePsys::GetSparam(float* OutValue)
{
    *OutValue = Sparam;
    return GR_OK;
}

// the psys facet's slot 4 through its vtable.
void FSceneScriptPlayerEnginePsys::Stop()
{
    static_cast<IPSysControl*>(this)->StopEffect();
}

// MOV EAX,0xfffffff9: not -1, the "not implemented" code.
void* FSceneScriptPlayerEnginePsys::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(kSceneScriptSlotNotImplemented));
}
