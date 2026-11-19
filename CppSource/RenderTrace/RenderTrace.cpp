// RenderTrace.dll (design/migration-plan.md P0.5): a logging proxy for the renderer.
//
// The DllMain registers the component "RenderTrace". Named in [System] instead of
// DirectX8, it creates the real DirectX8 renderer aggregated under itself and answers
// the renderer's seven interfaces (plus IAggregateComponent) with its own faces. Every
// method forwards to the real object with the same arguments and returns its result;
// RenderTraceGenerated.cpp holds the methods that only record their arguments, this
// file the component plumbing and the methods that track handles, locks and memory.
// README.md describes activation, the trace format and the replay contract.

#include "RenderTraceGenerated.h"
#include "readfile.h"

#include <new>
#include <utility>
#include <vector>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace
{
    const unsigned long kRegisterPriority = 0x40000000; // as DX8RENDERER registers DirectX8
    const unsigned long kAggDescSize = 0x14;
    void* const kMinusOne = (void*)((intptr_t)-1);

    // The 0x14-byte aggregate descriptor (system.cpp FSystemAggDesc, DX8RENDERER
    // FDx8RendererAggDesc): +8 the controlling unknown, +0xC where the inner unknown goes,
    // +0x10 the [System] line's text after '=' (the renderer's section name).
    struct FRenderTraceAggDesc
    {
        unsigned long Size;
        const char* Name;
        INeuronComponent* Outer;
        INeuronComponent** InnerOut;
        const char* Extra;
    };
    static_assert(sizeof(FRenderTraceAggDesc) == kAggDescSize, "aggregate descriptor is 0x14 bytes");

    static_assert(sizeof(D3DLIGHT8) == 26 * 4, "D3DLIGHT8 is 26 words");

    // The Direct3D 8 numbers of the 76 render states in DX8RENDERER's table
    // (kRenderStateProperties in DX8RENDERER.cpp, in its order; the Rust table
    // neuron-client render::ff::state::RenderStateId has the same numbers). GetRenderState
    // indexes the renderer's cache with the number unchecked, so only these are read.
    const unsigned int kDefaultRenderStates[] = {
        0x7,  0x8,  0x9,  0xA,  0xE,  0xF,  0x10, 0x13, 0x14, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C,
        0x1D, 0x1E, 0x22, 0x23, 0x24, 0x25, 0x26, 0x28, 0x2F, 0x30, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
        0x3A, 0x3B, 0x3C, 0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8B, 0x8C, 0x8D,
        0x8E, 0x8F, 0x91, 0x92, 0x93, 0x94, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0xA0,
        0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xAA, 0xAB, 0xAC, 0xAD,
    };
    const size_t kDefaultRenderStateCount = sizeof(kDefaultRenderStates) / sizeof(kDefaultRenderStates[0]);
    static_assert(sizeof(kDefaultRenderStates) / sizeof(kDefaultRenderStates[0]) == 76,
                  "DX8RENDERER's render-state table has 76 rows");
    // GetPipelineState answers ids 0 to 0xE.
    const size_t kPipelineIdCount = 15;

    // A selected frame's image larger than this is not read (a broken file).
    const long kMaxFrameImageBytes = 256L << 20;
    static_assert(sizeof(D3DMATERIAL8) == 17 * 4, "D3DMATERIAL8 is 17 words");

    bool IidEqual(const char* Left, const char* Right)
    {
        return Left != nullptr && Right != nullptr && strcmp(Left, Right) == 0;
    }

    // A surface's bytes, or null with a note when its size is not known.
    void SurfaceData(RenderTrace::FCall& Call, const char* Name, const void* Bits, unsigned int Format,
                     unsigned int Width, unsigned int Height, unsigned int Pitch)
    {
        const size_t Size = RenderTrace::SurfaceBytes(Format, Width, Height, Pitch);
        if (Bits == nullptr || Size == 0)
        {
            Call.Null(Name);
            if (Bits != nullptr)
                Call.Note("surface size unknown for this format; data not read");
            return;
        }
        Call.Bytes(Name, Bits, Size);
    }

    class FRenderTraceFactory final : public IComponentFactory
    {
    public:
        FRenderTraceFactory() : RefCount(1) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
        GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;

    private:
        unsigned long RefCount;
    };

    FRenderTraceFactory* GFactory = nullptr;

    GENRESULT NEURONCOMPONENTS_API FRenderTraceFactory::QueryInterface(const char* Iid, void** Out)
    {
        if (Out == nullptr)
            return GR_GENERIC;
        *Out = nullptr;
        if (!IidEqual(Iid, Iids::ComponentFactory))
            return GR_INTERFACE_UNSUPPORTED;
        *Out = (IComponentFactory*)(this);
        AddRef();
        return GR_OK;
    }

    unsigned long NEURONCOMPONENTS_API FRenderTraceFactory::AddRef()
    {
        return ++RefCount;
    }

    unsigned long NEURONCOMPONENTS_API FRenderTraceFactory::Release()
    {
        if (RefCount > 0)
            --RefCount;
        const unsigned long Left = RefCount;
        if (Left == 0)
        {
            if (GFactory == this)
                GFactory = nullptr;
            delete this;
        }
        return Left;
    }

    // As FDx8RendererFactory::CreateInstance: the 0x14-byte descriptor whose name is
    // exactly "RenderTrace"; with an outer unknown the object is aggregated and its inner
    // unknown goes to InnerOut, without one it controls itself.
    GENRESULT NEURONCOMPONENTS_API FRenderTraceFactory::CreateInstance(AGGDESC* Desc, void** Instance)
    {
        if (Instance == nullptr)
            return GR_INVALID_PARMS;
        *Instance = nullptr;
        FRenderTraceAggDesc* Ext = (FRenderTraceAggDesc*)(Desc);
        if (Ext == nullptr || Ext->Size != kAggDescSize || Ext->Name == nullptr ||
            strcmp(Ext->Name, kRenderTraceComponentName) != 0)
            return GR_INTERFACE_UNSUPPORTED;

        FRenderTrace* Object = new (std::nothrow) FRenderTrace();
        if (Object == nullptr)
            return GR_OUT_OF_MEMORY;
        if (Ext->Outer != nullptr)
        {
            Object->Outer_ = Ext->Outer;
            if (Ext->InnerOut != nullptr)
                *Ext->InnerOut = &Object->Inner_;
        }
        else
        {
            Object->Outer_ = &Object->Inner_;
        }

        const GENRESULT Result = Object->CreateReal(Ext->Extra);
        if (Result != GR_OK)
        {
            if (Ext->Outer != nullptr && Ext->InnerOut != nullptr)
                *Ext->InnerOut = nullptr;
            delete Object;
            return Result;
        }
        *Instance = (IRenderPipeline*)(Object);
        return GR_OK;
    }
}

BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID /*Reserved*/)
{
    if (Reason != DLL_PROCESS_ATTACH)
        return TRUE;
    DisableThreadLibraryCalls(Module);
    ICOManager* NeuronComponents = NEURONCOMPONENTS_Acquire();
    if (NeuronComponents == nullptr)
        return TRUE;
    GFactory = new (std::nothrow) FRenderTraceFactory();
    if (GFactory == nullptr)
        return TRUE;
    NeuronComponents->RegisterComponent(GFactory, kRenderTraceComponentName, (void*)(kRegisterPriority));
    GFactory->Release();
    return TRUE;
}

// -------------------------------------------------------------------------------------------
// Inner unknown

GENRESULT NEURONCOMPONENTS_API FRenderTraceInner::QueryInterface(const char* Iid, void** Out)
{
    if (Owner == nullptr)
    {
        if (Out != nullptr)
            *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }
    return Owner->QueryLocal(Iid, Out);
}

unsigned long NEURONCOMPONENTS_API FRenderTraceInner::AddRef()
{
    return ++RefCount;
}

unsigned long NEURONCOMPONENTS_API FRenderTraceInner::Release()
{
    if (RefCount > 0)
        --RefCount;
    if (RefCount != 0)
        return RefCount;
    // Stays referenced while the owner (which holds this object) is torn down.
    RefCount = 1;
    FRenderTrace* Object = Owner;
    Owner = nullptr;
    delete Object;
    return 0;
}

// -------------------------------------------------------------------------------------------
// Component

FRenderTrace::FRenderTrace()
    : Outer_(nullptr), RealInner_(nullptr), RealPipeline_(nullptr), RealVbMgr_(nullptr), RealDraw_(nullptr),
      RealIb_(nullptr), RealVb_(nullptr), RealGamma_(nullptr), RealTexture_(nullptr), RealAggregate_(nullptr)
{
    Inner_.Owner = this;
}

FRenderTrace::~FRenderTrace()
{
    Trace_.Close();
    RealPipeline_ = nullptr;
    RealVbMgr_ = nullptr;
    RealDraw_ = nullptr;
    RealIb_ = nullptr;
    RealVb_ = nullptr;
    RealGamma_ = nullptr;
    RealTexture_ = nullptr;
    RealAggregate_ = nullptr;
    // The faces above hold no reference (see CreateReal); the inner unknown holds the one
    // that keeps the real renderer alive, and its release destroys it.
    if (RealInner_ != nullptr)
    {
        INeuronComponent* Inner = RealInner_;
        RealInner_ = nullptr;
        Inner->Release();
    }
}

// Creates "DirectX8" with this object as its controlling unknown, as the [System] container
// creates its children: the real renderer's QueryInterface, AddRef and Release then reach
// this object's, so the faces it hands out are always the proxy's. Each face obtained here
// was AddRef'd on the controlling unknown; that reference is given back at once, as COM
// aggregation does for interfaces an outer object caches from its inner one.
GENRESULT FRenderTrace::CreateReal(const char* Section)
{
    ICOManager* NeuronComponents = NEURONCOMPONENTS_Acquire();
    if (NeuronComponents == nullptr)
        return GR_GENERIC;

    FRenderTraceAggDesc Desc;
    Desc.Size = kAggDescSize;
    Desc.Name = kDx8RendererComponentName;
    Desc.Outer = (IRenderPipeline*)(this);
    Desc.InnerOut = &RealInner_;
    Desc.Extra = Section;
    void* RealObject = nullptr;
    GENRESULT Result = NeuronComponents->CreateInstance((AGGDESC*)(&Desc), &RealObject);
    if (Result != GR_OK || RealInner_ == nullptr)
    {
        if (FDUMP != nullptr)
            FDUMP(0x100002, "%s(%d) : WARNING:General:%s", "RenderTrace\\RenderTrace.cpp", __LINE__,
                  "RenderTrace: cannot create DirectX8 (is DX8RENDERER.dll in [Libraries]?)");
        if (RealInner_ != nullptr)
        {
            RealInner_->Release();
            RealInner_ = nullptr;
        }
        return Result != GR_OK ? Result : GR_GENERIC;
    }

    struct FFace
    {
        const char* Iid;
        void** Slot;
    };
    const FFace Faces[] = {
        {Iids::RenderPipeline8B, (void**)(&RealPipeline_)},
        {Iids::VertexBufferManager, (void**)(&RealVbMgr_)},
        {Iids::RPDraw, (void**)(&RealDraw_)},
        {Iids::RPIndexBuffer, (void**)(&RealIb_)},
        {Iids::RPVertexBuffer, (void**)(&RealVb_)},
        {Iids::GammaControl, (void**)(&RealGamma_)},
        {Iids::RPTexture, (void**)(&RealTexture_)},
        {Iids::AggregateComponent, (void**)(&RealAggregate_)},
    };
    for (size_t Index = 0; Index < sizeof(Faces) / sizeof(Faces[0]); ++Index)
    {
        void* Face = nullptr;
        if (RealInner_->QueryInterface(Faces[Index].Iid, &Face) != GR_OK || Face == nullptr)
        {
            for (size_t Clear = 0; Clear < sizeof(Faces) / sizeof(Faces[0]); ++Clear)
                *Faces[Clear].Slot = nullptr;
            INeuronComponent* Inner = RealInner_;
            RealInner_ = nullptr;
            Inner->Release();
            return GR_INTERFACE_UNSUPPORTED;
        }
        *Faces[Index].Slot = Face;
        Outer_->Release();
    }

    Trace_.Configure(Section);
    return GR_OK;
}

