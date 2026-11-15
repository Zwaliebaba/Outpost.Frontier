// Author: Giperion.

#pragma once

// DX9RENDERER.dll: the Direct3D 9 fork of DX8RENDERER.dll (src/DX8RENDERER). Same NEURONCOMPONENTS interfaces, same IIDs, same
// [RenderPipeline] section; it registers as "DirectX9" (kDx9RendererComponentName). Every declaration
// here mirrors the same-named one in src/DX8RENDERER/DX8RENDERER.h (FDx8Renderer* -> FDx9Renderer*, FDirectX8 -> FDirectX9).
// DX8RENDERER is the reference implementation: a fix made there is repeated here. DX9RENDERER-GAP comments in
// DX9RENDERER.cpp and implementation notes list every place where D3D9 cannot reproduce DX8RENDERER exactly.

// Unlike DX8RENDERER this object does not keep baseline's 0x230C layout (no layout static_asserts), and
// the fields DX8RENDERER reaches through raw byte offsets are named members here. Data that leaves the
// pipeline keeps its D3D8 shape (the 0x520 adapter record, the 10-dword mode blob, D3DLIGHT8 /
// D3DMATERIAL8, D3D8 render-state and texture-stage numbering).

#include "render/d3d8_min.h"
#include "neuroncomponents.h"
#include "luaprofile.h"
#include "render/IRenderPipeline.h"
#include "render/IRenderPipelineEx.h"
#include "Dx9RendererD3d8Types.h"

#include <stddef.h>

#include <map>
#include <vector>

static const unsigned long kDx9RendererRegisterPriority = 0x40000000;
// QueryDeviceAbility accepts 0..0x14. Indices 0..0x13 live here; 0x14 reads render-state entry
// 0, which is where DX8RENDERER's 21st ability dword overlaps (see FDirectX9::QueryDeviceAbility).
static const unsigned int kDx9RendererAbilityCount = 20;
// Pipeline-state ids 0..0xD (DX8RENDERER object +0x354, 0xC bytes each).
static const unsigned int kDx9RendererPipelineStateCount = 14;
// DX8RENDERER's default INI section, shared.
static const char* const kDx9RendererDefaultSection = kDx8RendererDefaultSection;

// The pipeline-state ids, named after the property map entries of FDirectX9::Initialize
// (dx8renderer.dll the routine). Id 6 carries the usable texture-stage count once the caps are known.
enum EDx9RendererPipelineState : int
{
    PipelineStateTexture = 0x0,
    PipelineStateLighting = 0x1,
    PipelineStateTextureLod = 0x2,
    PipelineStateTextureAllow8Bit = 0x3,
    PipelineStateTextureAllow32Bit = 0x4,
    PipelineStateTextureAllowDxt = 0x5,
    PipelineStateStageCount = 0x6, // "BROKEN_MULTITEXTURE" in the property map
    PipelineStateViewspaceLights = 0x7,
    PipelineStateClearColor = 0x8,
    PipelineStateClearZDepth = 0x9,
    PipelineStateClearStencil = 0xA,
    PipelineStateStateCache = 0xB,
    PipelineStateBrokenMod2x = 0xC,
    PipelineStateBrokenFlip = 0xD,
};

struct FDx9RendererAggDesc
{
    unsigned long Size;
    const char* Name;
    INeuronComponent* Outer;
    INeuronComponent** InnerOut;
    const char* Extra;
};

// The 10-dword mode descriptor callers pass to select_mode / create_buffers (external shape).
struct FDx9RendererMode
{
    int Field00;
    int Field04;
    int Width;
    int Height;
    int Format;
    int WantedZ;
    int WantedStencil;
    int Interval;
    int BufferCount;
    unsigned char SwapExtra;
    unsigned char Fullscreen;
    unsigned char Pad26[2];
};

static_assert(sizeof(FDx9RendererMode) == 0x28, "mode 10 dword");

// Profile property metadata and the value read at startup.
struct FDx9RendererProperty
{
    const char* Name;
    int Id;
    unsigned long Default;
    unsigned long Value;
    unsigned char bEnabled;
};

typedef std::map<int, FDx9RendererProperty> FDx9RendererPropertyMap;

