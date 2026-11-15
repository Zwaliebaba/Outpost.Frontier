// Author: Giperion.

#pragma once

#include "particlefx_types.h"

struct ICamera;

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// Shared constants, CRC/hash helpers, string helpers, FDUMP/debug helpers,
// appearance-pool alloc/free, COM-pointer helpers, particle/math helpers,
// mesh-pose helpers, byte-bag helpers, generic COM/VB call-thunk helpers and
// the ALE node-lib parsing helpers used by more than one of the concrete
// particlefx classes. Every shared global is declared extern here and defined
// exactly once in particlefx_common.cpp.
namespace ParticleFxImpl
{
    // ---- Constants ----------------------------------------------------
    // GR_INTERFACE_UNSUPPORTED / GR_OUT_OF_MEMORY are NEURONCOMPONENTS-wide and come from neuroncomponents.h;
    // only particlefx's own ALE codes live here.
    extern const GENRESULT GR_ALE_LOAD_FAIL;
    extern const GENRESULT GR_ALE_CREATE_FAIL;
    extern const void* kRegisterPriority;

    extern const char kAleNodeName[];
    extern const char kNodeLibName[];
    extern const char kFxNodeFactory[];
    extern const char kColideTypo[];
    extern const char kCollideFixed[];
    extern const unsigned long kNodeParentRoot;
    extern const char kAleLeaveNode[];
    extern const char kAleFileSystemName[];
    extern const unsigned long kAleDescSize;
    extern const unsigned long kAleAccessRead;
    extern const unsigned long kAleAccessWrite;
    extern const unsigned long kAleShareRead;
    extern const unsigned long kAleOpenExisting;
    extern const unsigned long kAleCreateAlways;
    extern const unsigned long kAleFileFlags;
    extern const unsigned long kAleVersion10Bits; // 1.0f the field
    extern const unsigned long kAleVersion11Bits; // 1.1f
    // Used as an array bound (char NameBuf[kAleNameMax]) across translation
    // units, so it must be a real constant expression, not an extern const.
    constexpr unsigned long kAleNameMax = 0x100;
    extern const unsigned long kCrc32Poly;

    extern const char kParticleFxSection[];
    // / the branch FDUMP - EXE/particlefx.dll exact bytes.
    extern const char kParticleFxCppPath[];
    extern const char kFdumpErrorFmt[];
    extern const char kFdumpWarningFmt[];
    extern const char kMsgAcquirePipeline[];
    extern const char kMsgAcquireTexLib[];
    extern const char kMsgAcquireVb[];
    extern const char kMsgAcquireIb[];
    extern const char kMsgAcquireDraw[];
    extern const char kMsgCreateVb[];
    extern const char kMsgDrawWithoutBegin[];
    extern const char kMsgBeginWithoutEnd[];
    extern const char kMsgEndWithoutBegin[];
    extern const unsigned int kParticleFxVbFvf; // D3DFVF_XYZ|DIFFUSE|TEX1
    extern const unsigned long kParticleFxVertexStride; // pointer += 0x18
    extern const unsigned long kInitNodeBoundBits;
    extern const unsigned long kBatchListCount; // .. the field stride 0xC
    extern const char kDrawContextCpp[];
    extern const char kMsgEndBeforeBegin[];
    extern const char kMsgMoreVerts[];
    extern const char kMsgMoreIndices[];
    extern const char kMsgEndBeforeDraw[];
    extern const unsigned long kAssocEmitterAppearance;
    extern const unsigned long kAssocAppearanceField;

    // / the routine defaults when the key or parser is missing.
    extern const unsigned long kDefaultPoolSize;
    extern const unsigned long kDefaultUseMaterialBatcher;

    // element sizes (third arg) per appearance pool.
    extern const unsigned long kStrideBasic;
    extern const unsigned long kStrideParticle;
    extern const unsigned long kStrideRect;
    extern const unsigned long kStridePerp;
    extern const unsigned long kStrideMesh;
    extern const unsigned long kStrideOriented;
    extern const unsigned long kStrideDust;
    extern const unsigned long kStrideBeam;

