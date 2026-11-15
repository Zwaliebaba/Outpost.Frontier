// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "readfile.h"
#include "x86math.h"
#include "luaprofile.h"
#include "engine/sceneruntime.h"
#include "render/IRenderPipeline.h"
#include "render/IMaterial.h"
#include "render/rendcomp.h"
#include "render/shading.h"
#include <cstddef>

// Unversioned aliases from deformable2 .data -- baseline accepts both the
// "1.11_"-stamped ids (Iids::Deformable, Iids::EngineComponent) and these bare names.
static const char* const G_IidDeformableShort = "IDeformable";
static const char* const G_IidEngineComponentShort = "IEngineComponent";
static const char* const G_DeformableComponentName = "Deformable";
static const char* const G_DeformableSection = "Deformable";
static const char* const G_DeformRateKey = "DeformRate";
static const char* const G_EyeMaterialName = "EyeMaterial";
static const char* const G_EyeMaterialPattern = "^eye*";
static const char* const G_FileSystemName = "FileSystem";
static const char* const G_Name3DMathEngine = "3DMathEngine";

// ReadFile UTF header (0x38). Deformable2 walks IFileSystem nodes, not this dword.
static const unsigned long G_UtfMagic = 0x20465455u; // "UTF "
static const unsigned long G_UtfVersion = 0x101u;
static const int G_MaxBoneCount = 150;

struct FUtfHeader
{
    unsigned long Magic;       // +0x00 "UTF "
    unsigned long Version;     // +0x04 0x101
    unsigned long TreeOffset;  // +0x08
    unsigned long Pad0C;
    unsigned long Pad10;
    unsigned long NodeSize;    // +0x14 0x0C or 0x2C
    unsigned long StringOff;   // +0x18
    unsigned long Pad1C;
    unsigned long Pad20;
    unsigned long DataOffset;  // +0x24
    unsigned long Pad28;
    unsigned long Pad2C;
    unsigned long Pad30;
    unsigned long Pad34;
};

static_assert(sizeof(FUtfHeader) == 0x38, "UTF header must be 0x38");

// / the routine FileSystem AGGDESC 0x34
// FileSystem desc and IFileSystem itself come from ReadFile.dll's published ABI
// (src/include/readfile.h) -- this module used to keep its own copy.

// face group 0x24. +0x10/+0x14 min/max after the routine.
struct FUvDeformBinding;

struct FFaceGroup
{
    unsigned long Kind;       // +0 = 4
    unsigned long MaterialCrc;
    IMaterial* Material;
    FUvDeformBinding* UvBinding; // formerly Pad0C
    unsigned long IndexMin;
    unsigned long IndexMax;
    unsigned short* Indices;
    unsigned long IndexCount;
    unsigned long UvBoneIndex; // +0x20 the routine: -1 none; 0 = EyeMaterial
};

static_assert(sizeof(FFaceGroup) == 0x24, "face group must be 0x24");

// zip of 9 Geometry streams. Draw lookup by BoneId, not vertex UV offset.
struct FUvDeformRecord
{
    int BoneId;
    int VertexCount;
    float PlaneDistance;
    float BoneXToU;
    float BoneYToV;
    float MinDu;
    float MaxDu;
    float MinDv;
    float MaxDv;
};

static_assert(sizeof(FUvDeformRecord) == 0x24, "UV-deform record must be 0x24");

struct FBoneRuntime;

// The triple the branch keeps on its stack (ESP+0x1C) and hangs off a UV-deformed face
// group's +0x0C, which the routine hands the material as context +0x20. The draw zeroes all
// three, fills the first two per group, and copies the third into mesh +0x48 afterwards.
struct FUvDeformBinding
{
    FUvDeformRecord* Record;   // UvDeform[group +0x20]
    FBoneRuntime* Bone;        // BoneSlots[Record->BoneId]
    // EyeMaterial Bind (the branch, 066001a3 INC [ESI+8]) counts every eye texture
    // transform it sets.
    unsigned long EyeUvTransformCount;
};

// Bone_id_chain + Bone_weight_chain pair. the branch applies as skin.
struct FSkinPair
{
    int BoneId;
    float Weight;
};

static_assert(sizeof(FSkinPair) == 8, "skin pair must be 8");

struct FSkinVertex
{
    int BoneCount;
    FSkinPair* Pairs;
};

static_assert(sizeof(FSkinVertex) == 8, "skin vertex must be 8");

