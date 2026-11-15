// Author: Giperion.

#pragma once

#include "neuroncomponents.h"
#include "particlefx.h"
#include "readfile.h"
#include "render/IRenderPipeline.h"
#include "render/shading.h"
#include "x86math.h"
#include "luaprofile.h"

#include <cstdint>
#include <list>

// Internal ParticleFx data records, particle pools, draw context and node interfaces.
// Cross-DLL contracts come from src/include/particlefx.h and other public headers.
namespace ParticleFxImpl
{
    // Declared at global scope in src/include/particlefx.h so that NeuronRuntime's exported
    // ::IParticleFx* / ::IFxRuntime* / ::IFxEffectLibrary* mean the same type as these.
    using ::FAleRec16;
    using ::FAleRec8;
    using ::IFxEffectArchetype;
    using ::IParticleFx;
    using ::IFxRuntime;
    using ::IFxEffectLibrary;
    using ::IFxEffect;

    struct FAggDesc8
    {
        unsigned long Size;
        const char* NameAt4;
    };

    struct FAggDesc14
    {
        unsigned long Size;
        const char* NameAt4;
        void* InnerAt8;
        void** WritebackAtC;
    };

    // FxEffect initialization is not a NEURONCOMPONENTS aggregation descriptor.
    struct FFxEffectCreateDesc
    {
        unsigned long Size;
        const char* NameAt4;
        IFxEffectArchetype* Archetype;
        INeuronComponent* RootParent;
    };

    struct FParticleFxDefaults
    {
        unsigned long BasicPoolSize;
        unsigned long ParticlePoolSize;
        unsigned long RectPoolSize;
        unsigned long PerpPoolSize;
        unsigned long MeshPoolSize;
        unsigned long OrientedPoolSize;
        unsigned long DustPoolSize;
        unsigned long BeamPoolSize;
        unsigned char bUseMaterialBatcher;
    };

    // pool: +0 Items, +4 FreeNodes (count*{Next,Prev}), +8 stride,
    // +0xC capacity, +0x10 free count, +0x14/+0x18 used head/tail,
    // +0x1C/+0x20 free head/tail. Alloc/free only — no particle update.
    struct FPoolLink
    {
        FPoolLink* Next;
        FPoolLink* Prev;
    };

    struct FAppearancePool
    {
        void* Items;
        FPoolLink* FreeNodes;
        unsigned long Stride;
        unsigned long Capacity;
        unsigned long FreeCount;
        FPoolLink* UsedHead;
        FPoolLink* UsedTail;
        FPoolLink* FreeHead;
        FPoolLink* FreeTail;
    };

    struct IGenericFx;
    class FGenericFxObjectBase;

    // four {head, tail, count} records, stride 0xc. The list is intrusive:
    // / the branch link the appearance itself through its outer +4 (prev)
    // and +8 (next) -- FGenericFxObjectBase::BatchPrev_ / BatchNext_ here -- with no
    // AddRef. The records are zeroed at load (the branch) and by BeginDraw when the
    // batcher is on (0x06201abf), never by the flush.
    struct FBatchList
    {
        IGenericFx* Head;
        IGenericFx* Tail;
        unsigned long Count;
    };

    // The draw context the routine (begin) / the routine (end) / the routine (draw) take in
    // ECX. baseline has six of them, one per billboard class, in .data (see G_BeamDrawContext and
    // siblings in particlefx_common.h); fields in baseline order:
    //   +0x00 primitive type, +0x04 / +0x06 vertex / index counters (u16), +0x08 in-begin
    //   flag (byte), +0x0a / +0x0c u16 counters (+0x0c = primitives written: quad stores add 2,
    //   triangle stores 1; both are zeroed by each class's per-frame update and by the
    //   __xc initializer the routine -- neither has a reader in particlefx.dll), +0x10 / +0x14
    //   locked vertex / index write pointers, +0x18 / +0x1c requested vertex / index counts,
    //   +0x20 / +0x24 the offsets the VB and IB Lock calls return (Draw hands the VB offset to
    //   SelectIb as the base vertex and the IB offset to DrawIndexedPrimitive as the start
    //   index). The VB handle Begin fetches is NOT per context: it is the module global
    // (G_DrawVbHandle), shared by all six.
    struct FAlDrawContext
    {
        unsigned long Prim;          // +0x00
        unsigned short VertCount;    // +0x04
        unsigned short IndexCount;   // +0x06
        unsigned char bInBegin;      // +0x08
        unsigned short Field_0xA;    // +0x0a TODO: meaning unknown (zeroed, never incremented)
        unsigned short PrimCount;    // +0x0c
        void* LockedVerts;           // +0x10
        void* LockedIndices;         // +0x14
        unsigned long RequestedVerts;   // +0x18
        unsigned long RequestedIndices; // +0x1c
        unsigned int VbOffset;       // +0x20
        unsigned int IbOffset;       // +0x24
    };

    struct FParticleFxVertex
    {
        float X;
        float Y;
        float Z;
        unsigned long Diffuse;
        float U;
        float V;
    };

    // Pool item: the routine / the routine. Stride Basic 0x30; Beam/Mesh 0x34
    // Extra2 +0x30 = COM (the routine), not a 3x3.
    
    // kStrideParticle (0x74, G_PoolParticle / FxParticleAppearance only): the tail
    // past these 0x30 bytes is FParticleAppRecord (particlefx_appearances.h) -- +0x30
    // and +0x4c float triples, +0x3c / +0x58 quaternions, +0x68 blend, and at
    // +0x6c / +0x70 the record's life and death child EFFECTS (IFxEffect from
    // IFxEffectLibrary::CreateEffect, keyed by ParticleApp_LifeName / DeathName). The
    // spawn the routine (FxParticleAppearance::Pad47) builds them; the record
    // teardown the routine (FreePoolItem's G_PoolParticle branch) calls IFxEffect
    // slot 13(7) on each and releases them. (Rewritten 2026-09-26 by ALC-1: the old
    // text here called the slots child ramps and the construction an open gap.)
    struct FPooledParticle
    {
        FPooledParticle* Next;
        FPooledParticle* Prev;
        FGenericFxObjectBase* Appearance;
        float PosX;
        float PosY;
        float PosZ;
        float VelX;
        float VelY;
        float VelZ;
        float Age;
        float Duration;
        float InvDuration;
    };

    static_assert(sizeof(FPooledParticle) == 0x30,
                  "FPooledParticle must match Basic/Rect stride 0x30");

    struct IAnimatedTransform;
    struct INestedSingleRamp;
    struct IRampColorRamp;
    struct IRampSingle;
    struct IRampColor;
    struct IFxAnimatedSingle;

    // ---- Node property sets (ALC-6, 2026-09-30) ------------------------------------------
    
    // baseline keeps a node's authored properties in a separate refcounted object, the
    // property set ("descriptor"): an outer COM object (QI / AddRef / Release, refcount at
    // its end) with the set proper at +4. Every node class has its own set class, created
    // by the node's slot +0x48 (sphere the routine, cone the routine, basic the routine,
    // ...), which the node's Initialize the routine calls on every factory creation. The
    // node holds the outer in +0x7c (AddRef'd) and the set in +0x80 (emitters / fields also
    // in +0x9c, appearances in +0xa8 / +0xf0); the clone base the routine copies both
    // pointers, so the archetype and all its clones share ONE set. The .ale reader
    // (the routine) writes into it, the per-frame updates read from it.
    
    // The set's inner vtable (base 0x0624f8d8) is the property protocol:
    //   +0x04 GetBool / +0x08 SetBool (byte), +0x0c GetInt / +0x10 SetInt,
    //   +0x14 GetFloat / +0x18 SetFloat, +0x1c GetPair / +0x20 SetPair (two dwords),
    //   +0x24 kind 0x101 / +0x28 kind 0x100 / +0x2c kind 0x102 getters (-8 in every class),
    //   +0x30 GetName / +0x34 SetName, +0x38 kind 0x201 / +0x3c 0x200 / +0x40 0x202 ramp
    //   getters (AddRef'd), +0x44 name CRC, +0x48 name, +0x4c set name, +0x50 kind 0x105
    //   transform getter (AddRef'd), +0x54 enumerate the class's property table.
    // Every accessor returns 0 on a hash it owns and -8 (0xfffffff8) on any other. The
    // inner +0x00 is the set's Initialize (base the routine), called only by the set
    // factories (the live "FxNodePropertySet" one, CreateInstance the routine).
    
    // The outer vtable (IFxNodePropertySet below) is what the rest of particlefx.dll sees:
    // node outer +0x80 hands it out AddRef'd (FxNode the routine) and the .ale reader /
    // writer (0x06228d1b / 0x06228280) call the protocol through it. Its slots are thunks
    // into the inner vtable (`ADD [ESP+4],4; JMP`) or copies of the inner bodies.
    
    // The port has one C++ object per set (FFxNodePropertySet and its derived classes,
    // one per baseline set class): the outer's interface is its IFxNodePropertySet base,
    // the inner vtable's bodies are its virtual overrides, the data its members.

    // One row of a property-set table in .data (0x14 bytes: name, hash, kind, min, max).
    // Declared ahead of the interface, which enumerates rows.
    struct FNodePropertyRow;

    // The enumeration callback (cdecl (Row, Context)): a non-zero return stops the walk and
    // is what the walk returns (the routine 0x0623f93f, the routine 0x0623f982).
    typedef int (*FNodePropertyRowCallback)(const FNodePropertyRow* Row, void* Context);