GENRESULT FRenderTrace::QueryLocal(const char* Iid, void** Out)
{
    if (Out == nullptr)
        return GR_GENERIC;
    *Out = nullptr;
    INeuronComponent* Face = nullptr;
    if (IidEqual(Iid, Iids::RenderPipeline8B))
        Face = (IRenderPipeline*)(this);
    else if (IidEqual(Iid, Iids::VertexBufferManager))
        Face = (IVertexBufferManager*)(this);
    else if (IidEqual(Iid, Iids::RPDraw))
        Face = (IRPDraw*)(this);
    else if (IidEqual(Iid, Iids::RPIndexBuffer))
        Face = (IRPIndexBuffer*)(this);
    else if (IidEqual(Iid, Iids::RPVertexBuffer))
        Face = (IRPVertexBuffer*)(this);
    else if (IidEqual(Iid, Iids::GammaControl))
        Face = (IGammaControl*)(this);
    else if (IidEqual(Iid, Iids::RPTexture))
        Face = (IRPTexture*)(this);
    else if (IidEqual(Iid, Iids::AggregateComponent))
        Face = (IAggregateComponent*)(this);
    if (Face == nullptr)
        return GR_INTERFACE_UNSUPPORTED;
    Face->AddRef();
    *Out = Face;
    return GR_OK;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::QueryInterface(const char* Iid, void** Out)
{
    if (Outer_ != nullptr && Outer_ != &Inner_)
        return Outer_->QueryInterface(Iid, Out);
    return QueryLocal(Iid, Out);
}

unsigned long NEURONCOMPONENTS_API FRenderTrace::AddRef()
{
    if (Outer_ != nullptr && Outer_ != &Inner_)
        return Outer_->AddRef();
    return Inner_.AddRef();
}

unsigned long NEURONCOMPONENTS_API FRenderTrace::Release()
{
    if (Outer_ != nullptr && Outer_ != &Inner_)
        return Outer_->Release();
    return Inner_.Release();
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::Initialize()
{
    const GENRESULT Result = RealAggregate_->Initialize();
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IAggregateComponent", "Initialize");
        Call.Return(Result);
    }
    return Result;
}

// -------------------------------------------------------------------------------------------
// Synthetic lines

// The LightData lines of the pending lights, before a call that flushes the renderer's
// lights forwards (README.md, LightData). Each reads the caller's D3DLIGHT8 now, as the
// flush is about to. In frames that are not selected the lines only update their state
// slots. bReachesFlush is false for a call whose arguments make it return before the flush:
// nothing is read then and the lights stay pending.
void FRenderTrace::TraceLightData(bool bReachesFlush)
{
    if (!bReachesFlush || !Trace_.Tracking())
        return;
    RenderTrace::FScopedLock Guard(Trace_);
    std::vector<std::pair<long long, const void*> > Lights;
    Trace_.TakePendingLights(&Lights);
    for (size_t Index = 0; Index < Lights.size(); ++Index)
    {
        // The session lock is recursive: each FCall takes it again on this thread.
        RenderTrace::FCall Call(Trace_, RenderTrace::kState, "IRenderPipeline", "LightData");
        if (!Call.Active())
            continue;
        Call.Synthetic();
        Call.HandleId("Handle", Lights[Index].first);
        Call.Words("Light", Lights[Index].second, 26);
        char Key[48];
        snprintf(Key, sizeof(Key), "L:%lld:LightData", Lights[Index].first);
        Call.KeepAsState(Key);
    }
}

// After a successful CreateBuffers: the render-state values the device was just given and
// the pipeline properties, read through the real renderer's getters (not recorded).
void FRenderTrace::TraceDefaults()
{
    if (!Trace_.Tracking())
        return;
    unsigned long RenderValues[kDefaultRenderStateCount];
    bool RenderRead[kDefaultRenderStateCount];
    for (size_t Index = 0; Index < kDefaultRenderStateCount; ++Index)
    {
        int Value = 0;
        RenderRead[Index] =
            RealPipeline_->GetRenderState((D3DRENDERSTATETYPE)(kDefaultRenderStates[Index]), &Value) == GR_OK;
        RenderValues[Index] = (unsigned long)(unsigned int)(Value);
    }
    int PipelineValues[kPipelineIdCount];
    bool PipelineRead[kPipelineIdCount];
    for (size_t Index = 0; Index < kPipelineIdCount; ++Index)
    {
        int Value = 0;
        PipelineRead[Index] = RealPipeline_->GetPipelineState((int)(Index), &Value) == GR_OK;
        PipelineValues[Index] = Value;
    }
    RenderTrace::FScopedLock Guard(Trace_);
    Trace_.WriteDefaults(kDefaultRenderStates, RenderValues, RenderRead, kDefaultRenderStateCount, PipelineValues,
                         PipelineRead, kPipelineIdCount);
}

// After the SwapBuffers of a selected frame: the real renderer's PrintScreen (a copy of the
// front buffer, so the window must not be covered) into a temporary BMP, read back, stored
// as a blob and deleted. The PrintScreen call is not recorded.
void FRenderTrace::TraceFrameImage()
{
    const std::string Path = Trace_.FrameImagePath();
    DeleteFileA(Path.c_str());
    const GENRESULT Printed = RealTexture_->PrintScreen(nullptr, Path.c_str());
    std::vector<unsigned char> Bytes;
    char Note[96] = "";
    if (Printed != GR_OK)
    {
        snprintf(Note, sizeof(Note), "PrintScreen returned %ld", (long)(Printed));
    }
    else
    {
        FILE* File = fopen(Path.c_str(), "rb");
        if (File == nullptr)
        {
            snprintf(Note, sizeof(Note), "cannot open the PrintScreen file");
        }
        else
        {
            long Size = -1;
            if (fseek(File, 0, SEEK_END) == 0)
                Size = ftell(File);
            if (Size <= 0 || Size > kMaxFrameImageBytes || fseek(File, 0, SEEK_SET) != 0)
            {
                snprintf(Note, sizeof(Note), "the PrintScreen file is empty or too large");
            }
            else
            {
                Bytes.resize((size_t)(Size));
                if (fread(&Bytes[0], 1, Bytes.size(), File) != Bytes.size())
                {
                    Bytes.clear();
                    snprintf(Note, sizeof(Note), "cannot read the PrintScreen file");
                }
            }
            fclose(File);
        }
    }
    DeleteFileA(Path.c_str());
    RenderTrace::FScopedLock Guard(Trace_);
    if (Bytes.empty())
        Trace_.WriteFrameImage(nullptr, 0, Note);
    else
        Trace_.WriteFrameImage(&Bytes[0], Bytes.size(), nullptr);
}

// -------------------------------------------------------------------------------------------
// IRenderPipeline: device, frames, back buffer, lights, textures

// CreateBuffers re-applies the default render states and stages; the defaults line follows
// its own line.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::CreateBuffers(HWND Hwnd, int* Mode, int* OutMode)
{
    const GENRESULT Result = RealPipeline_->CreateBuffers(Hwnd, Mode, OutMode);
    if (Trace_.Tracking())
    {
        {
            RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "CreateBuffers");
            if (Call.Active())
            {
                Call.Raw("Hwnd", (const void*)(Hwnd));
                Call.Words("Mode", (const void*)(Mode), 10);
                Call.Words("OutMode", (const void*)(OutMode), 10);
                Call.Return(Result);
            }
            if (Result >= 0)
                Trace_.OnDeviceReset();
        }
        if (Result >= 0)
            TraceDefaults();
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::SwapBuffers()
{
    const GENRESULT Result = RealPipeline_->SwapBuffers();
    if (Trace_.Tracking())
    {
        {
            RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "SwapBuffers");
            Call.Return(Result);
        }
        if (Trace_.FrameRecorded())
            TraceFrameImage();
        Trace_.EndFrame();
    }
    return Result;
}

// Out: {width, height, pitch, bits, pfenum} of a read-only copy of the back buffer.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::LockBuffer(void* Out)
{
    const GENRESULT Result = RealPipeline_->LockBuffer(Out);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kFrame, "IRenderPipeline", "LockBuffer");
        if (Call.Active())
        {
            if (Result >= 0 && Out != nullptr)
            {
                const unsigned long* Desc = (const unsigned long*)(Out);
                Call.BeginObject("Out");
                Call.UInt("Width", Desc[0]);
                Call.UInt("Height", Desc[1]);
                Call.UInt("Pitch", Desc[2]);
                Call.UInt("Format", Desc[4]);
                SurfaceData(Call, "data", (const void*)((uintptr_t)Desc[3]), Desc[4], Desc[0], Desc[1], Desc[2]);
                Call.EndObject();
            }
            else
            {
                Call.Raw("Out", Out);
            }
            Call.Return(Result);
        }
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::UnlockBuffer()
{
    const GENRESULT Result = RealPipeline_->UnlockBuffer();
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kFrame, "IRenderPipeline", "UnlockBuffer");
        Call.Return(Result);
    }
    return Result;
}

