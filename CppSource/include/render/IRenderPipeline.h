// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "render/d3d8_min.h"

#include <cstddef>

// IRPTexture parameter types: the FileSystem interface is declared in readfile.h, the image
// info record is the DirectX 8.1 SDK's D3DXIMAGE_INFO (d3dx8tex.h).
struct IFileSystem;
struct _D3DXIMAGE_INFO;


// DX8RENDERER-only names. Every IID is Iids::* from neuroncomponents_iids.h and IAggregateComponent itself is
// declared in neuroncomponents.h, so nothing here is shared with render/shading.h any more.


static const char kDx8RendererComponentName[] = "DirectX8";
static const char kDx8RendererDefaultSection[] = "RenderPipeline";
// DX9RENDERER.dll: the Direct3D 9 mirror of DX8RENDERER (same IIDs, same default section). See implementation notes.
static const char kDx9RendererComponentName[] = "DirectX9";


// The C++ type is IRenderPipeline; its IID string is "1.11_IRenderPipeline8B" (Iids::RenderPipeline8B).
// baseline evidence: NeuronRuntime.dll exports ?Pipeline@NeuronRuntime@@3PAUIRenderPipeline@@A (OutpostFrontier.exe
// imports the same name), so the pointee of NeuronRuntime::Pipeline is a struct named IRenderPipeline,
// and that pointer is filled by QueryInterface for "1.11_IRenderPipeline8B" (NeuronRuntime 065c12e8).
// No baseline binary carries "IRenderPipeline8B" anywhere except that IID string (particlefx,
// deformable2, surfacematerials, rendcomp, dx8renderer, shading, NeuronRuntime); their error texts name the IID macro
// "IID_IRenderPipeline" ("ParticleFx: init: unable to acquire IID_IRenderPipeline"). "8B" is the
// IID's version suffix, not part of the type name.
struct IRenderPipeline : INeuronComponent
{
    // slot 3
    virtual GENRESULT NEURONCOMPONENTS_API Startup(const char* SectionName) = 0;
    // slot 4 the routine
    virtual GENRESULT NEURONCOMPONENTS_API ShutdownPipeline() = 0;
    // slot 5 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetPipelineState(int Id, int Value) = 0;
    // slot 6 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetPipelineState(int Id, int* Out) = 0;
    // slot 7 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetDeviceInfo(void* Out520) = 0;
    // slot 8 the routine
    virtual GENRESULT NEURONCOMPONENTS_API QueryDeviceAbility(unsigned int Index, unsigned long* Out) = 0;
    // slot 9 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetDisplayModeCount(unsigned int* Out) = 0;
    // slot 10 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetDisplayModeEnum(unsigned int Index, void* Out) = 0;
    // slot 11 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SelectMode(int* Mode, unsigned int* OutIndex) = 0;
    // slot 12 the routine
    virtual GENRESULT NEURONCOMPONENTS_API CreateBuffers(HWND Hwnd, int* Mode, int* OutMode) = 0;
    // slot 13 the branch
    virtual GENRESULT NEURONCOMPONENTS_API GetModeBlob(void* OutAdapter, void* OutMode10Dwords) = 0;
    // slot 14 the routine
    virtual GENRESULT NEURONCOMPONENTS_API DestroyGpuResources() = 0;
    // slot 15 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API ClearBuffers(unsigned long Flags, const void* Rects) = 0;
    // slot 16 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SwapBuffers() = 0;
    // slot 17 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API LockBuffer(void* Out) = 0;
    // slot 18 the routine
    virtual GENRESULT NEURONCOMPONENTS_API UnlockBuffer() = 0;
    // slot 19 the routine RET 0xC — this + Out + unread Unused (body never
    // touches the second stack dword; every baseline caller still pushes it).
    virtual GENRESULT NEURONCOMPONENTS_API GetBufferInterface(void* Out, void* Unused) = 0;
    // slot 20 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetDeviceStats(void* Out) = 0;
    // slot 21 the routine RET 0x14
    virtual GENRESULT NEURONCOMPONENTS_API SetViewportRect(int X, int Y, int W, int H) = 0;
    // slot 22 the routine RET 0x14
    virtual GENRESULT NEURONCOMPONENTS_API GetViewportRect(int* OutX, int* OutY, int* OutW, int* OutH) = 0;
    // slot 23 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetDepthRange(float MinZ, float MaxZ) = 0;
    // slot 24 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetDepthRange(float* OutMinZ, float* OutMaxZ) = 0;
    // slot 25 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API SetWindow(HWND Hwnd, int X, int Y, int W, int H) = 0;
    // slot 26 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetWindow(HWND* OutHwnd, int* OutLeft, int* OutRight, int* OutW,
                                          int* OutH) = 0;
    // slot 27 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetWorld(const float* M12) = 0;
    // slot 28 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API GetWorld(float* Out12) = 0;
    // slot 29 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API SetView(const float* M12) = 0;
    // slot 30 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API GetView(float* Out12) = 0;
    // slot 31 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetModelview(const float* M12) = 0;
    // slot 32 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API GetModelview(float* Out12) = 0;
    // slot 33 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API SetProjection(const float* M16) = 0;
    // slot 34 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetProjection(float* Out16) = 0;
    // slot 35 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetLookat(float EyeX, float EyeY, float EyeZ, float AtX, float AtY,
                                          float AtZ, float UpX, float UpY, float UpZ) = 0;
    // slot 36 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetOrtho(float Left, float Right, float Bottom, float Top,
                                         float NearPlane, float FarPlane) = 0;
    // slot 37 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetPerspective(float FovDeg, float Aspect, float NearPlane,
                                             float FarPlane) = 0;
    // slot 38 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API SetLight(void* Handle, const D3DLIGHT8* Light) = 0;
    // slot 39 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API DestroyLight(void* Handle) = 0;
    // slot 40 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetLight(void* Handle, D3DLIGHT8* OutLight) = 0;
    // slot 41 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetLightEnable(void* Handle, int Enable) = 0;
    // slot 42 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetLightEnable(void* Handle, int* OutEnable) = 0;
    // slot 43 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetMaterial(const D3DMATERIAL8* Material) = 0;
    // slot 44 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetMaterial(D3DMATERIAL8* Material) = 0;
    // slot 45 the routine
    virtual GENRESULT NEURONCOMPONENTS_API CreateTexture(int Width, int Height, unsigned int* Format,
                                              unsigned int Levels, unsigned int Flags,
                                              void** OutHandle) = 0;
    // slot 46 the routine
    virtual GENRESULT NEURONCOMPONENTS_API DestroyTexture(void* Handle) = 0;
    // slot 47 the routine
    virtual GENRESULT NEURONCOMPONENTS_API IsTexture(void* Handle) = 0;
    // slot 48 the routine
    virtual GENRESULT NEURONCOMPONENTS_API LockTexture(void* Handle, int Level, void* Out) = 0;
    // slot 49 the routine
    virtual GENRESULT NEURONCOMPONENTS_API UnlockTexture(void* Handle, int Level) = 0;
    // slot 50 the routine RET 0xC
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureFormat(void* Handle, int* OutFormat) = 0;
    // slot 51 the routine RET 0x14 -- four arguments, not three. 06d0e972/06d0e97a write the
    // width and the height, 06d0e9b1/06d0e9e4 write the level count (6 for a cube texture).
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureDim(void* Handle, unsigned int* OutW, unsigned int* OutH,
                                              int* OutLevels) = 0;
    // slot 52 the routine RET 0x10 — this + Handle + two unread stack dwords.
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureInterface(void* Handle, void* UnusedA,
                                                    void* UnusedB) = 0;
    // slot 53 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetTextureLevelData(void* Handle, int Level, int SrcW, int SrcH,
                                                    unsigned int Pitch, unsigned int* SrcFormat,
                                                    const void* Pixels, void* A, void* B) = 0;
    // slot 54 the routine. RET 0x34 = 13 stdcall stack dwords (this + 12 explicit),
    // confirmed by instruction-level trace of dx8renderer @ 22029-22437. Only 4 of the
    // 12 explicit dwords are ever read by baseline: SrcHandle, SrcLevel, DstHandle,
    // DstLevel (roles confirmed the same way LockTexture/UnlockTexture's Extra==0/1
    // GetSurfaceLevel/GetCubeMapSurface branch was confirmed). The remaining 8 dwords
    // sit as two untouched 4-dword runs immediately after SrcLevel and after DstLevel
    // -- exactly sizeof(RECT) passed by value each -- matching an earlier RE note's "Круг
    // 2/3" (Handle,Level,Rect) x2 hypothesis, but baseline's own the routine blit call
    // is invoked unconditionally with NULL rect pointers/counts (0x06d0f649), so those
    // 8 dwords are provably dead code in this build regardless of what they represent.
    // No caller of this vtable slot was found in any available analysis (dx8renderer/shading/
    // rendcomp/ximage/OutpostFrontier.exe), so the two RECT-by-value slots are intentionally
    // NOT added to this signature (would be unfalsifiable ABI cosplay for parameters
    // that are read by nobody, on either side). If a caller ever surfaces requiring
    // partial-rect blit, extend this signature then -- do not guess it now.
    // arity-ok: decl width 0x14 vs baseline RET 0x34 is intentional (see above).
    // Direction (RC-3 2026-09-30): the routine = D3DXLoadSurfaceFromSurface(pDest, 0, 0,
    // pSrc, 0, 0, D3DX_FILTER_POINT, 0) with pDest = the FIRST handle's surface (EBP+0xC/+0x10)
    // and pSrc = the SECOND's (EBP+0x24/+0x28): the first pair is the destination.
    virtual GENRESULT NEURONCOMPONENTS_API BlitTexture(void* DstHandle, int DstLevel, void* SrcHandle, int SrcLevel) = 0;
    // slot 55 the routine RET 0x10 — this + Handle + two unread stack dwords.
    virtual GENRESULT NEURONCOMPONENTS_API SetRenderTarget(void* Handle, void* UnusedA,
                                                void* UnusedB) = 0;
    // slot 56 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetRenderTarget(void* Handle) = 0;
    // slot 57 the routine
    virtual GENRESULT NEURONCOMPONENTS_API BeginScene() = 0;
    // slot 58 the routine
    virtual GENRESULT NEURONCOMPONENTS_API EndScene() = 0;
    // slot 59 the routine
    virtual GENRESULT NEURONCOMPONENTS_API ResetRenderStatesToDefaults() = 0;
    // slot 60 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetRenderState(D3DRENDERSTATETYPE State, int Value) = 0;
    // slot 61 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetRenderState(D3DRENDERSTATETYPE State, int* Out) = 0;
    // slot 62 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetTextureStageState(int Stage, int Type, int Value) = 0;
    // slot 63 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureStageState(int Stage, int Type, int* Out) = 0;
    // slot 64 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetTextureStageTransform(int Stage, const float* Matrix16) = 0;
    // slot 65 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureStageTransform(int Stage, float* OutMatrix16) = 0;
    // slot 66 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetTextureStageTexture(int Stage, void* Handle) = 0;
    // slot 67 the routine
    virtual GENRESULT NEURONCOMPONENTS_API GetTextureStageTexture(int Stage, void** OutHandle) = 0;
    // slot 68 the routine
    virtual GENRESULT NEURONCOMPONENTS_API VerifyState() = 0;
    // slot 69 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API DrawPrimitive(unsigned long Prim, unsigned int Fvf, const void* Verts,
                                              unsigned int Count, void* Unused) = 0;
    // slot 70 the routine RET 0x20 -- eight dwords, so this plus SEVEN arguments, the last
    // one unread by the callee just like slot 72's. OutpostFrontier.exe 0x004217b6 and 0x00421973
    // both push eight: PUSH EBX (the trailing dword), index count, indices, vertex count,
    // vertices, PUSH 0x142 (an FVF: XYZ|DIFFUSE|TEX1), PUSH 2 or 4 (the primitive type), this.
    virtual GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveUp(unsigned long Prim, unsigned int Fvf,
                                                       const void* Verts, unsigned int VertCount,
                                                       const void* Indices,
                                                       unsigned int IndexCount,
                                                       unsigned long Unused) = 0;
    // slot 71 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API DrawPrimitiveVb(unsigned long Prim, void* Vb, unsigned int StartVertex,
                                                unsigned int Count, void* Unused) = 0;
    // slot 72 the routine RET 0x20 -- eight dwords, so this plus SEVEN arguments. analysis
    // lists only param_1..param_7 because the last one is never read, but the callee pops it
    // all the same and every baseline caller pushes it: shading.dll 0x06eb328a, 0x06eb3c0f,
    // and 0x06eba8ff each open with PUSH 0x0 and push eight dwords in total.
    // Declaring six here made ShadingInternal.h's PipeDrawIndexedVb underfill the stack.
    virtual GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitiveVb(unsigned long Prim, void* Vb,
                                                       unsigned int BaseVertex, unsigned int NumVerts,
                                                       const void* Indices,
                                                       unsigned int IndexCount,
                                                       unsigned long Unused) = 0;
    // slot 73 the routine
    virtual GENRESULT NEURONCOMPONENTS_API AddLight(void* Handle) = 0;
    // slot 74 the routine
    virtual GENRESULT NEURONCOMPONENTS_API RemoveLight(void* Handle) = 0;
    // slot 75 the routine
    // (int,uint) forwards its uint arg unchanged into the routine,
    // which passes it straight through to the routine's tree lookup on
    // the SAME tree that the routine/the routine (called from
    // AddLight/SetLightEnable/DestroyLight/RemoveLight) exclusively insert/erase
    // keyed by the raw Handle pointer value (dx8renderer/21828/22012/22037-39;
    // confirmed by tracing the routine/the routine's own key locals, which copy
    // the incoming `int*` Handle param by value -- never dereference it -- before
    // taking its address as the generic comparison key). Since that tree is never
    // populated with plain D3D indices, a genuine index key here would make
    // UpdateLight permanently fail to find anything; analysis's `uint` typing of
    // this parameter is a analysis tool artifact (no dereference inside the routine
    // gave it a pointer cue), not evidence of a second key domain. Handle
    // confirmed correct 2026-09-10 (dx8renderer.dll module, item 0x06d2ca43 notes).
    virtual GENRESULT NEURONCOMPONENTS_API UpdateLight(void* Handle) = 0;
    // slot 76 the routine
    virtual GENRESULT NEURONCOMPONENTS_API SetWorldN(int WorldIndex, const float* M12) = 0;
};

