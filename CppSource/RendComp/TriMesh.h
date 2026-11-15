// Author: Giperion.

#pragma once

// TriMesh archetype, face groups and UV animation state.

#include "RendComp.h"
#include "render/shading.h" // IMaterialLibrary (archetype +0x21C)

#include <cstddef>
#include <vector>

struct IMaterial;

enum ETriMeshTriangleClass : unsigned long
{
    TriMeshTriangleAllFront = 0x1,
    TriMeshTriangleAllBack = 0x2,
    TriMeshTriangleFrontFrontBack = 0x88,
    TriMeshTriangleFrontBackFront = 0x108,
    TriMeshTriangleFrontBackBack = 0x18,
    TriMeshTriangleBackBackFront = 0x48,
    TriMeshTriangleBackFrontBack = 0x28,
    TriMeshTriangleBackFrontFront = 0x208,
};

// One face group: operator new(0x40) + the routine (Type 4, Bucket 4, every other dword 0).
// Copy ctor the routine, assignment the routine, dtor the routine.
struct FTriMeshGroup
{
    static constexpr unsigned long ExtraStreamCount = 4;

    void Initialize(); // formerly TriMeshGroupInit
    void ReleaseResources(); // formerly TriMeshGroupDestroy
    void CopyFrom(const FTriMeshGroup* Source); // formerly TriMeshGroupAssign
    void SetIndices(unsigned long Count, const unsigned short* Source); // formerly TriMeshGroupSetIndices
    void SetEdges(unsigned long Count, const unsigned short* Indices, const float* Angles); // formerly TriMeshGroupSetEdges
    void Rebase(); // formerly TriMeshGroupRebase
    void Unrebase(); // formerly TriMeshGroupUnrebase
    void CopyHeader(const FTriMeshGroup* Source); // formerly TriMeshGroupAssignHeader
    bool HasChannels(unsigned long Channels) const; // formerly TriMeshGroupHasChannels
    void ConvertStripToList(); // formerly TriMeshGroupStripToList
    void PrivatizeVertices(); // formerly TriMeshGroupPrivatize
    void CopyTriangles(const FTriMeshGroup* Source, const unsigned long* Classes, unsigned long Wanted, unsigned long Count); // formerly TriMeshGroupTakeTriangles
    unsigned long ClassifyTriangle(int Triangle, const float* Plane, float PlaneD) const; // formerly TriMeshClassifyTriangle
    void ClipStraddling(FTriMeshGroup* Front, FTriMeshGroup* Back, const unsigned long* Classes, int StraddleCount) const; // formerly TriMeshClipStraddling

    void AllocateExtraStreams(); // formerly TriMeshGroupAllocExtras
    void FreeExtraStreams(); // formerly TriMeshGroupFreeExtras

    unsigned long Type;                // +0x00 D3D primitive type: 4 list, 5 strip
    unsigned long MaterialCrc;         // +0x04 CRC of the lower-cased "Material_name"
    IMaterial* Material;               // +0x08 bound by the hook-up (the branch)
    void* Field_0x0C;                  // +0x0C IMesh Slot16 reads / Slot17 writes it; Draw puts it
                                       //       in submit ctx+0x20. TODO: meaning (no reader here)
    unsigned long MinIndex;            // +0x10 the routine: smallest raw index, subtracted out
    unsigned long MaxIndex;            // +0x14 the routine: largest raw index
    unsigned short* Indices;           // +0x18 "Face_indices" / "Tristrip_indices", rebased
    unsigned long IndexCount;          // +0x1C
    unsigned short* EdgeIndices;       // +0x20 "Edge_indices" (position indices, the routine)
    float* EdgeAngles;                 // +0x24 "Edge_angles", EdgeCount / 2 entries
    unsigned long EdgeCount;           // +0x28
    FRpVertexSrcDesc* Vertices;        // +0x2C the vertex source this group draws from
    FRpVertexSrcDesc* SharedVertices;  // +0x30 the archetype's +0x1C; == Vertices when shared
    unsigned long* ExtraIndexCounts;   // +0x34 new(0x10) (the routine): per extra stream count
    FRpVertexStream* ExtraStreams;     // +0x38 new(0x50): four streams, IMesh channels 0x4000..
    unsigned long Bucket;              // +0x3C hook-up sort key 0..4 (IMaterial flags)
};

static_assert(sizeof(FTriMeshGroup) == 0x40, "TriMesh face group is 0x40 bytes");