// Lights are keyed by the caller's pointer. Their state slots are "L:<id>:<call>" and go
// away with RemoveLight or DestroyLight.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::SetLight(void* Handle, const D3DLIGHT8* Light)
{
    const GENRESULT Result = RealPipeline_->SetLight(Handle, Light);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kState, "IRenderPipeline", "SetLight");
        if (Call.Active())
        {
            const long long Id = Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Words("Light", Light, 26);
            Call.Return(Result);
            char Key[48];
            snprintf(Key, sizeof(Key), "L:%lld:SetLight", Id);
            Call.KeepAsState(Key);
        }
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::SetLightEnable(void* Handle, int Enable)
{
    const GENRESULT Result = RealPipeline_->SetLightEnable(Handle, Enable);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kState, "IRenderPipeline", "SetLightEnable");
        if (Call.Active())
        {
            const long long Id = Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Int("Enable", Enable);
            Call.Return(Result);
            char Key[48];
            snprintf(Key, sizeof(Key), "L:%lld:SetLightEnable", Id);
            Call.KeepAsState(Key);
        }
        // A light switched on sends its data at the next flush if it was not sent yet.
        if (Result == GR_OK && Enable != 0)
            Trace_.OnLightEnabled(Handle);
    }
    return Result;
}

// AddLight's handle is the caller's D3DLIGHT8, which the renderer reads at the next light
// flush; the line holds its contents at the time of the call, and the LightData line before
// the flush holds what the renderer read.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::AddLight(void* Handle)
{
    const GENRESULT Result = RealPipeline_->AddLight(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kState, "IRenderPipeline", "AddLight");
        if (Call.Active())
        {
            const long long Id = Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Words("Light", Handle, 26);
            Call.Return(Result);
            char Key[48];
            snprintf(Key, sizeof(Key), "L:%lld:AddLight", Id);
            Call.KeepAsState(Key);
        }
        if (Result == GR_OK)
            Trace_.OnLightData(Handle);
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::UpdateLight(void* Handle)
{
    const GENRESULT Result = RealPipeline_->UpdateLight(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kState, "IRenderPipeline", "UpdateLight");
        if (Call.Active())
        {
            const long long Id = Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Words("Light", Handle, 26);
            Call.Return(Result);
            char Key[48];
            snprintf(Key, sizeof(Key), "L:%lld:UpdateLight", Id);
            Call.KeepAsState(Key);
        }
        if (Result == GR_OK)
            Trace_.OnLightData(Handle);
    }
    return Result;
}

// Disables the light and flushes the renderer's lights (so the LightData lines of the other
// pending lights go first), then forgets it.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::RemoveLight(void* Handle)
{
    TraceLightData(true);
    const GENRESULT Result = RealPipeline_->RemoveLight(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kFrame, "IRenderPipeline", "RemoveLight");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Return(Result);
        }
        const long long Id = Trace_.FindHandle(RenderTrace::kLight, Handle);
        if (Result == GR_OK && Id > 0)
        {
            Trace_.OnLightGone(Id);
            Trace_.OnLightRemoved(Handle);
            Trace_.RetireHandle(RenderTrace::kLight, Handle);
        }
    }
    return Result;
}