// The source-vertex descriptor IVertexBufferManager::CopyVertexData (slot 8) takes as its third
// argument: baseline dx8renderer.dll the routine's param_3. 0x70 bytes -- a flags word, five 0x14-byte
// streams and the source FVF with its vertex count. The client builders write every dword of it
// (OutpostFrontier.exe the routine for the asteroid field, the routine for the part cache, both
// with Flags 2 and Count in each stream); DX8RENDERER's converters read Flags, SrcFvf and each stream's
// Base / Stride / IndexArray (the generic fallbacks also a texcoord stream's ByteSize), never a
// stream's Count or the trailing SrcCount.
struct FRpVertexStream
{
    const unsigned char* Base;       // +0x00
    unsigned int ByteSize;           // +0x04  bytes of the element (0xc position, 4 colour, ...)
    unsigned int Stride;             // +0x08  bytes between two vertices of this stream
    const unsigned long* IndexArray; // +0x0c  one DWORD per vertex when Flags & 4 (indexed)
    unsigned int Count;              // +0x10  vertices in the stream
};

struct FRpVertexSrcDesc
{
    unsigned int Flags;              // +0x00  2: interleaved source, 4: indexed (family B)
    FRpVertexStream Position;        // +0x04
    FRpVertexStream Normal;          // +0x18
    FRpVertexStream Diffuse;         // +0x2c
    FRpVertexStream Tex[2];          // +0x40 / +0x54  texcoord sets 0 and 1
    unsigned int SrcFvf;             // +0x68
    unsigned int SrcCount;           // +0x6c
};