    extern const char kCharsetValue[];
    extern const char kCharsetDigits[];

    // Field descriptor ramp defaults; see the definitions in particlefx_common.cpp for the
    // builder addresses and property hashes.
    extern const float kDefaultGravity;
    extern const float kDefaultAirApproach;
    extern const float kDefaultAirMagnitude;
    // SetKey literals 0x41200000/0x3e99999a/0x42480000
    // (10.0/0.3/50.0), confirmed by direct analysis read wave 12
    // (particlefx-wave12-lane2). Read order in FGenericFxObject::BeginField's
    // FxRadialField branch matches the ctor's ramp-build order (Magnitude,
    // Attenuation, Approach).
    // ALC-4: FxRadialField's descriptor ramp defaults, named by the property table at
    // .data 0x062569c8 (Magnitude 10.0, Approach 0.3, Radius 50.0).
    extern const float kDefaultRadialMagnitude;
    extern const float kDefaultRadialApproach;
    extern const float kDefaultRadialRadius;
    // Turbulence descriptor ramps: inner +0x4c TurbulenceField_Approach 1.0, +0x50
    // TurbulenceField_Magnitude 10.0 (see particlefx_common.cpp).
    extern const float kDefaultTurbulenceApproach;
    extern const float kDefaultTurbulenceMagnitude;
    // wave 14 (sonnet:particlefx-wave14-lane1): the routine (FxCollideField's
    // real ctor) hardcodes 0.7f — the sole occurrence of that literal in the
    // whole binary. Was previously an arbitrary 1.0f placeholder.
    extern const float kDefaultCollideReflect;
    extern const float kDefaultCollideWidth;
    extern const float kDefaultCollideHeight;
    extern const unsigned long kDustKillAgeBits;

    // ---- Shared globals -------------------------------------------------
    // / IParticleFx+0x14: the field table (= neuroncomponents the field),
    // case-preserving. NOT IEEE 0xEDB88320. NOT NEURONCOMPONENTS_CRC::GetCRC32 (lowercase).
    extern unsigned long G_Crc32Table[256];
    extern unsigned char G_bCrc32Ready;
    extern char G_ResolvedName[0x100];
    extern char G_NodeLibraryResolvedName[0x100];
    extern char G_EffectResolvedName[0x100];

    extern FParticleFxDefaults G_Defaults;
    extern FAppearancePool G_PoolBasic;
    extern FAppearancePool G_PoolParticle;
    extern FAppearancePool G_PoolRect;
    extern FAppearancePool G_PoolPerp;
    extern FAppearancePool G_PoolMesh;
    extern FAppearancePool G_PoolOriented;
    extern FAppearancePool G_PoolDust;
    extern FAppearancePool G_PoolBeam;

    extern unsigned long G_MaxVertices; // / the field
    extern unsigned long G_MaxIndices;  // / the field

    extern bool G_bInDraw;              
    extern IParticleFx* G_EffectParticleFx;             
    extern IFxNodeLibrary* G_EffectNodeLibrary;    
    extern IParticleFx* G_InitializedParticleFx;        
    // IFxRuntime +0x24 the branch store / +0x28 the branch get+AddRef.
    // Client the routine writes the field = the routine (8-byte, vtable 005d98bc).
    // Not IEngine. Mesh Extra2 is the OUT of factory+0x0C = the routine, not this pointer.
    extern INeuronComponent* G_FxMeshFactory;
    // IFxRuntime Bind* raw stores (no AddRef): the field (the branch, the IParticleFx),
    // (the branch) and the field (the branch, the IFxEffectLibrary).
    // The particle appearance's child-effect spawn the routine / the routine reads
    // b98 and b90 directly. Were FFxRuntime members until 2026-09-26.
    extern void* G_RuntimeParticleFx;
    extern void* G_RuntimeUnknown;
    extern void* G_RuntimeEffectLibrary;
    extern I3DMathEngine* G_3DMath;    // via the routine
    extern INeuronComponent* G_RootNode;  // - GetRootNode / Detach root
    extern INeuronComponent* G_PipelineOuter;   // desc+8
    extern IRenderPipeline* G_RenderPipeline;  
    extern INeuronComponent* G_TextureLibrary;  
    extern INeuronComponent* G_RpVertexBuffer;  
    extern INeuronComponent* G_RpIndexBuffer;   
    extern INeuronComponent* G_RpDraw;          
    extern void* G_VbHandle;                
    extern void* G_IbHandle;
    extern unsigned long G_DrawCookie;            
    extern unsigned long G_DrawCounters[9];       
    extern unsigned char G_DrawFlagA;             
    extern unsigned char G_DrawFlagB;             