    // "1.11_IFxNodePropertySet", the set's outer vtable (basic 0x0624cd20, sphere
    // , FxNode 0x0624f980, particle 0x0624e730, ...). Private to particlefx.dll:
    // nothing outside it names the interface. Offsets are the outer's; the inner slot
    // each one reaches is in parentheses.
    struct IFxNodePropertySet : INeuronComponent
    {
        // +0x0C basic 062189c0 (inner +0x4c, the routine): Node_Name := Name, RET 8, 0.
        virtual GENRESULT NEURONCOMPONENTS_API SetInstanceName(const char* Name) = 0;
        // +0x10 basic 06218a90 (inner +0x48): *Out = the Node_Name buffer, RET 8, 0.
        virtual GENRESULT NEURONCOMPONENTS_API GetInstanceName(const char** Out) = 0;
        // +0x14 basic 06218ab0 (inner +0x44): *Out = the Node_Name CRC, RET 8, 0.
        virtual GENRESULT NEURONCOMPONENTS_API GetInstanceNameHash(unsigned long* Out) = 0;
        // +0x18 (inner +0x50): kind 0x105, the transform AddRef'd.
        virtual GENRESULT NEURONCOMPONENTS_API GetPropTransform(unsigned long Hash,
                                                     IAnimatedTransform** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropBool(unsigned long Hash, unsigned char* Out) = 0;   // +0x1C (+0x04)
        virtual GENRESULT NEURONCOMPONENTS_API SetPropBool(unsigned long Hash, unsigned char Value) = 0;  // +0x20 (+0x08)
        virtual GENRESULT NEURONCOMPONENTS_API GetPropInt(unsigned long Hash, unsigned long* Out) = 0;    // +0x24 (+0x0c)
        virtual GENRESULT NEURONCOMPONENTS_API SetPropInt(unsigned long Hash, unsigned long Value) = 0;   // +0x28 (+0x10)
        virtual GENRESULT NEURONCOMPONENTS_API GetPropFloat(unsigned long Hash, float* Out) = 0;          // +0x2C (+0x14)
        virtual GENRESULT NEURONCOMPONENTS_API SetPropFloat(unsigned long Hash, float Value) = 0;         // +0x30 (+0x18)
        virtual GENRESULT NEURONCOMPONENTS_API GetPropPair(unsigned long Hash, unsigned long* Out2) = 0;  // +0x34 (+0x1c)
        virtual GENRESULT NEURONCOMPONENTS_API SetPropPair(unsigned long Hash,
                                                const unsigned long* In2) = 0;                 // +0x38 (+0x20)
        // +0x3C / +0x40 / +0x44 (+0x24 / +0x28 / +0x2c): kinds 0x101 / 0x100 / 0x102, -8 in
        // every set class.
        virtual GENRESULT NEURONCOMPONENTS_API GetPropColorRamp(unsigned long Hash, IRampColor** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropSingleRamp(unsigned long Hash, IRampSingle** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingle(unsigned long Hash,
                                                          IFxAnimatedSingle** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropName(unsigned long Hash, const char** Out) = 0;     // +0x48 (+0x30)
        virtual GENRESULT NEURONCOMPONENTS_API SetPropName(unsigned long Hash, const char* Name) = 0;     // +0x4C (+0x34)
        // +0x50 / +0x54 / +0x58 (+0x38 / +0x3c / +0x40): kinds 0x201 / 0x200 / 0x202, the
        // set's own ramp AddRef'd into *Out (the reader Resets it and fills it in place).
        virtual GENRESULT NEURONCOMPONENTS_API GetPropColorRampRamp(unsigned long Hash,
                                                         IRampColorRamp** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropSingleRampRamp(unsigned long Hash,
                                                          INestedSingleRamp** Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                              INestedSingleRamp** Out) = 0;
        // +0x5C (+0x54): the class's rows, base table first; RET 0xc.
        virtual int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) = 0;
    };

    // One row of a property-set table in .data (0x14 bytes: name, hash, kind, min, max).
    // The base table is 0x062572b0; each class appends its own (the routine walks one
    // table, the routine the base's). Min / Max are authoring bounds, not defaults.
    struct FNodePropertyRow
    {
        const char* Name;
        unsigned long Hash;
        unsigned long Kind;
        float Min;
        float Max;
    };

    // The unbounded row limit the tables hold (0x7f7fffff = FLT_MAX; its negation
    // for signed rows).
    const float kNodePropertyRowMax = 3.40282347e+38f;

    // A baseline hashed string (vtable 0x0624bd84: {vt, hash, string {..., owned, buffer,
    // length, capacity}}): the set's Node_ClassName (+0x04) and Node_Name (+0x28), the
    // particle set's life / death names, the mesh set's mesh name. Assign is the body of
    // 's Node_Name branch (0x0623f9ef..0x0623faa6) and the routine: a null
    // text frees an owned buffer and clears it; the text already held is kept; otherwise
    // the length is stored and a buffer of length + 1 is allocated unless length <
    // capacity (`CMP ECX,EAX; JL` at 0x0623fa1a -- the equal case reallocates), then the
    // text is copied with its terminator. Every path ends with Hash = the routine(buffer).
    struct FFxHashedName
    {
        unsigned long Hash;
        char* Buffer;
        unsigned long Length;
        unsigned long Capacity;

        FFxHashedName();
        ~FFxHashedName();
        void Assign(const char* Text);
        void ReleaseBuffer();

    private:
        bool bBufferReleased_ = false;

        FFxHashedName(const FFxHashedName&) = delete;
        FFxHashedName& operator=(const FFxHashedName&) = delete;
    };

    // The base set, the routine (inner vtable 0x0624f8d8). The beam field's set (inner
    // , outer 0x0624fcf0) is this class as it stands; FxNode's own set (inner
    // , outer 0x0624f980) is FFxNodeOwnPropertySet below.
    class FFxNodePropertySet : public IFxNodePropertySet
    {
    public:
        // names empty (their CRCs of null), +0x24 a default
        // FxAnimatedTransform (the routine, vtable 0x0624bbb8) that gets Reset (+0xc,
        // ), +0x48 = 3.0f (0x0623f80f).
        FFxNodePropertySet();
        virtual ~FFxNodePropertySet() noexcept(false);

        // Outer +0x00, the table QI every set class has (basic 06218670, sphere 0623cd20,
        // beam field 062452c0, particle 062310c0, FxNode 06240c50): the table is walked
        // with a strcmp, a hit stores this (offset 0) AddRef'd and returns 0; past the end
        // *Out = 0 and -3. The tables hold the class's IID (PrimaryIid) then
        // "1.11_INeuronComponent".
        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        // Outer +0x04 / +0x08 (e.g. sphere 0623cdd0 / 0623cde0): the count lives at the end
        // of the outer object; Release decrements only while non-zero and destroys at 0.
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        // The base set's bodies (inner 0x0624f8d8; the FxNode outer 0x0624f980 has copies
        // at 0x06240450..0x06240800). See particlefx_genericfx.cpp.
        GENRESULT NEURONCOMPONENTS_API SetInstanceName(const char* Text) override;
        GENRESULT NEURONCOMPONENTS_API GetInstanceName(const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetInstanceNameHash(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropTransform(unsigned long Hash, IAnimatedTransform** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropBool(unsigned long Hash, unsigned char* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropBool(unsigned long Hash, unsigned char Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropInt(unsigned long Hash, unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropInt(unsigned long Hash, unsigned long Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropFloat(unsigned long Hash, float* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropFloat(unsigned long Hash, float Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropPair(unsigned long Hash, unsigned long* Out2) override;
        GENRESULT NEURONCOMPONENTS_API SetPropPair(unsigned long Hash, const unsigned long* In2) override;
        GENRESULT NEURONCOMPONENTS_API GetPropColorRamp(unsigned long Hash, IRampColor** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropSingleRamp(unsigned long Hash, IRampSingle** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingle(unsigned long Hash,
                                                  IFxAnimatedSingle** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropName(unsigned long Hash, const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropName(unsigned long Hash, const char* Text) override;
        GENRESULT NEURONCOMPONENTS_API GetPropColorRampRamp(unsigned long Hash, IRampColorRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropSingleRampRamp(unsigned long Hash,
                                                  INestedSingleRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        // Inner +0x00, the routine (RET 8; the descriptor is not read): Transform Reset
        // (+0xc), the Node_Name string freed and emptied and its CRC retaken; 0. Called by
        // the set factories only (CreateInstance the routine at 0x06240b9b).
        virtual GENRESULT Initialize(const AGGDESC* Desc);

        FFxHashedName ClassName;       // +0x04: Node_ClassName 0xF54EF296 (read-only)
        IAnimatedTransform* Transform; // +0x24: Node_Transform 0xE13A59A1
        FFxHashedName Name;            // +0x28: Node_Name 0xEFA8CE01 (hash +0x2c)
        float LifeSpan;                // +0x48: Node_LifeSpan 0xF27FDE7D, 3.0f

    protected:
        // The first IID of the class's QI table: "1.11_IFxNodePropertySet" in every set
        // class but FxNode's own.
        virtual const char* PrimaryIid() const;

        // (Rows, Callback, Context): Callback(Row, Context) for every row up
        // to the one whose hash is 0; the first non-zero result stops the walk and is
        // returned; 0 otherwise. the routine is the same walk over the base table.
        static int WalkPropertyRows(const FNodePropertyRow* Rows, FNodePropertyRowCallback Callback,
                                    void* Context);

    private:
        unsigned long RefCount_;

        void CleanupFailedConstruction();
        FFxNodePropertySet(const FFxNodePropertySet&) = delete;
        FFxNodePropertySet& operator=(const FFxNodePropertySet&) = delete;
    };

    // FxNode's own set: created by FxNode's slot +0x48 the routine and by the
    // "FxNodePropertySet" factory's CreateInstance the routine (0x06240b52..0x06240b6e:
    // operator_new(0x54), the routine, inner vtable 0x0624f9e0, outer 0x0624f980, count
    // 1). Its bodies are the base set's; its QI (06240c50) walks the table 0x0624f8c0 =
    // {"1.11_IFxNode", "1.11_INeuronComponent"} -- a set that does not answer to
    // "1.11_IFxNodePropertySet", as baseline has it.
    class FFxNodeOwnPropertySet final : public FFxNodePropertySet
    {
    protected:
        const char* PrimaryIid() const override;
    };

    // The appearance base set, the routine (vtable 0x0624fb78), under all eight appearance
    // sets: +0x4c/+0x50/+0x54 an empty std::list (allocator byte, 0xc-byte self-linked
    // head, size 0) and +0x58 Appearance_LODCurve 0x0C53B008, an FxRampSingleRamp with one
    // FxRampSingle key 1.0 (0x06244057..0x062440f1), handed out by the routine.
    class FFxAppearancePropertySet : public FFxNodePropertySet
    {
    public:
        FFxAppearancePropertySet();
        ~FFxAppearancePropertySet() override;

        // Inner +0x3c the routine (0x06244330..0x06244357): Appearance_LODCurve
        // -> +0x58 AddRef'd, 0; else -8. Every appearance set's 0x200 getter
        // ends here (particle and mesh have it as the slot itself). Inner +0x54
        // is `JMP the routine`, the base walk, so it is not overridden.
        GENRESULT NEURONCOMPONENTS_API GetPropSingleRampRamp(unsigned long Hash,
                                                  INestedSingleRamp** Out) override;

        // +0x50: unused private trivial-word list; actual methods neither populate
        // nor observe its values or publish its storage.
        std::list<std::uint32_t> UnusedValues;
        INestedSingleRamp* EffectScaleRamp; // +0x58: Appearance_LODCurve
    };

    // The six billboard appearance sets (basic the routine inner / the routine create,
    // rect the routine, perp the routine, oriented the routine, dust the routine, beam
    // ). Offsets are the outer object's, as the draws read them through
    // node+0xf0 (dust +0xe8): Flags +0x60, TextureCrc +0x64, BlendInfo +0x68 / +0x6c,
    // SizeRamp +0x70 (BasicApp_Alpha), ColorRamp +0x74, PrimarySizeRamp +0x78, TexFrameRamp
    // +0x7c, AxisOverrideRamp +0x80, FrameRamp +0x84, CommonTexFrameRamp +0x88,
    // LocalTransform +0x8c (oriented, beam). Each class maps its own hashes onto these
    // fields (particlefx_appearances.cpp); the ramp kinds differ per class.
    class FAppearanceRampDescriptor : public FFxAppearancePropertySet
    {
    public:
        FAppearanceRampDescriptor();
        ~FAppearanceRampDescriptor() override;

        unsigned long Flags;
        // BasicApp_TexName 0x1BA23359: the set stores NEURONCOMPONENTS_CRC::GetCRC32(name) only
        // (basic the routine and its five twins); the getter hands out "???".
        unsigned long TextureCrc;
        // BasicApp_BlendInfo 0x1506EB6C: {5, 2} (D3DBLEND_SRCALPHA, D3DBLEND_ONE) from every
        // builder (basic 0x062155c3 / 0x062155ca and the five twins).
        unsigned long BlendInfo[2];
        INestedSingleRamp* SizeRamp;
        IRampColorRamp* ColorRamp;
        INestedSingleRamp* PrimarySizeRamp;
        INestedSingleRamp* TexFrameRamp;
        INestedSingleRamp* AxisOverrideRamp;
        INestedSingleRamp* FrameRamp;
        INestedSingleRamp* CommonTexFrameRamp;
        IAnimatedTransform* LocalTransform;
    };

    // FxParticleAppearance's set (create the routine, 0xa8 bytes, inner vtable
    // ): two hashed strings assigned "" (the routine with the field at
    // / 0x0622f601) -- ParticleApp_LifeName 0x0EC7A290 (inner +0x5c) and
    // ParticleApp_DeathName 0x03B76E6C (+0x7c) -- and the flags dword +0x9c = 0:
    // ParticleApp_UseDynamicRotation 0xE34C3C55 bit 0, ParticleApp_SmoothRotation
    // bit 1 (the routine / the routine).
    class FFxParticleAppearancePropertySet : public FFxAppearancePropertySet
    {
    public:
        FFxParticleAppearancePropertySet();

        GENRESULT NEURONCOMPONENTS_API GetPropBool(unsigned long Hash, unsigned char* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropBool(unsigned long Hash, unsigned char Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropName(unsigned long Hash, const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropName(unsigned long Hash, const char* Text) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        FFxHashedName LifeName;
        FFxHashedName DeathName;
        unsigned long Flags;
    };

    // FxMeshAppearance's set (inner ctor the routine, create the routine, 0x90 bytes,
    // inner vtable 0x0624dff0): MeshApp_MeshId 0x0B7EE41C int +0x5c = 0, MeshApp_MeshName
    // hashed string +0x60 = "", MeshApp_UseParticleTransform 0x001FC112 byte
    // +0x80 = 0, MeshApp_ParticleTransform 0xF0004D58 +0x84 = a default FxAnimatedTransform.
    class FFxMeshAppearancePropertySet : public FFxAppearancePropertySet
    {
    public:
        FFxMeshAppearancePropertySet();
        ~FFxMeshAppearancePropertySet() override;

        GENRESULT NEURONCOMPONENTS_API GetPropBool(unsigned long Hash, unsigned char* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropBool(unsigned long Hash, unsigned char Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropInt(unsigned long Hash, unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropInt(unsigned long Hash, unsigned long Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropName(unsigned long Hash, const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropName(unsigned long Hash, const char* Text) override;
        GENRESULT NEURONCOMPONENTS_API GetPropTransform(unsigned long Hash, IAnimatedTransform** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

        unsigned long MeshId;
        FFxHashedName MeshName;
        unsigned char bUseParticleTransform;
        IAnimatedTransform* ParticleTransform;
    };

    struct FNodeLibEntry
    {
        unsigned long Hash;
        char Name[256];
        INeuronComponent* Node;
    };

    // / IParticleFx+0x50: parent + child. No node+0x28 walk this wave.
    struct FParticleFxAttach
    {
        INeuronComponent* Parent;
        INeuronComponent* Child;
    };

    // rec8 edge after QI. Kind 1 = emitter→appearance, 2 = appearance→field.
    struct FFxRec8Assoc
    {
        INeuronComponent* Left;
        INeuronComponent* Right;
        unsigned long Kind;
    };

    // Owning effect record: its node reference has observable copy/assignment calls.
    struct FFxEffectNodeRec
    {
        INeuronComponent* Node;
        // baseline list record +4: the archetype rec16's first dword (Rec.A). 0 = node
        // looked up through ParticleFx+0x2C, non-zero = CreateNamedFx("FxNode") group.
        // 0621f52b gates the child-duration pass on Kind == 1.
        unsigned long Kind;
        unsigned long Id;
        unsigned long Parent;

        FFxEffectNodeRec(INeuronComponent* InitialNode, unsigned long InitialKind,
                         unsigned long InitialId, unsigned long InitialParent)
            : Node(InitialNode)
        {
            // the reference is published before AddRef.
            if (Node != nullptr)
                Node->AddRef();
            Kind = InitialKind;
            Id = InitialId;
            Parent = InitialParent;
        }

        FFxEffectNodeRec(const FFxEffectNodeRec& Other) : Node(Other.Node)
        {
            // read the scalar fields after the AddRef callback.
            if (Node != nullptr)
                Node->AddRef();
            Kind = Other.Kind;
            Id = Other.Id;
            Parent = Other.Parent;
        }

        FFxEffectNodeRec& operator=(const FFxEffectNodeRec& Other)
        {
            // has no self-assignment guard and releases before acquiring.
            if (Node != nullptr)
            {
                Node->Release();
                Node = nullptr;
            }
            Node = Other.Node;
            if (Node != nullptr)
                Node->AddRef();
            Kind = Other.Kind;
            Id = Other.Id;
            Parent = Other.Parent;
            return *this;
        }

        ~FFxEffectNodeRec() noexcept(false)
        {
            // clear only after Release returns.
            if (Node != nullptr)
            {
                Node->Release();
                Node = nullptr;
            }
        }
    };

    // No QI: the caller supplies its live FileSystem interface.
    struct FAleChunk
    {
        IFileSystem* Filesys;
        unsigned long Handle;
        unsigned char bEntered_;

        FAleChunk() : Filesys(nullptr), Handle(0xFFFFFFFF), bEntered_(0) {}
        ~FAleChunk() { CloseChunk(); }

        int OpenNode(const char* Name);

        // Enter the named directory and open its same-named data leaf.
        // The caller owns directory navigation separately from chunk closure.
        bool OpenNamedChunk(IFileSystem* Fs, const char* Name, unsigned char bWrite);

        void LeaveNamedNode();

        void DeleteNode(const char* Name);

        bool OpenChunk(IFileSystem* Fs, const char* Name, unsigned char bWrite);

        void CloseChunk();

        int ReadBytes(void* Buffer, unsigned long Size);

        int WriteBytes(const void* Buffer, unsigned long Size);

        // uint16 length + bytes; odd length gets 1 pad byte. RET <0 fail.
        int ReadString(char* Buffer, unsigned long MaxChars);

        // uint16 (strlen+1 or 0) + bytes + odd pad.
        int WriteString(const char* Text);
    };

    // IFxNode returned by QI. The property protocol (baseline IFxNodePropertySet, the set
    // reached through outer +0x80) is not on this interface: see FFxNodePropertySet above
    // and FGenericFxObjectBase's GetProp* / SetProp* (particlefx_genericfx.h).
    struct IGenericFx : INeuronComponent
    {
        // +0x0C RET 8 -- clone this node: every outer vtable thunks it (`ADD [ESP+4],4;
        // JMP`, FxNode 062026b0, cone 0621c190) into the class's clone (FxNode the routine,
        // cone the routine, basic the routine, gravity the routine, ...): operator_new,
        // the class's constructor inline (the same body its factory inlines), the class's
        // copy base (the routine and the per-family the routine / the routine /
        // ), the class's set cache; *Out = the new node, 0. The property set is
        // shared with this node, not created. Caller: the node library's the routine
        // (0x06227769), which returns the result as its own.
        virtual GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) = 0;               // +0x0C
        // +0x10 RET 0xC -- the node-transform compose. Every outer vtable forwards this
        // slot to its node sub-object's slot 2: FxNode the routine (this+4) and the mesh
        // (this+0xc) both JMP with the two stack arguments untouched, into
        // (shared) or a class override (the mesh's the routine). The
        // caller is IParticleFx the branch / the routine, which pushes (A, &the field):
        // A is the walk's own argument and the routine never reads it; Parent12 is the
        // parent's composed transform. Declared as a zero-argument Pad4 until 2026-09-26,
        // so every walk call leaked eight bytes of the caller's frame.
        virtual GENRESULT NEURONCOMPONENTS_API ComposeNodeTransform(void* Unused,
                                                         const float* Parent12) = 0;  // +0x10
        // +0x14 RET 8 -- the per-node update, forwarded to the node sub-object's slot 3.
        // The shared default is the routine: `this->vt[0x44](Dt)` (TickLifetime), return
        // 0 -- FxNode's own outer thunk the routine lands there. Classes with their own
        // update override it (the mesh the routine, FLBeamAppearance the routine,
        // the emitters the branch cone / the branch cube / the branch sphere,
        // each of which ends in the shared emitter body the routine).
        // The "CopyTransformToLinkedChildren" body this slot carried until 2026-09-26 had
        // no baseline counterpart. Caller: IParticleFx the branch / the routine, reached
        // every frame from IFxEffect slot 8 (the routine).
        virtual GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) = 0;                             // +0x14
        // +0x18 RET 4 -- the per-node draw (IParticleFx the branch). Shared default
        // `XOR EAX,EAX; RET 4` (FxNode's thunk the routine).
        // ALC-1 (2026-09-26): every appearance's draw sits HERE, on the outer vtable's
        // +0x18 adjustor thunk (this+0xc -> node vt+0x10): basic the routine @0624cc58
        // -> the routine, rect the branch @0624f020 -> the routine, oriented
        // @0624e2f0 -> the routine, beam the routine @0624b7e8 ->
        // , perp the routine @0624e9e8 -> the routine, dust the routine
        // @0624c1d0 -> the routine; the particle appearance's is the routine (return 0).
        // Was Pad6 (a no-op) with DrawOrEnqueue declared at +0x78 until 2026-09-26, so
        // the draw walk never reached any appearance's draw.
        virtual GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() = 0;                            // +0x18
        // +0x1C RET 4 -- `MOV EAX,[node+4]; SHR EAX,0x1f`: the sign bit of the node flags,
        // i.e. "this node is dead". Identical on every outer vtable (FxNode the branch,
        // cone emitter the branch, basic appearance the branch, field the branch and
        // the other appearances). Callers test AL: the effect walks skip a dead node, the
        // emitter body the routine (0x06245c2f) drops a dead appearance. Was the
        // always-zero Pad7 until 2026-09-26.
        virtual GENRESULT NEURONCOMPONENTS_API IsNodeDead() = 0;                                // +0x1C
        // +0x20 RET 8 -- `*Out = [node+4]; return 0` (FxNode the branch, cone
        // , basic appearance the routine, field the branch). IParticleFx's
        // update walk (the routine 0x06201839) reads bit 0 (lifetime expired) through it.
        // This slot was declared as a two-argument SetPropBool(Hash, Value) until
        // 2026-09-26; the walk called it with one argument, so every call popped a dword
        // the caller never pushed. SetPropBool is port-only and lives in the tail now.
        virtual GENRESULT NEURONCOMPONENTS_API GetNodeFlags(unsigned long* Out) = 0;            // +0x20
        // +0x24 RET 8 -- `[node+4] = Flags; return 0` (FxNode the branch, basic
        // appearance the routine, field the branch). The emitters override it with
        // , which first tells a bound appearance to drop its emitter count
        // (appearance +0xc8) when the new flags are negative. Was Pad9 until 2026-09-26.
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeFlags(unsigned long Flags) = 0;           // +0x24
        virtual INeuronComponent* NEURONCOMPONENTS_API GetAttachParent() = 0;                       // +0x28 the routine RET 4
        virtual INeuronComponent* NEURONCOMPONENTS_API GetFirstChild() = 0;                         // +0x2C the routine RET 4
        virtual INeuronComponent* NEURONCOMPONENTS_API GetPrevSibling() = 0;                        // +0x30 the routine RET 4
        virtual INeuronComponent* NEURONCOMPONENTS_API GetNextSibling() = 0;                        // +0x34 the routine RET 4
        virtual int NEURONCOMPONENTS_API GetSortKey() = 0;                                      // +0x38 the routine RET 4
        // +0x3C / +0x40 -- the node-transform cache accessors. The outer slots are this+N
        // adjustor thunks (mesh the routine / the routine, both this+0xc) into the
        // shared node-base bodies the routine (set) and the routine (get):
        //   Index 0 = the composed world transform (node+0x4c; set ORs flag 0x8, which
        //             makes the next compose keep it instead of recomputing),
        //   Index 1 = the raw local transform (node+0x1c; set ORs flag 0x4),
        //   any other Index -> 0xfffffffe, nothing written.
        // The setter copies 12 floats in; the getter hands out the slot's address.
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeTransform(unsigned long Index,
                                                     const float* Source12) = 0;    // +0x3C RET 0xC
        virtual GENRESULT NEURONCOMPONENTS_API GetNodeTransform(unsigned long Index,
                                                     float** OutSlot) = 0;          // +0x40 RET 0xC
        // wave89 (sonnet:particlefx-wave89-lane1, 2026-09-15): CORRECTED from the
        // zero-arg `Pad17()` every prior wave stubbed here. Real baseline body
        // is the branch (particlefx, RET 4 -- one float
        // stack arg beyond the implicit `this`): `age(this+0x84) += Dt;
        // if (age > duration(this+0x88)) this->NodeFlags_ |= 1;` (strictly
        // greater: FCOMP age vs duration, AND EAX,0x4100 (C3|C0) / JNZ skips
        // on less-or-equal, fall-through sets the bit; corrected
        // 2026-09-26 from `>=`) -- a
        // generic per-object elapsed-time tick shared verbatim (byte-
        // identical single function address) across all 21 distinct
        // FGenericFxObjectBase-leaf outer vtables this wave could find
        // installing a slot at this exact +0x44 position (cross-checked
        // against FxBasicAppearance's own ctor, particlefx,
        // which installs the routine as its real instance vtable and
        // has the branch at 0624cb20+0x44=0624cb64, one of the 21) --
        // confirming this is FGenericFxObjectBase's own shared Pad17 body,
        // not a per-leaf override. `this+0x4` bit0 here is the SAME dword
        // wave71 independently identified as NodeFlags_ (see that field's
        // own doc comment, particlefx_genericfx.h) via a completely separate
        // trace (Pad25's zero-init) -- two independent byte traces landing
        // on the same field is strong confirmation, not a guess. See
        // FGenericFxObjectBase::TickLifetime (particlefx_genericfx.cpp) for the
        // real implementation this wave wired in, and Age_/Duration_
        // (particlefx_genericfx.h) for the new backing fields.
        // 2026-09-26 correction: the branch is slot 17 (+0x44) of the NODE sub-object's
        // vtable (e.g. 0624d3c8+0x44 for the cone emitter), which the port folds into this
        // one interface. The OUTER vtable's +0x44 is a this+N thunk into the routine
        // (FxNode the branch, cone the branch, basic appearance the routine) --
        // FGenericFxObjectBase::ComputeRelativeTransform. Nothing outside a node calls
        // this slot through the interface.
        virtual GENRESULT NEURONCOMPONENTS_API TickLifetime(float Dt) = 0;                      // +0x44 the branch RET 4 (this in ECX)
        virtual GENRESULT NEURONCOMPONENTS_API SetParent(INeuronComponent* Parent) = 0;             // +0x48 the routine RET 8
        virtual GENRESULT NEURONCOMPONENTS_API SetFirstChild(INeuronComponent* Child) = 0;          // +0x4C the routine RET 8
        virtual GENRESULT NEURONCOMPONENTS_API SetPrevSibling(INeuronComponent* Prev) = 0;          // +0x50 the routine RET 8
        virtual GENRESULT NEURONCOMPONENTS_API SetNextSibling(INeuronComponent* Next) = 0;          // +0x54 the routine RET 8
        // +0x58..+0x6C are three float get/set pairs on the node block, identical on every
        // outer vtable (node at outer+4 for FxNode/emitters/fields, outer+0xc for the
        // appearances): +0x58/+0x5C node+0x8c (the ramp "outer" time every property
        // evaluation passes first), +0x60/+0x64 node+0x84 (Age), +0x68/+0x6C node+0x88
        // (Duration). Getters are `*Out = [node+N]; return 0` RET 8, setters
        // `FLD [ESP+8]; FSTP [node+N]; return 0` RET 8 (FxNode the branch..the branch,
        // cone emitter the branch..the branch, basic appearance the branch..
        // , field the branch..the branch). The setters take the float
        // by value; FFxEffectObject's walks (slot 6 -> +0x5C, slot 5 -> +0x64,
        // slot 14 -> +0x88) forward that float to every stored node.
        // Named Pad22/Slot23/Pad24/Pad25 until 2026-09-26; Pad25 carried an invented
        // emitter-tick + appearance-integrate body.
        virtual GENRESULT NEURONCOMPONENTS_API GetNodeOuterTime(float* Out) = 0;                // +0x58
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeOuterTime(float OuterTime) = 0;          // +0x5C
        virtual GENRESULT NEURONCOMPONENTS_API GetNodeAge(float* Out) = 0;                      // +0x60
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeAge(float Age) = 0;                      // +0x64
        // +0x68 -- GetSubtreeDuration. This slot carried a three-argument `EmitOne`
        // until 2026-09-16 and that was wrong in the dangerous direction: the declared
        // callee popped sixteen bytes where the caller had pushed eight, so a caller's
        // frame was destroyed, not merely leaked. Both baseline call sites (0x0621ee6b and
        // ) push exactly one argument plus `this`, and each one FLDs that
        // argument's target as a float immediately afterwards. The port's own
        // FFxEffectObject::ComputeMaxSubtreeDuration reads it the same way.
        
        // EmitOne is still real -- it is just not this slot. It is declared at the END of
        // this interface now (see below), because it IS dispatched polymorphically:
        // EmitAtNodeTransform calls it on a QI'd appearance, not on itself.
        // TODO: find its actual slot. Appending it is safe only because no baseline DLL is
        // loaded any more (implementation notes), so slot order is a verification
        // convention here rather than an ABI constraint.
        // 2026-09-26: the body is `*Out = [node+0x88]` (Duration), see the +0x58 block.
        virtual GENRESULT NEURONCOMPONENTS_API GetSubtreeDuration(float* OutSeconds) = 0;
        // +0x6C RET 8 -- a float, not a pointer. Both baseline callers push one:
        // pushes the literal 0x7f7fffff (FLT_MAX, the same constant this port
        // already spells kInitNodeBoundBits) and 0x0621f5c1 pushes the accumulator the
        // GetSubtreeDuration loop above just built. Same width, so nothing was corrupted;
        // the port simply carried a float through a void* and cast it back at the far end.
        // 2026-09-26: the body stores it into node+0x88 (Duration) -- renamed from
        // SetNodeBound, which was a stub.
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeDuration(float Value) = 0;
        // +0x70, __thiscall, no arguments: the factory name, [[node+0x80]+0x14] -- appearance
        // outers the branch (beam) / 0x06218dd0 / 0x06226eb0 / ... read [this+0x8c]
        // (node at +0xc), the field outer the branch reads [this+0x84]. The beam fill
        // compares it against "FLBeamField" (0x062057ac). Was Pad28 plus a
        // port-only copy appended at the end of this interface until 2026-09-26.
        virtual const char* NEURONCOMPONENTS_API GetFxFactoryName() = 0;
        // +0x74 / +0x78 / +0x7C, RET 8 each -- the node's instance name. Every outer forwards
        // them to its "1.11_IFxNodePropertySet" object ([outer+0x88] on the appearances,
        // [outer+0x80] on FxNode) slots +0x14 / +0x10 / +0xC: dust the branch/490/4b0,
        // particle the branch/030/050, FxNode the branch/the branch/the routine.
        // Bodies (particle's set): the routine `*Out = [ps+0x30]` (the cached CRC),
        // `*Out = [ps+0x3c]` (the buffer), the routine copy + recompute the
        // CRC. Readers: FindNode the routine (+0x74) and the node-by-name search
        // (0x06227507, +0x78). Until 2026-09-26 these were Pad29 / Pad30 /
        // Pad31 with the port's copies appended at the end of this interface (FindNode
        // pushed an out-parameter into the zero-argument Pad29), and DrawOrEnqueue sat
        // at +0x78: wave79 read the branch -- FxPerpAppearance's 0xc-byte factory
        // vtable -- as the object vtable; 0x0624e8c0 is the next table's +0x10, the node
        // draw, reached from outer +0x18.
        virtual GENRESULT NEURONCOMPONENTS_API GetInstanceNameHash(unsigned long* Out) = 0;     // +0x74
        virtual GENRESULT NEURONCOMPONENTS_API GetInstanceName(const char** Out) = 0;           // +0x78
        virtual GENRESULT NEURONCOMPONENTS_API SetInstanceName(const char* Name) = 0;           // +0x7C
        // +0x80 RET 8 -- the node's property set: *Out = node+0x7c (the set's outer),
        // AddRef (+4), 0. The same body in all fifteen outer vtables (FxNode 06202880, cone
        // , cube 0621db40, sphere 0623c560, the appearances 0620e0e0 / 06212440 /
        // / 062270a0 / 0622ebd0 / 06231fe0 / 06235900 / 0623ae50 reading
        // [outer+0x88], the fields 0620ebe0 / 062139c0 / 06236c50). Callers: the .ale
        // reader 0x06228d1b and writer 0x06228280. Was Pad32 (returning 0) until ALC-7.
        virtual GENRESULT NEURONCOMPONENTS_API GetPropertySet(IFxNodePropertySet** Out) = 0;  // +0x80
        // +0x84 / +0x88 -- get/set of node+0x90, a float the node base init the routine
        // seeds to 1.0 (0x0623fc02). Same shape as the +0x58 block: FxNode the branch /
        // , cone emitter the branch / the branch, basic appearance
        // / the routine, field the branch / the branch. Its only
        // reader found so far is the emitter body the routine (0x06245dd4), which
        // evaluates Emitter_LODCurve at (node+0x8c, node+0x90) -- hence the name.
        // Setter reached from IFxEffect slot 14 (the branch, 0x0621f867). Were
        // Pad33/Slot34 until 2026-09-26.
        virtual GENRESULT NEURONCOMPONENTS_API GetNodeLodInput(float* Out) = 0;                 // +0x84
        virtual GENRESULT NEURONCOMPONENTS_API SetNodeLodInput(float LodInput) = 0;            // +0x88
        // +0x8C -- one out-parameter, not zero arguments. particlefx 0x0621f8a7 and
        // both push a buffer address plus `this`, and 0x0621f8d7 reads the
        // result back with FLD. Four bytes of the caller's stack were leaking per call.
        // TODO: the port's FFxEffectObject reads four floats out of it (x, y, z, radius),
        // which is consistent but not proven -- baseline only reads the first.
        virtual GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) = 0;
        // +0x90 / +0x94 / +0x98 -- the three "does this node have flag N?" queries, each
        // taking ONE out-parameter, not zero arguments. 0x0621fb0d, 0x0621fac4 and
        // each seed a byte local, LEA its address, push it and push `this`, and
        // read the byte back with `MOV AL,[ESP+0x18]` right after the call. The port's own
        // FFxEffectObject::CallNodeFlagSlot passed an `unsigned char*` all along.
        // 2026-09-26: the bodies are `*Out = byte [node+0x94 / +0x95 / +0x96]` (FxNode
        // /980/9a0, cone the branch/460/480, basic appearance
        // /350/370, field the branch/ce0/d00). The node base init
        // zeroes all three; the family constructors set exactly one:
        // +0x94 = 1 in the shared appearance init (0x0624439f), +0x95 = 1 in the three
        // emitter constructors (0x0621b93a, 0x0621d2ca, 0x0623bafa), +0x96 = 1 in the
        // seven field constructors (0x0620e566 ...). So these are "is an appearance /
        // emitter / field" queries; the effect's Slot11/Slot13 flag bits 4/1/2 select them.
        virtual GENRESULT NEURONCOMPONENTS_API GetFlagBit4(unsigned char* Out) = 0;              // +0x90
        virtual GENRESULT NEURONCOMPONENTS_API GetFlagBit1(unsigned char* Out) = 0;              // +0x94
        virtual GENRESULT NEURONCOMPONENTS_API GetFlagBit2(unsigned char* Out) = 0;              // +0x98
        virtual GENRESULT NEURONCOMPONENTS_API AssociateAppearance(INeuronComponent* Appearance) = 0; // +0x9C the routine RET 8
        virtual GENRESULT NEURONCOMPONENTS_API Pad40() = 0;
        // +0xA4 on an appearance: fill this appearance's particles into the draw context
        // the batch flush the routine has already begun (0x06201d2a calls it for every
        // appearance in a batch list, between the head's +0x9C begin and +0xA0 draw).
        // FxBasicAppearance: the branch -> the branch = ++G_DrawCounters[3], then
        // or the routine by descriptor flag 2. Other kinds keep the no-op
        // default until their own fill thunks are ported.
        virtual GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() = 0;
        virtual GENRESULT NEURONCOMPONENTS_API Pad42() = 0;
        virtual GENRESULT NEURONCOMPONENTS_API AssociateField(INeuronComponent* Field) = 0;         // +0xAC the routine RET 8
        virtual GENRESULT NEURONCOMPONENTS_API Pad44(INeuronComponent* Value) = 0;                // +0xB0 the routine RET 8
        // +0xB4 RET 4 -- on every appearance a this+0xc thunk into the routine: for each
        // field in the appearance's list (node+0x9c..+0xa0) call field+0x9c(field,
        // node+0x18). Bracketed with +0xC0 (the routine) around the emitter's spawn loop
        // (the routine 0x06246035 / 0x06246091). Not ported yet on the appearance side.
        virtual GENRESULT NEURONCOMPONENTS_API Pad45() = 0;                                     // +0xB4
        // 2026-09-26: on every appearance +0xB8 is a this+0xc thunk into the routine
        // (basic the branch), not a pre-check of Pad47's; the emitter samplers call it
        // before +0xBC and skip +0xBC when it returns negative.
        // wave65 (sonnet:particlefx-wave65-lane1, 2026-09-11): CORRECTED from the
        // zero-arg `Pad46()` every prior wave (through wave64) stubbed here.
        // wave64 independently re-verified, at all 3 emitter-shape "combine
        // step" tail call sites (the branch/Cone item 133141, the branch/
        // Cube item 133262, the branch/Sphere item 134817 -- byte-identical
        // shape at all 3), that `CALL [vtbl+0xb8]` pushes 3 explicit stack
        // dwords beyond the trailing `this` (RET 0x10, matching Pad47's own
        // arity below) but did not resolve their types/meaning (out of that
        // wave's file-ownership scope). This wave re-read the raw bytes at
        // Sphere's site (particlefx, 0x0623bf34-0x0623bf52)
        // and Cone's twin (0x0621bc70-0x0621bc7e) directly to finish that:
        
        //   MOV ECX,[ESI+0xb4]         ; ESI = emitter this
        //   MOV EAX,[ESI+0x98]         ; EAX = AssociatedAppearance_ (confirmed
        //                              ;   by items 133141/134817/133262's own
        //                              ;   wave37 trace -- the vtbl this CALL
        //                              ;   dispatches through belongs to the
        //                              ;   ASSOCIATED APPEARANCE object, not
        //                              ;   the emitter itself)
        //   PUSH ECX                   ; arg (rightmost/3rd explicit param)
        //   LEA  ECX,[ESP+0x20]        ; &local -- verified this address is
        //                              ;   EXACTLY the just-computed
        //                              ;   TransformXform Dst (the combine
        //                              ;   step's finished world-space
        //                              ;   position), and that the 3 dwords
        //                              ;   immediately following it in the
        //                              ;   same local are the parallel
        //                              ;   world-space direction/velocity
        //                              ;   triple built one block earlier
        //                              ;   (particlefx) --
        //                              ;   i.e. &local is a pointer to 6
        //                              ;   CONTIGUOUS floats (Position[3],
        //                              ;   Velocity[3]), the exact same
        //                              ;   layout Pad47/the routine's own
        //                              ;   "Six" parameter below already uses.
        //   MOV  EDX,[EAX]             ; vtable of AssociatedAppearance_
        //   PUSH EDI                   ; arg (2nd explicit param) -- EDI was
        //                              ;   loaded (0623bf2c, outside this
        //                              ;   excerpt) via a plain MOV from a
        //                              ;   stack slot last written by FSTP
        //                              ;   (particlefx) -- a raw
        //                              ;   FLOAT bit-pattern transfer, NOT an
        //                              ;   address computation (no LEA) --
        //                              ;   confirming this arg is a float
        //                              ;   value, not a pointer (correcting
        //                              ;   wave64's tentative "FinalPos ptr"
        //                              ;   label for this same register).
        //   PUSH ECX                   ; &local (1st explicit param)
        //   PUSH EAX                   ; this = AssociatedAppearance_
        //   CALL [EDX+0xb8]            ; Pad46
        
        // Right-to-left stdcall push order (this pushed LAST = 1st formal
        // param, matching every other NEURONCOMPONENTS_API slot in this interface) gives
        // formal order (this, &local, EDI-float, ESI+0xb4-float) -- i.e.
        // (this, Six-ptr, arg2-float, arg3-float). Immediately after this
        // call (TEST EAX,EAX; JL skip), baseline conditionally makes a SECOND
        // call through the SAME AssociatedAppearance_ pointer at
        // `[vtbl+0xbc]` -- Pad47 below -- pushing the IDENTICAL 3 registers
        // in the IDENTICAL order (particlefx). Pad47's
        // arity/signature is already independently confirmed (see its own
        // comment) as `Pad47(const float* Six, float Age, float Duration)`,
        // dispatched to the 4 Cluster-A appearance leaves'
        // SpawnPrivateInstance (particlefx_appearances.cpp) -- the real
        // "commit a new particle" entry point. Given Pad46 is called through
        // the exact same pointer with the exact same 3 register values
        // immediately before Pad47, and gates whether Pad47 runs at all
        // (negative result -> skip the commit), the best-supported hypothesis
        // is that Pad46 shares Pad47's exact signature and is a "can this
        // particle be committed" pre-check/reservation (e.g. a private-pool
        // capacity probe) -- named identically here for that reason. This is
        // inferred from call-site register parity, not from independently
        // reading Pad46's own body on the appearance side (which lives on
        // FxBasicAppearance/FxRectAppearance/FxPerpAppearance/
        // FxOrientedAppearance et al, all outside this file's ownership) --
        // flag this as the one open point for whichever future wave
        // implements a real (non-GR_OK-stub) override.
        virtual GENRESULT NEURONCOMPONENTS_API Pad46(const float* Six, float Age,
                                          float Duration) = 0;                       // +0xB8 RET 0x10
        // , confirmed via byte trace (particlefx-wave18-lane1,
        // 2026-09-10): RET 0x10 (4 stack dwords: this+3), not 2 args as
        // wave17 assumed. Six = 6 floats (pos+vel), copied verbatim into the
        // spawned item. The 3rd/4th args are NOT (Duration) alone — tracing
        // 's field writes against this port's own FPooledParticle
        // layout (Age@+0x24/Duration@+0x28/InvDuration@+0x2c) shows the 3rd
        // arg lands at item+0x24 (Age) and the 4th is what the routine's
        // FDIV actually divides 1.0 by and stores raw at item+0x28
        // (Duration) — i.e. baseline lets the caller seed a non-zero initial
        // Age, not just Duration. See particlefx_appearances.h/.cpp for the
        // wiring this corrects.
        virtual GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age,
                                          float Duration) = 0;                       // +0xBC the routine RET 0x10
        // +0xC0 RET 4 -- appearance this+0xc thunk into the routine, the closing half of
        // the +0xB4 field pass. Not ported yet on the appearance side.
        virtual GENRESULT NEURONCOMPONENTS_API Pad48() = 0;                                     // +0xC0
        // +0xC4 / +0xC8 RET 4 -- `INC` / `DEC dword [outer+0xbc]` (node+0xb0), return 0,
        // identical on all eight appearance vtables (basic the branch / the branch,
        // beam the branch / the branch, rect the branch / the branch ...). The
        // emitter increments it when it binds the appearance (the routine 0x06245b9c)
        // and decrements it when the emitter dies (the routine 0x06245bcb); the basic
        // appearance update (the routine 0x06216447) only lets the appearance expire once
        // this count and its particle list are both empty. Were Pad49/Pad50 until
        // 2026-09-26.
        virtual GENRESULT NEURONCOMPONENTS_API AddEmitterRef() = 0;                             // +0xC4
        virtual GENRESULT NEURONCOMPONENTS_API ReleaseEmitterRef() = 0;                         // +0xC8
        virtual GENRESULT NEURONCOMPONENTS_API GetLiveInstanceCount(unsigned long* Out) = 0;    // +0xCC the branch
        virtual GENRESULT NEURONCOMPONENTS_API GetQueuedVertCount(unsigned long* Out) = 0;      // +0xD0 the branch
        virtual GENRESULT NEURONCOMPONENTS_API GetQueuedIndexCount(unsigned long* Out) = 0;     // +0xD4 the branch
        // IFxAppearance +0xD8 / +0xDC (node +0x84 / +0x88): count and indexed
        // field getter. the routine takes Index and Out; success writes the field
        // and AddRefs it, an invalid index reports -1 without changing Out.
        virtual GENRESULT NEURONCOMPONENTS_API GetAssociatedFieldCount(unsigned long* Out) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API GetAssociatedFieldAt(unsigned long Index,
                                                        INeuronComponent** Out) = 0;
        // Port-only: after +0xDC. Kind 2/3/0x103/0x104 setters are IFxNodePropertySet,
        // not IFxNode +0x30/+0x38/+0x4C (those are prev / sort key / first-child).
        virtual GENRESULT NEURONCOMPONENTS_API SetLiveInstanceCount(unsigned long Count) = 0;
        // Port-only: IFxField +0x9C Begin (the branch / the branch / the branch).
        // Appearance is the appearance whose particles this field is about to act on
        // (baseline Begin's one stack argument, RET 8). The Begin bodies that need a
        // transform take this field's transform relative to it through node slot 7,
        // (Gravity the branch 0x06224219, Air 0x06214c69, Radial
        // , Collide 0x0621ab3d) -- FGenericFxObjectBase::ComputeRelativeTransform.
        // Dt is port-only (no baseline Begin reads a second argument).
        virtual void NEURONCOMPONENTS_API BeginField(INeuronComponent* Appearance, float Dt) = 0;
        // Port-only: the routine +0xa4(field, pos+0xC, age+0x24).
        virtual void NEURONCOMPONENTS_API ApplyFieldToVel(float* PosVel6, float* Age, float Dt) = 0;
        virtual INeuronComponent* NEURONCOMPONENTS_API GetAssociatedField() = 0;
        // Port-only: the billboard property set the draws read through node+0xf0 (dust
        // +0xe8) -- the node's shared FAppearanceRampDescriptor. Only the six billboard
        // appearances have one; every other node returns nullptr. No reference is added.
        virtual const FAppearanceRampDescriptor* NEURONCOMPONENTS_API GetAppearanceRampDescriptor() = 0;

        // Port-only (wave22, sonnet:particlefx-wave22-lane1): accessor for the
        // appearance-level context float baseline calls with a value read from
        // [this+0x8c] every time it evaluates SizeRamp/PrimarySizeRamp/
        // EffectScaleRamp/AxisOverrideRamp via vtable+0x1c (the
        // "OuterTime" argument of INestedSingleRamp::Eval -- see
        // FAppearanceRampDescriptor's own comment). Confirmed as
        // a genuine per-instance data READ, not a phantom analysis tool
        // artifact: the raw call sites (particlefx for
        // FxOrientedAppearance, and the matching Rect/Basic/Dust sites the
        // wave18/20 notes on implementation tracker items 133923/134625/132830/
        // 132831/132378 already trace byte-for-byte) show a plain
        // `MOV reg, [this+0x8c]` through a real register holding the
        // appearance this-pointer, consistently re-read unchanged across
        // every ramp call within one Draw of that appearance.
        
        // wave22 (sonnet:particlefx-wave22-lane1) searched for the matching
        // WRITE exhaustively -- every literal `+ 0x8c) =` assignment in
        // the whole particlefx inspect (10 matches) and every raw
        // `[reg + 0x8c],` store-destination instruction in the whole
        // particlefx listing (34 matches) -- and cross-checked each
        // one against the address ranges of the routine/the routine/
        // /the routine/the routine plus their shared base
        // constructor (the routine/the routine). NONE belong to any of
        // those functions or write an appearance-shaped object at this
        // field: the matches are either a same-offset AddRef/Release
        // refcount pair on a completely different, unrelated COM class
        // (the routine/the routine and two siblings), an unrelated
        // property-name-buffer flag (the routine), or plain ESP-relative
        // SEH/stack-frame markers the analysis tool renders with the same
        // literal "0x8c" text by coincidence (e.g. inside the routine
        // itself at particlefx, `MOV [ESP+0x8c],0xffffffff` --
        // an exception-handler sentinel, not an object write). No producer
        // for this field exists anywhere in this module's reachable code.
        
        // Given that (ALC-2 correction: the branch has no cached index; it
        // returns key 0's child for OuterTime <= key 0's time or a single key,
        // see FRampSingleRamp in particlefx_ramp.h), a field that is simply
        // never written behaves identically, for every currently
        // observable code path, to one permanently pinned at 0.0f. This
        // port therefore backs this accessor with a real field defaulting
        // to 0.0f (FGenericFxObjectBase::AppearanceOuterTime_,
        // particlefx_genericfx.h/.cpp) instead of leaving OuterTime
        // unreachable, and exposes a setter alongside the getter so a
        // future wave that DOES locate a real producer (most likely
        // outside this module, e.g. the host EXE's effect-instance driver
        // poking this value before calling DrawNodes -- particlefx.dll's own
        // FParticleFx::DrawNodes plumbing was checked and does not set it
        // either) can wire it up without another interface change.
        // Appended at the end of this already-established port-only
        // extension zone (same rationale as GetAppearanceRampDescriptor
        // above), so this is safe to add without disturbing any real ABI
        // offset.
        virtual float NEURONCOMPONENTS_API GetAppearanceOuterTime() = 0;
        virtual GENRESULT NEURONCOMPONENTS_API SetAppearanceOuterTime(float Time) = 0;

        // DrawOrEnqueue is the +0x18 slot (see there); an appended copy that used to
        // live here was removed by wave79.

        // wave117 (sonnet:wave117:globals-final, 2026-09-15): baseline
        // FLDustAppearance's real [this+0x12C] "previous frame's camera
        // distance" field (the routine, particlefx/11706 -- read
        // once near the top of the function to compute the frame-to-frame
        // distance-CHANGE ("pop") fade against kDustPopChangeNear/Far
        // (the field/a08, implementation tracker items 136705/136706), then
        // overwritten with this frame's own distance at the very end of the
        // function so the NEXT frame's call sees it). Same per-appearance-
        // INSTANCE-state shape as AppearanceOuterTime_ above (a field this
        // port's original architecture note in particlefx_appearances.h claimed
        // "none [of the 8 leaf types] needs any extra member fields" got
        // wrong for exactly one case), so it is exposed the same way --
        // appended here rather than added as a concrete FLDustAppearance
        // member -- so particlefx_common.cpp's ComputeDustCameraFadeAlpha can
        // reach it through the existing QueryGenericFx(Appearance) accessor
        // path already used for GetAppearanceOuterTime/GetAppearanceRampDescriptor,
        // with no downcast to a concrete leaf type. Only FLDustAppearance's
        // own DrawOrEnqueue path (via ComputeDustCameraFadeAlpha) ever calls
        // this pair; every other appearance kind's Get returns the shared
        // base default (0.0f, see FGenericFxObjectBase's ctor) and is never
        // read.
        virtual float NEURONCOMPONENTS_API GetDustPrevCameraDistance() = 0;
        virtual GENRESULT NEURONCOMPONENTS_API SetDustPrevCameraDistance(float Distance) = 0;

        // Port-only position. baseline has this method -- the routine / the routine /
        // are its three bodies -- but not at +0x68, where it used to be
        // declared. It is parked at the end of the table until its real slot is read.
        virtual GENRESULT NEURONCOMPONENTS_API EmitOne(const float* Six, unsigned long Extra,
                                            float Duration) = 0;

        // baseline +0x9C / +0xA0 on the four batched appearances' outer vtables (basic
        // / the routine, rect, perp the routine / the routine, beam
        // / the branch). The batch flush the routine calls them on the head
        // of each list: begin (0x06201d21) with the list's summed vertex and index counts,
        // then +0xA4 on every member, then end-and-draw (0x06201d3b). The same two offsets
        // mean AssociateAppearance / BeginField on emitters and fields, which this shared
        // interface already declares there, so the appearance meaning is parked here. Slot
        // order is a verification convention only (no baseline DLL is loaded).
        // BeginBatchedDraw is the routine(ctx, 4, Verts, Indices) folded to 0 / -1 by
        // `NEG AL; SBB; NEG; DEC` in all four bodies. EndBatchedDraw differs per kind.
        virtual GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long Verts,
                                                     unsigned long Indices) = 0;
        virtual GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() = 0;
    };

    // ---- Keyed ramps (ALC-2, 2026-09-26) -------------------------------------------------
    // baseline has five keyed-ramp classes, each with its own vtable (RTTI COL at [-4]) and
    // its own key type. Every one embeds the same key container at +8: {vptr, Data,
    // ?, LastIndex (count - 1, -1 when empty), Capacity}, holding 8-byte {float Time;
    // T Value} keys sorted by Time; AddKey inserts after keys of equal time.
    //   "FxRampSingle"         0x0624bbf0  IRampSingle          T = float
    //   "FxRampColor"          0x0624bc40  IRampColor           T = 0x00RRGGBB
    //   "FxRampSingleRamp"     0x0624bb18  IRampSingleRamp      T = IRampSingle*
    //   "FxRampColorRamp"      0x0624bb68  IRampColorRamp       T = IRampColor*
    //   "FxAnimatedSingleRamp" 0x0624bac8  IAnimatedSingleRamp  T = IFxAnimatedSingle*
    // Slot order is baseline's (a verification convention: no baseline DLL is loaded). The
    // float evaluators return x87 ST0 unrounded (the process runs at 53-bit precision
    // control) and callers keep computing on it, so those slots return double. baseline
    // declares a few of these slots __thiscall (Initialize on the flat ramps, +0x3C/+0x44
    // on the nested ones); caller and callee are both ours, so every slot is NEURONCOMPONENTS_API.
    // The concrete classes are in particlefx_ramp.h.

    // FxRampSingle, vtable 0x0624bbf0, object 0x2C: +4 Mode, +8 keys, +0x1C InvSpan,
    // +0x20 cached segment, +0x24 interpolator, +0x28 RefCount.
    struct IRampSingle : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                      // +0x0C 06208c50
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(IRampSingle* Other) = 0;                 // +0x10 06242810
        virtual int NEURONCOMPONENTS_API AddKey(float Time, const float* Value) = 0;             // +0x14 06208c90
        virtual GENRESULT NEURONCOMPONENTS_API RemoveKey(int Index) = 0;                         // +0x18 06208d80
        virtual double NEURONCOMPONENTS_API Eval(float Time) = 0;                                // +0x1C 06242b10 (ST0)
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyTime(int Index, float* Out) = 0;            // +0x20 06208e00
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyValue(int Index, float* Out) = 0;           // +0x24 06208e20
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyTime(int Index, float Time,
                                               int* OutNewIndex) = 0;                 // +0x28 06208e40
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyValue(int Index, const float* Value) = 0;   // +0x2C 062090f0
        virtual unsigned long NEURONCOMPONENTS_API GetMode() = 0;                                // +0x30 06209110
        virtual GENRESULT NEURONCOMPONENTS_API SetMode(unsigned long Mode) = 0;                  // +0x34 06242880
        virtual int NEURONCOMPONENTS_API GetKeyCount() = 0;                                      // +0x38 06209120
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyFactoryName(const char** Out) = 0;          // +0x3C 06209130
        virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                    // +0x40 06209210
    };

    // FxRampColor, vtable 0x0624bc40: the FxRampSingle layout with a packed 0x00RRGGBB
    // value (bytes: B at +0, G at +1, R at +2, the top byte 0).
    struct IRampColor : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                      // +0x0C 062092d0
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(IRampColor* Other) = 0;                  // +0x10 06241ae0
        virtual int NEURONCOMPONENTS_API AddKey(float Time, const float* Rgb) = 0;               // +0x14 062098e0
        virtual GENRESULT NEURONCOMPONENTS_API RemoveKey(int Index) = 0;                         // +0x18 06209400
        virtual float* NEURONCOMPONENTS_API EvalRgb(float* Out, float Time) = 0;                 // +0x1C 06242410
        virtual unsigned long NEURONCOMPONENTS_API EvalPacked(float Time) = 0;                   // +0x20 062424b0
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyTime(int Index, float* Out) = 0;            // +0x24 06209480
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyRgb(int Index, float* Out) = 0;             // +0x28 062099f0
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyTime(int Index, float Time,
                                               int* OutNewIndex) = 0;                 // +0x2C 062094c0
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyRgb(int Index, const float* Rgb) = 0;       // +0x30 06209a70
        virtual unsigned long NEURONCOMPONENTS_API GetMode() = 0;                                // +0x34 06209790
        virtual GENRESULT NEURONCOMPONENTS_API SetMode(unsigned long Mode) = 0;                  // +0x38 06241b50
        virtual int NEURONCOMPONENTS_API GetKeyCount() = 0;                                      // +0x3C 062097a0
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyFactoryName(const char** Out) = 0;          // +0x40 062097b0
        virtual int NEURONCOMPONENTS_API AddKeyPacked(float Time, const unsigned long* Packed) = 0; // +0x44 06209310
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyPacked(int Index, unsigned long* Out) = 0;  // +0x48 062094a0
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyPacked(int Index,
                                                 const unsigned long* Packed) = 0;    // +0x4C 06209770
        virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                    // +0x50 06209890
    };

    // Port-only name for the slot shape FxRampSingleRamp (0x0624bb18) and
    // FxAnimatedSingleRamp (0x0624bac8) share: object 0x20, +4 Mode, +8 keys of
    // {float Time; child*}, +0x1C RefCount. The two differ only in the child class and in
    // the child slot Eval calls (IRampSingle::Eval at child +0x1C, IFxAnimatedSingle::Eval
    // at child +0x28). The appearance descriptors hold either kind in the same field
    // (the routine builds +0x7C with the routine, the routine builds +0x88 with it),
    // and the draw fills only call +0x1C on them, so the field type is this base. The
    // child travels as INeuronComponent*; each class casts it to its own child type.
    struct INestedSingleRamp : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                      // +0x0C
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(INestedSingleRamp* Other) = 0;           // +0x10
        virtual int NEURONCOMPONENTS_API AddKey(float Time, INeuronComponent* Child) = 0;            // +0x14
        virtual GENRESULT NEURONCOMPONENTS_API RemoveKey(int Index) = 0;                         // +0x18
        virtual double NEURONCOMPONENTS_API Eval(float OuterTime, float InnerTime) = 0;          // +0x1C (ST0)
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyTime(int Index, float* Out) = 0;            // +0x20
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyValue(int Index, INeuronComponent** Out) = 0;   // +0x24 AddRefs
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyTime(int Index, float Time,
                                               int* OutNewIndex) = 0;                 // +0x28
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyValue(int Index, INeuronComponent* Child) = 0;  // +0x2C
        virtual unsigned long NEURONCOMPONENTS_API GetMode() = 0;                                // +0x30
        virtual GENRESULT NEURONCOMPONENTS_API SetMode(unsigned long Mode) = 0;                  // +0x34
        virtual int NEURONCOMPONENTS_API GetKeyCount() = 0;                                      // +0x38
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyFactoryName(const char** Out) = 0;          // +0x3C
        virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                    // +0x40
        virtual GENRESULT NEURONCOMPONENTS_API CreateKeyValue(INeuronComponent** Out) = 0;           // +0x44
    };

    // FxRampSingleRamp, vtable 0x0624bb18: Reset 062074f0, CopyFrom 06207530, AddKey
    // , RemoveKey 062076f0, Eval 06207740, GetKeyTime 06207880, GetKeyValue
    // , SetKeyTime 062078c0, SetKeyValue 06207b70, GetMode 06207bb0, SetMode
    // , GetKeyCount 06207bd0, GetKeyFactoryName 06209b40 ("FxRampSingle"),
    // Initialize 062074e0, CreateKeyValue 06207be0.
    struct IRampSingleRamp : INestedSingleRamp
    {
    };

    // FxAnimatedSingleRamp, vtable 0x0624bac8: Reset 06206d40, CopyFrom 06206d80, AddKey
    // , RemoveKey 06206f40, Eval 06206f90, GetKeyTime 062070d0, GetKeyValue
    // , SetKeyTime 06207110, SetKeyValue 062073c0, GetMode 06207400, SetMode
    // , GetKeyCount 06207420, GetKeyFactoryName 06209b60 ("FxAnimatedSingle"),
    // Initialize 06206d30, CreateKeyValue 06207430.
    struct IAnimatedSingleRamp : INestedSingleRamp
    {
    };

    // FxRampColorRamp, vtable 0x0624bb68 (object 0x20, the FxRampSingleRamp layout with
    // IRampColor* children). Two evaluators: EvalRgb blends the children's RGB floats,
    // EvalPacked blends their packed colours (weighting by InnerTime, see the class).
    struct IRampColorRamp : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                      // +0x0C 06207ca0
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(IRampColorRamp* Other) = 0;              // +0x10 06207ce0
        virtual int NEURONCOMPONENTS_API AddKey(float Time, IRampColor* Child) = 0;              // +0x14 06207e00
        virtual GENRESULT NEURONCOMPONENTS_API RemoveKey(int Index) = 0;                         // +0x18 06207ea0
        virtual float* NEURONCOMPONENTS_API EvalRgb(float* Out, float OuterTime,
                                         float InnerTime) = 0;                        // +0x1C 06207ef0
        virtual unsigned long NEURONCOMPONENTS_API EvalPacked(float OuterTime, float InnerTime) = 0; // +0x20 06241320
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyTime(int Index, float* Out) = 0;            // +0x24 062080f0
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyValue(int Index, IRampColor** Out) = 0;     // +0x28 06208110 AddRefs
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyTime(int Index, float Time,
                                               int* OutNewIndex) = 0;                 // +0x2C 06208130
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyValue(int Index, IRampColor* Child) = 0;    // +0x30 062083e0
        virtual unsigned long NEURONCOMPONENTS_API GetMode() = 0;                                // +0x34 06208420
        virtual GENRESULT NEURONCOMPONENTS_API SetMode(unsigned long Mode) = 0;                  // +0x38 06208430
        virtual int NEURONCOMPONENTS_API GetKeyCount() = 0;                                      // +0x3C 06208440
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyFactoryName(const char** Out) = 0;          // +0x40 06209b50
        virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                    // +0x44 06207c90
        virtual GENRESULT NEURONCOMPONENTS_API CreateKeyValue(INeuronComponent** Out) = 0;           // +0x48 06208450
    };

    // FxAnimatedSingle ("1.11_IAnimatedSingle", vtable 0x0624bca0; its abstract base's
    // vtable 0x0624fab0 holds the same bodies with _purecall in the INeuronComponent slots).
    // Object 0x10: +4 Weight (the value while no key exists, 1.0f at construction),
    // +8 a lazily allocated key track (the routine, 0x14 bytes: +0 LoopType, then a
    // std::vector of 16-byte keys {float Value; float Time; float InSlope; float OutSlope}
    // sorted by Time), +0xC RefCount. Created by the factory 06243390 (runs Initialize),
    // by the routine (SetWeight) and inline by the routine. The "value triple" the key
    // slots take and return is {Value, InSlope, OutSlope}. Slot order is baseline's (a
    // verification convention: no baseline DLL is loaded); +0x48 is the scalar deleting
    // destructor (06209b10), which Release reaches as `delete this`.
    struct IFxAnimatedSingle : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                   // +0x0C 06242ec0
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(IFxAnimatedSingle* Other) = 0;        // +0x10 06242f30
        virtual int NEURONCOMPONENTS_API GetKeyCount() = 0;                                   // +0x14 06243000
        virtual GENRESULT NEURONCOMPONENTS_API SetWeight(float Weight) = 0;                   // +0x18 06242fd0
        virtual GENRESULT NEURONCOMPONENTS_API GetWeight(float* Out) = 0;                     // +0x1C 06242fe0
        virtual int NEURONCOMPONENTS_API AddKey(float Time, const float* Value) = 0;          // +0x20 06243030
        virtual GENRESULT NEURONCOMPONENTS_API RemoveKey(int Index) = 0;                      // +0x24 06243080
        virtual double NEURONCOMPONENTS_API Eval(float Time) = 0;                             // +0x28 062430e0 (ST0)
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyTime(int Index, float* Out) = 0;         // +0x2C 06243100
        virtual GENRESULT NEURONCOMPONENTS_API GetKeyValue(int Index, float* Out) = 0;        // +0x30 06243130
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyTime(int Index, float Time,
                                               int* OutNewIndex) = 0;              // +0x34 062431a0
        virtual GENRESULT NEURONCOMPONENTS_API SetKeyValue(int Index, const float* Value) = 0; // +0x38 062431d0
        // "Failed to set non-zero loop type" (the routine's FDUMP) names this slot.
        virtual GENRESULT NEURONCOMPONENTS_API SetLoopType(unsigned long LoopType) = 0;       // +0x3C 06243210
        virtual GENRESULT NEURONCOMPONENTS_API GetLoopType(unsigned long* Out) = 0;           // +0x40 06243230
        // Recomputes every key's slopes for a mode 0..6 and zeroes LoopType (06246550).
        virtual GENRESULT NEURONCOMPONENTS_API ComputeSlopes(unsigned long Mode) = 0;         // +0x44 06243260
        // +0x48 in baseline is the scalar deleting destructor (06209b10); the port's Release
        // deletes directly, so Initialize sits one slot earlier here.
        virtual GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) = 0;                 // +0x4C 06242f20
    };

    // IAnimatedTransform (baseline vtable 0x0624bbb8, QI string
    // "1.11_IAnimatedTransform", implementation tracker interface id 56) -- the
    // object GetComposeAnimatedTransform (particlefx_common.h) returns: each
    // billboard-appearance-descriptor's own private "local animated
    // orientation" helper, composing an animated Translate(3)+Rotate(3)+
    // Scale(3) into a 3x4 world transform for ComposeAppearanceWorldTransform
    // (particlefx_common.cpp). See FFxAnimatedTransform (particlefx_ramp.h/.cpp)
    // for the concrete port.
    
    // wave120 (sonnet:wave120:animatedtransform): built from scratch this
    // wave -- waves 74/75/89 investigated this vtable but explicitly
    // deferred building a concrete class as out of scope for their own
    // single-item briefs (see implementation tracker items
    // 135220/135221/135223-135231/137029's own notes for the full
    // history). Full byte trace of all real slots plus the constructor
    // (the routine, particlefx), Reset (the routine,
    // 138639-138715) and the per-channel default-value table
    // (the field, 9 floats read directly from .data this wave:
    // {0,0,0, 0,0,0, 1,1,1} -- Translate/Rotate/Scale defaults):
    
    //   Storage (baseline, all this-relative): +0x4 PackedModes (3 packed
    //   4-bit nibbles, ctor default 0x435 = SetPackedModes(4,3,5));
    //   +0x8 ChannelFlags (9-bit mask, bit i set means channel i is at its
    //   default/unanimated value -- 0x1FF = "every channel default" =
    //   IsIdentity); +0xC a FIXED 9-element array of
    //   IAnimatedSingleRamp-shaped child pointers (one per TRS channel:
    //   [0..2]=Translate X/Y/Z, [3..5]=Rotate X/Y/Z in degrees,
    //   [6..8]=Scale X/Y/Z), each an FAnimatedSingleRamp (particlefx_ramp.h)
    //   holding one FFxAnimatedSingle default key.
    
    //   +0xC Reset() (the routine): Reset()s each of the 9 children,
    //   builds a fresh default FFxAnimatedSingle key (Weight =
    // [i]) and AddKey(0, ...)s it into the child, then
    //   force-sets ChannelFlags=0x1FF. Real, byte-confirmed.
    
    //   +0x10 CopyFrom(IAnimatedTransform* Other) (the branch, RET 0x8 --
    //   this+1 explicit arg, NOT the zero-arg "RefreshAll" an earlier
    //   wave's note guessed): for each of the 9 channels,
    //   `Other->GetChild(i)` (AddRef'd) then
    //   `this->Children[i]->CopyFrom(OtherChild)` (IAnimatedSingleRamp
    //   +0x10, the routine), then
    //   Release(OtherChild); afterward copies Other's PackedModes
    //   (GetPackedModes/SetPackedModes round-trip) and ChannelFlags
    //   (GetChannelFlags straight into this->ChannelFlags) verbatim.
    
    //   +0x14 GetChild(Index, INeuronComponent** Out) (the branch):
    //   `*Out = Children[Index]; (*Out)->AddRef();` -- no null-check
    //   before the AddRef (baseline trusts a populated slot).
    
    //   +0x18 UpdateChannelFlags() (the routine): for each of the 9
    //   channels, walks the child's own KeyCount-many entries (always
    //   re-reading entry INDEX 0 each iteration -- a baseline quirk,
    //   reproduced as-is) comparing its resolved value (the entry child's
    //   GetWeight() when it has 0 sub-keys, or its first {X,Y,Z} value's X
    //   component when it has exactly 1, else treated as an immediate
    //   mismatch) against the field[i] via a float-equality test (the
    //   FCOMP/FNSTSW/TEST AH,0x44/JP idiom -- JP taken means NOT
    //   equal-or-unordered, i.e. this channel is flagged customized and
    //   the OR below is skipped); if every iteration matches, ORs bit i
    //   into ChannelFlags. Real, byte-confirmed end-to-end this wave (a
    //   genuinely subtle algorithm -- see
    //   FFxAnimatedTransform::UpdateChannelFlags, particlefx_ramp.cpp).
    
    //   +0x1C GetChannelFlags(unsigned long* Out) (the branch): plain
    //   getter. +0x20 IsIdentity() (the branch): `return ChannelFlags ==
    //   0x1FF;` -- already cited by name in this port's own
    //   particlefx_common.cpp (ComposeAppearanceWorldTransform's
    //   PfnIsIdentity cast). +0x24 SetPackedModes(A,B,C) / +0x28
    //   GetPackedModes(*A,*B,*C) (the branch/the branch): pack/unpack
    //   3 4-bit nibbles into/out of PackedModes (`(A<<8)|(B<<4)|C`, no
    //   masking on write -- baseline never guards a >0xF input bleeding
    //   into the adjacent nibble, reproduced as-is).
    
    //   +0x2C Evaluate(TimeA, TimeB, Out12) (the branch, RET 0x10): Out12 is
    //   a 3x3 (Out12[0..8], rows) plus translation (Out12[9..11]). Every child is
    //   evaluated as Eval(TimeA, TimeB) -- TimeA is the ramps' outer time (ALC-3
    //   2026-09-30: the port had the two swapped). Rotation children 4, 5, 3 feed
    // (Out; c3, c5, c4); scale children 6, 7, 8 build a diagonal that
    //   pre-multiplies Out through 3DMathEngine +0x1C; translation children 2, 1, 0 go
    //   to Out12[11], [10], [9]. See FFxAnimatedTransform::Evaluate (particlefx_ramp.cpp).
    struct IAnimatedTransform : INeuronComponent
    {
        virtual GENRESULT NEURONCOMPONENTS_API Reset() = 0;                                     // +0x0C
        virtual GENRESULT NEURONCOMPONENTS_API CopyFrom(IAnimatedTransform* Other) = 0;          // +0x10
        virtual GENRESULT NEURONCOMPONENTS_API GetChild(unsigned long Index,
                                             INeuronComponent** Out) = 0;                // +0x14
        virtual GENRESULT NEURONCOMPONENTS_API UpdateChannelFlags() = 0;                        // +0x18
        virtual GENRESULT NEURONCOMPONENTS_API GetChannelFlags(unsigned long* Out) = 0;         // +0x1C
        virtual unsigned char NEURONCOMPONENTS_API IsIdentity() = 0;                            // +0x20
        virtual GENRESULT NEURONCOMPONENTS_API SetPackedModes(unsigned long A, unsigned long B,
                                                   unsigned long C) = 0;             // +0x24
        virtual GENRESULT NEURONCOMPONENTS_API GetPackedModes(unsigned long* A, unsigned long* B,
                                                   unsigned long* C) = 0;            // +0x28
        virtual void NEURONCOMPONENTS_API Evaluate(unsigned long TimeA, unsigned long TimeB,
                                        float* Out12) = 0;                          // +0x2C
        // +0x30 the branch (item 135221): `XOR EAX,EAX; RET 8` -- an
        // always-success 1-arg stub shared with several other trivial
        // vtable fillers in this module. No observable behavior beyond
        // returning GR_OK; declared for vtable-shape completeness.
        virtual GENRESULT NEURONCOMPONENTS_API Slot0x30(unsigned long Arg) = 0;
    };

    struct IFxEffect;

} // namespace ParticleFxImpl
