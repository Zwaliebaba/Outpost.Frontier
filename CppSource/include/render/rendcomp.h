// Author: Giperion.

#pragma once

// RendComp.dll — documented IIDs and on-disk records (implementation notes).
// Method names describe observed behavior; slot order is unchanged.

#include "neuroncomponents.h"
#include "render/IMaterial.h"
#include "luaprofile.h"
#include "render/mesh_iface.h"
#include "engine/sceneruntime.h"


static const char* const G_NameRenderManager = "RenderManager";
static const char* const G_NameVMesh = "VMesh";
static const char* const G_NameTriMesh = "TriMesh";
static const char* const G_NameSphereMesh = "SphereMesh";
static const char* const G_NameVMeshLibrary = "VMeshLibrary";
static const char* const G_NameVMeshWire = "VMeshWire";

// IEngineComponent now has one home: src/include/engine/sceneruntime.h.

// ICamera is declared in common_misc.h (slot 16 +0x40 GetViewTransform is what RendComp reads).
struct ICamera;

struct IRenderer2 : INeuronComponent
{

    virtual int NEURONCOMPONENTS_API QueryGeometry(void* A, float Lod, void* C, void* D, void* E, void* F,
                                void* G) = 0; // formerly Slot3
    virtual int NEURONCOMPONENTS_API GetBounds(void* Id, float Lod, void* Out) = 0; // formerly Slot4
    virtual int NEURONCOMPONENTS_API GetCentroid(void* Id, float Lod, void* Out) = 0; // formerly Slot5
    virtual int NEURONCOMPONENTS_API NoOp(void* A, void* B, void* C, void* D, void* E, void* F,
                                void* G) = 0; // formerly Slot6
    virtual int NEURONCOMPONENTS_API RenderInstance(void* Camera, void* Inst, float Threshold, unsigned long Flags,
                                void* Matrix, void* Extra) = 0; // formerly Slot7
    virtual int NEURONCOMPONENTS_API GetInstanceLodCount(void* Inst, void* Out) = 0; // formerly Slot8
    virtual int NEURONCOMPONENTS_API GetInstanceCurrentLod(void* Inst, void* Out) = 0; // formerly Slot9
    virtual int NEURONCOMPONENTS_API SetInstanceLod(void* Inst, int Index) = 0; // formerly Slot10
    virtual int NEURONCOMPONENTS_API GetArchetypeLodCount(void* Id, int* Out) = 0; // formerly Slot11
    virtual int NEURONCOMPONENTS_API GetArchetypeCurrentLod(void* Id, int* Out) = 0; // formerly Slot12
    virtual int NEURONCOMPONENTS_API SetArchetypeLod(void* Id, int Index) = 0; // formerly Slot13
    virtual int NEURONCOMPONENTS_API GetArchetypeLodRange(void* Id, int Index, void* OutA, void* OutB) = 0; // formerly Slot14
    virtual int NEURONCOMPONENTS_API SetArchetypeLodRange(void* Id, int Index, float A, float B) = 0; // formerly Slot15

    virtual int NEURONCOMPONENTS_API GetInstanceLodRange(void* Inst, int Index, void* OutA, void* OutB) = 0; // formerly Slot16
    virtual int NEURONCOMPONENTS_API SetInstanceLodRange(void* Inst, int Index, float A, float B) = 0; // formerly Slot17
    virtual int NEURONCOMPONENTS_API QueryGeometryAtLod(void* A, void* B, void* C, void* D, void* E, void* F,
                                 void* G) = 0; // formerly Slot18
    virtual int NEURONCOMPONENTS_API SetArchetypeRootLodSelection(void* Id, void* Unused, unsigned long Flags) = 0; // formerly Slot19
    virtual int NEURONCOMPONENTS_API SetInstanceRootLodSelection(void* Id, void* A, void* B) = 0; // formerly Slot20
    virtual int NEURONCOMPONENTS_API GetArchetypeRootLodSelection(void* Key, void* Unused, unsigned long* Out) = 0; // formerly Slot21
    virtual int NEURONCOMPONENTS_API GetInstanceRootLodSelection(void* Id, void* A, void* B) = 0; // formerly Slot22
};

