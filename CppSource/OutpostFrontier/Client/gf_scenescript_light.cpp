// Author: Giperion.

#include "gf_scenescript_light.h"

#include "neuroncomponents.h"
#include "engine_light.h"
#include "gf_script.h"
#include "gf_script_object.h"
#include "x86math.h"

#include <cstdint>
#include <cstring>

namespace
{
    char const* const GScriptLightFile = "Client\\GF\\ScriptLight.cpp";

    GENRESULT const kSceneScriptReferenceMissing = static_cast<GENRESULT>(-1);

    // this file's 3DMathEngine, acquired (the routine) on first use by each of
    // the three bodies that read it (0x00453168, 0x0045320d, 0x00453352).
    I3DMathEngine* G_SceneScriptLightMath = nullptr;

    I3DMathEngine* SceneScriptLightMath()
    {
        if (G_SceneScriptLightMath == nullptr)
            G_SceneScriptLightMath = Acquire3DMathEngine();
        return G_SceneScriptLightMath;
    }

    FEngineLight* EngineLightOf(FGfLightWrapper* Wrapper)
    {
        return Wrapper->Light;
    }

    // The tail shared by the branch / the branch / the branch: the direction carried into
    // the current orientation (3DMathEngine slot 11, TransformMatrix(out, Orient, Direction)),
    // LookMatrixYup of it, and the routine on the engine light when the wrapper holds one. The
    // wrapper itself is dereferenced without a test (`MOV ECX,[EDX]`).
    void PushLightOrientation(FSceneScriptLight& Self)
    {
        FVector Rotated;
        SceneScriptLightMath()->TransformMatrix(Rotated, *reinterpret_cast<FMatrix const*>(&Self.Orient),
                                          *reinterpret_cast<FVector const*>(&Self.Direction));
        Vector Look;
        Look.x = Rotated.X;
        Look.y = Rotated.Y;
        Look.z = Rotated.Z;
        Matrix const LookOrient = LookMatrixYup(Look);
        FEngineLight* const Light = EngineLightOf(Self.Wrapper);
        if (Light != nullptr)
            EngineLightSetOrientation(Light, &LookOrient.E[0][0]);
    }
}

// ---------------------------------------------------------------------------------------
// SceneScript_Light
// ---------------------------------------------------------------------------------------

// . After the base ctor and the vptrs: stack copies of the snap's position (+0x08),
// orientation (+0x14) and the whole 0x24-dword light props (+0x44); the routine(&pos,
// &props+0x28 (the D3DLIGHT8), &props+0x0c, snap+0x40 light group) into +0x6c
// (0x0045304c..0x004530b1); +0x74 = the orientation, +0x98 = props+0x0c; then the first null
// registry slot (0x004530b6 loop, 0x40 slots) takes the wrapper and the enable byte
// props+0x20 != 0, and its index goes to +0x70 -- or -1 when all are taken.
FSceneScriptLight::FSceneScriptLight(Csys const& ScriptFrame, FGfScript* InOwner, FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Wrapper(nullptr),
      RegistryIndex(-1)
{
    float const* const Props = reinterpret_cast<float const*>(&Desc->Light);
    Wrapper = GfLightWrapperCreate(Desc->Pos, SceneScriptLightPropsD3d(Props),
                                   SceneScriptLightPropsTarget(Props), Desc->LtGrp);
    memcpy(&Orient, Desc->Orient, sizeof(Orient));
    Direction.x = Desc->Light.Direction[0];
    Direction.y = Desc->Light.Direction[1];
    Direction.z = Desc->Light.Direction[2];
    RegistryIndex = GfLightRegistryAdd(Wrapper, Desc->Light.On != 0);
}

// (identity, RET 8). the routine with Transform+0x24 when the wrapper holds a
// light (no script-frame transform), +0x74 = the rotation, then the direction push.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetTransform(float const* Transform12)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
        EngineLightSetPosition(Light, Transform12 + 9);
    memcpy(&Orient, Transform12, sizeof(Orient));
    PushLightOrientation(*this);
    return GR_OK;
}

// (identity, RET 8). Out+0x24 = the engine light's position (+0x98) when there is
// a light, Out+0 = +0x74.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetTransform(float* OutTransform12)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
    {
        OutTransform12[9] = Light->Position[0];
        OutTransform12[10] = Light->Position[1];
        OutTransform12[11] = Light->Position[2];
    }
    memcpy(OutTransform12, &Orient, sizeof(Orient));
    return GR_OK;
}

// (geo facet, RET 8). the routine(Position) on the light when there is one.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetPosition(float* Position)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
        EngineLightSetPosition(Light, Position);
    return GR_OK;
}

// (geo facet, RET 8). +0x74 = Orient9 (facet +0x68), then the direction push.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetOrientation(float* Orient9)
{
    memcpy(&Orient, Orient9, sizeof(Orient));
    PushLightOrientation(*this);
    return GR_OK;
}

// (geo facet, RET 8). The light's +0x98 when there is one; otherwise nothing is
// written.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetPosition(float* OutPosition)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
    {
        OutPosition[0] = Light->Position[0];
        OutPosition[1] = Light->Position[1];
        OutPosition[2] = Light->Position[2];
    }
    return GR_OK;
}

// (geo facet, RET 8). REP MOVSD 9 from +0x74.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetOrientation(float* OutOrient9)
{
    memcpy(OutOrient9, &Orient, sizeof(Orient));
    return GR_OK;
}

// (D3D-light facet, RET 8). Facet +0x5c = +0x70: when the index is not -1, the
// registry enable byte = On != 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetOn(int On)
{
    if (RegistryIndex != -1)
        GfLightRegistrySetEnabled(RegistryIndex, On != 0);
    return GR_OK;
}

