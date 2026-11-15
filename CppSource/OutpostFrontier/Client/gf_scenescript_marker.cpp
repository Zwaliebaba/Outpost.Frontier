// Author: Giperion.

#include "gf_scenescript_marker.h"

#include "gf_scenescript_light.h" // the reference forwarders shared with the light reference

#include <cstdint>
#include <cstring>

// ---------------------------------------------------------------------------------------
// SceneScript_Marker
// ---------------------------------------------------------------------------------------

// . Base ctor, vptrs, then +0x6c..+0x74 = 0 (0x004536ed..0x00453718) and +0x78 the
// identity (0x0045371b..0x0045373a).
FSceneScriptMarker::FSceneScriptMarker(Csys const& ScriptFrame, FGfScript* InOwner,
                           FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc)
{
    World.Pos.x = 0.0f;
    World.Pos.y = 0.0f;
    World.Pos.z = 0.0f;
    MatrixSetIdentity(World.Orient);
}

// (identity, RET 8). trans_from_frame of Transform+0x24 into +0x6c, then of the
// rotation into +0x78.
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::SetTransform(float const* Transform12)
{
    World.Pos = Frame.trans_from_frame(*reinterpret_cast<Vector const*>(Transform12 + 9));
    World.Orient = Frame.trans_from_frame(*reinterpret_cast<Matrix const*>(Transform12));
    return GR_OK;
}

// (identity, RET 8). trans_to_frame of +0x6c into Out+0x24, of +0x78 into Out+0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::GetTransform(float* OutTransform12)
{
    Vector const LocalPos = Frame.trans_to_frame(World.Pos);
    OutTransform12[9] = LocalPos.x;
    OutTransform12[10] = LocalPos.y;
    OutTransform12[11] = LocalPos.z;
    Matrix const LocalOrient = Frame.trans_to_frame(World.Orient);
    memcpy(OutTransform12, &LocalOrient, sizeof(float) * 9);
    return GR_OK;
}

// (geo facet, RET 8). Facet +0x20 = the frame, +0x60 = +0x6c.
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::SetPosition(float* Position)
{
    World.Pos = Frame.trans_from_frame(*reinterpret_cast<Vector const*>(Position));
    return GR_OK;
}

// (geo facet, RET 8). REP MOVSD 9 into facet +0x6c (= +0x78).
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::SetOrientation(float* Orient9)
{
    World.Orient = Frame.trans_from_frame(*reinterpret_cast<Matrix const*>(Orient9));
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::GetPosition(float* OutPosition)
{
    Vector const Local = Frame.trans_to_frame(World.Pos);
    OutPosition[0] = Local.x;
    OutPosition[1] = Local.y;
    OutPosition[2] = Local.z;
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptMarker::GetOrientation(float* OutOrient9)
{
    Matrix const Local = Frame.trans_to_frame(World.Orient);
    memcpy(OutOrient9, &Local, sizeof(float) * 9);
    return GR_OK;
}

// .
void* FSceneScriptMarker::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ---------------------------------------------------------------------------------------
// SceneScript_MarkerReference
// ---------------------------------------------------------------------------------------

// . Base ctor, vptrs, 0x00453967 +0x6c = 0.
FSceneScriptMarkerReference::FSceneScriptMarkerReference(Csys const& ScriptFrame, FGfScript* InOwner,
                                             FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Target(nullptr)
{
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::SetTransform(float const* Transform12)
{
    return SceneScriptReferenceSetTransform(Target, Transform12);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::GetTransform(float* OutTransform12)
{
    return SceneScriptReferenceGetTransform(Target, OutTransform12);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::SetPosition(float* Position)
{
    return SceneScriptReferenceSetPosition(Target, Position);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::SetOrientation(float* Orient9)
{
    return SceneScriptReferenceSetOrientation(Target, Orient9);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::GetPosition(float* OutPosition)
{
    return SceneScriptReferenceGetPosition(Target, OutPosition);
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptMarkerReference::GetOrientation(float* OutOrient9)
{
    return SceneScriptReferenceGetOrientation(Target, OutOrient9);
}

// .
void FSceneScriptMarkerReference::Stop()
{
    Target = nullptr;
}

// .
void* FSceneScriptMarkerReference::GetEngineInstance()
{
    return SceneScriptReferenceGetEngineInstance(Target);
}
