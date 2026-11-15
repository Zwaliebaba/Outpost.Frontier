// Author: Giperion.

#include "win_object.h"

#include "win3d_engine_submit.h"

#include "NeuronRuntime.h"
#include "render/IRenderPipeline.h"

WinObject::WinObject()
{
    // the Win3D part (inlined), the holder (the routine) with five 1.0f and
    // anim -1, the tint zeroed and bits 0..2 of +0xa4 cleared.
    ClientMeshHolderConstruct(&MeshHolder);
    MeshHolder.UserFloats[4] = 1.0f;
    MeshHolder.UserFloats[3] = 1.0f;
    MeshHolder.UserFloats[2] = 1.0f;
    MeshHolder.UserFloats[1] = 1.0f;
    MeshHolder.UserFloats[0] = 1.0f;
    MeshHolder.AnimOrArchIndex = -1;
    TintRgba[3] = 0;
    TintRgba[2] = 0;
    TintRgba[1] = 0;
    TintRgba[0] = 0;
    DrawFlags = static_cast<unsigned char>(DrawFlags & 0xf8);
}

WinObject::~WinObject()
{
    // nothing but the member dtor the routine (0x004c9259, ~FClientMeshHolder:
    // close the animation script, release a bound instance); the Win3D dtor runs as the base.
}

void WinObject::SetPosition(float const* NewPosition)
{
    // copy to +0x60, then tail-jump into the routine on the holder with the
    // same argument (Engine SetInstancePosition; no UpdateInstance).
    Position[0] = NewPosition[0];
    Position[1] = NewPosition[1];
    Position[2] = NewPosition[2];
    ClientMeshHolderSetPosition(&MeshHolder, NewPosition);
}

void WinObject::Destroy()
{
    // .
    if (ClientMeshHolderHasInstance(&MeshHolder))
        ClientUiMeshHolderRelease(&MeshHolder);
    Flags = static_cast<unsigned char>(Flags & 0xfc);
    for (Win3D* Child = ChildHead; Child != nullptr; Child = Child->NextSibling)
        Child->Destroy();
    Win3DUnregister(this);
}

void WinObject::Update()
{
    // the Win3D child walk (the routine inlined), then the routine on the
    // holder with this+0x3c -- Engine SetInstanceMatrix (+0x9c) from the orientation and
    // position, which are contiguous there.
    Win3D* Child = ChildHead;
    while (Child != nullptr)
    {
        Win3D* const Next = Child->NextSibling;
        if ((Child->Flags & kWin3DActive) != 0)
            Child->Update();
        else
            Child->UpdateInactive();
        Child = Next;
    }
    NeuronRuntime::Engine->SetInstanceMatrix(MeshHolder.EngineInstance, Orientation);
}

bool WinObject::LoadMeshAndCreate(Win3D* Parent, char const* MeshPath,
                                  char const* MaterialPath)
{
    // .
    if (!ClientMeshLoadFromPath(&MeshHolder, MeshPath, MaterialPath, false))
        return false;
    return Create(Parent, nullptr);
}

void WinObject::Draw()
{
    // .
    if ((Flags & kWin3DVisible) == 0)
        return;
    Win3DDrawChildren(this);

    // Plain pass unless bit1: render states 0x17 = 8 and 0xe = 1, then the routine.
    if ((DrawFlags & 2) == 0)
    {
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_ZFUNC, 8);
        NeuronRuntime::Pipeline->SetRenderState(D3DRS_ZWRITEENABLE, 1);
        SubmitWin3DEngineInstance(&MeshHolder, Win3DGetCamera(), 0, 1.0f, 0);
    }

    // Tinted pass when bit0: the routine with front culling, the +0xa5 tint and lighting
    // unless bit1.
    if ((DrawFlags & 1) != 0)
    {
        unsigned char const bLighting = static_cast<unsigned char>((~DrawFlags >> 1) & 1);
        SubmitWin3DTintedEngineInstanceWrapped(Win3DGetCamera(), MeshHolder.EngineInstance, 1,
                                               TintRgba, bLighting, 1.0f, 0);
    }
}