// One pipeline-state slot {value, has-default, set-by-ini} (DX8RENDERER: [this + 0x354 + Id*0xC]).
struct FDx9RendererPipelineStateSlot
{
    unsigned long Value;
    unsigned long bHasDefault;
    unsigned long bOverridden;
};

// DX8RENDERER FDx8RendererAdapterMode: one display-mode record of an adapter. Only the two pf indices are set.
struct FDx9RendererAdapterMode
{
    unsigned long Field00;
    unsigned long Field04;
    unsigned long Field08;
    unsigned long Field0C;
    unsigned long PfIndex10;
    unsigned long PfIndex14;
    unsigned long Field18;
};

// DX8RENDERER FDx8RendererAdapter: one 0x520-byte adapter record, copied out whole by get_device_info. The
// record keeps DX8RENDERER's D3D8 shape; Identifier and Caps are filled from the D3D9 structures.
struct FDx9RendererAdapter
{
    FDx9RendererIdentifier8 Identifier;        // +0x000 D3DADAPTER_IDENTIFIER8 shape
    D3DDISPLAYMODE DisplayMode;        // +0x42C GetAdapterDisplayMode (same layout in D3D8/9)
    FDx9RendererCaps8 Caps;                    // +0x43C D3DCAPS8 shape
    unsigned long Field510;            // +0x510 TODO: no writer found in DX8RENDERER's startup either
    unsigned long CanRenderWindowed;   // +0x514 Caps.Caps2 & D3DCAPS2_CANRENDERWINDOWED
    FDx9RendererAdapterMode* Modes;            // +0x518 operator new(ModeCount * 0x1C)
    unsigned long ModeCount;           // +0x51C display-mode count (DX9RENDERER mode list)
};

static_assert(sizeof(FDx9RendererAdapter) == 0x520, "get_device_info copies a 0x520-byte record");
static_assert(offsetof(FDx9RendererAdapter, DisplayMode) == 0x42C, "adapter record display mode");
static_assert(offsetof(FDx9RendererAdapter, Caps) == 0x43C, "adapter record caps");
static_assert(offsetof(FDx9RendererAdapter, CanRenderWindowed) == 0x514, "adapter record windowed flag");

// A vertex-buffer node: create_vb's list buffers and the values of the VB pool map.
struct FDx9RendererVb
{
    void ReleaseGpu(); // formerly ReleaseVbGpu

    unsigned long Flags;
    unsigned long Fvf;
    unsigned long Count;
    unsigned long Used;
    unsigned long Locked;
    FDx9RendererVb* Next;
    FDx9RendererVb* Prev;
    unsigned long Field1C;
    IDirect3DVertexBuffer9* Gpu;
};

struct FDx9RendererIb
{
    HRESULT LockRange(unsigned int Start, unsigned int Count, void** OutBits,
                      unsigned int* OutStart, unsigned char bSysLock); // formerly LockIbRange

    unsigned long Flags;
    IDirect3DIndexBuffer9* Gpu;
    void* LockedBits;
    FDx9RendererIb* Next;
    FDx9RendererIb* Prev;
    unsigned long Cursor;
    unsigned long Capacity;
};

// The intrusive lists of create_vb / create_ib nodes (DX8RENDERER object +0x22E4 / +0x22F0).
struct FDx9RendererVbList
{
    FDx9RendererVb* First;
    FDx9RendererVb* Last;
    unsigned long Count;
};

struct FDx9RendererIbList
{
    FDx9RendererIb* First;
    FDx9RendererIb* Last;
    unsigned long Count;
};

enum EDx9RendererStreamDirtyFlag : unsigned char
{
    StreamDirtyVertexSource = 1,
    StreamDirtyIndices = 2
};

struct FDx9RendererStreamDirtyFlags
{
    unsigned char Dirty;
    unsigned char Reserved[3];
};

struct FDx9RendererStreamCache
{
    void Reset(); // formerly ResetStreamCache
    void SelectVertexStream(FDx9RendererVb* Buffer, unsigned long Fvf); // formerly MarkStreamVb / MarkStreamFvf
    void SelectIndices(FDx9RendererIb* Buffer, unsigned int BaseVertex); // formerly MarkStreamScratchIb / SelectIb cache stores

    unsigned long Fvf;
    FDx9RendererVb* VertexBuffer;
    FDx9RendererIb* IndexBuffer;
    unsigned int BaseVertex;
    FDx9RendererStreamDirtyFlags Flags; // selection touches Dirty; reset clears all four bytes
};