// Disables the light and flushes the renderer's lights (so the LightData lines of the other
// pending lights go first), then forgets it.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::DestroyLight(void* Handle)
{
    TraceLightData(true);
    const GENRESULT Result = RealPipeline_->DestroyLight(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kFrame, "IRenderPipeline", "DestroyLight");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kLight, Handle);
            Call.Return(Result);
        }
        const long long Id = Trace_.FindHandle(RenderTrace::kLight, Handle);
        if (Result == GR_OK && Id > 0)
        {
            Trace_.OnLightGone(Id);
            Trace_.OnLightRemoved(Handle);
            Trace_.RetireHandle(RenderTrace::kLight, Handle);
        }
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::CreateTexture(int Width, int Height, unsigned int* Format,
                                                         unsigned int Levels, unsigned int Flags, void** OutHandle)
{
    const GENRESULT Result = RealPipeline_->CreateTexture(Width, Height, Format, Levels, Flags, OutHandle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "CreateTexture");
        // *OutHandle is written only on the paths that return a non-negative result.
        const void* Created = (Result >= 0 && OutHandle != nullptr) ? *OutHandle : nullptr;
        const long long Id = Created != nullptr ? Trace_.NewHandle(RenderTrace::kTexture, Created) : 0;
        if (Call.Active())
        {
            Call.Int("Width", Width);
            Call.Int("Height", Height);
            Call.OutU32("Format", Format);
            Call.UInt("Levels", Levels);
            Call.UInt("Flags", Flags);
            Call.HandleId("OutHandle", Id);
            Call.Return(Result);
        }
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::DestroyTexture(void* Handle)
{
    const GENRESULT Result = RealPipeline_->DestroyTexture(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "DestroyTexture");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kTexture, Handle);
            Call.Return(Result);
        }
        if (Result == GR_OK && Handle != nullptr)
        {
            Trace_.OnTextureUnbound(Handle);
            Trace_.RetireHandle(RenderTrace::kTexture, Handle);
        }
    }
    return Result;
}

// Out: {width, height, pitch, bits, pfenum}. The caller writes the level after this call;
// UnlockTexture records what it wrote.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::LockTexture(void* Handle, int Level, void* Out)
{
    const GENRESULT Result = RealPipeline_->LockTexture(Handle, Level, Out);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "LockTexture");
        const unsigned long* Desc = (const unsigned long*)(Out);
        if (Result >= 0 && Handle != nullptr && Desc != nullptr)
        {
            RenderTrace::FTextureLock Lock = {};
            Lock.Bits = (const unsigned char*)((uintptr_t)Desc[3]);
            Lock.Width = Desc[0];
            Lock.Height = Desc[1];
            Lock.Pitch = Desc[2];
            Lock.Format = Desc[4];
            Trace_.SetTextureLock(Handle, Level, Lock);
        }
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kTexture, Handle);
            Call.Int("Level", Level);
            if (Result >= 0 && Desc != nullptr)
            {
                Call.BeginObject("Out");
                Call.UInt("Width", Desc[0]);
                Call.UInt("Height", Desc[1]);
                Call.UInt("Pitch", Desc[2]);
                Call.UInt("Format", Desc[4]);
                Call.EndObject();
            }
            else
            {
                Call.Raw("Out", Out);
            }
            Call.Return(Result);
        }
    }
    return Result;
}

// The locked level is read before it is unlocked.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::UnlockTexture(void* Handle, int Level)
{
    if (!Trace_.Tracking())
        return RealPipeline_->UnlockTexture(Handle, Level);
    RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "UnlockTexture");
    RenderTrace::FTextureLock Lock = {};
    const bool bLocked = Trace_.PopTextureLock(Handle, Level, &Lock);
    if (Call.Active())
    {
        Call.Handle("Handle", RenderTrace::kTexture, Handle);
        Call.Int("Level", Level);
        if (bLocked)
        {
            Call.UInt("Width", Lock.Width);
            Call.UInt("Height", Lock.Height);
            Call.UInt("Pitch", Lock.Pitch);
            Call.UInt("Format", Lock.Format);
            SurfaceData(Call, "data", Lock.Bits, Lock.Format, Lock.Width, Lock.Height, Lock.Pitch);
        }
        else
        {
            Call.Null("data");
            Call.Note("no open lock seen for this level");
        }
    }
    const GENRESULT Result = RealPipeline_->UnlockTexture(Handle, Level);
    Call.Return(Result);
    return Result;
}

// Pixels: what D3DXLoadSurfaceFromMemory reads for SrcW x SrcH at Pitch in *SrcFormat;
// B: the 256-entry RGB palette (768 bytes) of an 8-bit source. A is never read.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                               unsigned int Pitch, unsigned int* SrcFormat,
                                                               const void* Pixels, void* A, void* B)
{
    const GENRESULT Result =
        RealPipeline_->SetTextureLevelData(Handle, Level, SrcW, SrcH, Pitch, SrcFormat, Pixels, A, B);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRenderPipeline", "SetTextureLevelData");
        const unsigned int Format = SrcFormat != nullptr ? *SrcFormat : 0;
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kTexture, Handle);
            Call.Int("Level", Level);
            Call.Int("SrcW", SrcW);
            Call.Int("SrcH", SrcH);
            Call.UInt("Pitch", Pitch);
            Call.OutU32("SrcFormat", SrcFormat);
            if (SrcW > 0 && SrcH > 0)
                SurfaceData(Call, "Pixels", Pixels, Format, (unsigned int)(SrcW), (unsigned int)(SrcH), Pitch);
            else
                Call.Null("Pixels");
            Call.Raw("A", A);
            Call.Bytes("B", B, 256 * 3);
            Call.Return(Result);
        }
        // The renderer unbinds the texture from every stage once the handle and format pass.
        if (Handle != nullptr && Format != 0 && Result != GR_INVALID_PARMS)
            Trace_.OnTextureUnbound(Handle);
    }
    return Result;
}

// -------------------------------------------------------------------------------------------
// IVertexBufferManager

