// Author: Giperion.

#include "fl_fx_mesh.h"

#include "client_cliobj.h"     // Archetype::ReleaseMaterialEntrySlot (the routine)
#include "client_mesh_load.h"  // FMeshHolder (baseline ObjectHandle) and its FUN_00420xxx helpers

#include "particlefx.h"           // IFxMesh, IFxMeshFactory
#include "common_archetype.h"  // Archetype::Load
#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>

IFxMeshFactory* G_FLFxMeshFactory = nullptr;

namespace
{
    // baseline keeps the field as a NEURONCOMPONENTS smart pointer: static initializer the branch zeroes
    // it and registers the branch through atexit (the routine), which releases a held factory
    // through vt+8 and zeroes the slot. This guard's destructor is that atexit entry.
    struct FFxMeshFactoryAtExit
    {
        ~FFxMeshFactoryAtExit()
        {
            if (G_FLFxMeshFactory != nullptr)
            {
                G_FLFxMeshFactory->Release();
                G_FLFxMeshFactory = nullptr;
            }
        }
    };
    FFxMeshFactoryAtExit G_FxMeshFactoryAtExit;

    // 's destination: a length and 32 characters, 0x24 bytes.
    struct FFxMeshPath
    {
        unsigned Length; // +0x00
        char Text[32];   // +0x04
    };
    static_assert(sizeof(FFxMeshPath) == 0x24, "module state string is 0x24 bytes");

    // s_fx\misc\%s.3db_005d98d0.
    char const kFxMeshPathFormat[] = "fx\\misc\\%s.3db";

    // (cdecl Out, Format, ...): vsprintf into a 0x20-byte local, the length capped
    // at 0x1f, that many characters copied to Out+4 and terminated, the length stored at +0.
    void FxMeshPathFormat(FFxMeshPath* Out, char const* Format, ...)
    {
        char Buffer[0x20];
        va_list Args;
        va_start(Args, Format);
        vsprintf(Buffer, Format, Args);
        va_end(Args);
        size_t Length = strlen(Buffer);
        if (Length >= 0x20)
            Length = 0x1f;
        memcpy(Out->Text, Buffer, Length);
        Out->Text[Length] = '\0';
        Out->Length = static_cast<unsigned>(Length);
    }

    // The two render-state guards the routine keeps on its stack (EH states 0 and 1): the
    // pipeline, the state and its value read at construction (IRenderPipeline slot 61 +0xf4),
    // written back on destruction (slot 60 +0xf0).
    struct FFxMeshRenderState
    {
        IRenderPipeline* Pipe;
        int State;
        int Saved;

        explicit FFxMeshRenderState(int InState)
            : Pipe(NeuronRuntime::Pipeline), State(InState), Saved(0)
        {
            Pipe->GetRenderState((D3DRENDERSTATETYPE)State, &Saved);
        }

        ~FFxMeshRenderState()
        {
            Pipe->SetRenderState((D3DRENDERSTATETYPE)State, Saved);
        }
    };

    struct FLFxMesh : IFxMesh, FMeshHolder
    {
        ~FLFxMesh() override;