static_assert(sizeof(FRpVertexStream) == 0x14, "FRpVertexStream is 0x14 bytes");
static_assert(sizeof(FRpVertexSrcDesc) == 0x70, "FRpVertexSrcDesc is 0x70 bytes");
static_assert(offsetof(FRpVertexSrcDesc, Normal) == 0x18, "FRpVertexSrcDesc +0x18 normal");
static_assert(offsetof(FRpVertexSrcDesc, Diffuse) == 0x2c, "FRpVertexSrcDesc +0x2c diffuse");
static_assert(offsetof(FRpVertexSrcDesc, Tex) == 0x40, "FRpVertexSrcDesc +0x40 texcoords");
static_assert(offsetof(FRpVertexSrcDesc, SrcFvf) == 0x68, "FRpVertexSrcDesc +0x68 source FVF");

// Borrowed vertex-buffer handle and draw offset.
struct FVbListing
{
    void* VertexBuffer;
    unsigned long Fvf;
    unsigned long BaseVertex;
};

// DX8RENDERER writes the listing and locked memory; materials borrow the listing prefix.
struct FVertexMemoryLock
{
    FVbListing Listing;
    void* Vertices;
};
static_assert(sizeof(FVbListing) == 0xC, "vertex listing three words");
static_assert(offsetof(FVertexMemoryLock, Vertices) == 0xC, "vertex memory after listing");
static_assert(sizeof(FVertexMemoryLock) == 0x10, "vertex memory lock four words");