    extern FBatchList G_BatchLists[];

    // The six draw contexts (each the routine-initialised from the __xc table) and the VB
    // handle the routine snapshots for all of them.
    extern FAlDrawContext G_BeamDrawContext;     
    extern FAlDrawContext G_DustDrawContext;     
    extern FAlDrawContext G_BasicDrawContext;    
    extern FAlDrawContext G_OrientedDrawContext; 
    extern FAlDrawContext G_PerpDrawContext;     
    extern FAlDrawContext G_RectDrawContext;     
    extern void* G_DrawVbHandle;                 

    extern unsigned long G_ParticleFxRandState;
    extern unsigned long G_ParticleFxRandCounter;

    // ---- CRC / hash / string helpers ------------------------------------
    void EnsureCrc32Table();
    unsigned long HashNameCrc(const char* Name);
    void CopyCString(char* Dest, unsigned long DestChars, const char* Src);
    void FixColideFieldAlias(char* Name);

    // ---- FDUMP helpers ----------------------------------------------------
    void FdumpParticleFxError(int Line, const char* Message);
    void FdumpParticleFxWarning(int Line, const char* Message);
    void FdumpDrawContextError(int Line, const char* Message);
    bool IidEqual(const char* Left, const char* Right);
    bool NameEqual(const char* Left, const char* Right);

    GENRESULT CreateNamedFx(const char* Name, INeuronComponent** Out);

    // map hit → prototype->vtable[+0x0C](out). Port: CreateNamedFx
    // by GetFxFactoryName(), then the archetype copy (the prototype's property set
    // shared, the routine, and an appearance's field list).
    // Entry->Name is the instance-name key (Rec.B), not a NEURONCOMPONENTS class name.
    GENRESULT InstantiateFromNodeLibPrototype(INeuronComponent* Prototype, INeuronComponent** Out);

    // Forward declaration only (defined in particlefx_genericfx.h): lets
    // IntegratePool/ApplyFieldsToPool below take a pointer to the owning
    // appearance instance without this header including
    // particlefx_genericfx.h (which itself includes this header — a real
    // include cycle). particlefx_common.cpp includes particlefx_genericfx.h
    // directly in the .cpp (not here) to see the full type for member
    // access; see IntegratePool/ApplyFieldsToPool's doc comments there.
    class FGenericFxObjectBase;