struct FDx9RendererTextureStageState
{
    unsigned long Value;
    unsigned char bValid;
    unsigned char Reserved[3];
};

struct FDx9RendererTextureBinding
{
    void* Handle;
    unsigned char bValid;
    unsigned char Reserved[3];
};

struct FDx9RendererTransformCache
{
    void Commit(IDirect3DDevice9* Device, D3DTRANSFORMSTATETYPE Type); // formerly Dx8RendererCommitTransform
    void StoreAffine(const float* Matrix); // formerly StoreMatrix12
    void LoadAffine(float* Matrix) const; // formerly LoadMatrix12
    void StoreTransposed(const float* Matrix); // formerly StoreMatrix16
    void LoadTransposed(float* Matrix) const; // formerly LoadMatrix16

    float Values[16];
    unsigned char bCommitted;
    unsigned char bApplyMarker; // combined apply sets this; device submission clears it
    unsigned char Reserved[2];
};

struct FDx9RendererDeviceStateCaches
{
    static const unsigned int StageCount = 8;
    static const unsigned int StateCount = 29;

    // Type is a D3D8 D3DTSS_* number (the cache is indexed in D3D8 numbering); sampler states
    // are read back with GetSamplerState.
    unsigned long ReadTextureStageState(IDirect3DDevice9* Device, unsigned long Stage,
                                        unsigned long Type); // formerly ReadTssCache

    FDx9RendererTextureStageState TextureStates[StageCount][StateCount];
    FDx9RendererTextureBinding TextureBindings[StageCount];
    FDx9RendererTransformCache TextureTransforms[StageCount];
};

// DX9RENDERER Ex (port-only, DX9RENDEREREx.cpp): a fixed-function stage (0..7) the Ex pass binds a texture
// on. The stage's cached binding and its sampler-state entries (D3D8 TSS 13..21, 25) are saved
// here and marked invalid for the pass (so in-pass IRenderPipeline calls reach the device);
// EndExPass re-sends the saved values (FDirectX9::ExSaveStage / ExRestoreStage).
struct FDx9RendererExStageSave
{
    static const unsigned int SamplerTypeCount = 10;
    static const unsigned long SamplerTypes[SamplerTypeCount];

    FDx9RendererTextureBinding Binding;
    FDx9RendererTextureStageState Samplers[SamplerTypeCount];
};

struct FDx9RendererPipelineTransforms
{
    FDx9RendererTransformCache View;
    FDx9RendererTransformCache World;
    FDx9RendererTransformCache Projection;
};

// A format class owns an ordered array of candidate format IDs.
struct FormatClassNode
{
    unsigned int Key;
    unsigned int Count;
    unsigned int* Values;
};

// The StateCache.cpp light manager (DX8RENDERER object +0x21F4). The light records themselves live in
// StateCache globals (DX9RENDERER.cpp).
struct FDx9RendererLightManager
{
    void Initialize(); // formerly LightManagerConstruct
    int FindOrCreate(unsigned long Handle, const D3DLIGHT8* Data); // formerly LightFindOrCreate
    bool Remove(unsigned long Handle); // formerly LightRemove
    unsigned char GetEnable(unsigned long Handle); // formerly LightGetEnable
    bool Update(unsigned long Handle); // formerly LightUpdate
    bool SetEnable(unsigned long Handle, int Enable); // formerly LightSetEnable
    void Reset(); // formerly LightReset
    HRESULT Flush(IDirect3DDevice9* Device); // formerly LightFlush

    long EnabledCount; // lights whose LightEnable state on the device is on
    long Generation;   // flush counter
};

class FDirectX9;
class FMgsdbDatabase;
struct FDx9RendererExState; // DX9RENDEREREx.h

struct FDx9RendererRenderStateEntry
{
    unsigned long Cached;
    unsigned long Pending;
    unsigned long Generation;
    unsigned long bEnabled;
};

// Indexed by D3D8 D3DRENDERSTATETYPE; Apply translates a state to its D3D9 call.
struct FDx9RendererRenderStateCache
{
    static const unsigned int StateCount = 256;