struct IRenderComponent : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API Claim(void* File) = 0;
    virtual int NEURONCOMPONENTS_API CloneHandle(void* Handle) = 0; // formerly Slot4
    virtual int NEURONCOMPONENTS_API DestroyHandle(void* Handle) = 0;
    virtual int NEURONCOMPONENTS_API QueryHandle(void* Handle, const char* Iid, void** Out) = 0; // formerly Slot6
    virtual int NEURONCOMPONENTS_API CreateMeshInstance(void* InstId, void* Handle) = 0; // formerly Slot7
    virtual int NEURONCOMPONENTS_API DestroyMeshInstance(void* InstId, void* Handle) = 0; // formerly Slot8
    virtual int NEURONCOMPONENTS_API UpdateMeshInstance(void* InstId, void* Arg) = 0; // formerly Slot9
    virtual int NEURONCOMPONENTS_API QueryMeshInstance(void* InstId, void* Handle, const char* Iid, void** Out) = 0; // formerly Slot10
    virtual int NEURONCOMPONENTS_API UpdateMeshInstances(void* InstId) = 0; // formerly Slot11
    virtual int NEURONCOMPONENTS_API QueryGeometry(void* Handle, int Mode, void* A, void* B, void* C, void* D,
                                 void* E) = 0; // formerly Slot12
    virtual int NEURONCOMPONENTS_API GetBounds(void* Handle, float Lod, void* Out) = 0; // formerly Slot13
    virtual int NEURONCOMPONENTS_API GetCentroid(void* Handle, float Lod, void* Out) = 0; // formerly Slot14
    virtual int NEURONCOMPONENTS_API SplitByPlane(void* Handle, void* A, void* B, void* C, void* D, void* E,
                                 void* F) = 0; // formerly Slot15
    virtual int NEURONCOMPONENTS_API RenderInstance(ICamera* Camera, void* Handle, void* Inst, unsigned long Flags,
                                 void* Matrix, void* Extra) = 0; // formerly Slot16
    virtual int NEURONCOMPONENTS_API BindMeshInstance(void* Inst, void* Handle) = 0; // formerly Slot17
};

// IMesh — see render/mesh_iface.h (shared with surfacematerials.h).

struct IMatHookUp : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API HookUpMaterials() = 0; // formerly Slot3
};

struct IRenderableMesh : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API GetVertexCount(int* Out) = 0; // formerly Slot3
    virtual int NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, unsigned int StartVertex, unsigned int VertexCount) = 0; // formerly Slot4
    virtual int NEURONCOMPONENTS_API GetIndexCount(int* Out) = 0; // formerly Slot5
    virtual int NEURONCOMPONENTS_API CopyIndices(unsigned short** Dest, int Count) = 0; // formerly Slot6
};

struct FVmeshRef;
struct FVmeshDataHeader;

// VMesh archetype facet queried as Iids::VMeshLibrary.
// The library component exposes a different facet for the same IID.
struct IVMeshPart : INeuronComponent
{
    // +0x0C the branch (RET 0x14): draw every facegroup of the part.
    virtual bool NEURONCOMPONENTS_API Draw(ICamera* Camera, void* Inst, unsigned long Flags,
                                const void* Matrix, IMaterial* Override) = 0;
    // +0x10 the routine (RET 8): make the group VB/IB current; fills a 12-byte VB listing
    // {VB, FVF, base vertex} and an 8-byte IB scratch {IB, start index}.
    virtual bool NEURONCOMPONENTS_API Prepare(void* Listing, void* Scratch) = 0;
    virtual FVmeshRef* NEURONCOMPONENTS_API GetRef() = 0;               // +0x14 the routine
    virtual FVmeshDataHeader* NEURONCOMPONENTS_API GetHeader() = 0;     // +0x18 the routine (0 w/o data)
};

struct ISphereMesh : INeuronComponent
{
    virtual int NEURONCOMPONENTS_API SetLodDistanceScale(float Value) = 0; // formerly Slot3
    virtual int NEURONCOMPONENTS_API GetLodDistanceScale(float* Out) = 0; // formerly Slot4
    virtual int NEURONCOMPONENTS_API NoOp05(void* Arg) = 0; // formerly Slot05
    virtual int NEURONCOMPONENTS_API NoOp06(void* Arg) = 0; // formerly Slot06
    virtual int NEURONCOMPONENTS_API SetMaxTessellationLevel(int Value) = 0; // formerly Slot7
    virtual int NEURONCOMPONENTS_API GetMaxTessellationLevel(int* Out) = 0; // formerly Slot8
    virtual int NEURONCOMPONENTS_API SetRadius(float Value) = 0; // formerly Slot9
    virtual int NEURONCOMPONENTS_API GetRadius(float* Out) = 0; // formerly Slot10
    virtual int NEURONCOMPONENTS_API RequestRebuild(void* Arg) = 0; // formerly Slot11
    virtual int NEURONCOMPONENTS_API GetMaterialCount(int* OutCount) = 0; // formerly Slot12
    virtual int NEURONCOMPONENTS_API SetMaterial(int Index, IMaterial* Material) = 0; // formerly Slot13
    virtual int NEURONCOMPONENTS_API GetMaterial(int Index, IMaterial** Out) = 0; // formerly Slot14
};