    // ---- Appearance-pool alloc/free --------------------------------------
    void UnlinkPoolNode(FPoolLink** Head, FPoolLink** Tail, FPoolLink* Node);
    void AppendPoolNode(FPoolLink** Head, FPoolLink** Tail, FPoolLink* Node);
    // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): same shape as
    // UnlinkPoolNode/AppendPoolNode above, but typed for FPooledParticle's
    // OWN Next/Prev fields (particlefx_types.h) instead of FPoolLink's. Backs
    // the per-owner-instance active-particle list (FGenericFxObjectBase::
    // InstanceListHead_/InstanceListTail_, particlefx_genericfx.h) that
    // IntegratePool/ApplyFieldsToPool now walk instead of Pool->UsedHead —
    // see implementation tracker item 132111 for the full baseline trace this closes.
    void UnlinkParticleNode(FPooledParticle** Head, FPooledParticle** Tail, FPooledParticle* Node);
    void AppendParticleNode(FPooledParticle** Head, FPooledParticle** Tail, FPooledParticle* Node);
    void PrependParticleNode(FPooledParticle** Head, FPooledParticle** Tail, FPooledParticle* Node);
    void ResetAppearancePoolLists(FAppearancePool* Pool);
    void FreeAppearancePool(FAppearancePool* Pool);
    unsigned long PoolUsedCount(const FAppearancePool* Pool);
    FAppearancePool* PoolForAppearanceFactory(const char* Factory);
    void AppearanceDrawUnits(const char* Factory, unsigned long* VertsPer,
                             unsigned long* IndicesPer);

    // ---- COM-pointer helpers ----------------------------------------------
    INeuronComponent** AssignComPtr(INeuronComponent** Slot, INeuronComponent* Value);
    void ReleaseComPtr(INeuronComponent** Slot);
    // Typed Release-and-null for a smart-pointer member (the the routine /
    // / the routine / the routine family: release a non-null pointer, null
    // the slot).
    template <typename TInterface>
    void ReleaseDescriptorSlot(TInterface*& Slot)
    {
        if (Slot != nullptr)
        {
            Slot->Release();
            Slot = nullptr;
        }
    }

    // ---- Particle / math helpers -------------------------------------------
    FPooledParticle* ParticleFromUsedLink(FAppearancePool* Pool, FPoolLink* Link);
    void* EmitOneParticle(FAppearancePool* Pool, FGenericFxObjectBase* Appearance, const float* Six, float Age,
                          float Duration);
    void FillIdentity3x3(float* Nine);
    // 12-float FTransform (I3DMathEngine layout): Rotate (+0x30) / TransformXform (+0x28).
    void TransformDirection(const float* Xform12, float X, float Y, float Z, float* Out);
    void TransformPoint(const float* Xform12, float X, float Y, float Z, float* Out);
    void LerpVel3(float* Vel, const float* Target, float Alpha);
    void LerpVel3(float* Vel, double TargetX, float TargetY, float TargetZ, float Alpha);
    double ParticleFxRand01(); // the x87 product, unrounded
    // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): Owner is the
    // appearance instance whose OWN active list (InstanceListHead_/
    // InstanceListTail_) this walks — see the .cpp definition and
    // implementation tracker item 132111 for the full baseline trace and the removal
    // of wave21's now-superseded (and, if reinstated, actively unsafe —
    // see Pad25's doc comment) ShouldRunPoolTick guard this replaces.
    void IntegratePool(FGenericFxObjectBase* Owner, FAppearancePool* Pool, const float& Dt);

    // ---- 3D-math / mesh-pose helpers ---------------------------------------
    I3DMathEngine* Create3DMath();
    I3DMathEngine* Acquire3DMath();
    ICamera* DrawCameraFromCookie();
    bool GetDrawCameraPosition(float* Out);
    const float* GetDrawCameraTransform();
    const float* GetDrawCameraOrientation();
    // wave48 (wave48-lane2, 2026-09-11): the routine (implementation tracker item
    // 133933) -- wraps I3DMathEngine::GeneralInverse (vtable+0x5c, see
    // src/x86math/x86math.h) on a 12-float rest transform (this file's
    // Xform12 layout). See the .cpp definition for the full baseline trace and
    // the three confirmed callers (Oriented/Perp/Rect's own per-appearance
    // "local rest space" setup).
    FVector* TransformOrientedBoundsCenter(const FTransform& Transform, FVector& Out,
                                           const FVector& Input);
    FVector* TransformPerpBoundsCenter(const FTransform& Transform, FVector& Out,
                                       const FVector& Input);
    FTransform* ComputeGeneralInverseTransform(const FTransform& Source,
                                              FTransform& Out, float& OutW);
    // 2026-09-26: the node-transform cache (the routine / the routine / the routine,
    // node+0x4 flags, +0x1c raw, +0x4c composed, provider +0x80 -> Node_Transform) is
    // now FGenericFxObjectBase itself (GetNodeTransform / SetNodeTransform /
    // ComposeNodeTransform, particlefx_genericfx.h). The raw-offset helpers that stood in
    // for it here (GetAppearanceXformSlot, SetAppearanceXformSlot,
    // GetComposeMixinProvider, SetComposeMixinProvider, GetComposeAnimatedTransform,
    // ComposeAppearanceWorldTransform) had no callers and are gone, as are the mesh-pose
    // helpers (PoseOneMeshExtra2 / PoseMeshExtra2 / PoseAllMeshExtra2), replaced by
    // FxMeshAppearance::ComposeNodeTransform (the routine). The old compose helper
    // also had two defects: it multiplied by the unused first argument instead of the
    // parent, and composed the animated tail as Mul(Local, world) where baseline
    // (0x062402dd) does Mul(world, Local).

