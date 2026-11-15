// Author: Giperion.

#pragma once

#include "render/rendcomp.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "readfile.h"
#include "x86math.h"

#include <cstddef>
#include <vector>

static const int kFdumpError = 0x100001;
static const int kFdumpWarning = 0x100002;
static const int kFdumpNotice = 0x100003;
static const char* const G_FdumpFmtError = "%s(%d) : ERROR:General:%s";
static const char* const G_FdumpFmtWarning = "%s(%d) : WARNING:General:%s";
static const char* const G_FdumpFmtNotice = "%s(%d) : NOTICE:General:%s";
// baseline __FILE__ strings, each passed as FDUMP's file argument by the functions of its source file.
// .data 06c37048: the TriMesh plugin dtor the routine (PUSH @ 06c210bb).
static const char* const G_FileTriMesh =
    "RendComp\\TriMesh\\TriMesh.cpp";
// .data 06c371b8: the TriMesh loader the routine (PUSH @ 06c247d1).
static const char* const G_FileTriMeshArchetype =
    "RendComp\\TriMesh\\TriMeshArchetype.cpp";
// .data 06c37a38: every RenderManager FDUMP (06c32185, 06c323b6, 06c32474, 06c324ac, 06c33a64, 06c33aa6).
static const char* const G_FileRenderMgr =
    "RendComp\\RenderMgr.cpp";
// .data 06c37550 / 06c37650 / 06c376c8 / 06c37778:
static const char* const G_FileVMeshData =
    "RendComp\\VMesh\\VMeshData.cpp";
static const char* const G_FileVMeshLibrary =
    "RendComp\\VMesh\\VMeshLibrary.cpp";
static const char* const G_FileVMeshWire =
    "RendComp\\VMesh\\VMeshWire.cpp";
static const char* const G_FileVMPartArch =
    "RendComp\\VMesh\\VMPartArch.cpp";
// rendcomp.dll .data 06c37450 (the sphere archetype load the routine).
static const char* const G_FileSphereMeshArchetype =
    "RendComp\\SphereMesh\\SphereMeshArchetype.cpp";

static const unsigned long G_RegisterPriority = 0x80000000;
static const unsigned long G_HandleFail = 0xFFFFFFFF;
static const float G_DefaultLodDistance = 1000000.0f;

struct FNeuronComponentsFactory;
typedef GENRESULT (NEURONCOMPONENTS_API* FNeuronComponentsFactoryCreateFn)(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);

// The 0xC-byte factory every RendComp DllMain registrar allocates: {vptr, RefCount, Name}.
// baseline gives each factory its own four-slot vtable sharing slots 0-2 (the branch QI,
// AddRef, the routine Release) and differing only in slot 3 CreateInstance.
// NEURONCOMPONENTS (ICOManager::RegisterComponent) holds and calls it as an IComponentFactory
// (src/include/neuroncomponents.h). Until 2026-10-16 this was a plain struct with a `void** Vtable`
// field pointed at hand-filled `void*[4]` arrays and C-cast to INeuronComponent*, i.e. NEURONCOMPONENTS
// reached it through an interface it did not derive from. The per-factory slot 3 is now the
// Create member, which CreateInstance forwards to.
struct FNeuronComponentsFactory final : IComponentFactory
{
    explicit FNeuronComponentsFactory(FNeuronComponentsFactoryCreateFn CreateIn)
        : RefCount(0), Name(nullptr), Create(CreateIn)
    {
    }

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API CreateInstance(AGGDESC* Desc, void** Out) override;

    unsigned long RefCount;
    const char* Name;
    FNeuronComponentsFactoryCreateFn Create;
};

// The 0x10 descriptor RenderManager's plugin loader (the routine @ 06c33a39 / 06c33b28..
// ) hands NEURONCOMPONENTS CreateInstance for every [RenderManager] name. The VMesh factory
// (the branch) requires exactly this size, and the routine requires +8 and +0xC non-null.
struct FRenderPluginDesc
{
    unsigned long Size;         // +0x00 0x10
    const char* Name;           // +0x04 the [RenderManager] line, cut at the first tab/space/';'
    INeuronComponent* Parent;       // +0x08 RenderManager +0x28 (the system container)
    IEngine* Engine;            // +0x0C the field
};

static_assert(sizeof(FRenderPluginDesc) == 0x10, "render plugin AGGDESC 0x10");

struct FPluginNode
{
    FPluginNode* Next;
    FPluginNode* Prev;
    IRenderComponent* Plugin;
};