GENRESULT NEURONCOMPONENTS_API FRenderTrace::LockVertexMemory(unsigned int Fvf, unsigned int Count,
                                                            FVertexMemoryLock* Out)
{
    const GENRESULT Result = RealVbMgr_->LockVertexMemory(Fvf, Count, Out);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IVertexBufferManager", "LockVertexMemory");
        const bool bLocked = (int)(Result) >= 0 && Out != nullptr;
        const void* Vb = bLocked ? Out->Listing.VertexBuffer : nullptr;
        if (bLocked)
        {
            Trace_.MarkPoolVb(Vb, Fvf);
            RenderTrace::FBufferLock Lock = {};
            Lock.Bits = (const unsigned char*)(Out->Vertices);
            Lock.First = (unsigned int)(Out->Listing.BaseVertex);
            Lock.bFirstKnown = true;
            Lock.Count = Count;
            Lock.Stride = RenderTrace::FvfStride((unsigned int)(Out->Listing.Fvf));
            Trace_.PushBufferLock(RenderTrace::kVertexBuffer, Vb, Lock);
        }
        if (Call.Active())
        {
            Call.UInt("Fvf", Fvf);
            Call.UInt("Count", Count);
            if (bLocked)
            {
                Call.BeginObject("Out");
                Call.Handle("VertexBuffer", RenderTrace::kVertexBuffer, Vb);
                Call.UInt("Fvf", Out->Listing.Fvf);
                Call.UInt("BaseVertex", Out->Listing.BaseVertex);
                Call.EndObject();
            }
            else
            {
                Call.Raw("Out", Out);
            }
            Call.Return(Result);
        }
        // The pool buffer for Fvf is selected before the lock, whatever the lock returns.
        if (bLocked)
            Trace_.SelectVb(Vb);
        else
            Trace_.SelectPoolVb(Fvf);
    }
    return Result;
}

// The locked vertices are read before they are unlocked.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::UnlockVertexMemory(FVertexMemoryLock* Out)
{
    if (!Trace_.Tracking())
        return RealVbMgr_->UnlockVertexMemory(Out);
    RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IVertexBufferManager", "UnlockVertexMemory");
    const void* Vb = Out != nullptr ? Out->Listing.VertexBuffer : nullptr;
    RenderTrace::FBufferLock Lock = {};
    const bool bLocked = Vb != nullptr && Trace_.PopBufferLock(RenderTrace::kVertexBuffer, Vb, &Lock);
    if (Call.Active())
    {
        Call.Handle("VertexBuffer", RenderTrace::kVertexBuffer, Vb);
        if (bLocked)
        {
            Call.UInt("FirstVertex", Lock.First);
            Call.UInt("Count", Lock.Count);
            Call.UInt("Stride", Lock.Stride);
            Call.Bytes("data", Lock.Bits, (size_t)(Lock.Count) * Lock.Stride);
        }
        else
        {
            Call.Null("data");
            Call.Note("no open lock seen for this buffer");
        }
    }
    const GENRESULT Result = RealVbMgr_->UnlockVertexMemory(Out);
    Call.Return(Result);
    return Result;
}

// Writes converted vertices into memory the caller locked; UnlockVertexMemory or UnlockVb
// records the result. The line (selected frames only) shows the source it read.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::CopyVertexData(void* Dest, unsigned int Fvf,
                                                          const FRpVertexSrcDesc* Source,
                                                          unsigned int StartVertex, unsigned int Count)
{
    const GENRESULT Result = RealVbMgr_->CopyVertexData(Dest, Fvf, Source, StartVertex, Count);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kFrame, "IVertexBufferManager", "CopyVertexData");
        if (Call.Active())
        {
            const void* Vb = nullptr;
            size_t Offset = 0;
            const unsigned char* Written =
                (const unsigned char*)(Dest) + (size_t)(StartVertex) * RenderTrace::FvfStride(Fvf);
            if (Dest != nullptr && Trace_.FindVbRegion(Written, &Vb, &Offset))
            {
                Call.BeginObject("Dest");
                Call.Handle("VertexBuffer", RenderTrace::kVertexBuffer, Vb);
                Call.UInt("ByteOffset", Offset);
                Call.EndObject();
            }
            else
            {
                Call.Raw("Dest", Dest);
            }
            Call.UInt("Fvf", Fvf);
            Call.VertexSource("Source", Source, StartVertex, Count, Fvf);
            Call.UInt("StartVertex", StartVertex);
            Call.UInt("Count", Count);
            Call.Return(Result);
        }
    }
    return Result;
}

// -------------------------------------------------------------------------------------------
// IRPIndexBuffer

GENRESULT NEURONCOMPONENTS_API FRenderTrace::CreateIb(unsigned int Count, unsigned int** Out, unsigned int Flags)
{
    const GENRESULT Result = RealIb_->CreateIb(Count, Out, Flags);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPIndexBuffer", "CreateIb");
        // *Out is written whenever the device exists, even when the resize failed.
        const void* Created = (Result != GR_GENERIC && Out != nullptr) ? (const void*)(*Out) : nullptr;
        const long long Id = Created != nullptr ? Trace_.NewHandle(RenderTrace::kIndexBuffer, Created) : 0;
        if (Call.Active())
        {
            Call.UInt("Count", Count);
            Call.HandleId("Out", Id);
            Call.UInt("Flags", Flags);
            Call.Return(Result);
        }
    }
    return Result;
}

// Handle points to the caller's handle variable, which the renderer clears.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::DestroyIb(void** Handle)
{
    const void* Target = Handle != nullptr ? *Handle : nullptr;
    const GENRESULT Result = RealIb_->DestroyIb(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPIndexBuffer", "DestroyIb");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kIndexBuffer, Target);
            Call.Return(Result);
        }
        if (Result == GR_OK && Target != nullptr && Target != kMinusOne)
        {
            Trace_.OnIbGone(Target);
            Trace_.RetireHandle(RenderTrace::kIndexBuffer, Target);
        }
    }
    return Result;
}

// OutOffset carries the start index in and the index the data landed at out.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::CopyIndices(void* Handle, unsigned int* OutOffset,
                                                       const unsigned short* SrcData, unsigned int Count)
{
    const unsigned int OffsetIn = OutOffset != nullptr ? *OutOffset : 0;
    const GENRESULT Result = RealIb_->CopyIndices(Handle, OutOffset, SrcData, Count);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPIndexBuffer", "CopyIndices");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kIndexBuffer, Handle);
            if (OutOffset != nullptr)
            {
                Call.UInt("OffsetIn", OffsetIn);
                Call.OutU32("OutOffset", OutOffset);
            }
            else
            {
                Call.Null("OutOffset");
            }
            if (Result >= 0)
                Call.Bytes("SrcData", SrcData, (size_t)(Count) * 2u);
            else
                Call.Null("SrcData");
            Call.UInt("Count", Count);
            Call.Return(Result);
        }
    }
    return Result;
}