    void* AllocPoolItem(FAppearancePool* Pool);
    void FreePoolItem(FAppearancePool* Pool, void* Item);
    void ReleaseAppearancePoolItem(FAppearancePool* Pool, void* Item);
    void AllocateAppearancePool(FAppearancePool* Pool, unsigned long Count, unsigned long Stride);
    void FreeAllAppearancePools();
    unsigned long MaxULong(unsigned long Left, unsigned long Right);
    void ComputeDrawBufferHints();

    // ---- Byte-bag helpers ---------------------------------------------------

    // ---- Generic COM/VB call-thunk helpers -----------------------------------
    GENRESULT CallIfaceQi(INeuronComponent* Outer, const char* Iid, INeuronComponent** Out);
    void ReleaseCachedIface(INeuronComponent* Ptr);
    GENRESULT CallCreateVb(INeuronComponent* Vb, unsigned int Fvf, unsigned int Count, void** Out,
                           unsigned int Flags);
    GENRESULT CallDestroyVb(INeuronComponent* Vb, void** Handle);
    GENRESULT CallSelectVb(INeuronComponent* Vb, void* Handle);
    GENRESULT CallGetRenderState(IRenderPipeline* Pipeline, int State, int* Out);
    GENRESULT CallSetRenderState(IRenderPipeline* Pipeline, int State, int Value);
    GENRESULT RecreateParticleFxVertexBuffer();
    void ClearPipelineCache();
    GENRESULT CallLockVb(INeuronComponent* Vb, void* Handle, unsigned int* Offset, void** Out,
                         unsigned int Count);
    GENRESULT CallUnlockVb(INeuronComponent* Vb, void* Handle);
    GENRESULT CallLockIb(INeuronComponent* Ib, void* Handle, unsigned int* Offset, void** Out,
                         unsigned int Count);
    GENRESULT CallUnlockIb(INeuronComponent* Ib, void* Handle);
    GENRESULT CallSelectIb(INeuronComponent* Ib, void* Handle, unsigned int BaseVertexIndex);
    GENRESULT CallDrawIndexed(INeuronComponent* Draw, unsigned long Prim, unsigned int MinIndex,
                              unsigned int NumVerts, unsigned int StartIndex, unsigned int Count);
    // Out = I3DMathEngine::MulTransform(A, B), 12-float transforms.
    void ComposeTransform(float* Out12, const float* A12, const float* B12);
    unsigned short AlSubmitVertex(FAlDrawContext& Context, float X, float Y, float Z,
                                  unsigned long Diffuse, float U, float V);
    void AlSubmitQuadIndices(FAlDrawContext& Context, unsigned short I0, unsigned short I1,
                             unsigned short I2, unsigned short I3);
    void AlSubmitTriangleIndices(FAlDrawContext& Context, unsigned short I0, unsigned short I1,
                                 unsigned short I2);
    bool AlDrawContextBegin(FAlDrawContext& Context, unsigned long Prim, unsigned long VertCount,
                            unsigned long IndexCount);
    unsigned long AlDrawContextEnd(FAlDrawContext& Context);
    GENRESULT AlDrawContextDraw(FAlDrawContext& Context);
    void ClearBatchLists();
    GENRESULT EnqueueAppearanceToList(unsigned long ListIndex, INeuronComponent* Appearance);
    void FlushAppearanceBatches();
    unsigned long ParseParticleFxIniValue(char* Buffer, unsigned long Default);
    unsigned long ReadParticleFxIniValue(ICOManager* Manager, IProfileParser* Parser,
                                     const char* SectionName, const char* Key,
                                     unsigned long Default, unsigned long* Out);
    void ApplyParticleFxIniAndPools(ICOManager* Manager);

