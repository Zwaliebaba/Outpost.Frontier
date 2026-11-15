// Author: Giperion.

// Effect base — Client/space/Effect.cpp (the routine / 004f79f0 / 004f7a80 / 004f7d20,
// kill, the routine freeze-to-world, the routine world transform).
// Agent: cursor:equip-fx-factory.

#include "effect_base.h"

#include "beam_effect.h"
#include "effect_type_info.h"      // EffectTypeInfoByIndex (the routine)
#include "effect_type_list.h"
#include "effect_vertex_batch.h"   // G_EffectMathEngine (the field)
#include "fl_fx_mesh.h"            // G_FLFxMeshFactory (the field)
#include "space_camera.h"          // AsCamera (the routine)
#include "texture_shapes.h"        // G_EffectShapeDB (the field)

#include "NeuronRuntime.h"
#include "particlefx.h"               // IFxMeshFactory (Release)
#include "common_cobject.h"        // CObject::get_velocity (vt+0x54)
#include "common_csys.h"           // Acquire3DMathEngine (the routine)
#include "common_misc.h"
#include "engine/sceneruntime.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace
{
    // — the Common.dll Heap pool for 0x84-byte effects, {ElementSize 0x84,
    // FreeList 0, MaxFreeCount -1, CurrentFreeCount 0}, stored by the static initializer
    // (Heap ctor inlined there). the routine / the routine allocate and free
    // through it, the routine / the routine shrink and free into it.
    Heap G_EffectHeap(0x84u, -1);

    // — a second {0x84, 0, -1, 0} Heap set up by the static initializer
    // . Nothing in the exe allocates from it (its only other reference is the Shrink
    // at 0x004f721c); kept because the routine shrinks it.
    Heap G_EffectHeapSecond(0x84u, -1);
}

void EffectSystemShutdown()
{
    // (called from the client teardown at 0x005b228b).
    // every effect in every bucket is killed and released -- the same
    // loop as the routine.
    EffectTypeListKillAll();

    // the field (the FLFxMeshFactory the routine creates) released
    // through its vt+8 and cleared.
    if (G_FLFxMeshFactory != nullptr)
    {
        G_FLFxMeshFactory->Release();
        G_FLFxMeshFactory = nullptr;
    }

    // the routine on the field.
    G_EffectShapeDB.Clear();

    // Heap::Shrink on the field, then on the field.
    G_EffectHeap.Shrink();
    G_EffectHeapSecond.Shrink();
}

void* EffectAlloc(unsigned Size)
{
    // size 0x84 → JMP Heap::Malloc(the field); anything else → JMP malloc.
    if (Size != 0x84u)
        return malloc(Size);
    return G_EffectHeap.Malloc();
}

void EffectFree(void* Ptr, unsigned Size)
{
    // size 0x84 → Heap::Free(the field, Ptr); anything else → free(Ptr).
    if (Size != 0x84u)
    {
        free(Ptr);
        return;
    }
    G_EffectHeap.Free(Ptr);
}

void FEffectBase::operator delete(void* Memory, size_t Size)
{
    EffectFree(Memory, (unsigned)Size);
}

FEffectBase::FEffectBase(unsigned TypeId)
{
    EffectBaseConstruct(this, TypeId);
}

void EffectBaseConstruct(FEffectBase* This, unsigned TypeId)
{
    // — listing dword writes through +0x68.
    if (This == nullptr)
        return;
    std::memset((unsigned char*)This + sizeof(void*), 0, sizeof(FEffectBase) - sizeof(void*));
    This->Priority0C = 1.0f;
    // Identity-ish transform diagonals (listing 0x3f800000 at [3]/[4]/[8]/[0xc]).
    This->LocalXform.Rotation.D[0][0] = 1.0f;
    This->LocalXform.Rotation.D[1][1] = 1.0f;
    This->LocalXform.Rotation.D[2][2] = 1.0f;
    This->EngineIndex = -1;
    This->RefCount = 1;
    This->EffectTypeId = TypeId;
    This->LinkPrev = nullptr;
    This->LinkNext = nullptr;
    This->LinkSelf = nullptr;
}

void FEffectBase::AddRef()
{
    FEffectBase* const This = this;
    // MOV EAX,[ECX+0x58] / INC / MOV [ECX+0x58],EAX.
    This->RefCount = This->RefCount + 1;
}