struct ITriMeshAnim : INeuronComponent
{
    virtual void AdvanceUvAnimation(float Dt) = 0; // formerly Slot3
};

// IVMeshLibrary lives at object+4 (NeuronRuntime / MaterialLib::Load). 5 slots.
struct IVMeshLibrary : INeuronComponent
{
    virtual void* NEURONCOMPONENTS_API Load(void* File, int LodFilter) = 0;    // +0x0C
    virtual void NEURONCOMPONENTS_API Unload(void* Handle) = 0;                // +0x10
};

// IVMeshWire — see render/mesh_iface.h (shared with surfacematerials.h).

// VMeshData leaf header. Facegroups (0xC each) follow at +0x10, then Indices ushorts, then the
// vertices (stride from Fvf). +0x01..+0x03 are unread (do not invent).
#pragma pack(push, 1)
struct FVmeshDataHeader
{
    unsigned char Flags;        // +0x00 bit0 set = facegroup-relative indices (the routine);
                                //        clear = "mesh in old format" (the routine refuses it)
    unsigned char Unread01[3];  // +0x01..+0x03 unread
    unsigned long PrimitiveType; // +0x04 IMaterial::Submit PrimType (the branch), IMesh Slot13
    unsigned short FaceGroups;  // +0x08
    unsigned short Indices;     // +0x0A
    unsigned short Fvf;         // +0x0C
    unsigned short Vertices;    // +0x0E
};

// VMeshRef record 0x3C. +0x00 unread. +0x04 is the VMeshData CRC (the branch).
struct FVmeshRef
{
    unsigned long Unread00;     // +0x00 unread
    unsigned long Crc;          // +0x04 VMeshData key (the branch)
    unsigned short VertexStart; // +0x08 the routine / the routine
    unsigned short VertexCount; // +0x0A
    unsigned short IndexStart;  // +0x0C the routine / the branch
    unsigned short IndexCount;  // +0x0E IRenderableMesh Slot5/Slot6 (the branch / the branch)
    unsigned short FacegroupStart; // +0x10
    unsigned short FacegroupCount; // +0x12
    unsigned long Box14[6];     // +0x14 copied out whole by the routine (IRC Slot13)
    float Extra2C[3];           // +0x2C center (the routine / the routine)
    float Extra38;              // +0x38 radius (the routine / SetInstanceExtent)
};

// Facegroup 0xC (header +0x10 + 12 * index).
struct FVmeshFacegroup
{
    unsigned long MaterialCrc;  // +0x00 IMaterialLibrary Slot16 key (the branch)
    unsigned short VertexBegin; // +0x04 added onto each index (the routine), Submit Start
    unsigned short VertexEnd;   // +0x06 Submit Count = End - Begin + 1 (the branch)
    unsigned short IndexCount;  // +0x08
    unsigned short Unread0A;    // +0x0A
};

// VWireData header 0x10 before the index array. +0x00/+0x02 unread.
struct FVwireDataHeader
{
    unsigned short Unread00;    // +0x00
    unsigned short Unread02;    // +0x02
    unsigned long Crc;          // +0x04 VMeshData key (the routine / the routine)
    unsigned short IbBias;      // +0x08 added to VMeshData+0x14 for select_ib (the routine)
    unsigned short RemapCount;  // +0x0A unread in RendComp (the exe part cache reads it)
    unsigned short Count;       // +0x0C index count: CopyIndices and the draw count
    unsigned short NumVertices; // +0x0E IRPDraw NumVertices
};
#pragma pack(pop)

static_assert(sizeof(FVmeshDataHeader) == 0x10, "VMeshData header 0x10");

static_assert(sizeof(FVmeshRef) == 0x3C, "VMeshRef 0x3C");
static_assert(sizeof(FVmeshFacegroup) == 0xC, "facegroup 0xC");
static_assert(sizeof(FVwireDataHeader) == 0x10, "VWireData header 0x10");