    // ---- ALE node-lib parsing helpers (shared by FParticleFx / FFxEffectArchetype
    //      / FFxEffectObject / FGenericFxObject) ------------------------------
    IGenericFx* QueryGenericFx(INeuronComponent* Node);
    // (cos -> Out[0], sin -> Out[1]); the routine when the CPU has SSE.
    void ComputeSinCosPair(float AngleRadians, float* OutCos, float* OutSin);
    using FnParticleFxSinCos = void (*)(float, float*);
    extern FnParticleFxSinCos G_ParticleFxSinCos;
    void SelectParticleFxMathImplementation();
    // bLiveItemsOnly: apply only to items with Age < Duration (the particle appearance's
    // inline walk, 0x0623046b); every other baseline copy applies to every listed item.
    void ApplyFieldsToPool(FGenericFxObjectBase* Owner, bool bLiveItemsOnly);
    // Direct node-library readers; mutations and COM cleanup follow each chunk read.
    int ReadRampSingleBlock(FAleChunk* Chunk, IRampSingle* Target);
    int ReadRampColorBlock(FAleChunk* Chunk, IRampColor* Target);
    int ReadAnimatedSingleBlock(FAleChunk* Chunk, IFxAnimatedSingle* Target);
    int ReadAnimatedSingleRamp(FAleChunk* Chunk, INestedSingleRamp* Outer,
                               unsigned long VersionBits);
    int ReadAnimatedTransform(FAleChunk* Chunk, IAnimatedTransform* Anim,
                              unsigned long VersionBits);
    void CallAssociateAppearance(IGenericFx* Sink, INeuronComponent* Appearance);
    void CallAssociateField(IGenericFx* Sink, INeuronComponent* Field);
    IGenericFx* AsFxNode(void* Node);
    INeuronComponent* CallGetAttachParent(void* Node);
    INeuronComponent* CallGetFirstChild(void* Node);
    INeuronComponent* CallGetPrevSibling(void* Node);
    INeuronComponent* CallGetNextSibling(void* Node);
    int CallGetSortKey(void* Node);
    void CallSetParent(INeuronComponent* Node, INeuronComponent* Parent);
    void CallSetFirstChild(INeuronComponent* Node, INeuronComponent* Child);
    void CallSetPrevSibling(INeuronComponent* Node, INeuronComponent* Prev);
    void CallSetNextSibling(INeuronComponent* Node, INeuronComponent* Next);
    void LinkNodeTree(INeuronComponent* Parent, INeuronComponent* Child);
    GENRESULT UnlinkNodeTree(INeuronComponent* Node);
    unsigned long RemapNodeLibHash(unsigned long Hash);
    unsigned long RemapNodeLibKind(unsigned long Kind, unsigned long VersionBits);
    // The node library's property stream: the loader's loop over the routine and the
    // save's per-node body of the routine (class name, the branch per row, zero type).
    int ReadNodeLibProperties(FAleChunk* Chunk, IFxNodePropertySet* Set, unsigned long VersionBits);
    int WriteNodeLibProperties(FAleChunk* Chunk, INeuronComponent* Node);

} // namespace ParticleFxImpl