void EffectAddRef(FEffectBase* This)
{
    This->AddRef();
}

int FEffectBase::Release()
{
    FEffectBase* const This = this;
    // DEC [+0x58]; JG keeps it; otherwise `PUSH 1; CALL [vtable+0x20]`.
    int const Next = static_cast<int>(This->RefCount) - 1;
    This->RefCount = static_cast<unsigned>(Next);
    if (Next < 1)
    {
        delete This;
    }
    return Next;
}

int EffectRelease(FEffectBase* This)
{
    return This->Release();
}

unsigned char FEffectBase::Start(FEffectArch* Arch)
{
    FEffectBase* const This = this;
    // .
    if (Arch != nullptr)
    {
        // `TEST ECX,ECX / JG` on the type id -- signed.
        if (static_cast<int>(This->EffectTypeId) <= 0)
            This->EffectTypeId = *(unsigned const*)Arch;
        // the routine with baseline's own null test.
        FEffectTypeInfo const* const Info =
            EffectTypeInfoByIndex(static_cast<int>(This->EffectTypeId));
        if (Info != nullptr)
            This->Priority0C = Info->Priority;
    }
    return 1;
}

unsigned char EffectBaseStart(FEffectBase* This, FEffectArch* Arch)
{
    return This->FEffectBase::Start(Arch);
}

void EffectBaseDestruct(FEffectBase* This)
{
    // only a non-empty watcher is reset (set_pointer(0)); set() makes
    // the same call once it sees the target differs.
    if (This->Watcher.target() != nullptr)
        This->Watcher.set(nullptr);
}

FEffectBase::~FEffectBase()
{
    EffectBaseDestruct(this);
}

void FEffectBase::SetIntensity(float /*Intensity*/)
{
    // Former slot +0x0c: an empty RET 4.
}

void FEffectBase::Attach(void* WatchableOwner, int EngineIndex, FTransform const* LocalXform)
{
    FEffectBase* const This = this;
    // (thiscall, RET 0xc). No null tests: the transform is copied with
    // REP MOVSD straight from the third argument.
    Watchable const* WatchablePtr = nullptr;
    if (WatchableOwner != nullptr)
        WatchablePtr =
            reinterpret_cast<Watchable const*>(static_cast<unsigned char*>(WatchableOwner) + 8);
    if (WatchablePtr != This->Watcher.target())
        This->Watcher.set(WatchablePtr);
    This->EngineIndex = EngineIndex;
    This->LocalXform = *LocalXform;
    // +0x4c/+0x50/+0x54 zeroed as three dwords (listing the routine).
    std::memset(This->Pad4C, 0, sizeof(This->Pad4C));
}

void EffectAttach(FEffectBase* This, void* WatchableOwner, int EngineIndex,
                  FTransform const* LocalXform)
{
    This->Attach(WatchableOwner, EngineIndex, LocalXform);
}

void* EffectWatchedObject(FEffectBase const* This)
{
    // `MOV EAX,[this+0x44] / TEST / ADD EAX,-8 / TEST`: both an empty watcher and one holding
    // exactly 8 read as "no object".
    unsigned char const* const Held =
        reinterpret_cast<unsigned char const*>(This->Watcher.target());
    if (Held == nullptr)
        return nullptr;
    return const_cast<unsigned char*>(Held - 8);
}

namespace
{
    // , lazily filled by the routine at every use site (0x004f7cbe, 0x004f7e9e,
    // , 0x004f863b).
    I3DMathEngine* EffectMathEngine()
    {
        if (G_EffectMathEngine == nullptr)
            G_EffectMathEngine = Acquire3DMathEngine();
        return G_EffectMathEngine;
    }