// One LOD of a render archetype, baseline 8 bytes {handle, plugin}: the entry ctor the routine
// seeds {-1, 0}, the routine / the routine write the two fields, the routine indexes them.
// The plugin pointer is a raw copy of the RenderManager plugin list value (no AddRef).
struct FLodEntry
{
    void* Handle;             // +0x00
    IRenderComponent* Plugin; // +0x04
};

// The value of RenderManager's archetype map (baseline node+0x10, ctor the routine zeroes it).
// baseline keeps two raw new[] arrays; the port keeps vectors with the same sizes: Entries holds
// LodCount entries, Switch LodCount + 1 switch distances (the routine).
struct FLodSet
{
    std::vector<FLodEntry> Entries; // +0x00
    int LodCount;                   // +0x04
    int CurrentLod;                 // +0x08
    unsigned char bFlag;            // +0x0C IRenderer Slot19 / Slot21 bit 0
    std::vector<float> Switch;      // +0x10 IRenderer Slot14 / Slot15, the routine

    FLodSet() : LodCount(0), CurrentLod(0), bFlag(0) {}
};

void LodSetResize(FLodSet& Set, int Count);
void LodSetAssign(FLodSet& Dest, const FLodSet& Src);
FLodEntry* LodSetPick(FLodSet& Set, float Distance);

struct FVmeshGroup;

// VMeshData, 0x38 bytes: operator new(0x38) + ctor the routine in the routine.
struct FVmeshData
{
    unsigned long Crc;          // +0x00 NEURONCOMPONENTS_CRC::GetCRC32(Name)
    char* Name;                 // +0x04 _strdup of the .vms directory name (case kept)
    IMaterial** Materials;      // +0x08 one per facegroup, zeroed (the routine)
    void* VertexBuffer;         // +0x0C the group's VB (the routine)
    unsigned long Fvf;          // +0x10 the group's FVF
    unsigned long BaseVertex;   // +0x14 first vertex of this mesh inside the group VB
    void* IndexBuffer;          // +0x18 the group's IB (the routine)
    unsigned long StartIndex;   // +0x1C first index of this mesh inside the group IB
    FVmeshData* Next;           // +0x20 group list
    FVmeshData* Prev;           // +0x24
    FVmeshGroup* Group;         // +0x28
    int RefCount;               // +0x2C
    float SizeKib;              // +0x30 leaf size * (1/1024), "Memory_VMeshData" LogEvent
    FVmeshDataHeader* Header;   // +0x34 the whole VMeshData leaf, owned
};

// Shared-buffer group, 0x28 bytes: operator new(0x28) + ten zero stores (the branch).
struct FVmeshGroup
{
    unsigned long VertexTotal;  // +0x00 sum of member Header->Vertices
    unsigned long IndexTotal;   // +0x04 sum of member Header->Indices
    unsigned long Fvf;          // +0x08 0 until the first member is added
    unsigned long Field0C;      // +0x0C TODO: zeroed at construction, no reader in 06c2d330..06c2f480
    unsigned long Field10;      // +0x10 TODO: zeroed at construction, no reader in 06c2d330..06c2f480
    void* VertexBuffer;         // +0x14
    void* IndexBuffer;          // +0x18
    FVmeshData* First;          // +0x1C
    FVmeshData* Last;           // +0x20
    int Count;                  // +0x24
};

// The {name, crc} pair the routine takes; only +4 is read.
struct FVmeshKey
{
    const char* Name;
    unsigned long Crc;
};

static_assert(offsetof(FVmeshData, Group) == 0x28, "VMeshData+0x28 group");
static_assert(offsetof(FVmeshData, Header) == 0x34, "VMeshData+0x34 header");
static_assert(sizeof(FVmeshData) == 0x38, "VMeshData 0x38");
static_assert(sizeof(FVmeshGroup) == 0x28, "VMesh group 0x28");

// FDUMP(Flags, "%s(%d) : <severity>:General:%s", File, Line, Message) exactly as baseline pushes it.
void RendCompFdumpAt(int Flags, const char* File, int Line, const char* Message);
bool RendCompIidEqual(const char* Left, const char* Right);

int FileOpenNode(void* File, const char* Name);
int FileClose(void* File, int Handle);
int FileRead(void* File, int Handle, void* Buffer, unsigned long Size, unsigned long* ReadOut);
unsigned long FileGetSize(void* File, int Handle);
int FileGetCurrentPath(void* File, char* Out, unsigned long MaxChars);
int FileOpenByName(void* File, const char* ChildName);
// the 0x34 FileSystem descriptor (Path, "FileSystem", GENERIC_READ, share 1,
// OPEN_EXISTING, 0x8000080, +0x30 = -1).
FFileSystemDesc* FileDescInit(FFileSystemDesc* Desc, const char* Path, const char* ComponentName);