struct IVertexBufferManager : INeuronComponent
{
    // slot 3 the routine RET 0x14 -- four explicit dwords, and the body reads NONE of them:
    // it is `return 0`. Declared to the width anyway, because the width is the contract.
    virtual GENRESULT NEURONCOMPONENTS_API VbMgrInit(void* A, void* B, void* C, void* D) = 0;
    // slot 4 the routine
    virtual GENRESULT NEURONCOMPONENTS_API VbMgrReleaseAll() = 0;
    // slot 5 the routine RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API LockVertexMemory(unsigned int Fvf, unsigned int Count, FVertexMemoryLock* Out) = 0;
    // slot 6 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API UnlockVertexMemory(FVertexMemoryLock* Out) = 0;
    // slot 7 the routine return 0xFFFFFFF9
    virtual GENRESULT NEURONCOMPONENTS_API UnsupportedManagerOperation() = 0; // formerly VbMgrSlot7
    // slot 8 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf,
                                              const FRpVertexSrcDesc* Source,
                                              unsigned int StartVertex, unsigned int Count) = 0;
};

struct IRPDraw : INeuronComponent
{
    // slot 3 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API DrawIndexedPrimitive(unsigned long Prim, unsigned int MinIndex,
                                                     unsigned int NumVertices,
                                                     unsigned int StartIndex,
                                                     unsigned int Count) = 0;
};