// / the routine: per-bone std::vector<ushort> of vertex indices (invert, not skin apply).
struct FBoneVertexList
{
    unsigned char Pad00;
    unsigned char Pad01[3];
    unsigned short* Start;
    unsigned short* Finish;
    unsigned short* CapacityEnd;
};

static_assert(sizeof(FBoneVertexList) == 0x10, "bone vertex list must be 0x10");

// per-vertex vector<uint> of Point_indices slots (mesh+0x64).
struct FVertexRemapList
{
    unsigned char Pad00[4];
    unsigned int* Start;
    unsigned int* Finish;
    unsigned int* CapacityEnd;
};

static_assert(sizeof(FVertexRemapList) == 0x10, "vertex remap list must be 0x10");

struct FDeformPart;
struct FLodHolder;

// Shared polymorphic interface for DFM meshes and VMesh wrappers.
struct FDeformVbDesc;
struct FDeformIbDesc;
struct ICamera;

struct FDeformMesh
{
    int RefCount;           // +0x04
    unsigned long NameCrc;  // +0x08

    explicit FDeformMesh(const char* Name);
    virtual int GetIndexCount();                                              // 0 +0x00
    virtual int GetVertexCapacity();                                          // 1 +0x04
    // 2 +0x08 RET 4 -- the part's index-buffer descriptor (&part+0x24), the routine 065f6a7b.
    virtual unsigned char FillIndexBuffer(FDeformIbDesc* Ib);
    // 3 +0x0C RET 0xC -- (part+0 holder, &part+0x14, a flag byte), the routine 065f6a20..23.
    // The flag goes over as a whole dword (EBX) but the branch reads only its low byte.
    virtual unsigned char Skin(FLodHolder* Holder, FDeformVbDesc* Vb, unsigned char bForce);
    virtual unsigned char GetBoundSphere(float* OutRadius, float* OutCenter); // 4 +0x10
    virtual unsigned char Draw(ICamera* Camera, FDeformPart* Part) = 0;       // 5 +0x14
    virtual ~FDeformMesh();                                                   // 6 +0x18
};

// on calloc(1, 0x84). Vtable the branch.
struct FMeshArchetype : FDeformMesh
{
    explicit FMeshArchetype(const char* Name);
    ~FMeshArchetype() override;
    int GetIndexCount() override;
    int GetVertexCapacity() override;
    unsigned char FillIndexBuffer(FDeformIbDesc* Ib) override;
    unsigned char Skin(FLodHolder* Holder, FDeformVbDesc* Vb, unsigned char bForce) override;
    unsigned char GetBoundSphere(float* OutRadius, float* OutCenter) override;
    unsigned char Draw(ICamera* Camera, FDeformPart* Part) override;

    unsigned long ParseTag;     // +0x0C FVF (0x112 after UTF)
    int PointIndexCount;        // +0x10
    unsigned long* PointIndices;         // +0x14
    int FaceGroupCount;         // +0x18
    FFaceGroup* FaceGroups;     // +0x1C counted body
    // +0x20: read only by slot 1 (the branch, added to +0x10). No MOV/LEA/arith write to
    // [reg+0x20] of the mesh anywhere in deformable2.dll (the 065f7e80 loader does not store
    // the pair count), so it stays 0 from the calloc of the routine.
    unsigned long Field_0x20;   // +0x20 TODO: meaning (never written)
    int VertexCount;            // +0x24 Points bytes / 0xC
    void* Vertices;             // +0x28 count * 0x18
    FSkinPair* SkinPairs;       // +0x2C flattened id/weight
    FSkinVertex* SkinTable;     // +0x30 vertexCount * 8
    FBoneVertexList* BoneVerts; // +0x34 counted 0x10, indexed by bone id
    unsigned char* FaceFlags;            // +0x38 byte[VertexCount]
    int FaceFlagCount;          // +0x3C = VertexCount
    // Per-draw skinning and UV transform counters.
    unsigned int SkinnedVertexCount; // +0x40
    unsigned int SkinnedBoneCount;   // +0x44
    unsigned long EyeUvTransformCount; // +0x48 formerly Field_0x48
    int UvCount;                // +0x4C  UV0 bytes >> 3
    void* UvData;               // +0x50  count * 8
    unsigned long* UvIndices;            // +0x54  UV0_indices
    int UvDeformCount;          // +0x58 the routine rows
    FUvDeformRecord* UvDeform;  // +0x5C count * 0x24
    unsigned char NameHint;     // +0x60 vector pad
    unsigned char Pad61[3];
    FVertexRemapList* RemapStart;    // +0x64 the routine
    FVertexRemapList* RemapFinish;   // +0x68
    FVertexRemapList* RemapCapacity; // +0x6C
    unsigned char Flag70;       // +0x70
    unsigned char SphereReady;  // +0x71 the branch cache
    unsigned char Pad72[2];
    float SphereCenter[3];      // +0x74
    float SphereRadius;         // +0x80
};