    // The body the routine runs once it knows the index is set and the watcher holds an
    // object; the routine (0x004f7c4a..0x004f7d03) and the branch (0x004f85c7..0x004f867f)
    // carry the same instructions inlined.
    void EffectBakeInstance(FEffectBase* This)
    {
        void* const Instance = reinterpret_cast<void*>(static_cast<intptr_t>(This->EngineIndex));

        // IEngine +0x80, then CObject vt+0x54 into a stack Vector, copied to +0x4c.
        float* const Velocity = reinterpret_cast<float*>(This->Pad4C);
        CObject* const Object = static_cast<CObject*>(NeuronRuntime::Engine->GetInstanceUserData(Instance));
        if (Object != nullptr)
        {
            Vector const V = Object->get_velocity();
            Velocity[0] = V.x;
            Velocity[1] = V.y;
            Velocity[2] = V.z;
        }
        else
        {
            Velocity[2] = 0.0f;   // +0x54, +0x50, +0x4c: baseline's store order
            Velocity[1] = 0.0f;
            Velocity[0] = 0.0f;
        }

        // The watcher and the index are tested again (the object may have been the one the
        // velocity call just touched); only then is the instance transform multiplied in.
        FTransform Product;
        FTransform const* Source = &This->LocalXform;
        if (EffectWatchedObject(This) != nullptr && This->EngineIndex != -1)
        {
            FTransform const* const InstanceXform = reinterpret_cast<FTransform const*>(
                NeuronRuntime::Engine->GetInstanceTransform(
                    reinterpret_cast<void*>(static_cast<intptr_t>(This->EngineIndex))));
            EffectMathEngine()->MulTransform(Product, *InstanceXform, This->LocalXform);
            Source = &Product;
        }

        // Two 0xc-dword copies through a stack temporary: source -> temp -> +0x10.
        FTransform const Temp = *Source;
        This->LocalXform = Temp;
        This->EngineIndex = -1;
    }
}

void EffectFreezeToWorld(FEffectBase* This)
{
    // .
    if (This->EngineIndex == -1)
        return;

    if (EffectWatchedObject(This) == nullptr)
    {
        // the routine inlined (link unless linked or dead, RefCount++).
        EffectTypeListLink(This);
        // `CALL the routine` on the same object. The index is still set and the
        // watcher still empty, so baseline recurses until the stack runs out if this is reached;
        // reproduced as written.
        EffectFreezeToWorld(This);
        This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
        return;
    }

    EffectBakeInstance(This);
}

void FEffectBase::Kill()
{
    FEffectBase* const This = this;
    // .
    EffectTypeListLink(This);      // inlined

    if (This->EngineIndex != -1)
    {
        if (EffectWatchedObject(This) == nullptr)
        {
            // `CALL the routine` on itself, then the flag. Same unbounded recursion
            // as the routine's; reproduced as written.
            EffectKill(This);
            This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
            return;
        }
        EffectBakeInstance(This);
    }

    This->Flags08 = static_cast<unsigned char>(This->Flags08 | 2u);
}

void EffectKill(FEffectBase* This)
{
    This->FEffectBase::Kill();
}

void EffectGetWorldTransform(FEffectBase const* This, FTransform* Out)
{
    // .
    if (EffectWatchedObject(This) != nullptr && This->EngineIndex != -1)
    {
        FTransform const* const InstanceXform = reinterpret_cast<FTransform const*>(
            NeuronRuntime::Engine->GetInstanceTransform(
                reinterpret_cast<void*>(static_cast<intptr_t>(This->EngineIndex))));
        FTransform Product;
        EffectMathEngine()->MulTransform(Product, *InstanceXform, This->LocalXform);
        *Out = Product;
        return;
    }
    *Out = This->LocalXform;
}

unsigned char EffectInCameraView(FEffectBase* This, FSpaceCamera* Cam)
{
    // (RET 4). 0x004f7b53..0x004f7b5f: the effect's own slot +8 -- world
    // position into a local, radius left on the FPU and stored as a float.
    FVector WorldPos;
    float const Radius = This->GetWorldPos(&WorldPos);

    // the camera's slot +0x40 (view transform), then I3DMathEngine
    // +0x28 (the field, lazily the routine) into a second local.
    ICamera* const CamIface = AsCamera(Cam);
    FTransform const* const ViewXform =
        reinterpret_cast<FTransform const*>(CamIface->GetViewTransform());
    FVector ViewPos;
    EffectMathEngine()->TransformXform(ViewPos, *ViewXform, WorldPos);

    // copied into a third local, the camera's slot +0x3c with it and
    // the radius; CMP EAX,3 / SETGE.
    FVector Centre = ViewPos;
    int const Class = CamIface->ClassifySphere(&Centre.X, Radius);
    return static_cast<unsigned char>(Class >= 3 ? 1 : 0);
}