// (D3D-light facet, RET 8). the routine(D3dLight8) on the light when there is one.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetLight(void const* D3dLight8)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
        EngineLightSetLight(Light, static_cast<FD3dLight8 const*>(D3dLight8));
    return GR_OK;
}

// (D3D-light facet, RET 8). +0x98 = Dir3 (facet +0x84), then the direction push.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::SetDirection(float* Dir3)
{
    Direction.x = Dir3[0];
    Direction.y = Dir3[1];
    Direction.z = Dir3[2];
    PushLightOrientation(*this);
    return GR_OK;
}

// (D3D-light facet, RET 8). *OutOn = 0, then the enable byte (MOVZX) when the
// index is not -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetOn(int* OutOn)
{
    *OutOn = 0;
    if (RegistryIndex != -1)
        *OutOn = GfLightRegistryIsEnabled(RegistryIndex) ? 1 : 0;
    return GR_OK;
}

// (D3D-light facet, RET 8). 0x1a dwords from the light's +0x0c (its D3DLIGHT8) when
// there is a light.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetLight(void* OutD3dLight8)
{
    FEngineLight* const Light = EngineLightOf(Wrapper);
    if (Light != nullptr)
        memcpy(OutD3dLight8, &Light->D3d, sizeof(FD3dLight8));
    return GR_OK;
}

// (D3D-light facet, RET 8). +0x98.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLight::GetDirection(float* OutDir3)
{
    OutDir3[0] = Direction.x;
    OutDir3[1] = Direction.y;
    OutDir3[2] = Direction.z;
    return GR_OK;
}

// . The registry slot +0x70 and its enable byte cleared (no -1 test), +0x70 = -1,
// (&+0x6c).
void FSceneScriptLight::Stop()
{
    GfLightRegistryRemove(RegistryIndex);
    RegistryIndex = -1;
    GfLightWrapperDestroy(&Wrapper);
}

// .
void* FSceneScriptLight::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ---------------------------------------------------------------------------------------
// The reference forwarders (ICF-shared by the light, marker and sound references)
// ---------------------------------------------------------------------------------------

GENRESULT SceneScriptReferenceSetPosition(FSceneScriptComponent* Target, float* Position)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->SetPosition(Position);
}

GENRESULT SceneScriptReferenceSetOrientation(FSceneScriptComponent* Target, float* Orient9)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->SetOrientation(Orient9);
}

GENRESULT SceneScriptReferenceGetPosition(FSceneScriptComponent* Target, float* OutPosition)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetPosition(OutPosition);
}

GENRESULT SceneScriptReferenceGetOrientation(FSceneScriptComponent* Target, float* OutOrient9)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetOrientation(OutOrient9);
}

GENRESULT SceneScriptReferenceSetTransform(FSceneScriptComponent* Target, float const* Xform12)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->SetTransform(Xform12);
}

GENRESULT SceneScriptReferenceGetTransform(FSceneScriptComponent* Target, float* OutXform12)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetTransform(OutXform12);
}

void* SceneScriptReferenceGetEngineInstance(FSceneScriptComponent* Target)
{
    if (Target == nullptr)
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    return Target->GetEngineInstance();
}

// ---------------------------------------------------------------------------------------
// SceneScript_LightReference
// ---------------------------------------------------------------------------------------

// . Base ctor, vptrs, then +0x6c = the routine(the field, 5, Desc->Name) --
// the snap's name, not the malloc'd copy (0x004534f2); none found: FDUMP(0x100002, "invalid
// light reference", ScriptLight.cpp, 0x177, Desc->Name).
FSceneScriptLightReference::FSceneScriptLightReference(Csys const& ScriptFrame, FGfScript* InOwner,
                                           FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Target(nullptr)
{
    Target = G_GfScriptManager.FindCreatedEntity(kSceneScriptEntityLight, Desc->Name);
    if (Target == nullptr)
        FDUMP(0x100002, "%s(%d) : *** WARNING: invalid light reference to name %s",
              GScriptLightFile, 0x177, Desc->Name);
}

// / the branch -- identity 11 / 12 of the target, -1 without one.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetTransform(float const* Transform12)
{
    return SceneScriptReferenceSetTransform(Target, Transform12);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetTransform(float* OutTransform12)
{
    return SceneScriptReferenceGetTransform(Target, OutTransform12);
}

// / the branch / the branch / the branch -- geo 3..6 of the target.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetPosition(float* Position)
{
    return SceneScriptReferenceSetPosition(Target, Position);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetOrientation(float* Orient9)
{
    return SceneScriptReferenceSetOrientation(Target, Orient9);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetPosition(float* OutPosition)
{
    return SceneScriptReferenceGetPosition(Target, OutPosition);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetOrientation(float* OutOrient9)
{
    return SceneScriptReferenceGetOrientation(Target, OutOrient9);
}

// .. the branch -- facet +0x58 (= +0x6c) non-null: the target's D3D-light facet
// (+0x14), same slot; otherwise -1.
GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetOn(int On)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return static_cast<ID3DLightControl*>(Target)->SetOn(On);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetLight(void const* D3dLight8)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->SetLight(D3dLight8);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::SetDirection(float* Dir3)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->SetDirection(Dir3);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetOn(int* OutOn)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetOn(OutOn);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetLight(void* OutD3dLight8)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetLight(OutD3dLight8);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptLightReference::GetDirection(float* OutDir3)
{
    if (Target == nullptr)
        return kSceneScriptReferenceMissing;
    return Target->GetDirection(OutDir3);
}

// .
void FSceneScriptLightReference::Stop()
{
    Target = nullptr;
}

// the target's identity slot 17, or -1.
void* FSceneScriptLightReference::GetEngineInstance()
{
    return SceneScriptReferenceGetEngineInstance(Target);
}