struct IRPIndexBuffer : INeuronComponent
{
    // slot 3 the routine RET 0x10 -- THREE explicit dwords, not two. The body reads only
    // the first two; the trailing one mirrors CreateVb's Flags and is ignored on the index
    // path. Every caller in this tree used to push two and leave the callee popping four.
    virtual GENRESULT NEURONCOMPONENTS_API CreateIb(unsigned int Count, unsigned int** Out,
                                         unsigned int Flags) = 0;
    // slot 4 the routine RET 0x8 (the vtable entry is 06d13002; 06d13070 is an instruction
    // inside it, not the function start). The argument is a POINTER TO the caller's handle:
    // reads [Handle] and compares it against -1, 06d13087 writes 0 back through it.
    // Passing the handle by value makes the callee read the wrapper's first field as the
    // handle and then zero it.
    virtual GENRESULT NEURONCOMPONENTS_API DestroyIb(void** Handle) = 0;
    // slot 5 the routine
    virtual GENRESULT NEURONCOMPONENTS_API ResizeIb(void* Handle, unsigned int Count) = 0;
    // slot 6 the routine RET 0x14
    virtual GENRESULT NEURONCOMPONENTS_API CopyIndices(void* Handle, unsigned int* OutOffset, const unsigned short* SrcData, unsigned int Count) = 0;
    // slot 7 the routine — Handle==-1 resolves to ADDRESS this+0x140 (inline scratch
    // FDx8RendererIb at object+0x14C created in create_buffers), not a dereference of *(this+0x140).
    virtual GENRESULT NEURONCOMPONENTS_API LockIb(void* Handle, unsigned int* Offset, void** Out,
                                       unsigned int Count) = 0;
    // slot 8 the routine RET 0x8 (again, the vtable entry, not the inner address 06d136e0)
    virtual GENRESULT NEURONCOMPONENTS_API UnlockIb(void* Handle) = 0;
    // slot 9 the routine RET 0x14 -- FOUR explicit dwords. The FDUMP string in the body
    // names it select_ib(). It reads the first two: the handle, and a scalar it caches next to
    // the handle at this+0x12C (+8 = buffer, +0xC = this value, bit 1 of +0x10 = dirty). The
    // D3D8 shape of that pair is SetIndices(pIndexData, BaseVertexIndex), which is what the
    // port's own DX8RENDERER::SelectIb writes -- it just had no argument to put there and hardcoded 0.
    // Arguments 3 and 4 are never read; Deformable2 0x065f6a55 pushes literal zeros for them.
    virtual GENRESULT NEURONCOMPONENTS_API SelectIb(void* Handle, unsigned int BaseVertexIndex,
                                         unsigned int Unused3, unsigned int Unused4) = 0;
    // slot 10 the routine RET 0xC -- TWO explicit dwords, and the second is an out-param:
    // the body writes [buffer+0x18] through it and returns GR_OK, or 0x80004005 when it cannot.
    // The port declared one argument and returned the capacity AS the result code.
    virtual GENRESULT NEURONCOMPONENTS_API GetIbCapacity(void* Handle, unsigned int* OutCapacity) = 0;
    // slot 11 the routine
    virtual GENRESULT NEURONCOMPONENTS_API IsIbValid(void* Handle) = 0;
};