// Offset carries the start index in (static buffers) and the start index out.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::LockIb(void* Handle, unsigned int* Offset, void** Out,
                                                  unsigned int Count)
{
    const unsigned int OffsetIn = Offset != nullptr ? *Offset : 0;
    const GENRESULT Result = RealIb_->LockIb(Handle, Offset, Out, Count);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPIndexBuffer", "LockIb");
        if (Result >= 0 && Out != nullptr && *Out != nullptr && Handle != nullptr)
        {
            RenderTrace::FBufferLock Lock = {};
            Lock.Bits = (const unsigned char*)(*Out);
            Lock.First = Offset != nullptr ? *Offset : 0;
            Lock.bFirstKnown = Offset != nullptr;
            Lock.Count = Count;
            Lock.Stride = 2;
            Trace_.PushBufferLock(RenderTrace::kIndexBuffer, Handle, Lock);
        }
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kIndexBuffer, Handle);
            if (Offset != nullptr)
            {
                Call.UInt("OffsetIn", OffsetIn);
                Call.OutU32("Offset", Offset);
            }
            else
            {
                Call.Null("Offset");
            }
            Call.UInt("Count", Count);
            Call.Return(Result);
        }
    }
    return Result;
}

// The locked indices are read before they are unlocked.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::UnlockIb(void* Handle)
{
    if (!Trace_.Tracking())
        return RealIb_->UnlockIb(Handle);
    RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPIndexBuffer", "UnlockIb");
    RenderTrace::FBufferLock Lock = {};
    const bool bLocked = Handle != nullptr && Trace_.PopBufferLock(RenderTrace::kIndexBuffer, Handle, &Lock);
    if (Call.Active())
    {
        Call.Handle("Handle", RenderTrace::kIndexBuffer, Handle);
        if (bLocked)
        {
            if (Lock.bFirstKnown)
                Call.UInt("FirstIndex", Lock.First);
            else
                Call.Null("FirstIndex");
            Call.UInt("Count", Lock.Count);
            Call.Bytes("data", Lock.Bits, (size_t)(Lock.Count) * 2u);
        }
        else
        {
            Call.Null("data");
            Call.Note("no open lock seen for this buffer");
        }
    }
    const GENRESULT Result = RealIb_->UnlockIb(Handle);
    Call.Return(Result);
    return Result;
}

// -------------------------------------------------------------------------------------------
// IRPVertexBuffer

// Flags & 1 returns the renderer's pooled buffer for Fvf (the same one each time, never
// destroyed) and selects it.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                                    unsigned int Flags)
{
    const GENRESULT Result = RealVb_->CreateVb(Fvf, Count, Out, Flags);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPVertexBuffer", "CreateVb");
        const bool bPool = (Flags & 1u) != 0;
        const void* Created = (Result >= 0 && Out != nullptr) ? (const void*)(*Out) : nullptr;
        long long Id = 0;
        if (Created != nullptr)
        {
            if (bPool)
            {
                bool bNew = false;
                Id = Trace_.LookupHandle(RenderTrace::kVertexBuffer, Created, &bNew);
                Trace_.MarkPoolVb(Created, Fvf);
            }
            else
            {
                Id = Trace_.NewHandle(RenderTrace::kVertexBuffer, Created);
            }
        }
        if (Call.Active())
        {
            Call.UInt("Fvf", Fvf);
            Call.UInt("Count", Count);
            Call.HandleId("Out", Id);
            Call.UInt("Flags", Flags);
            Call.Return(Result);
        }
        if (bPool && Created != nullptr)
            Trace_.SelectVb(Created);
    }
    return Result;
}

// Handle points to the caller's handle variable, which the renderer clears. Pooled buffers
// are not destroyed and keep their id.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::DestroyVb(void** Handle)
{
    const void* Target = Handle != nullptr ? *Handle : nullptr;
    const GENRESULT Result = RealVb_->DestroyVb(Handle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPVertexBuffer", "DestroyVb");
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kVertexBuffer, Target);
            Call.Return(Result);
        }
        if (Result == GR_OK && Target != nullptr && Target != kMinusOne && !Trace_.IsPoolVb(Target))
        {
            Trace_.OnVbGone(Target);
            Trace_.RetireHandle(RenderTrace::kVertexBuffer, Target);
        }
    }
    return Result;
}

// The renderer converts the source into the buffer itself (locking it when it is not
// locked already), so the line carries the resolved source; the replayer converts it.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::CopyVertices(void* Handle, unsigned int* OutOffset,
                                                        const FRpVertexSrcDesc* SrcData, unsigned int StartVertex,
                                                        unsigned int Count)
{
    const unsigned int OffsetIn = OutOffset != nullptr ? *OutOffset : 0;
    const GENRESULT Result = RealVb_->CopyVertices(Handle, OutOffset, SrcData, StartVertex, Count);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPVertexBuffer", "CopyVertices");
        if (Call.Active())
        {
            // The destination format: the pooled buffer for the source FVF, or the buffer's
            // own (GetVbInfo reads it and changes nothing).
            unsigned int DstFvf = 0;
            if (Handle == kMinusOne)
            {
                DstFvf = SrcData != nullptr ? SrcData->SrcFvf : 0;
            }
            else if (Handle != nullptr)
            {
                unsigned int Fvf = 0;
                if (RealVb_->GetVbInfo(Handle, &Fvf, nullptr) == GR_OK)
                    DstFvf = Fvf;
            }
            Call.Handle("Handle", RenderTrace::kVertexBuffer, Handle);
            if (OutOffset != nullptr)
            {
                Call.UInt("OffsetIn", OffsetIn);
                Call.OutU32("OutOffset", OutOffset);
            }
            else
            {
                Call.Null("OutOffset");
            }
            Call.VertexSource("SrcData", SrcData, StartVertex, Count, DstFvf);
            Call.UInt("StartVertex", StartVertex);
            Call.UInt("Count", Count);
            Call.UInt("DstFvf", DstFvf);
            Call.Return(Result);
        }
        if (Handle == kMinusOne && Result >= 0 && SrcData != nullptr)
            Trace_.SelectPoolVb(SrcData->SrcFvf);
    }
    return Result;
}