void* QueryIid(INeuronComponent* Object, const char* Iid);
void ReleaseIf(INeuronComponent* Object);

// NEURONCOMPONENTS_Acquire, then ICOManager +0xC CreateInstance({0xC, "3DMathEngine", 0},
// &Out), and Out is returned. No cache and no checks. Every baseline caller caches the result in
// its own global and calls this only while that global is null -- MathEngineCached(Slot).
I3DMathEngine* AcquireMathEngine();

// The four per-caller caches (.data, zero-initialised).
extern I3DMathEngine* G_MathTriMeshDraw; // TriMesh draw the routine (4 sites)
extern I3DMathEngine* G_MathSphere;      // sphere draw 06c2b4ff..06c2c145 (9 sites)
extern I3DMathEngine* G_MathVMesh;       // VMesh draw the branch (4 sites)
extern I3DMathEngine* G_MathRenderMgr;   // RenderManager 06c32ba0..06c33111 (6 sites)

// The inline pattern at every call site: `if (Cache == 0) Cache = the routine(); use Cache`.
inline I3DMathEngine* MathEngineCached(I3DMathEngine*& Cache)
{
    if (Cache == nullptr)
        Cache = AcquireMathEngine();
    return Cache;
}

// Materials use the shared vertex/index submission records.

void PipelineApplySubmit(ICamera* Camera, void* Inst, const void* Transform,
                         const FTransform** OutCameraView, const FTransform** OutWorld); // part
void VectorNegate3(const float* Src, float* Dest);
float* TransposeXform3x3(const float* Src12, float* Dest9);
float* TransformPoint3(float* Dest3, const float* Matrix12, const float* Vec3);

// VMeshData layer (src/RendComp/VMeshLibrary.cpp).
FVmeshData* VMeshDataLoad(IFileSystem* File, const char* Name);
FVmeshData* VMeshDataFind(const FVmeshKey* Key);
int VMeshDataAddRef(FVmeshData* Data);
int VMeshDataRelease(FVmeshData* Data);
int VMeshGroupEnsureVb(FVmeshGroup* Group);
int VMeshGroupEnsureIb(FVmeshGroup* Group, unsigned long BaseVertex);
void VMeshGroupDestroyVb(FVmeshGroup* Group);
void VMeshGroupDestroyIb(FVmeshGroup* Group);

// triples {channelId, fvfTestMask, elemByteSize}, filled lazily by the first
// channel query (flag the field).
struct FVmeshChannelRecord
{
    void* Pointer;              // +0x00 first element of the channel
    int ElemBytes;              // +0x04 table entry +8
    int Stride;                 // +0x08 FVF vertex stride
    int Unwritten0C;            // +0x0C never written by the routine / the routine
    int VertexCount;            // +0x10
};

bool VMeshChannelPresent(unsigned long Fvf, unsigned int ChannelId);
int VMeshChannelByteOffset(unsigned long Fvf, unsigned int ChannelId);
unsigned int VMeshFvfStride(unsigned long Fvf);
// (VertexStart 0, VertexCount Header->Vertices) and the routine (the VMeshRef's
// range): false and Out untouched when the channel is absent.
bool VMeshFillChannelRecord(const FVmeshDataHeader* Header, unsigned long VertexStart,
                            unsigned long VertexCount, unsigned int ChannelId,
                            FVmeshChannelRecord* Out);
// / the routine: the 0x70 vertex-source descriptor.
void VMeshBuildVertexDesc(const FVmeshDataHeader* Header, unsigned long VertexStart,
                          unsigned long VertexCount, FRpVertexSrcDesc* Out);

// The object-building halves of the TriMesh (the branch) and SphereMesh (the branch)
// factories, after PluginFactoryCreate's AGGDESC checks (Plugins.cpp).
GENRESULT CreateTriMeshPlugin(const FRenderPluginDesc* Desc, void** Out);
GENRESULT CreateSphereMeshPlugin(const FRenderPluginDesc* Desc, void** Out);

GENRESULT NEURONCOMPONENTS_API RenderManagerFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API VMeshLibraryFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API VMeshWireFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API VMeshPluginFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);
GENRESULT NEURONCOMPONENTS_API PluginFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out);

extern IRenderPipeline* G_Pipeline;
extern IRPDraw* G_Draw;
extern IRPIndexBuffer* G_IndexBuffer;
extern IRPVertexBuffer* G_VertexBuffer;
extern IVertexBufferManager* G_VbManager;
extern IMaterialLibrary* G_MaterialLibrary;
extern IEngine* G_Engine;
