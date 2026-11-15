// Author: Giperion.

#include "gf_scenescript_camera.h"

#include "neuroncomponents.h"
#include "gf_script.h"
#include "room_camera.h"

#include <cstdint>
#include <cstring>

namespace
{
    char const* const GScriptCameraFile = "Client\\Gf\\ScriptCamera.cpp";
    char const* const GInvalidCameraReference =
        "%s(%d) : *** ERROR: Using invalid camera reference %s\n";

    GENRESULT const kSceneScriptReferenceMissing = static_cast<GENRESULT>(-1);

    // The pool slot is indexed without a range check by every body below (`SHL reg,6;
    // ADD reg,the field`). A slot of -1 -- Allocate found the 256 slots full -- would reach
    // the 0x40 bytes before the pool, exactly as baseline does.
    FGfCameraSlot& PoolSlotAt(int Slot)
    {
        return G_GfCameraPool.Slots[Slot];
    }

    // The head of the branch / the branch: to world through the script frame, into the slot,
    // when this slot is the field.
    void StorePoolPosition(Csys const& Frame, int Slot, float const* Local3)
    {
        Vector const World = Frame.trans_from_frame(*reinterpret_cast<Vector const*>(Local3));
        FGfCameraSlot& Target = PoolSlotAt(Slot);
        Target.Pos[0] = World.x;
        Target.Pos[1] = World.y;
        Target.Pos[2] = World.z;
        if (Slot == G_GfCameraPool.ActiveIndex)
            RoomCameraSetPosition(Target.Camera, Target.Pos);
    }

    // / the tail of the branch: the same for the orientation (REP MOVSD 9),
    // .
    void StorePoolOrientation(Csys const& Frame, int Slot, float const* Local9)
    {
        Matrix const World = Frame.trans_from_frame(*reinterpret_cast<Matrix const*>(Local9));
        FGfCameraSlot& Target = PoolSlotAt(Slot);
        memcpy(Target.Orient, &World, sizeof(Target.Orient));
        if (Slot == G_GfCameraPool.ActiveIndex)
            RoomCameraSetOrientation(Target.Camera, Target.Orient);
    }

    void LoadPoolPosition(Csys const& Frame, int Slot, float* Out3)
    {
        FGfCameraSlot const& Source = PoolSlotAt(Slot);
        Vector const Local = Frame.trans_to_frame(*reinterpret_cast<Vector const*>(Source.Pos));
        Out3[0] = Local.x;
        Out3[1] = Local.y;
        Out3[2] = Local.z;
    }

    void LoadPoolOrientation(Csys const& Frame, int Slot, float* Out9)
    {
        FGfCameraSlot const& Source = PoolSlotAt(Slot);
        Matrix const Local = Frame.trans_to_frame(*reinterpret_cast<Matrix const*>(Source.Orient));
        memcpy(Out9, &Local, sizeof(float) * 9);
    }

    GENRESULT ReportInvalidReference(char const* Name, int Line)
    {
        FDUMP(0x100001, GInvalidCameraReference, GScriptCameraFile, Line, Name);
        return kSceneScriptReferenceMissing;
    }
}

// ---------------------------------------------------------------------------------------
// SceneScript_Camera
// ---------------------------------------------------------------------------------------