static_assert(sizeof(FMeshArchetype) == 0x84, "mesh archetype must be 0x84");
static_assert(offsetof(FMeshArchetype, Field_0x20) == 0x20, "never-written field +20");
static_assert(offsetof(FMeshArchetype, SkinPairs) == 0x2C, "skin pairs +2C");
static_assert(offsetof(FMeshArchetype, SkinTable) == 0x30, "skin table +30");
static_assert(offsetof(FMeshArchetype, BoneVerts) == 0x34, "bone vertex lists +34");
static_assert(offsetof(FMeshArchetype, FaceFlags) == 0x38, "face flags +38");
static_assert(offsetof(FMeshArchetype, FaceFlagCount) == 0x3C, "face flag count +3C");
static_assert(offsetof(FMeshArchetype, UvCount) == 0x4C, "UV0 count +4C");
static_assert(offsetof(FMeshArchetype, UvIndices) == 0x54, "UV0_indices +54");
static_assert(offsetof(FMeshArchetype, UvDeformCount) == 0x58, "UV-deform count +58");
static_assert(offsetof(FMeshArchetype, RemapStart) == 0x64, "vertex remap +64");
static_assert(offsetof(FMeshArchetype, SphereReady) == 0x71, "sphere cache flag +71");
static_assert(offsetof(FMeshArchetype, SphereCenter) == 0x74, "sphere center +74");
static_assert(offsetof(FMeshArchetype, SphereRadius) == 0x80, "sphere radius +80");


// IVMeshLibrary wrapper 0x14. Vtable the branch.
struct FVmeshWrapper : FDeformMesh
{
    FVmeshWrapper(const char* Name, void* Handle, IVMeshPart* Part);
    ~FVmeshWrapper() override;
    unsigned char Draw(ICamera* Camera, FDeformPart* Part) override;

    void* EngineHandle;          // +0x0C IEngine +0x30 archetype handle
    // +0x10 AddRef'd in the ctor: what IEngine +0x48 answers for "1.11_IVMeshLibrary" on the
    // archetype -- RendComp's VMesh part answers that IID with its IVMeshPart face
    // (FVMeshPart::QueryInterface, the branch), and the branch calls its +0x0C Draw.
    IVMeshPart* VMeshPart;
};

static_assert(sizeof(FVmeshWrapper) == 0x14, "VMesh wrapper must be 0x14");

// The EyeMaterial classes (the branch operator new(0x78)) live in EyeMaterial.h/.cpp.

// Shared between Deformable2.cpp and EyeMaterial.cpp (module-internal, not exported).
extern IEngine* G_Engine;
char* FormatDeformMessage(char* Buffer, const char* Format, ...);
void MathTransformVec3(float* Dest, const float* A, const float* B);
void FillFileDesc(FFileSystemDesc* Desc, const char* Path);


// Aggregate descriptor used to create the Deformable component.
struct FAggDesc18
{
    unsigned long Size;
    const char* NameAt4;
    void* Outer;
    void** InnerOut;
    unsigned long Pad10;
    INeuronComponent* SystemAt14;
};

// Factory the routine: operator_new(0xC). CreateInstance stays on derived types.
struct FDeformableFactory : IComponentFactory
{
    unsigned long RefCount;
    const char* Name;

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
};

// Shared implementation and its concrete NEURONCOMPONENTS aggregate wrapper.
struct FDeformable : IDeformable, IEngineComponent
{
    FDeformable();
    ~FDeformable();