struct IRPVertexBuffer : INeuronComponent
{
    // slot 3 the routine RET 0x14
    virtual GENRESULT NEURONCOMPONENTS_API CreateVb(unsigned int Fvf, unsigned int Count, unsigned int** Out,
                                         unsigned int Flags) = 0;
    // slot 4 the routine -- same shape as DestroyIb: a POINTER TO the caller's handle.
    // reads [Handle], 06d11e38 writes 0 back through it.
    virtual GENRESULT NEURONCOMPONENTS_API DestroyVb(void** Handle) = 0;
    // slot 5 the routine
    virtual GENRESULT NEURONCOMPONENTS_API ResizeVb(void* Handle, unsigned int Fvf, unsigned int Count) = 0;
    // slot 6 the routine RET 0x18
    virtual GENRESULT NEURONCOMPONENTS_API CopyVertices(void* Handle, unsigned int* OutOffset, const FRpVertexSrcDesc* SrcData, unsigned int StartVertex, unsigned int Count) = 0;
    // slot 7 the routine RET 0x14
    virtual GENRESULT NEURONCOMPONENTS_API LockVb(void* Handle, unsigned int* Offset, void** Out,
                                       unsigned int Count) = 0;
    // slot 8 the routine
    virtual GENRESULT NEURONCOMPONENTS_API UnlockVb(void* Handle) = 0;
    // slot 9 the routine RET 0x8 -- one explicit dword, unread; the body is
    // `return 0xFFFFFFF9` ("unsupported"). Nothing in the tree calls it.
    virtual GENRESULT NEURONCOMPONENTS_API UnsupportedVertexBufferOperation(void* Handle) = 0; // formerly VbSlot9
    // slot 10 the routine RET 0x8
    virtual GENRESULT NEURONCOMPONENTS_API SelectVb(void* Handle) = 0;
    // slot 11 the routine RET 0x10
    virtual GENRESULT NEURONCOMPONENTS_API GetVbInfo(void* Handle, unsigned int* OutFvf, unsigned int* OutCount) = 0;
    // slot 12 the routine
    virtual GENRESULT NEURONCOMPONENTS_API IsVbValid(void* Handle) = 0;
};