// Offset carries a byte offset in (static buffers) and the first vertex out.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::LockVb(void* Handle, unsigned int* Offset, void** Out,
                                                  unsigned int Count)
{
    const unsigned int OffsetIn = Offset != nullptr ? *Offset : 0;
    const GENRESULT Result = RealVb_->LockVb(Handle, Offset, Out, Count);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPVertexBuffer", "LockVb");
        unsigned int Stride = 0;
        if (Result >= 0 && Out != nullptr && *Out != nullptr && Handle != nullptr && Handle != kMinusOne)
        {
            unsigned int Fvf = 0;
            if (RealVb_->GetVbInfo(Handle, &Fvf, nullptr) == GR_OK)
                Stride = RenderTrace::FvfStride(Fvf);
            RenderTrace::FBufferLock Lock = {};
            Lock.Bits = (const unsigned char*)(*Out);
            Lock.First = Offset != nullptr ? *Offset : 0;
            Lock.bFirstKnown = Offset != nullptr;
            Lock.Count = Count;
            Lock.Stride = Stride;
            Trace_.PushBufferLock(RenderTrace::kVertexBuffer, Handle, Lock);
        }
        if (Call.Active())
        {
            Call.Handle("Handle", RenderTrace::kVertexBuffer, Handle);
            if (Offset != nullptr)
            {
                Call.UInt("OffsetIn", OffsetIn);
                Call.OutU32("Offset", Offset);
            }
            else
            {
                Call.Null("Offset");
            }
            Call.UInt("Count", Count);
            Call.UInt("Stride", Stride);
            Call.Return(Result);
        }
    }
    return Result;
}

// The locked vertices are read before they are unlocked.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::UnlockVb(void* Handle)
{
    if (!Trace_.Tracking())
        return RealVb_->UnlockVb(Handle);
    RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPVertexBuffer", "UnlockVb");
    RenderTrace::FBufferLock Lock = {};
    const bool bLocked = Handle != nullptr && Trace_.PopBufferLock(RenderTrace::kVertexBuffer, Handle, &Lock);
    if (Call.Active())
    {
        Call.Handle("Handle", RenderTrace::kVertexBuffer, Handle);
        if (bLocked)
        {
            if (Lock.bFirstKnown)
                Call.UInt("FirstVertex", Lock.First);
            else
                Call.Null("FirstVertex");
            Call.UInt("Count", Lock.Count);
            Call.UInt("Stride", Lock.Stride);
            Call.Bytes("data", Lock.Bits, (size_t)(Lock.Count) * Lock.Stride);
        }
        else
        {
            Call.Null("data");
            Call.Note("no open lock seen for this buffer");
        }
    }
    const GENRESULT Result = RealVb_->UnlockVb(Handle);
    Call.Return(Result);
    return Result;
}

// -------------------------------------------------------------------------------------------
// IRPTexture

namespace
{
    // The bytes of the file LoadTexture and LoadCubemap decode: the same descriptor and
    // open, size, mapping and view sequence as DX8RENDERER's OpenTextureFileMap, read
    // again after the real call (which has closed its own handles). False when any step
    // fails; the file system is only read.
    bool ReadRendererFile(IFileSystem* FileSystem, const char* Name, std::vector<unsigned char>& Out)
    {
        Out.clear();
        if (FileSystem == nullptr || Name == nullptr)
            return false;
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Path = Name;
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x8000080;
        Desc.Field30 = 0xFFFFFFFF;
        const int Handle = FileSystem->OpenByDesc(&Desc);
        if (Handle == -1)
            return false;
        const unsigned long Size = FileSystem->GetSize((unsigned long)(Handle), 0);
        const int Mapping = FileSystem->CreateMapping((unsigned long)(Handle), 0, 2, 0, 0, 0);
        bool bRead = false;
        if (Mapping != -1)
        {
            const int View = FileSystem->MapFileView((unsigned long)(Mapping), 4, 0, 0, 0);
            if (View != 0)
            {
                const unsigned char* Bytes = (const unsigned char*)((uintptr_t)(View));
                Out.assign(Bytes, Bytes + Size);
                bRead = true;
                FileSystem->UnmapFileView((unsigned long)(View));
            }
            FileSystem->CloseFile((unsigned long)(Mapping));
        }
        FileSystem->CloseFile((unsigned long)(Handle));
        return bRead;
    }

    // `Data`: the file's bytes (a blob), or null when the proxy could not read them.
    void FileData(RenderTrace::FCall& Call, IFileSystem* FileSystem, const char* FileName)
    {
        std::vector<unsigned char> Bytes;
        if (ReadRendererFile(FileSystem, FileName, Bytes) && !Bytes.empty())
            Call.Bytes("Data", Bytes.data(), Bytes.size());
        else
            Call.Null("Data");
    }
}

// The encoded file is read by the renderer through FileSystem; the line has its name
// and, as `Data`, the file's bytes the proxy reads through the same file system.
GENRESULT NEURONCOMPONENTS_API FRenderTrace::LoadTexture(IFileSystem* FileSystem, const char* FileName,
                                                       void** OutHandle)
{
    const GENRESULT Result = RealTexture_->LoadTexture(FileSystem, FileName, OutHandle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPTexture", "LoadTexture");
        const void* Created = (Result >= 0 && OutHandle != nullptr) ? *OutHandle : nullptr;
        const long long Id = Created != nullptr ? Trace_.NewHandle(RenderTrace::kTexture, Created) : 0;
        if (Call.Active())
        {
            Call.Raw("FileSystem", FileSystem);
            Call.Str("FileName", FileName);
            FileData(Call, FileSystem, FileName);
            Call.HandleId("OutHandle", Id);
            Call.Return(Result);
        }
    }
    return Result;
}

GENRESULT NEURONCOMPONENTS_API FRenderTrace::LoadCubemap(IFileSystem* FileSystem, const char* FileName,
                                                       void** OutHandle)
{
    const GENRESULT Result = RealTexture_->LoadCubemap(FileSystem, FileName, OutHandle);
    if (Trace_.Tracking())
    {
        RenderTrace::FCall Call(Trace_, RenderTrace::kAlways, "IRPTexture", "LoadCubemap");
        const void* Created = (Result >= 0 && OutHandle != nullptr) ? *OutHandle : nullptr;
        const long long Id = Created != nullptr ? Trace_.NewHandle(RenderTrace::kTexture, Created) : 0;
        if (Call.Active())
        {
            Call.Raw("FileSystem", FileSystem);
            Call.Str("FileName", FileName);
            FileData(Call, FileSystem, FileName);
            Call.HandleId("OutHandle", Id);
            Call.Return(Result);
        }
    }
    return Result;
}