// The UV0 animation block at archetype +0x8C (seven dwords, zeroed by the ctor and the reset).
struct FTriMeshUvAnim
{
    unsigned long LookupCount;  // +0x00 (+0x8C) "UV0_anim_lookup" entries
    long* Lookup;               // +0x04 (+0x90) vertex slot per entry, -1 = skip
    unsigned long FrameCount;   // +0x08 (+0x94) "UV0_frame_count"
    float* Frames;              // +0x0C (+0x98) "UV0_anim": LookupCount (u, v) pairs per frame
    float Fps;                  // +0x10 (+0x9C) "UV0_fps"
    unsigned long Interpolate;  // +0x14 (+0xA0) "UV0_interpolate"
    float Period;               // +0x18 (+0xA4) (FrameCount - 1) / Fps
};

static_assert(sizeof(FTriMeshUvAnim) == 0x1C, "TriMesh UV0 animation block is 0x1C bytes");


// Group-level helpers (cdecl / thiscall in baseline).
FTriMeshGroup* TriMeshGroupCopy(const FTriMeshGroup* Source);                // new + the routine
void TriMeshStreamFree(FRpVertexStream* Stream);
void TriMeshStreamCopy(const FRpVertexStream* Source, FRpVertexStream* Dest,
                       unsigned long IndexCount);
void TriMeshDescFree(FRpVertexSrcDesc* Desc);

class FTriMeshArch : public IMesh
{
public:
    // +0x04: vtable the branch (4 slots). QI/AddRef/Release are the this-4 thunks
    // / the branch (the routine = 1); Slot3 is the material hook-up.
    class FHookFacet : public IMatHookUp
    {
    public:
        FTriMeshArch* Owner;

        explicit FHookFacet(FTriMeshArch* InOwner) : Owner(InOwner) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
        int NEURONCOMPONENTS_API HookUpMaterials() override; // // formerly Slot3
    };

    // +0x08: vtable the branch (7 slots). QI/AddRef/Release are the this-8 thunks
    // / the branch.
    class FRenderableFacet : public IRenderableMesh
    {
    public:
        FTriMeshArch* Owner;

        explicit FRenderableFacet(FTriMeshArch* InOwner) : Owner(InOwner) {}

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
        int NEURONCOMPONENTS_API GetVertexCount(int* Out) override;                         // // formerly Slot3
        int NEURONCOMPONENTS_API CopyVertexData(void* Dest, unsigned int Fvf, unsigned int StartVertex, unsigned int VertexCount) override; // // formerly Slot4
        int NEURONCOMPONENTS_API GetIndexCount(int* Out) override;                         // // formerly Slot5
        int NEURONCOMPONENTS_API CopyIndices(unsigned short** Dest, int Count) override;           // // formerly Slot6
    };

    FHookFacet HookFacet;                  // +0x04
    FRenderableFacet RenderableFacet;      // +0x08
    // +0x0C..+0x18: VC6 std::vector<FTriMeshGroup*> (allocator byte, first, last, end).
    std::vector<FTriMeshGroup*> Groups;
    FRpVertexSrcDesc Vertices;             // +0x1C the shared vertex source (loader, the routine)
    FTriMeshUvAnim UvAnim;                 // +0x8C
    unsigned long Flags;                   // +0xA8 bit0 loaded, bit1 sphere cached, bit2 box
                                           //       cached, bit3 centroid computed
    char Name[0x104];                      // +0xAC the node path (IFileSystem +0xA4)
    float Center[3];                       // +0x1B0 the routine
    float Box[6];                          // +0x1BC max x, min x, max y, min y, max z, min z
    float SphereCenter[3];                 // +0x1D4 the routine
    float SphereRadius;                    // +0x1E0 -1.0f after a reset
    void* Field_0x1E4;                     // +0x1E4 the routine hands it to Draw as a ready
                                           //       vertex listing; only ever zeroed. TODO
    // +0x1E8 the edit cursor, baseline a Group** into the vector; the port keeps the index and
    // treats Cursor == Groups.size() as baseline's "cursor == end".
    size_t Cursor;
    unsigned long LockedMask;              // +0x1EC channels locked by IMesh Slot20
    unsigned short* Locked[10];            // +0x1F0..+0x214 per-channel locked index copies
    int EditMode;                          // +0x218 IMesh Slot9 mode (2 = private geometry)
    IMaterialLibrary* MaterialLibrary;     // +0x21C the field (loader the routine)
    IRenderPipeline* Pipeline;           // +0x220 the field
    IVertexBufferManager* VbManager;       // +0x224 the field