// . 0x004507b4 base ctor, 0x004507b9..0x004507f0 the nine vptrs (the compiler's),
// +0x6c = -1.
FSceneScriptCamera::FSceneScriptCamera(Csys const& ScriptFrame, FGfScript* InOwner,
                           FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      PoolSlot(-1)
{
}

// . 0x0045083f..0x00450847: PUSH bCanBeActive, PUSH Camera, PUSH Priority, ECX =
// the pool; 0x0045084c the answer into +0x6c.
void FSceneScriptCamera::Register(FRoomCamera* Camera, int Priority, unsigned char bCanBeActive)
{
    PoolSlot = G_GfCameraPool.Allocate(Priority, Camera, bCanBeActive);
}

// (identity, RET 8). The position (Transform + 0x24) first -- trans_from_frame,
// slot +0x04, the routine when active (0x0045094f..0x00450998) -- then the rotation
// (Transform + 0), slot +0x10, the routine when active (0x0045099b..0x004509da). 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetTransform(float const* Transform12)
{
    StorePoolPosition(Frame, PoolSlot, Transform12 + 9);
    StorePoolOrientation(Frame, PoolSlot, Transform12);
    return GR_OK;
}

// (identity, RET 8). trans_to_frame of the slot position into Out + 0x24
// (0x00450aaa..0x00450af3), then of the slot orientation into Out + 0 (0x00450af6..0x00450b28).
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetTransform(float* OutTransform12)
{
    LoadPoolPosition(Frame, PoolSlot, OutTransform12 + 9);
    LoadPoolOrientation(Frame, PoolSlot, OutTransform12);
    return GR_OK;
}

// (geo facet, RET 8). Facet +0x20 = the frame, +0x60 = the slot.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetPosition(float* Position)
{
    StorePoolPosition(Frame, PoolSlot, Position);
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetOrientation(float* Orient9)
{
    StorePoolOrientation(Frame, PoolSlot, Orient9);
    return GR_OK;
}

// (geo facet, RET 8). The slot position copied to the stack, trans_to_frame, out.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetPosition(float* OutPosition)
{
    LoadPoolPosition(Frame, PoolSlot, OutPosition);
    return GR_OK;
}

// (geo facet, RET 8).
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetOrientation(float* OutOrient9)
{
    LoadPoolOrientation(Frame, PoolSlot, OutOrient9);
    return GR_OK;
}

// (camera facet, RET 8). Facet +0x5c = the slot. Slot +0x34, then the routine
// with the value when the slot is the field.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetFov(const float* Fov)
{
    FGfCameraSlot& Slot = PoolSlotAt(PoolSlot);
    float Value;
    std::memcpy(&Value, Fov, sizeof(Value));
    std::memcpy(&Slot.HalfFovXDeg, &Value, sizeof(Value));
    if (PoolSlot == G_GfCameraPool.ActiveIndex)
        RoomCameraSetHalfFovXValue(Slot.Camera, &Value);
    return GR_OK;
}

// slot +0x38, the routine.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetNear(const float* Near)
{
    FGfCameraSlot& Slot = PoolSlotAt(PoolSlot);
    float Value;
    std::memcpy(&Value, Near, sizeof(Value));
    std::memcpy(&Slot.ZNear, &Value, sizeof(Value));
    if (PoolSlot == G_GfCameraPool.ActiveIndex)
        RoomCameraSetZNearValue(Slot.Camera, &Value);
    return GR_OK;
}

// slot +0x3c, the routine.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::SetFar(const float* Far)
{
    FGfCameraSlot& Slot = PoolSlotAt(PoolSlot);
    float Value;
    std::memcpy(&Value, Far, sizeof(Value));
    std::memcpy(&Slot.ZFar, &Value, sizeof(Value));
    if (PoolSlot == G_GfCameraPool.ActiveIndex)
        RoomCameraSetZFarValue(Slot.Camera, &Value);
    return GR_OK;
}

// / the branch / the branch -- FLD slot +0x34 / +0x38 / +0x3c, FSTP [Out], 0.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetFov(float* OutFov)
{
    *OutFov = PoolSlotAt(PoolSlot).HalfFovXDeg;
    return GR_OK;
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetNear(float* OutNear)
{
    *OutNear = PoolSlotAt(PoolSlot).ZNear;
    return GR_OK;
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptCamera::GetFar(float* OutFar)
{
    *OutFar = PoolSlotAt(PoolSlot).ZFar;
    return GR_OK;
}

// G_GfCameraPool.Free(+0x6c). +0x6c keeps its value.
void FSceneScriptCamera::Stop()
{
    G_GfCameraPool.Free(PoolSlot);
}

// OR EAX,-1 (shared by /OPT:ICF with every "-1" getter of the exe).
void* FSceneScriptCamera::GetEngineInstance()
{
    return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
}

// ---------------------------------------------------------------------------------------
// SceneScript_CameraReference -- every forwarder: +0x6c null -> FDUMP(0x100001, "Using invalid
// camera reference", ScriptCamera.cpp, line, Name) and -1; otherwise a tail JMP into the
// target's same facet slot with the target's facet as `this`.
// ---------------------------------------------------------------------------------------

// . Base ctor, vptrs, 0x00450cb7 +0x6c = 0.
FSceneScriptCameraReference::FSceneScriptCameraReference(Csys const& ScriptFrame, FGfScript* InOwner,
                                             FSceneScriptEntitySnap const* Desc)
    : FSceneScriptComponent(ScriptFrame, InOwner, Desc),
      Target(nullptr)
{
}

// identity slot 11 (+0x2c), line 0x226.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetTransform(float const* Transform12)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x226);
    return Target->SetTransform(Transform12);
}

// identity slot 12 (+0x30), line 0x24d.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetTransform(float* OutTransform12)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x24d);
    return Target->GetTransform(OutTransform12);
}

// geo slot 3 (target + 0xc, +0xc), line 0x20c.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetPosition(float* Position)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x20c);
    return Target->SetPosition(Position);
}

// geo slot 4, line 0x219.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetOrientation(float* Orient9)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x219);
    return Target->SetOrientation(Orient9);
}

// geo slot 5, line 0x233.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetPosition(float* OutPosition)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x233);
    return Target->GetPosition(OutPosition);
}

// geo slot 6, line 0x240.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetOrientation(float* OutOrient9)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x240);
    return Target->GetOrientation(OutOrient9);
}

// camera slot 3 (target + 0x10, +0xc), line 0x25a.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetFov(const float* Fov)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x25a);
    return Target->SetFov(Fov);
}

// camera slot 6, line 0x267.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetNear(const float* Near)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x267);
    return Target->SetNear(Near);
}

// camera slot 7, line 0x274.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::SetFar(const float* Far)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x274);
    return Target->SetFar(Far);
}

// camera slot 8, line 0x281.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetFov(float* OutFov)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x281);
    return Target->GetFov(OutFov);
}

// camera slot 11, line 0x28e.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetNear(float* OutNear)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x28e);
    return Target->GetNear(OutNear);
}

// camera slot 12, line 0x29b.
GENRESULT NEURONCOMPONENTS_API FSceneScriptCameraReference::GetFar(float* OutFar)
{
    if (Target == nullptr)
        return ReportInvalidReference(Name, 0x29b);
    return Target->GetFar(OutFar);
}

// +0x6c = 0 (shared with the light, marker and sound references).
void FSceneScriptCameraReference::Stop()
{
    Target = nullptr;
}

// the target's identity slot 17, or FDUMP line 0x1ff and -1.
void* FSceneScriptCameraReference::GetEngineInstance()
{
    if (Target == nullptr)
    {
        ReportInvalidReference(Name, 0x1ff);
        return reinterpret_cast<void*>(static_cast<intptr_t>(-1));
    }
    return Target->GetEngineInstance();
}