// Slot order from dx8renderer.dll's IGammaControl vtables (module storage and 06d5e540, identical
// from slot 3 on): 06d153e6, 06d1560c, 06d15757, 06d1535d, 06d153a4. Confirmed by the only exe
// caller the routine: +0x18 (slot 6) with one byte argument, +0x0c (slot 3) with five
// (7, gamma, 0, 1.0f, 0). The earlier declaration order (enable/get-enable first) was wrong.
struct IGammaControl : INeuronComponent
{
    // slot 3 the routine. baseline RET 0x18 pops 6 stack dwords (this + 5 explicit args): a
    // channel bitmask (bit0=red/bit1=green/bit2=blue) plus 4 floats consumed straight off the
    // caller's stack (analysis never recovered named parameters for them, only "Stack[...]"
    // slots) that drive a per-channel curve: value = clamp(Brightness + pow(Contrast*x + Bias,
    // 1/Gamma), 0, 1) for x = i/255, i in [0,256). The previous single `void* Channels`
    // placeholder didn't carry the 4 floats baseline actually reads and has been replaced.
    virtual GENRESULT NEURONCOMPONENTS_API SetGammaFunction(unsigned int ChannelMask, float Gamma,
                                                 float Brightness, float Contrast,
                                                 float Bias) = 0;
    // slot 4 the routine. ChannelMask selects exactly one channel (1/2/4); RampData points at
    // that channel's 256 WORD source values.
    virtual GENRESULT NEURONCOMPONENTS_API SetGammaRamp(unsigned int ChannelMask,
                                             const unsigned short* RampData) = 0;
    // slot 5 the routine. Channel selects exactly one channel (1/2/4); OutRamp receives that
    // channel's 256 WORD values.
    virtual GENRESULT NEURONCOMPONENTS_API GetGammaRamp(unsigned int Channel, unsigned short* OutRamp) = 0;
    // slot 6 the routine. baseline param_2 is a byte (only bit0 is read); RET pops one stack
    // dword beyond the implicit this, i.e. exactly one explicit argument -- no hidden out-param.
    virtual GENRESULT NEURONCOMPONENTS_API SetGammaEnable(unsigned char Enable) = 0;
    // slot 7 the routine. baseline takes ZERO explicit stack arguments (RET pops only the
    // implicit this) -- the enabled/disabled state is communicated purely via the return code
    // (GR_OK enabled, GR_GENERIC not-enabled, 0xFFFFFFFD unsupported), nothing is written
    // through an out-pointer. The previous `void* OutEnable` placeholder param didn't exist
    // in baseline and has been removed.
    virtual GENRESULT NEURONCOMPONENTS_API GetGammaEnable() = 0;
};

struct IRPTexture : INeuronComponent
{
    // slot 3 the routine (RET 0xc: this + 2 args). ParentFileSystem != 0 -> FDUMP warning
    // "IFilesystem parent not yet supported" and GR_INVALID_PARMS (06d13de8); FileName == 0 ->
    // "filename not specified" and GR_INVALID_PARMS (06d13e47). FileName is a char path
    // (strrchr '.' at 06d13ebb). OutpostFrontier.exe the routine calls it as (0, path) (00425254).
    virtual GENRESULT NEURONCOMPONENTS_API PrintScreen(void* ParentFileSystem, const char* FileName) = 0;
    // slot 4 the routine RET 0x10: opens FileName through FileSystem (+0x7C with a
    // "FileSystem" descriptor), maps it (+0x3C/+0x40) and decodes the view with
    // D3DXCreateTextureFromFileInMemory; *OutHandle receives the texture handle.
    virtual GENRESULT NEURONCOMPONENTS_API LoadTexture(IFileSystem* FileSystem, const char* FileName,
                                            void** OutHandle) = 0;
    // slot 5 the routine RET 0x14: Level is the mip level of a 2D handle or the cube face of a
    // cube handle (get_subsurface); SrcSize/SrcData are an encoded image in memory, loaded into
    // that subsurface with D3DXLoadSurfaceFromFileInMemory (06d147f6 pushes param_5 as pSrcData
    // and param_4 as SrcDataSize).
    virtual GENRESULT NEURONCOMPONENTS_API LoadSurfaceData(void* Handle, int Level, unsigned int SrcSize,
                                                const void* SrcData) = 0;
    // slot 6 the routine RET 0x10: D3DXGetImageInfoFromFileInMemory(SrcData, SrcSize, Info).
    // Shading's load_dds_mips (06ebfe74) calls it and then compares Info.ImageFileFormat with 4
    // (D3DXIFF_DDS).
    virtual GENRESULT NEURONCOMPONENTS_API GetImageInfo(_D3DXIMAGE_INFO* Info, unsigned int SrcSize,
                                             const void* SrcData) = 0;
    // slot 7 the routine RET 0x10: load_texture for a cube map (TEXTURE_CUBEMAPS ability,
    // D3DXCreateCubeTextureFromFileInMemory). Shading 06ec0399 calls it as
    // (FileSystem, "CUBE", OutHandle).
    virtual GENRESULT NEURONCOMPONENTS_API LoadCubemap(IFileSystem* FileSystem, const char* FileName,
                                            void** OutHandle) = 0;
};