        GENRESULT NEURONCOMPONENTS_API Slot3(void* Transform) override;
        float* NEURONCOMPONENTS_API Slot4() override;
        GENRESULT NEURONCOMPONENTS_API Slot5(void* Arg) override;
        GENRESULT NEURONCOMPONENTS_API Slot6(void* Camera) override;
        GENRESULT NEURONCOMPONENTS_API Slot7() override;
        GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) override;
    };
    static_assert(offsetof(FLFxMesh, EngineInstance) == 0x08, "FLFxMesh +0x04 ObjectHandle base");
    static_assert(sizeof(FLFxMesh) == 0x14, "FLFxMesh base is 0x14");

    struct FLFxMeshComponent : FLFxMesh
    {
        unsigned long RefCount; // +0x14 NeuronComponent<FLFxMesh>

        FLFxMeshComponent() : RefCount(1) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
    };
    static_assert(offsetof(FLFxMeshComponent, EngineInstance) == 0x08, "FLFxMeshComponent instance +0x08");
    static_assert(offsetof(FLFxMeshComponent, RefCount) == 0x14, "FLFxMeshComponent reference count +0x14");
    static_assert(sizeof(FLFxMeshComponent) == 0x18, "FLFxMeshComponent is operator new(0x18)");

    FLFxMesh::~FLFxMesh()
    {
        // (entered on the ObjectHandle face): the abstract vtables (005d991c /
        // ), then the routine called directly (not through the vtable), then
        // on the handle, which only restores its own vtable.
        FLFxMesh::Slot7();
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMeshComponent::QueryInterface(const char* Iid, void** Out)
    {
        // the one-entry table at 005d9960 {"1.11_IFxMesh" (005d9958), offset 0}.
        // A hit AddRefs the found face through its slot 1 and hands it out; a miss nulls *Out
        // and returns 0xfffffffd.
        if (strcmp(Iids::FxMesh, Iid) == 0)
        {
            AddRef();
            *Out = static_cast<IFxMesh*>(this);
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FLFxMeshComponent::AddRef()
    {
        // .
        RefCount += 1;
        return RefCount;
    }

    unsigned long NEURONCOMPONENTS_API FLFxMeshComponent::Release()
    {
        // an unsigned decrement that stops at zero; at zero the count is set back
        // to 1 and the ObjectHandle face's slot 0 (the routine, the scalar deleting destructor)
        // runs with 1, returning 0.
        if (RefCount > 0)
            RefCount -= 1;
        if (RefCount != 0)
            return RefCount;
        RefCount = 1;
        delete this;
        return 0;
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMesh::Slot3(void* Transform)
    {
        // (RET 8): the routine on the handle.
        ClientMeshHolderSetMatrix(this, Transform);
        return GR_OK;
    }

    float* NEURONCOMPONENTS_API FLFxMesh::Slot4()
    {
        // (RET 4): the routine on the handle, its result returned.
        return ClientMeshHolderGetTransform(this);
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMesh::Slot5(void* Arg)
    {
        // (RET 8): NeuronRuntime::Engine slot 27 UpdateInstance(+0x08, 0, Arg).
        NeuronRuntime::Engine->UpdateInstance(EngineInstance, 0, Arg);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMesh::Slot6(void* Camera)
    {
        // (RET 8). 0x004f6988..0x004f69bf: D3DRS_ZWRITEENABLE (0xe) saved and set
        // to 0.
        FFxMeshRenderState ZWrite(0xe);
        ZWrite.Pipe->SetRenderState((D3DRENDERSTATETYPE)ZWrite.State, 0);

        // D3DRS_ZFUNC (0x17) saved and set to 4 (D3DCMP_LESSEQUAL).
        FFxMeshRenderState ZFunc(0x17);
        ZFunc.Pipe->SetRenderState((D3DRENDERSTATETYPE)ZFunc.State, 4);

        // the routine on the handle with the camera and 0x100.
        ClientMeshHolderRender(this, Camera, 0x100u);

        // the guards restore ZFUNC, then ZWRITEENABLE.
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMesh::Slot7()
    {
        // (RET 4): the routine on the material entry (+0x0c; the auxiliary one at
        // +0x10 is left alone), then a bound instance (+0x08 != -1) destroyed through
        // NeuronRuntime::Engine slot 24 and set back to -1.
        Archetype::ReleaseMaterialEntrySlot(&MaterialEntry);
        void* const NoInstance = reinterpret_cast<void*>(static_cast<ptrdiff_t>(-1));
        if (EngineInstance != NoInstance)
        {
            NeuronRuntime::Engine->DestroyInstance(EngineInstance);
            EngineInstance = NoInstance;
        }
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMesh::Slot8(void* /*Arg*/)
    {
        // `OR EAX,0xffffffff; RET 8`.
        return GR_GENERIC;
    }

    struct FLFxMeshFactory : IFxMeshFactory
    {
        virtual ~FLFxMeshFactory() {} // ; private implementation slot 4.

        GENRESULT NEURONCOMPONENTS_API Slot3(void* ArgA, void* ArgB, float const* Position,
                                  IFxMesh** Out) override;
    };
    static_assert(sizeof(FLFxMeshFactory) == 0x4, "FLFxMeshFactory abstract base is 0x4");

    struct FLFxMeshFactoryComponent : FLFxMeshFactory
    {
        unsigned long RefCount; // +0x04 NeuronComponent<FLFxMeshFactory>

        FLFxMeshFactoryComponent() : RefCount(1) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
    };
    static_assert(offsetof(FLFxMeshFactoryComponent, RefCount) == 0x4, "FLFxMeshFactoryComponent reference count +0x4");
    static_assert(sizeof(FLFxMeshFactoryComponent) == 0x8, "FLFxMeshFactoryComponent is operator new(8)");

    GENRESULT NEURONCOMPONENTS_API FLFxMeshFactoryComponent::QueryInterface(const char* Iid, void** Out)
    {
        // the one-entry table at 005d9990 {"1.11_IFxMeshFactory" (005d9978),
        // offset 0}; the same hit / miss handling as FLFxMesh's.
        if (strcmp(Iids::FxMeshFactory, Iid) == 0)
        {
            AddRef();
            *Out = static_cast<IFxMeshFactory*>(this);
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FLFxMeshFactoryComponent::AddRef()
    {
        // (shared with other NeuronComponent<> instantiations whose count is at +4).
        RefCount += 1;
        return RefCount;
    }

    unsigned long NEURONCOMPONENTS_API FLFxMeshFactoryComponent::Release()
    {
        // as FLFxMesh::Release, but the destructor is this face's own slot 4
        // (the routine: the routine restores the abstract vtable 005d9944, then the free).
        if (RefCount > 0)
            RefCount -= 1;
        if (RefCount != 0)
            return RefCount;
        RefCount = 1;
        delete this;
        return 0;
    }

    GENRESULT NEURONCOMPONENTS_API FLFxMeshFactory::Slot3(void* /*ArgA*/, void* /*ArgB*/,
                                               float const* Position, IFxMesh** Out)
    {
        // (RET 0x14). ArgA and ArgB (particlefx's appearance +0x60 / +0x74) are never
        // read. 0x004f682b..0x004f6868: operator new(0x18) and the inlined constructor; a failed
        // allocation leaves a null mesh that the success path below would dereference, as in
        // baseline.
        FLFxMeshComponent* const Mesh = new (std::nothrow) FLFxMeshComponent;

        // the path. baseline formats "fx\misc\%s.3db" with the path
        // object itself -- still empty (length 0) -- copied by value as the variadic argument,
        // so %s reads its first dword, 0, as the string: the name is always "(null)". The port
        // passes the object the same way.
        FFxMeshPath Path;
        Path.Length = 0;
        Path.Text[0] = '\0';
        FxMeshPathFormat(&Path, kFxMeshPathFormat, Path);

        // Archetype::Load(path, &handle, 0); failure returns 0 with *Out
        // untouched and the new mesh never released.
        long ArchHandle = 0;
        if (!Archetype::Load(Path.Text, &ArchHandle, nullptr))
            return GR_OK;

        // *Out = the mesh; the routine(path, 0) on its handle,
        // (handle), the routine(Position), then NeuronRuntime::Engine slot 15
        // ReleaseHandle on the archetype handle.
        *Out = Mesh;
        void* const Loaded = reinterpret_cast<void*>(static_cast<ptrdiff_t>(ArchHandle));
        ClientMeshHolderSetMaterials(Mesh, Path.Text, nullptr);
        ClientMeshHolderBindInstance(Mesh, Loaded);
        ClientMeshHolderSetPosition(Mesh, Position);
        NeuronRuntime::Engine->ReleaseHandle(Loaded);
        return GR_OK;
    }
}

bool FLFxMeshFactoryCreate(IFxMeshFactory** Out)
{
    // operator new(8); non-null -> vtable 005d98bc, +4 = 1, *Out = it; null ->
    // *Out = 0. AL = 1 on both paths.
    FLFxMeshFactoryComponent* const Factory = new (std::nothrow) FLFxMeshFactoryComponent;
    *Out = Factory;
    return true;
}