    void Initialize(); // formerly InitRsQueue
    void ResetGeneration(); // formerly RsQueueResetGeneration
    unsigned long GetPending(unsigned int StateId) const; // formerly RsQueueReadPending
    HRESULT Apply(IDirect3DDevice9* Device, unsigned int StateId, unsigned long Value); // formerly RsQueueApply
    bool MarkDirty(unsigned int StateId, unsigned long Value); // formerly RsQueueMarkDirty
    void Flush(IDirect3DDevice9* Device); // formerly RsQueueFlush
    // DX9RENDERER Ex only: queue every state whose requested value differs from Snapshot (a copy of
    // the Pending column taken by BeginExPass) back to the snapshot value, so the next flush
    // re-sends what the pass changed. Disabled states are untouched, as MarkDirty does.
    void RestorePending(const unsigned long* Snapshot);

    FDx9RendererRenderStateEntry Entries[StateCount];
    unsigned long DirtyStates[StateCount];
    unsigned long DirtyCount;
    unsigned long Generation;
    // D3D9 only: depth bits of the auto depth-stencil surface, for the D3DRS_ZBIAS ->
    // D3DRS_DEPTHBIAS conversion (set by create_buffers before the defaults are applied).
    int DepthBiasZBits;
};

class FDx9RendererInner final : public INeuronComponent
{
public:
    FDx9RendererInner();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    unsigned long RefCount;
    FDirectX9* Owner;
};

class FDx9RendererFactory final : public IComponentFactory
{
public:
    FDx9RendererFactory();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Instance) override;

    unsigned long RefCount;
    const char* Name;
};