    FTriMeshArch();
    ~FTriMeshArch();

    bool CursorAtEnd() const { return Cursor == Groups.size(); }

    // Non-interface members (thiscall in baseline).
    int Reset();
    int Load(IFileSystem* File, IRenderPipeline* InPipeline, IVertexBufferManager* InVbManager,
             IMaterialLibrary* InLibrary, int bTristrips);
    int Clone(FTriMeshArch** Out);
    int SplitByPlane(const float* Plane, float PlaneD, unsigned char SplitFlags,
                     FTriMeshArch** OutFront, FTriMeshArch** OutBack);
    int CompactVertices();
    int GetBox(float* Out);
    int GetSphere(float* OutCenter, float* OutRadius);
    int GetCenter(float* Out);
    int LockChannel(unsigned long Channel);
    int UnlockChannel(unsigned long Channel);
    int LockEdgeAngles();
    int UnlockEdgeAngles();
    int TriangleCount();
    int VertexCount();
    bool GetPrebuiltListing(void** Out);
    int PrivatizeCursorGroup();
    int ShareGroupVertices(bool bAll);

    // INeuronComponent (the routine / the routine).
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    // IMesh, table the routine.
    int NEURONCOMPONENTS_API ResetMesh() override;                                           // // formerly Slot3
    int NEURONCOMPONENTS_API NoOp() override;                                           // // formerly Slot4
    int NEURONCOMPONENTS_API RefreshMaterials(unsigned long Flags) override;                        // // formerly Slot5
    int NEURONCOMPONENTS_API RejectOperation(void* A) override;                                    // // formerly Slot6
    int NEURONCOMPONENTS_API RejectOperation() override;                                           // // formerly Slot7
    int NEURONCOMPONENTS_API GetFaceGroupCount(int* Out) override;                                   // // formerly Slot8
    int NEURONCOMPONENTS_API OpenFaceGroup(int Index, int Mode) override;                        // // formerly Slot9
    int NEURONCOMPONENTS_API CloseFaceGroup() override;                                          // // formerly Slot10
    int NEURONCOMPONENTS_API AddFaceGroup(void* A) override;                                   // // formerly Slot11
    int NEURONCOMPONENTS_API RemoveFaceGroup(int Index) override;                                 // // formerly Slot12
    int NEURONCOMPONENTS_API GetFaceGroupPrimitiveType(unsigned int* Out) override;                                // // formerly Slot13
    int NEURONCOMPONENTS_API GetFaceGroupMaterial(IMaterial** Out) override;                           // // formerly Slot14
    int NEURONCOMPONENTS_API SetFaceGroupMaterial(IMaterial* Material) override;                    // // formerly Slot15
    int NEURONCOMPONENTS_API GetFaceGroupMaterialContext(void** Out) override;                                // // formerly Slot16
    int NEURONCOMPONENTS_API SetFaceGroupMaterialContext(void* A) override;                                   // // formerly Slot17
    int NEURONCOMPONENTS_API OpenFaceGroupChannels(unsigned long Flags) override;                       // // formerly Slot18
    int NEURONCOMPONENTS_API CloseFaceGroupChannels() override;                                          // // formerly Slot19
    int NEURONCOMPONENTS_API LockFaceGroupChannels(unsigned long Mask) override;                        // // formerly Slot20
    int NEURONCOMPONENTS_API UnlockFaceGroupChannels() override;                                          // // formerly Slot21
    int NEURONCOMPONENTS_API GetFaceGroupData(int Channel, void** A, void** B, void** C, void** D) override; // // formerly Slot22
    int NEURONCOMPONENTS_API SetFaceGroupData(int Channel, void* A, void* B, void* C, void* D) override;     // // formerly Slot23
    int NEURONCOMPONENTS_API GetFaceGroupIndices(int Channel, void** A, void** B) override;           // // formerly Slot24
    int NEURONCOMPONENTS_API SetFaceGroupIndices(int Channel, void* Src, int Count) override;         // // formerly Slot25
    int NEURONCOMPONENTS_API GetFaceGroupIndexCount(int Selector, void** Out) override;                  // // formerly Slot26
    int NEURONCOMPONENTS_API SetFaceGroupIndexCount(int Selector, int Count) override;                   // // formerly Slot27
    int NEURONCOMPONENTS_API AddFaceGroupChannels(unsigned long Flags) override;                       // // formerly Slot28
    int NEURONCOMPONENTS_API RemoveFaceGroupChannels(unsigned long Flags) override;                       // // formerly Slot29
};