    GENRESULT NEURONCOMPONENTS_API Create(void** OutInstance, int* Parts, void* Arg3, void* Arg4,
                                void* Arg5) override;
    GENRESULT NEURONCOMPONENTS_API CreateUnused(void* A, void* B, void* C, void* D, void* E, void* F,
                                       void* G) override;
    GENRESULT NEURONCOMPONENTS_API CreateAlias(void** OutInstance, int* Parts, void* Arg3, void* Arg4,
                                      void* Arg5) override;
    GENRESULT NEURONCOMPONENTS_API Destroy(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API Play(void* Instance, int* Out, void* Name, void* Extra, void* ArgC,
                              void* ArgD, void* ArgE, void* ArgF, void* ArgG, void* ArgH,
                              void* Script) override;
    GENRESULT NEURONCOMPONENTS_API PlayAlt(void* Instance, int* Out, void* Name, void* Extra, void* ArgC,
                                  void* ArgD, void* ArgE, void* ArgF, void* ArgG, void* ArgH,
                                  void* Script) override;
    GENRESULT NEURONCOMPONENTS_API DestroyAll(void* Instance) override;
    GENRESULT NEURONCOMPONENTS_API DestroyHandle(void* Instance, int Handle) override;
    GENRESULT NEURONCOMPONENTS_API StartAim(void* Instance, void** Out, unsigned char* AimDesc,
                                  void* Arg3) override;
    GENRESULT NEURONCOMPONENTS_API StopAim(void* Instance, void* Node) override;
    GENRESULT NEURONCOMPONENTS_API Unused13(void* A) override;
    GENRESULT NEURONCOMPONENTS_API Unused14(void* A) override;
    GENRESULT NEURONCOMPONENTS_API SetLod(void* Instance, float Dt, int* LodIds) override;
    GENRESULT NEURONCOMPONENTS_API Draw(void* Instance, void* Camera, void* UnusedB, void* RenderArg,
                              int* OutLod, unsigned int Flags) override;
    GENRESULT NEURONCOMPONENTS_API Unused17(void* A, void* B, void* C, void* D) override;
    GENRESULT NEURONCOMPONENTS_API GetFirstBone(void* Instance, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API SetPosePos(void* Instance, float* Pos3) override;
    GENRESULT NEURONCOMPONENTS_API SetPoseOri(void* Instance, void* Ori) override;
    GENRESULT NEURONCOMPONENTS_API Unused21(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused22(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused23(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API SetHeight(void* Instance, float Height) override;
    GENRESULT NEURONCOMPONENTS_API Unused25(void* A, void* B, void* C, void* D) override;
    GENRESULT NEURONCOMPONENTS_API GetListCount(void* Instance, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API CopyList(void* Instance, int* Out) override;
    GENRESULT NEURONCOMPONENTS_API Unused28(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused29(void* A, void* B, void* C, void* D, void* E, void* F, void* G,
                                  void* H, void* I) override;
    GENRESULT NEURONCOMPONENTS_API Unused30(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused31(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused32(void* A, void* B) override;
    GENRESULT NEURONCOMPONENTS_API Unused33(void* A, void* B, void* C) override;
    GENRESULT NEURONCOMPONENTS_API Unused34(void* A, void* B, void* C, void* D) override;
    GENRESULT NEURONCOMPONENTS_API GetOffset(void* Instance, float* Out) override;
    GENRESULT NEURONCOMPONENTS_API WalkHardpoints(void* Instance, void* UserFn, void* UserData) override;
    GENRESULT NEURONCOMPONENTS_API GetMeshStats(void* Instance, unsigned int* Out) override;
    GENRESULT NEURONCOMPONENTS_API GetBoundSphere(void* Instance, float* OutRadius, float* OutCenter) override;
    int NEURONCOMPONENTS_API Initialize() override;
    int NEURONCOMPONENTS_API CreateArchetype(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DuplicateArchetype(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DestroyArchetype(void* Arg1) override;
    int NEURONCOMPONENTS_API QueryArchetype(void* Arg1, const char* Iid, void** Out) override;
    int NEURONCOMPONENTS_API CreateInstance(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API DestroyInstance(void* Arg1) override;
    int NEURONCOMPONENTS_API UpdateInstance(void* Arg1, void* Arg2) override;
    int NEURONCOMPONENTS_API QueryInstance(void* Arg1, const char* Iid, void** Out) override;
    int NEURONCOMPONENTS_API Notify(void* Arg1) override;
};

// Nondelegating unknown owned by the concrete aggregate.
struct FDeformableObject;

struct FDeformableInner : INeuronComponent
{
    unsigned long RefCount;    // +0x04 (object +0x10)
    FDeformableObject* Owner;  // +0x08 (object +0x14)

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
};

// Component the routine: operator_new(0x18). IDeformable @ +0, IEngineComponent @ +4.
struct FDeformableObject : FDeformable
{
    INeuronComponent* Controlling; // +0x08 descriptor outer or the inner unknown
    FDeformableInner Inner;  // +0x0C

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
};

// ctor: 150 * 0x34 + header. the routine identity at bone+4.
struct FBoneSlot
{
    int Parent;
    float BoneToRoot[12];
};

static_assert(sizeof(FBoneSlot) == 0x34, "bone slot must be 0x34");

struct FSkeleton
{
    float Scale;
    int BoneCount;
    FBoneSlot Bones[150];
    void* EngineHandle;
    int RefCount;
};

static_assert(sizeof(FSkeleton) == 0x1E88, "skeleton must be 0x1e88");
static_assert(offsetof(FSkeleton, EngineHandle) == 0x1E80, "skel handle +1e80");

// Engine transform callback: local pose, dirty flags and computed world pose.
struct FBoneRuntime : IInstanceTransform
{
    void* Instance;           // +0x04 engine instance (slot 0), -1 until bound
    FBoneSlot* Source;        // +0x08 &Skeleton->Bones[i]
    float LocalTransform[12]; // formerly Pad0C: rotation followed by position
    unsigned char Flags;
    unsigned char FlagPadding[3];
    float Matrix[12];         // +0x40

    // The per-slot body of the the routine construct loop (065f15a0..15c8).
    FBoneRuntime();

    GENRESULT NEURONCOMPONENTS_API Init(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API AddRef(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API Release(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API SetPosition(void* InInstance, const float* Position) override;
    float* NEURONCOMPONENTS_API GetPosition(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API SetOrientation(void* InInstance, const float* Rows) override;
    float* NEURONCOMPONENTS_API GetOrientation(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API SetMatrix(void* InInstance, const float* InMatrix) override;
    float* NEURONCOMPONENTS_API GetMatrix(void* InInstance) override;
    GENRESULT NEURONCOMPONENTS_API GetExtra(void* InInstance, float* OutExtra, float* OutVec) override;
    GENRESULT NEURONCOMPONENTS_API SetExtra(void* InInstance, float Extra, const float* Vec) override;
    GENRESULT NEURONCOMPONENTS_API SetUser(void* InInstance, unsigned long User) override;
    unsigned long NEURONCOMPONENTS_API GetUser(void* InInstance) override;
    unsigned long NEURONCOMPONENTS_API MarkWorldDirty(void* InInstance) override; // formerly Slot13
};

static_assert(sizeof(FBoneRuntime) == 0x70, "bone runtime must be 0x70");
static_assert(offsetof(FBoneRuntime, LocalTransform) == 0x0C, "bone local transform +0c");
static_assert(offsetof(FBoneRuntime, Flags) == 0x3C, "bone flag byte +3c");
static_assert(offsetof(FBoneRuntime, Matrix) == 0x40, "bone world matrix +40");

// LOD/skel holder 0x10.
struct FLodHolder
{
    FSkeleton* Skeleton;
    FBoneRuntime* BoneSlots;
    int BindCursor;              // +8 the routine walk (also slot count after bind)
    int RefCount;

    unsigned char AttachSkeleton(FSkeleton* Skel); // formerly AttachLodSkeleton
};

static_assert(sizeof(FLodHolder) == 0x10, "LOD holder must be 0x10");

// / the routine: 12-stride Golub SVD (IK Jacobian ≤ 4×3).
static const int G_SvdMaxDim = 12;
static const int G_SvdMaxIter = 30;
static const int G_IkMaxJoints = 4;

struct FSvdMatrix
{
    float Cell[G_SvdMaxDim][G_SvdMaxDim]; // +0x00 stride 0xC
    int RowCount;       // +0x240 this+0x240
    int ColCount;       // +0x244 this+0x244
};

static_assert(sizeof(FSvdMatrix) == 0x248, "SVD matrix must be 0x248");
static_assert(offsetof(FSvdMatrix, RowCount) == 0x240, "SVD rows +240");
static_assert(offsetof(FSvdMatrix, ColCount) == 0x244, "SVD cols +244");

// list node 0x14. the routine IK event 0x44.
// Mixed list @ instance+0x0C: Active==0 animation handle @+4; Active!=0 FIkEvent*.
struct FIkListNode
{
    unsigned char Active; // +0 = 0 anim (the routine); 1 IK (the routine)
    unsigned char Pad01[3];
    struct FIkEvent* Event; // +4 handle or FIkEvent*
    unsigned long Extra8;   // +8 the routine local
    FIkListNode* Prev; // +0xC
    FIkListNode* Next; // +0x10
};

static_assert(sizeof(FIkListNode) == 0x14, "IK list node must be 0x14");
static_assert(offsetof(FIkListNode, Extra8) == 0x08, "list node extra +8");

// IK event owns its typed channels and serves as their owner callback.
struct FIkEvent : IChannelOwner
{
    FIkEvent() {}            // the inlined constructor's field writes stay at the call site
    ~FIkEvent();
    FIkEvent(const FIkEvent&) = delete;
    FIkEvent& operator=(const FIkEvent&) = delete;

    // (vtable +0x00).
    int NEURONCOMPONENTS_API Apply(float* Sampled, unsigned long ChannelKey, void* BindDesc,
                        float Time) override;

    FIkListNode* Owner;      // +0x04
    int* JointIds;           // +0x08
    float* BindMats;         // +0x0C count * 0x24
    int StartJoint;          // +0x10
    int EndJoint;            // +0x14
    void** Channels;         // +0x18
    float* Quats;            // +0x1C count * 0x10
    void* TargetA;           // +0x20 desc+0x10
    void* TargetB;           // +0x24 desc+0x14
    unsigned long Flags28;   // +0x28 desc+0x0C
    unsigned char* LockBytes;// +0x2C
    int Extra30;             // +0x30 desc+0x1C
    unsigned char Flags34;   // +0x34 ctor &= ~1
    unsigned char Pad35[3];
    float Damp38;            // +0x38 desc+0x18
    int Tick3C;              // +0x3C
    int JointCount;          // +0x40 the routine
};

static_assert(sizeof(FIkEvent) == 0x44, "IK event must be 0x44");
static_assert(offsetof(FIkEvent, JointIds) == 0x08, "IK joints +8");
static_assert(offsetof(FIkEvent, Flags28) == 0x28, "IK flags +28");
static_assert(offsetof(FIkEvent, Flags34) == 0x34, "IK flags34 +34");
static_assert(offsetof(FIkEvent, Damp38) == 0x38, "IK damp +38");
static_assert(offsetof(FIkEvent, JointCount) == 0x40, "IK joint count +40");

// arg4: +0 skip DLS; +0xC joint for +0xC4/+0xD4.
struct FIkApplyGate
{
    int SkipFlag;
    unsigned char Pad04[8];
    int JointAt0C;
};

static_assert(offsetof(FIkApplyGate, JointAt0C) == 0x0C, "IK apply gate +C");

struct FIkMathDesc
{
    unsigned long Size;
    const char* Name;
    void* Extra;
};

static_assert(sizeof(FIkMathDesc) == 0xC, "3DMathEngine desc must be 0xC");

// Material-context VB listing followed by its allocation capacity.
struct FDeformVbDesc
{
    FVbListing Listing;
    int Capacity;
};

// The part's dynamic index buffer (part +0x24): {Handle, lock offset, capacity}; the first
// eight bytes are the IB scratch the material context points at (ctx+0x18).
struct FDeformIbDesc
{
    FIbScratch Scratch;
    int Capacity;
};

static_assert(sizeof(FDeformVbDesc) == 0x10, "part VB descriptor is 0x10");
static_assert(sizeof(FDeformIbDesc) == 0xC, "part IB descriptor is 0xC");

// constructs it (13 zero dwords, then byte +0x30 |= 1).
struct FDeformPart
{
    FLodHolder* Lod;             // +0x00
    FDeformMesh* Current;        // +0x04
    FDeformMesh** Meshes;        // +0x08 counted body (the routine)
    int MeshCount;               // +0x0C
    int LodIndex;                // +0x10
    FDeformVbDesc Vb;            // +0x14
    FDeformIbDesc Ib;            // +0x24
    // Bit 0: the LOD changed (the routine) -- rebuild both buffers; bit 1: the skin is
    // stale (the routine). the routine clears both after a draw.
    unsigned char Flags;         // +0x30 = 1
    unsigned char Pad31[3];

    unsigned char BindMeshes(FLodHolder* Holder, FDeformMesh** InMeshes, int Count); // formerly BindMeshesToPart
    void Reset(); // formerly DestroyPart
};

static_assert(offsetof(FDeformPart, Vb) == 0x14, "part VB descriptor +0x14");
static_assert(offsetof(FDeformPart, Ib) == 0x24, "part IB descriptor +0x24");
static_assert(offsetof(FDeformPart, Flags) == 0x30, "part flags +0x30");
static_assert(sizeof(FDeformPart) == 0x34, "part is 0x34");

// Playback notifications forward to the caller-provided channel sink.
struct FDeformInstance : IChannelSink
{
    //                       // +0x00 the routine (3 slots), the compiler's table now
    int PartCount;           // +0x04
    FDeformPart* PartArray;  // +0x08 counted array body (header at -4)
    FIkListNode* ListHead;          // +0x0C
    FIkListNode* ListTail;          // +0x10
    int ListCount;           // +0x14
    IChannelSink* UserAt18;  // +0x18 Create arg3 (the routine param_2)
    void* UserAt1C;          // +0x1C Create arg4 (the routine param_3)
    float Offset20;          // +0x20 the routine set; pose Y add
    float Offset24;          // +0x24 the routine: bone+0x34 − Offset20
    float RateCookie;        // +0x28 the field / rand phase

    void NEURONCOMPONENTS_API OnKeys(unsigned long ChannelKey, void* Context,
                          FChannelKeyBatch* Batch) override;
    void NEURONCOMPONENTS_API OnOpaqueNotification(void* A, void* B) override; // formerly Slot01
    void NEURONCOMPONENTS_API OnLoop(unsigned long ChannelKey, float* Transform,
                          void* Context) override;
};

static_assert(sizeof(FDeformableFactory) == 0x0C, "factory must be 0xC");
static_assert(sizeof(FDeformableObject) == 0x18, "component must be 0x18");
static_assert(offsetof(FDeformableObject, Controlling) == 0x08, "deformable Controlling +8");
static_assert(offsetof(FDeformableObject, Inner) == 0x0C, "deformable inner +C");
static_assert(sizeof(FDeformableInner) == 0x0C, "deformable inner is 0xC (ref +0x10, owner +0x14)");
static_assert(sizeof(FDeformPart) == 0x34, "part must be 0x34");
static_assert(offsetof(FDeformPart, Meshes) == 0x08, "part meshes +8");
static_assert(offsetof(FDeformPart, MeshCount) == 0x0C, "part mesh count +C");
static_assert(offsetof(FDeformPart, Vb) + offsetof(FDeformVbDesc, Listing) +
                      offsetof(FVbListing, VertexBuffer) == 0x14, "part VB handle +14");
static_assert(offsetof(FDeformPart, Vb) + offsetof(FDeformVbDesc, Listing) +
                      offsetof(FVbListing, Fvf) == 0x18, "part VB FVF +18");
static_assert(offsetof(FDeformPart, Vb) + offsetof(FDeformVbDesc, Listing) +
                      offsetof(FVbListing, BaseVertex) == 0x1C, "part VB lock offset +1C");
static_assert(offsetof(FDeformPart, Vb) + offsetof(FDeformVbDesc, Capacity) == 0x20,
              "part vertex capacity +20");
static_assert(offsetof(FDeformPart, Ib) + offsetof(FDeformIbDesc, Scratch) +
                      offsetof(FIbScratch, IndexBuffer) == 0x24, "part IB handle +24");
static_assert(offsetof(FDeformPart, Ib) + offsetof(FDeformIbDesc, Scratch) +
                      offsetof(FIbScratch, Bias) == 0x28, "part IB lock offset +28");
static_assert(offsetof(FDeformPart, Ib) + offsetof(FDeformIbDesc, Capacity) == 0x2C,
              "part index capacity +2C");
static_assert(offsetof(FDeformPart, Flags) == 0x30, "part flags +30");
static_assert(sizeof(FDeformInstance) == 0x2C, "create instance must be 0x2C");
static_assert(offsetof(FDeformInstance, ListHead) == 0x0C, "instance list head +0c");
static_assert(offsetof(FDeformInstance, ListTail) == 0x10, "instance list tail +10");
static_assert(offsetof(FDeformInstance, Offset20) == 0x20, "instance offset +20");
static_assert(offsetof(FDeformInstance, Offset24) == 0x24, "instance offset +24");