class FDirectX9 final : public IRenderPipeline,
                        public IVertexBufferManager,
                        public IRPDraw,
                        public IRPIndexBuffer,
                        public IRPVertexBuffer,
                        public IGammaControl,
                        public IRPTexture,
                        public IAggregateComponent,
                        // Port-only Direct3D 9 extensions (render/IRenderPipelineEx.h, DX9RENDEREREx.cpp).
                        public IRenderPipelineEx,
                        public IRPDrawEx
{
public:
    FDirectX9();
    ~FDirectX9();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) override;
    GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() override;
    GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) override;
    GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) override;
    GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(unsigned int Index, void* Out) override;
    GENRESULT NEURONCOMPONENTS_API SelectMode(int* Mode, unsigned int* OutIndex) override;
    GENRESULT NEURONCOMPONENTS_API CreateBuffers(HWND Hwnd, int* Mode, int* OutMode) override;
    GENRESULT NEURONCOMPONENTS_API GetModeBlob(void* OutAdapter, void* OutMode10Dwords) override;
    GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() override;
    GENRESULT NEURONCOMPONENTS_API ClearBuffers(unsigned long Flags, const void* Rects) override;
    GENRESULT NEURONCOMPONENTS_API SwapBuffers() override;
    GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) override;
    GENRESULT NEURONCOMPONENTS_API UnlockBuffer() override;
    GENRESULT NEURONCOMPONENTS_API GetBufferInterface(void* Out, void* Unused) override;
    GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) override;
    GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) override;
    GENRESULT NEURONCOMPONENTS_API GetViewportRect(int* OutX, int* OutY, int* OutW, int* OutH) override;
    GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) override;
    GENRESULT NEURONCOMPONENTS_API GetDepthRange(float* OutMinZ, float* OutMaxZ) override;
    GENRESULT NEURONCOMPONENTS_API SetWindow(HWND Hwnd, int X, int Y, int W, int H) override;
    GENRESULT NEURONCOMPONENTS_API GetWindow(HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW,
                                  int* OutH) override;
    GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) override;
    GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) override;
    GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) override;
    GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) override;
    GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) override;
    GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) override;
    GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) override;
    GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) override;
    GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY,
                                  float AtZ, float UpX, float UpY, float UpZ) override;
    GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top,
                                 float NearPlane, float FarPlane) override;
    GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane,
                                       float FarPlane) override;
    GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) override;
    GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) override;
    GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) override;
    GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) override;
    GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) override;
    GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) override;
    GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format,
                                      unsigned int Levels, unsigned int Flags,
                                      void** OutHandle) override;
    GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) override;
    GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureDim(void* Handle, unsigned int* OutW, unsigned int* OutH,
                                      int* OutLevels) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureInterface(void* Handle, void* UnusedA, void* UnusedB) override;
    GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                            unsigned int Pitch, unsigned int* SrcFormat,
                                            const void* Pixels, void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API BlitTexture(void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) override;
    GENRESULT NEURONCOMPONENTS_API SetRenderTarget(void* Handle, void* UnusedA, void* UnusedB) override;
    GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API BeginScene() override;
    GENRESULT NEURONCOMPONENTS_API EndScene() override;
    GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() override;
    GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) override;
    GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) override;
    GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) override;
    GENRESULT NEURONCOMPONENTS_API VerifyState() override;
    GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                      unsigned int Count, void* Unused) override;
    GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                               unsigned int VertCount, const void* Indices,
                                               unsigned int IndexCount, unsigned long Unused) override;
    GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex,
                                        unsigned int Count, void* Unused) override;
    GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb, unsigned int BaseVertex,
                                               unsigned int NumVerts, const void* Indices,
                                               unsigned int IndexCount, unsigned long Unused) override;
    GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) override;
    GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) override;

    GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) override;
    GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() override;
    GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count, FVertexMemoryLock* Out) override;
    GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) override;
    GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() override; // formerly VbMgrSlot7
    GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf,
                                      const FRpVertexSrcDesc* Source,
                                      unsigned int StartVertex, unsigned int Count) override;

    GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int MinIndex,
                                             unsigned int NumVertices, unsigned int StartIndex,
                                             unsigned int Count) override;

    GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out,
                                 unsigned int Flags) override;
    GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) override;
    GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset, const unsigned short* SrcData, unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out,
                               unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API UnlockIb(void* A) override;
    GENRESULT NEURONCOMPONENTS_API SelectIb(void* A, unsigned int BaseVertexIndex,
                                 unsigned int Unused3, unsigned int Unused4) override;
    GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* A, unsigned int* OutCapacity) override;
    GENRESULT NEURONCOMPONENTS_API IsIbValid(void* A) override;

    GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                 unsigned int Flags) override;
    GENRESULT NEURONCOMPONENTS_API DestroyVb(void** A) override;
    GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* OutOffset, const FRpVertexSrcDesc* SrcData, unsigned int StartVertex, unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out,
                               unsigned int Count) override;
    GENRESULT NEURONCOMPONENTS_API UnlockVb(void* A) override;
    GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) override; // formerly VbSlot9
    GENRESULT NEURONCOMPONENTS_API SelectVb(void* A) override;
    GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API IsVbValid(void* A) override;

    GENRESULT NEURONCOMPONENTS_API SetGammaEnable(unsigned char Enable) override;
    GENRESULT NEURONCOMPONENTS_API GetGammaEnable() override;
    GENRESULT NEURONCOMPONENTS_API SetGammaFunction(unsigned int ChannelMask, float Gamma, float Brightness,
                                         float Contrast, float Bias) override;
    GENRESULT NEURONCOMPONENTS_API SetGammaRamp(unsigned int ChannelMask, const unsigned short* RampData) override;
    GENRESULT NEURONCOMPONENTS_API GetGammaRamp(unsigned int Channel, unsigned short* OutRamp) override;

    GENRESULT NEURONCOMPONENTS_API PrintScreen(void* ParentFileSystem, const char* FileName) override;
    GENRESULT NEURONCOMPONENTS_API LoadTexture(IFileSystem* FileSystem, const char* FileName,
                                    void** OutHandle) override;
    GENRESULT NEURONCOMPONENTS_API LoadSurfaceData(void* Handle, int Level, unsigned int SrcSize,
                                        const void* SrcData) override;
    GENRESULT NEURONCOMPONENTS_API GetImageInfo(_D3DXIMAGE_INFO* Info, unsigned int SrcSize,
                                     const void* SrcData) override;
    GENRESULT NEURONCOMPONENTS_API LoadCubemap(IFileSystem* FileSystem, const char* FileName,
                                    void** OutHandle) override;

    GENRESULT NEURONCOMPONENTS_API Initialize() override;

    // IRenderPipelineEx (DX9RENDEREREx.cpp).
    GENRESULT NEURONCOMPONENTS_API GetExCaps(FRPExCaps* Out) override;
    GENRESULT NEURONCOMPONENTS_API CreateExProgram(IFileSystem* FileSystem, const char* Path,
                                        const char* VsEntry, const char* PsEntry,
                                        FRPExProgram** Out) override;
    GENRESULT NEURONCOMPONENTS_API DestroyExProgram(FRPExProgram* Program) override;
    GENRESULT NEURONCOMPONENTS_API ReloadExPrograms() override;
    GENRESULT NEURONCOMPONENTS_API CreateExRenderTarget(const FRPExRenderTargetDesc* Desc,
                                             FRPExTexture** Out) override;
    GENRESULT NEURONCOMPONENTS_API CreateExVolumeTexture(unsigned long Width, unsigned long Height,
                                              unsigned long Depth, ERPExFormat Format,
                                              const void* Data, FRPExTexture** Out) override;
    GENRESULT NEURONCOMPONENTS_API DestroyExTexture(FRPExTexture* Texture) override;
    GENRESULT NEURONCOMPONENTS_API GetExTextureSize(FRPExTexture* Texture, unsigned long* OutWidth,
                                         unsigned long* OutHeight) override;
    GENRESULT NEURONCOMPONENTS_API GetSceneDepth(FRPExTexture** Out) override;
    GENRESULT NEURONCOMPONENTS_API BeginExPass(const FRPExPassDesc* Desc) override;
    GENRESULT NEURONCOMPONENTS_API EndExPass() override;
    GENRESULT NEURONCOMPONENTS_API SetExRenderTarget(FRPExTexture* ColorTarget,
                                          unsigned long bKeepDepth) override;
    GENRESULT NEURONCOMPONENTS_API BindExProgram(FRPExProgram* Program) override;
    GENRESULT NEURONCOMPONENTS_API SetExVertexConstants(unsigned long StartRegister, const float* Data,
                                             unsigned long Vector4Count) override;
    GENRESULT NEURONCOMPONENTS_API SetExPixelConstants(unsigned long StartRegister, const float* Data,
                                            unsigned long Vector4Count) override;
    GENRESULT NEURONCOMPONENTS_API BindExTexture(unsigned long Sampler, FRPExTexture* Texture,
                                      const FRPExSamplerState* State) override;
    GENRESULT NEURONCOMPONENTS_API BindExPipelineTexture(unsigned long Sampler, void* Handle,
                                              const FRPExSamplerState* State) override;

    // IRPDrawEx (DX9RENDEREREx.cpp).
    GENRESULT NEURONCOMPONENTS_API DrawFullscreenQuad() override;
    GENRESULT NEURONCOMPONENTS_API DrawExPrimitiveUp(unsigned long Prim, unsigned long Fvf,
                                          const void* Verts, unsigned long Count) override;

    // DX9RENDERER Ex hooks into the DX8RENDERER-mirrored lifetime (DX9RENDEREREx.cpp). With no Ex object ever created
    // and no INTZ support they do nothing, so the DX8RENDERER behaviour is unchanged.
    // CreateBuffers, before CreateDevice/Reset: true when the depth buffer becomes an INTZ
    // texture (24-bit depth picked and INTZ supported) instead of the auto depth-stencil.
    bool ExWantsIntzDepth(D3DFORMAT AdapterFormat, D3DFORMAT DepthFormat);
    // CreateBuffers, after CreateDevice/Reset succeeded: the explicit depth surface (INTZ, or a
    // plain DepthFormat surface when the INTZ texture cannot be created), then every
    // D3DPOOL_DEFAULT Ex resource, and the programs / volumes a device re-creation lost.
    HRESULT ExCreateDeviceResources(bool bIntzDepth, D3DFORMAT DepthFormat);
    // DestroyGpuResources: ends an open pass and releases the explicit depth surface and every
    // D3DPOOL_DEFAULT Ex resource (handles stay valid).
    void ExReleaseDeviceResources();
    // ShutdownPipeline, before the device goes: also the shaders and managed volume textures
    // (bytecode and volume data are kept for a later device).
    void ExReleaseAllGpu();
    // ~FDirectX9: frees every handle.
    void ExDestroyAll();
    // DX9RENDERER.cpp helpers on the DX8RENDERER-mirrored caches and handles.
    IDirect3DBaseTexture9* PipelineTextureFromHandle(void* Handle);
    void ExSaveStage(unsigned long Stage, FDx9RendererExStageSave* Out);
    void ExRestoreStage(unsigned long Stage, const FDx9RendererExStageSave* Saved);

    GENRESULT QueryLocal(const char* Iid, void** Out);
    void FlushDrawState(IDirect3DDevice9* Device); // formerly static FlushDrawState
    GENRESULT ApplySectionName(const FDx9RendererAggDesc* Desc);
    // DX8RENDERER the routine (startup and create_buffers call it with the device caps, in D3D8 shape).
    // The flag part runs only after the MGSDB database loaded; the ability-map overrides run on
    // every path.
    void ApplyDeviceCaps(const FDx9RendererCaps8* Caps);
    void ApplyMgsdbFlags(FMgsdbDatabase* Db);
    void ApplyAbilityOverrides();
    unsigned long QueryVidMem();
    D3DFORMAT PickDepthFormat(D3DFORMAT AdapterFormat, D3DFORMAT BackBufferFormat, int* WantedZ,
                              int* WantedS);
    bool CheckTextureFormat(unsigned int Format, unsigned long Usage, D3DRESOURCETYPE RType);
    unsigned int PrimCountFromVerts(unsigned long Prim, unsigned int Count);
    void EnumAdapters();
    // Shared lifetime checks retain the diagnostic and source line of each caller.
    GENRESULT NeedStartup(const char* Message, int Line);
    GENRESULT NeedBuffers(const char* Message, int Line);
    GENRESULT NeedDeviceLifetime(const char* Message, int Line);
    // DX8RENDERER FDirectX8::ObjectBase has no counterpart: its raw offsets are the named members below.
    // DX8RENDERER [this + 0x354 + Id*0xC]: the three dwords of pipeline-state Id; Id 0xE addresses
    // Ability_[0..2] as in DX8RENDERER. False for any other id (DX9RENDERER-GAP, see DX9RENDERER.cpp).
    bool PipelineStateWords(int Id, unsigned long** OutValue, unsigned long** OutHasDefault,
                            unsigned long** OutOverridden);
    // DX8RENDERER ResolveIbHandle(IbThis, Handle): handle -1 is the inline scratch index buffer.
    FDx9RendererIb* ResolveIbHandle(void* Handle);
    // D3D9 helpers: the adapter identifier in D3D8 shape, and the adapter's display modes as
    // one list (D3D9 enumerates per format; see AdapterModeCount in DX9RENDERER.cpp).
    void ReadAdapterIdentifier8(UINT Adapter, FDx9RendererIdentifier8* Out);
    UINT AdapterModeCount(UINT Adapter);
    HRESULT EnumAdapterMode(UINT Adapter, UINT Index, D3DDISPLAYMODE* Out);
    // / the routine: MulTransform(diag(1,1,-1), M12) — negate row2 + Tz.
    void MulZFlip12(float* Out12, const float* In12);
    void ApplyIdentityTransforms();
    void ApplyDeviceViewport();
    void ApplyDefaultRenderStates();
    void ApplyDefaultTextureStages();
    void ApplyPostDeviceHelpers();
    void UnbindTextureHandle(void* Handle);
    unsigned int PfIndex(unsigned int Format);
    unsigned int PfToD3d(unsigned int Format);
    unsigned int PfBytesPerPixel(unsigned int Format);
    unsigned int FvfStride(unsigned int Fvf);
    FDx9RendererVb* FindOrInsertPoolVb(unsigned int Fvf);
    void GrowPoolVb(FDx9RendererVb* Wrapper, unsigned int Fvf, unsigned int Count);
    void ReleasePoolVbs();
    unsigned long StageCount();
    int PipelineStateValue(int Id);

    // Pixel-format-class catalog (baseline the routine/the routine/the routine).
    FormatClassNode* FindFormatClass(unsigned int Key);
    FormatClassNode* FindOrInsertFormatClass(unsigned int Key);
    void ClearFormatCatalog();
    void ClearFormatClassCandidates(FormatClassNode* Node);
    void AppendFormatCandidate(FormatClassNode* Node, unsigned int Value);
    unsigned int ResolvePixelFormatByComponents(unsigned int Bpp, unsigned int RBits,
                                                unsigned int GBits, unsigned int BBits,
                                                unsigned int AlphaBits);
    unsigned int FormatClassKeyFromPfId(unsigned int PfId);
    void PopulateFormatCatalog();

    IProfileParser* Parser_;
    // AGGDESC+0x10 (or "RenderPipeline") is strcpy'd here; startup(0) reads this.
    char SectionName_[0x80];
    // The section startup last read DeviceId/DeviceType/DeviceTypeFallback from; a startup with
    // the same name skips that block. The ability map is read from this section too.
    char LastSection_[0x80];
    unsigned long CreateFlags_;
    // bit0 LOCKABLE_BACKBUFFER, bit1 USE_SYSLOCK, bit2 HANDLE_SWAPLOSS.
    unsigned long SwapFlags_;
    IDirect3D9* D3d_;
    IDirect3DDevice9* Device_;
    IDirect3DSurface9* LockBufferSurf_;
    FDx9RendererStreamCache StreamCache_;
    // DX8RENDERER +0x14C..+0x168 (ScratchIbFlags_, ScratchIb_, Pad154_, IbUsed_, IbCapacity_ there): the
    // inline scratch index-buffer node; IRPIndexBuffer handle -1 names it.
    FDx9RendererIb ScratchIb_;
    unsigned long VidMem_;
    // The VBMgr / create_vb(flags & 1) pool: map<FVF, vertex-buffer node>.
    std::map<unsigned long, FDx9RendererVb> VbPool_;
    // Bytes held by the pool's vertex buffers (stride * count per entry).
    unsigned long VbPoolBytes_;
    // bit0: create_buffers .. destroy_buffers window. bit1: IGammaControl enable
    // (set_gamma_enable writes it, set_gamma_function / set_gamma_ramp pass it as D3DSGR_CALIBRATE).
    unsigned long DeviceReady_;
    FormatClassNode* FormatCatalogBegin_;
    FormatClassNode* FormatCatalogEnd_;
    FormatClassNode* FormatCatalogCap_;
    std::vector<FDx9RendererAdapter> Adapters_;
    unsigned long AdapterIndex_;
    unsigned long DeviceType_;
    char MgsdbPath_[0x80];
    unsigned long Field230_;
    unsigned long BbPfIndex_;
    // {mode index, ?, width, height, colour bits, z bits, stencil bits, interval, buffer count,
    // byte flags (+0x24 swap-is-flip-or-copy, +0x25 fullscreen)}.
    int ModeBlob_[10];
    HWND Hwnd_;
    int WindowX_;
    int WindowY_;
    int WindowW_;
    int WindowH_;
    HWND WindowOverride_;
    // Startup properties, ability overrides and default device states.
    FDx9RendererPropertyMap PropertyMap_;
    FDx9RendererPropertyMap AbilityMap_;
    FDx9RendererPropertyMap RenderStateMap_;
    FDx9RendererPropertyMap StageStateMaps_[8];
    // DX8RENDERER +0x354..+0x3FC (Pad354_, TextureAllow8Bit_..Field398_, Pad39C_, BltNotFlip_,
    // Field3F4_, Field3F8_ there).
    FDx9RendererPipelineStateSlot PipelineStates_[kDx9RendererPipelineStateCount];
    unsigned long Ability_[kDx9RendererAbilityCount];
    FDx9RendererRenderStateCache RenderStates_;
    FDx9RendererDeviceStateCaches DeviceStates_;
    FDx9RendererLightManager LightManager_;
    FDx9RendererPipelineTransforms Transforms_;
    D3DVIEWPORT9 ViewportCache_;
    unsigned char bDepthRangeValid_;
    unsigned char bViewportRectValid_;
    // DX8RENDERER +0x22E4 / +0x22F0 (kOffVbList / kOffIbList inside Pad22E2_ there).
    FDx9RendererVbList VbList_;
    FDx9RendererIbList IbList_;
    // D3D9 only. D3D8 keeps the BaseVertexIndex of SetIndices as device state; D3D9 takes it as
    // an argument of every DrawIndexedPrimitive. This is the value the last successful flush of
    // the index stream gave the D3D8 device (0 after create/reset, as D3D8's default).
    INT DeviceBaseVertex_;
    INeuronComponent* Outer_;
    FDx9RendererInner Inner_;
    // DX9RENDERER Ex state (port-only; DX9RENDEREREx.h): programs, Ex textures, the INTZ scene depth, the open
    // pass. Allocated by the constructor, freed by the destructor.
    FDx9RendererExState* Ex_;
};
