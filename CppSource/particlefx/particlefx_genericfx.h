// Author: Giperion.

#pragma once

#include "particlefx_types.h"
#include "particlefx_common.h"

#include <cstring>

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.

// FGenericFxObjectBase: shared COM/tree machinery and the property-set protocol
// (each class's baseline property set, see FFxNodePropertySet) for every
// FxNode/emitter/field/appearance concrete type. Originally this was
// ONE class (FGenericFxObject) dispatching on FactoryName_ string compares
// with type-specific state (field magnitude/approach/attenuation/etc.) all
// living on the same object regardless of which of the 19 baseline types the
// instance actually was. That shape was replaced (2026-09-10, user-directed
// after wave 12 found kDefaultApproach had been silently mis-shared between
// FxRadialField and FxTurbulenceField) with a real class hierarchy: each of
// the 19 concrete types below is now its own C++ class with only the fields
// it actually needs, so a bug like "type X's state accidentally leaks into
// type Y's code path" is a compile error, not a silent runtime bug.

// Concrete leaves (declared in their own family headers):
//   FxNode                                            — this file (trivial)
//   FxSphereEmitter/FxConeEmitter/FxCubeEmitter        — particlefx_emitters.h
//   FxAirField/FxGravityField/FxCollideField/
//     FxRadialField/FxTurbulenceField/
//     FLDustField/FLBeamField                          — particlefx_fields.h
//   FxBasicAppearance/FxParticleAppearance/
//     FxRectAppearance/FxPerpAppearance/
//     FxMeshAppearance/FxOrientedAppearance/
//     FLDustAppearance/FLBeamAppearance                — particlefx_appearances.h

// Construction: particlefx_factory.cpp's CreateComponent constructs the exact
// leaf class by baseline component name (no more single parameterized class).

// Virtual hooks a leaf overrides (all default to a no-op in this base,
// matching what a non-applicable type already did via its old FactoryName_
// guard): BeginField/ApplyFieldToVel (IGenericFx interface methods — only
// the 7 field leaves override; FLBeamField genuinely leaves both as the
// inherited no-op, matching baseline's own the branch/30 no-op),
// OnAfterIntegrate (protected, not part of IGenericFx — only
// FxMeshAppearance overrides, for the Extra2/PoseMeshExtra2 special case).
// OnParticleEmitted (protected, not part of IGenericFx, added wave16-lane1
// 2026-09-10 — only FxParticleAppearance overrides, as a documented no-op;
// see its own doc comment).
// The emitter machinery (the routine and the shape samplers) lives on
// FFxEmitterBase in particlefx_emitters.h since 2026-09-26; the invented
// SampleShape / StoreEmitCountField hooks and the Pad25 emitter tick are gone.

// Deliberately NOT modeled here (2026-09-10, particlefx-wave15-lane1): a real
// baseline per-instance particle-instance-tracking mechanism (private pool +
// intrusive list + owner refcount for emitter-spawned instances; a second,
// separate per-particle "child COM object" slot for FxParticleAppearance).
// Fully traced byte-for-byte (see the large comment above EmitOne in
// particlefx_genericfx.cpp) but concluded to belong on specific EMITTER and
// FxParticleAppearance leaves (particlefx_emitters.*/particlefx_appearances.*),
// not here — baseline overloads the same base-class byte offset with two
// different per-leaf-type meanings for this, which is exactly the
// FactoryName_-gated god-class pattern this file's own split (above) was
// written to eliminate. Open TODO for a future wave with those files in
// scope (the emitter-side "Cluster A" list is still fully open; the
// FxParticleAppearance-side "Cluster B" child-object slot got a new
// port-only virtual hook this wave — OnParticleEmitted below — see its own
// doc comment and EmitOne's doc comment in particlefx_genericfx.cpp for what
// wave16-lane1 confirmed vs. left open).

// NOT virtual — called directly and non-virtually from the relevant leaf
// constructors instead (a virtual call from a BASE class constructor can
// never reach a derived override in C++, since the derived part of the
// object does not exist yet during base construction — so these are plain
// protected helpers a leaf ctor opts into after its own base subobject is
// already fully constructed): BuildDefaultAppearanceRamps() (called by the
// perp and dust ctors; basic, rect, oriented and beam have their own builders in
// particlefx_appearances.cpp). FxRadialField's
// old InitRadialFalloffRamp() is no longer here at all — it is a private
// method directly on FxRadialField now, since only that one type needs it.
namespace ParticleFxImpl
{
    // The lookup key ITextureLibrary +0x30 (Slot12) takes: name + CRC, as
    // the other modules' {Name, Crc} queries. The appearance texture path
    // (the routine / the routine) always passes the name "???"
    // (the field) and the descriptor's texture CRC.
    struct FFxTextureKey
    {
        const char* Name;
        unsigned long Crc;
    };

    // baseline ImpFxTexInfo (FDUMP path "...\Src\ALIL\ImpFxTexInfo.cpp"): the
    // texture state every billboard appearance embeds (node+0xb4; the beam
    // node's at node+0xb8), 0x24 bytes, built by the routine. The texture
    // CRC comes from the appearance descriptor (+0x64), which the
    // descriptor's name property 0x1BA23359 sets to GetCRC32(name)
    // (e.g. the branch on the perp descriptor).
    struct FFxTextureWrapper
    {
        unsigned long TexCrc;     // +0x00
        int Handle;               // +0x04 ITextureLibrary +0x34 id, -1 = none
        // +0x08..+0x18: ITextureLibrary +0x70 GetFrame output -- the frame's texture
        // dword, then its UV rect. The draw fills read [1..4] as floats and copy [0]
        // out bitwise.
        float Frame[5];
        int FrameCount;           // +0x1C ITextureLibrary +0x68
        // +0x20: zeroed by the routine; no other access in particlefx.dll (every
        // `+ 0xd4]` operand on an appearance is a vtable call). TODO: meaning unknown.
        unsigned long Field_0x20;

        FFxTextureWrapper();                        
        void ReleaseHandle();                       
        void SetTexture(const FFxTextureKey* Key);  
        void Resolve();                             
        void Advance(const float& Dt);              
        void LookupFrame(int FrameIndex, float* Out); 
    };

    // One case of a billboard descriptor's bool setter (descriptor vt+0x20): the hash and
    // the Flags (descriptor +0x60) bit it sets or clears. Mask 0 = accepted and ignored
    // (the beam's 0x03503B61 / 0xE2F60EEB / 0x0ABE0402 jump straight to `return 0`).
    struct FDescriptorFlagBit
    {
        unsigned long Hash;
        unsigned long Mask;
    };

    class FGenericFxObjectBase : public IGenericFx
    {
        // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): IntegratePool and
        // ApplyFieldsToPool (particlefx_common.h/.cpp — plain namespace
        // functions, not members) need direct access to
        // InstanceListHead_/InstanceListTail_ (and IntegratePool to
        // LiveInstanceCount_, on particle expiry) below. Friending these two
        // specific, already-shared, already-documented helpers is narrower
        // than making the fields public, and keeps the list's invariants
        // (only ever mutated via UnlinkParticleNode/AppendParticleNode)
        // enforced at exactly two call sites instead of anywhere in the
        // module. See implementation tracker item 132111 for the full baseline trace
        // this pairing implements.
        friend void IntegratePool(FGenericFxObjectBase* Owner, FAppearancePool* Pool, const float& Dt);
        friend void ApplyFieldsToPool(FGenericFxObjectBase* Owner, bool bLiveItemsOnly);

    public:
        FGenericFxObjectBase(const char* PrimaryIid, const char* FactoryName);
        virtual ~FGenericFxObjectBase() noexcept(false);

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        // Outer +0x0C, CloneNode: each leaf class overrides it with its baseline clone.
        // , the shared node-base compose (node vtable slot 2). The mesh
        // appearance overrides it with the routine (compose, then pose its meshes).
        GENRESULT NEURONCOMPONENTS_API ComposeNodeTransform(void* Unused, const float* Parent12) override;
        // (node vtable slot 3): TickLifetime(Dt) through the object's own
        // vtable, return 0. FLBeamAppearance and the three emitters override it
        // (the routine; the branch / the branch / the branch).
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // Outer +0x18, the per-node draw. the routine (node vtable slot 4 default):
        // `XOR EAX,EAX; RET 4`. The appearances override it with their draw.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override { return GR_OK; }
        // Outer +0x1C (FxNode the branch and every sibling): `[node+4] >> 31`.
        GENRESULT NEURONCOMPONENTS_API IsNodeDead() override
        {
            return static_cast<GENRESULT>(NodeFlags_ >> 31);
        }
        // Outer +0x20 / +0x24 (FxNode the branch / the branch): the flags dword.
        GENRESULT NEURONCOMPONENTS_API GetNodeFlags(unsigned long* Out) override
        {
            *Out = NodeFlags_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetNodeFlags(unsigned long Flags) override
        {
            NodeFlags_ = Flags;
            return GR_OK;
        }
        INeuronComponent* NEURONCOMPONENTS_API GetAttachParent() override { return AttachParent_; }
        INeuronComponent* NEURONCOMPONENTS_API GetFirstChild() override { return FirstChild_; }
        INeuronComponent* NEURONCOMPONENTS_API GetPrevSibling() override { return PrevSibling_; }
        INeuronComponent* NEURONCOMPONENTS_API GetNextSibling() override { return NextSibling_; }
        // Outer +0x38, `MOV EAX,imm; RET 4`, one constant per family (all 19 outer
        // vtables, located by their +0x44 thunk into the routine): FxNode 0x190
        // (the branch); every appearance 0x64 (beam 0620dfa0, dust 06212300, basic
        // , 06226f60, 0622ea90, 06231ea0, 062357c0, 0623ad10);
        // every field 0xc8 (0620eaa0, 06213880, air 06214900, 0621a890, 06223ee0,
        // , 0623e0f0); every emitter 0x12c (cone 0621c220, cube 0621da00, sphere
        // ). LinkNodeTree (the routine) orders siblings by it. The port has no
        // family base classes, so the family tag bytes (+0x94 / +0x95 / +0x96) select it.
        // Until ALC-5 this returned 0 for every node, so sibling order was insertion order.
        int NEURONCOMPONENTS_API GetSortKey() override
        {
            if (bIsAppearanceNode_ != 0)
                return 0x64;
            if (bIsFieldNode_ != 0)
                return 0xc8;
            if (bIsEmitterNode_ != 0)
                return 0x12c;
            return 0x190;
        }
        // / the routine (node vtable slots 6 / 5).
        GENRESULT NEURONCOMPONENTS_API SetNodeTransform(unsigned long Index, const float* Source12) override;
        GENRESULT NEURONCOMPONENTS_API GetNodeTransform(unsigned long Index, float** OutSlot) override;
        // wave89 (sonnet:particlefx-wave89-lane1, 2026-09-15): see
        // IGenericFx::TickLifetime's own doc comment (particlefx_types.h) for
        // the full byte trace this corrects (was a no-op `Pad17()` stub
        // through every prior wave). Real body: accumulate Dt into Age_,
        // and once Age_ exceeds Duration_ (strictly), latch the expired bit into
        // NodeFlags_ -- defined out-of-line in particlefx_genericfx.cpp since
        // it now has real logic instead of a one-line stub.
        GENRESULT NEURONCOMPONENTS_API TickLifetime(float Dt) override;
        GENRESULT NEURONCOMPONENTS_API SetParent(INeuronComponent* Parent) override
        {
            AttachParent_ = Parent;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetFirstChild(INeuronComponent* Child) override
        {
            FirstChild_ = Child;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetPrevSibling(INeuronComponent* Prev) override
        {
            PrevSibling_ = Prev;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetNextSibling(INeuronComponent* Next) override
        {
            NextSibling_ = Next;
            return GR_OK;
        }
        // Outer +0x58..+0x6C (FxNode the branch..the branch): get/set of node+0x8c,
        // +0x84 and +0x88 -- see the block comment in IGenericFx (particlefx_types.h).
        GENRESULT NEURONCOMPONENTS_API GetNodeOuterTime(float* Out) override
        {
            *Out = AppearanceOuterTime_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetNodeOuterTime(float OuterTime) override
        {
            AppearanceOuterTime_ = OuterTime;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetNodeAge(float* Out) override
        {
            *Out = Age_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetNodeAge(float Age) override
        {
            Age_ = Age;
            return GR_OK;
        }

        // / the routine / the routine. Declared at the end of IGenericFx
        // now: it used to occupy +0x68, which the baseline call sites prove belongs to
        // GetSubtreeDuration.
        GENRESULT NEURONCOMPONENTS_API EmitOne(const float* Six, unsigned long Extra,
                                    float Duration) override;
        // Outer +0x68 (FxNode the branch): `*Out = [node+0x88]`. Reported 0 until
        // 2026-09-26.
        GENRESULT NEURONCOMPONENTS_API GetSubtreeDuration(float* OutSeconds) override
        {
            *OutSeconds = Duration_;
            return GR_OK;
        }
        // Outer +0x6C (FxNode the branch): `FSTP [node+0x88]`. A stub until 2026-09-26.
        GENRESULT NEURONCOMPONENTS_API SetNodeDuration(float Value) override
        {
            Duration_ = Value;
            return GR_OK;
        }
        // Outer +0x70 (beam the branch, field the branch): the factory name.
        const char* NEURONCOMPONENTS_API GetFxFactoryName() override { return PropertySet_->ClassName.Buffer; }
        // Outer +0x74 / +0x78 / +0x7C (FxNode the branch / the branch / the routine):
        // tail jumps into the property set's outer +0x14 / +0x10 / +0xC, i.e. the set's
        // Node_Name hash (+0x2c), buffer (+0x38) and assign (the routine and the
        // per-class copies the routine / the routine / the routine, ...). The name
        // belongs to the shared set, so a clone renames its archetype too.
        GENRESULT NEURONCOMPONENTS_API GetInstanceNameHash(unsigned long* Out) override
        {
            return PropertySet_->GetInstanceNameHash(Out);
        }
        GENRESULT NEURONCOMPONENTS_API GetInstanceName(const char** Out) override
        {
            return PropertySet_->GetInstanceName(Out);
        }
        GENRESULT NEURONCOMPONENTS_API SetInstanceName(const char* Name) override
        {
            return PropertySet_->SetInstanceName(Name);
        }
        // Outer +0x80 (FxNode 06202880 and its fourteen twins): the set's outer, AddRef'd.
        GENRESULT NEURONCOMPONENTS_API GetPropertySet(IFxNodePropertySet** Out) override
        {
            *Out = PropertySet_;
            PropertySet_->AddRef();
            return GR_OK;
        }
        // Outer +0x84 / +0x88 (FxNode the branch / the branch): node+0x90.
        GENRESULT NEURONCOMPONENTS_API GetNodeLodInput(float* Out) override
        {
            *Out = LodInput_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API SetNodeLodInput(float LodInput) override
        {
            LodInput_ = LodInput;
            return GR_OK;
        }
        // Outer +0x8C for every node that has no sphere: FxNode the routine, cone
        // emitter the branch, field the branch are all `MOV EAX,0xfffffff9;
        // RET 8`. Until 2026-09-26 this reported success without writing, so
        // FFxEffectObject's union seeded itself from an unwritten buffer. Basic,
        // rect and perp override it; oriented (the routine), dust (the routine),
        // beam (the routine), particle (the routine) and mesh (the routine)
        // compute one in baseline and still land here -- not ported.
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* /*OutSphere*/) override
        {
            return static_cast<GENRESULT>(0xfffffff9);
        }
        // Outer +0x90 / +0x94 / +0x98 (FxNode the branch / 980 / 9a0): the family tag
        // bytes node+0x94 (appearance) / +0x95 (emitter) / +0x96 (field).
        GENRESULT NEURONCOMPONENTS_API GetFlagBit4(unsigned char* Out) override
        {
            *Out = bIsAppearanceNode_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFlagBit1(unsigned char* Out) override
        {
            *Out = bIsEmitterNode_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetFlagBit2(unsigned char* Out) override
        {
            *Out = bIsFieldNode_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API Pad40() override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() override { return GR_OK; }
        // Batch begin / end-and-draw (IGenericFx, baseline appearance +0x9C / +0xA0). The four
        // batched appearance kinds override them. Every other appearance reaches the
        // appearance node defaults (node vtable 0x0624fbd0 slots +0x4c / +0x50, entered by
        // `ADD [ESP+4],0xc; JMP`, e.g. dust 0x06212580 / 0x06212590): the routine
        // `XOR EAX,EAX; RET 0xC` and the routine `XOR EAX,EAX; RET 4` -- return 0, no
        // side effect. (FillBatchedGeometry's default above is the third one, the routine.)
        GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long /*Verts*/,
                                             unsigned long /*Indices*/) override
        {
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API Pad42() override { return GR_OK; }
        // , inner +0x5c / appearance +0xB0: the unchecked iterator
        // starts at the by-value argument itself. Out of line; this deliberately
        // preserves that malformed algorithm, not a search by field or index.
        GENRESULT NEURONCOMPONENTS_API Pad44(INeuronComponent* Value) override;
        // Appearance +0xB4 (every appearance vtable: `ADD this,0xc; JMP the routine`,
        // basic the branch). Begins every associated field. Out of line.
        GENRESULT NEURONCOMPONENTS_API Pad45() override;
        // Appearance +0xB8 (`ADD this,0xc; JMP the routine` on all eight appearance
        // vtables, basic the branch): will a particle spawned with (Six, Age,
        // Duration) survive this frame's fields? Out of line.
        GENRESULT NEURONCOMPONENTS_API Pad46(const float* Six, float Age, float Duration) override;
        // Appearance +0xBC: the spawn. Every appearance has its own body (basic
        // , rect the routine, perp the routine, oriented the routine,
        // beam the routine, dust the routine, particle the routine, mesh
        // ). The leaves that are ported override it; this default spawns
        // nothing.
        
        // Until 2026-09-26 this default carried the body of the routine, which
        // belongs to +0xB8 (Pad46) -- one slot off. Dust and Particle still have no
        // spawn and fall through to here: they never emit.
        GENRESULT NEURONCOMPONENTS_API Pad47(const float* /*Six*/, float /*Age*/,
                                  float /*Duration*/) override
        {
            return GR_OK;
        }
        // Appearance +0xC0 (`ADD this,0xc; JMP the routine`, basic the branch):
        // calls each associated field's +0xa0. Out of line.
        GENRESULT NEURONCOMPONENTS_API Pad48() override;
        // Appearance +0xC4 / +0xC8 (basic the branch / the branch): INC / DEC of
        // node+0xb0. Only appearance vtables have these slots in baseline.
        GENRESULT NEURONCOMPONENTS_API AddEmitterRef() override
        {
            ++EmitterRefCount_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API ReleaseEmitterRef() override
        {
            --EmitterRefCount_;
            return GR_OK;
        }

        GENRESULT NEURONCOMPONENTS_API GetLiveInstanceCount(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetQueuedVertCount(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetQueuedIndexCount(unsigned long* Out) override;

        GENRESULT NEURONCOMPONENTS_API SetLiveInstanceCount(unsigned long Count) override
        {
            LiveInstanceCount_ = Count;
            return GR_OK;
        }

        // The node's property set (baseline +0x7c outer / +0x80 set). Never null once the
        // node went through InitializeFromFactory or InitNodeFromArchetype. The property
        // protocol is the set's (IFxNodePropertySet, particlefx_types.h); each node class's
        // CreatePropertySet builds its own set class.
        FFxNodePropertySet* NodePropertySet() const { return PropertySet_; }

        // (node vtable slot 1, called by every node factory after
        // construction): create this class's property set (slot +0x48, CreatePropertySet),
        // give it the class name (the creation descriptor's +4 name, hashed), then
        // Duration = set LifeSpan. A null descriptor FDUMPs "you should set NEURONCOMPONENTSDESC for
        // create FxNode" and skips the name.
        GENRESULT InitializeFromFactory(const FAggDesc8* Desc);

        // baseline IFxEmitter+0x9C (this, appearance). One slot; last wins. No emit.
        GENRESULT NEURONCOMPONENTS_API AssociateAppearance(INeuronComponent* Appearance) override;

        // baseline IFxAppearance+0xAC = the routine: AddRef, insert at end of the
        // field vector (node+0x98 object, begin +0x9c / end +0xa0), Release. A null
        // field is stored too. Walk is the routine, not last-wins.
        GENRESULT NEURONCOMPONENTS_API AssociateField(INeuronComponent* Field) override;

        INeuronComponent* NEURONCOMPONENTS_API GetAssociatedField() override;
        GENRESULT NEURONCOMPONENTS_API GetAssociatedFieldCount(unsigned long* Out) override
        {
            *Out = FieldCount_;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetAssociatedFieldAt(unsigned long Index, INeuronComponent** Out) override;
        // Internal field-update loops borrow a vector entry directly in baseline.
        unsigned long BorrowAssociatedFieldCount() const { return FieldCount_; }
        INeuronComponent* BorrowAssociatedFieldAt(unsigned long Index);
        // wave104 (sonnet:particlefx-wave104-lane2, 2026-09-15): baseline's own
        // field-array auto-compaction-on-expiry (the routine else-branch,
        // implementation tracker item 132871 -- confirmed via the routine/the routine,
        // a smart-pointer-array shift-erase whose net effect is "release the
        // expired slot, shift everything after it down by one, shrink the
        // count"). Port-only helper (not part of IGenericFx, same category as
        // BorrowAssociatedFieldAt above) -- public so the free
        // function ApplyFieldsToPool (particlefx_common.cpp) can call it when a
        // field's own +0x1C gate reports done/expired, instead of leaving the
        // expired field attached forever.
        void RemoveAssociatedFieldAt(unsigned long Index);
        INeuronComponent** EraseAssociatedField(INeuronComponent** Position);
        // (the appearance archetype copy base, called by all eight
        // appearance clone ctors) after the routine: vector<ComPtr> operator= of the
        // field list (node+0x98). Out of line.
        void CopyAssociatedFieldsFrom(FGenericFxObjectBase* Source);
        const FAppearanceRampDescriptor* NEURONCOMPONENTS_API GetAppearanceRampDescriptor() override
        {
            return Descriptor_;
        }

        // wave22 (sonnet:particlefx-wave22-lane1): see IGenericFx::
        // GetAppearanceOuterTime/SetAppearanceOuterTime (particlefx_types.h)
        // for the full baseline [this+0x8c] trace and the exhaustive,
        // module-wide write-site search that came up empty. Backed by a
        // real field (AppearanceOuterTime_ below) rather than a hardcoded
        // constant so a future wave that DOES locate a real producer can
        // wire it up via SetAppearanceOuterTime without another interface
        // change.
        float NEURONCOMPONENTS_API GetAppearanceOuterTime() override { return AppearanceOuterTime_; }
        GENRESULT NEURONCOMPONENTS_API SetAppearanceOuterTime(float Time) override
        {
            AppearanceOuterTime_ = Time;
            return GR_OK;
        }

        // wave117 (sonnet:wave117:globals-final): see IGenericFx::
        // GetDustPrevCameraDistance/SetDustPrevCameraDistance (particlefx_types.h)
        // for the full baseline [this+0x12C] trace (FLDustAppearance-only).
        // Backed by a real field (DustPrevCameraDistance_ below) defaulting
        // to 0.0f, same convention as AppearanceOuterTime_ above.
        float NEURONCOMPONENTS_API GetDustPrevCameraDistance() override { return DustPrevCameraDistance_; }
        GENRESULT NEURONCOMPONENTS_API SetDustPrevCameraDistance(float Distance) override
        {
            DustPrevCameraDistance_ = Distance;
            return GR_OK;
        }

        // ALC-4 (2026-09-30): the appearance texture state (node+0xb4 in baseline) is the
        // FFxTextureWrapper Texture_ below. The old AtlasFrameCount_/AtlasFrameUV_/
        // TextureHandle_ trio and their accessors were that wrapper's +0x1c/+0x08/+0x04
        // split apart with no writer; the history that concluded "no writer exists" is
        // wrong -- the descriptor's name property 0x1BA23359 stores GetCRC32(name) at
        // descriptor +0x64 (the branch and eleven siblings), and every appearance
        // update resolves it through the routine / the routine.

        // wave54 (wave54-lane2, 2026-09-11): investigated item 134306's
        // (the routine) own wave47 blocker (a) -- a believed-missing
        // "per-appearance blend-factor field" that its
        // SetRenderState(D3DRS_SRCBLEND/D3DRS_DESTBLEND) call site
        // (particlefx, the field vtable+0xf0 with
        // render-state ids 0x13/0x14 = D3DRS_SRCBLEND/D3DRS_DESTBLEND)
        // appeared to need, now that TextureHandle_ above (the OTHER named
        // blocker) exists. Traced both values back to their real source:
        //   *(int*)(Appearance+0xf0)+0x68 (SrcBlend arg) and
        //   *(int*)(Appearance+0xf0)+0x6c (DestBlend arg)
        // i.e. NOT appearance-INSTANCE state at all -- both live on the
        // shared FAppearanceRampDescriptor object (particlefx_types.h,
        // already modeled for Flags/SizeRamp/ColorRamp/etc, cached at
        // Appearance+0xf0 by the routine). The identical
        // (Descriptor+0x68/+0x6c) pair, read by the identical
        // (the field vtable+0xf0, ids 0x13/0x14) SetRenderState call
        // shape, recurs in at least 4 OTHER appearance-kind fill functions
        // too (particlefx, 15464-15473, 28162-28165 -- this
        // one spells out the render-state ids as literal call args, e.g.
        // `(**(*the field+0xf0))(the field,0x13,*(int*)(*(int*)
        // (param_1+0xf0)+0x68))`, removing any doubt about the SetRenderState
        // identification -- and 35617-35626), confirming this is the whole
        // shared billboard-appearance family's blend state, not
        // FxPerpAppearance-specific.
        
        // Then traced the VALUES to their write site: SIX independent
        // per-appearance-kind descriptor constructors (siblings of
        // , which is the one the routine's own Perp descriptor
        // uses -- particlefx, 10968-10969, 14136-14137,
        // 27134-27135, 31705-31706 [the routine itself], 34491-34492) each
        // hardcode the IDENTICAL literal pair `param_1[0x19] = 5;
        // param_1[0x1a] = 2;` (local offsets 0x64/0x68 -> absolute
        // Descriptor+0x68/+0x6c, i.e. the exact two dwords the routine
        // reads) with ZERO variation between them. A full sweep of every
        // `+ 0x68) =`, `+ 0x6c) =`, `[0x19] =`, `[0x1a] =` write in the
        // whole inspected module found no other writer and no
        // property-driven/.ale-authored override path anywhere. 5 =
        // D3DBLEND_SRCALPHA, 2 = D3DBLEND_ONE -- every
        // FAppearanceRampDescriptor-based appearance in baseline draws with a
        // fixed additive-glow blend (SRCALPHA, ONE), not the plain alpha
        // blend (SRCALPHA, INVSRCALPHA) one might otherwise guess.
        
        // CONCLUSION: this is a compile-time constant baked identically
        // into every baseline descriptor build, not per-instance state.
        // Per this project's "document, don't invent" rule, deliberately
        // NOT adding SrcBlend_/DestBlend_ fields to this class -- there is
        // no real per-instance value for them to hold. A future wave
        // wiring the routine (or any sibling fill function) should call
        // RenderPipeline->SetRenderState(D3DRS_SRCBLEND, 5) and
        // SetRenderState(D3DRS_DESTBLEND, 2) as plain literals, exactly as
        // baseline does, unless a future .ale-driven override is later found
        // (in which case the right home is two new plain dword fields on
        // FAppearanceRampDescriptor itself, particlefx_types.h -- out of this
        // lane's file ownership this wave -- not on this class). See
        // implementation tracker items 134306/134307 for the full byte citations.
        // This closes out blocker (a) from item 134306's wave47 note as
        // "answered: no field needed" -- blockers (c) (persistent
        // camera-rotation cache) and (d) (a new IGenericFx per-frame-driver
        // slot) remain genuinely open and out of scope.
        
        // wave67 (sonnet:particlefx-wave67-lane2, 2026-09-11): closed blocker
        // (d) for real -- added the new port-only IGenericFx::DrawOrEnqueue
        // slot (particlefx_types.h) plus a base implementation here
        // (DrawOrEnqueue, declared below near GetQueuedIndexCount, defined
        // in particlefx_genericfx.cpp) that wires the one piece of
        // 's own dispatch that is genuinely generic across every
        // leaf type -- the begin()/end() empty-instance-particle-list guard.
        
        // Also RE-EXAMINED blocker (c) from scratch (this wave's own
        // mandate) rather than taking "still open" on faith. Byte-traced
        // 's own camera-related call sequence
        // (particlefx:0x062332b3-0x06233311) end to end:
        //   MOV EAX,[the field]; ...; CALL [EAX_vtbl+0x40]
        // is the EXACT same G_DrawCookie-vtable+0x40 call shape wave48
        // already resolved into GetDrawCameraTransform() (particlefx_common.h/
        // .cpp) -- confirmed by directly comparing this function's raw bytes
        // against GetDrawCameraTransform's own documented baseline trace, not
        // by analogy. The result is passed straight into a math-engine
        // MulTransform(CameraTransform, this+0x4c) call (vtable+0x1c on the
        // 3DMathEngine singleton -- Acquire3DMath() in this
        // port), whose OUTPUT is written to a STACK-local scratch buffer and
        // handed directly to RenderPipeline::SetTransform (the field
        // vtable+0x7c) within the SAME function call -- never written back
        // to the appearance object, never read again anywhere in this
        // function's own body. Unlike the sibling Dust/Basic vertex-fill
        // functions (particlefx_common.cpp's own AppearanceRestMatrix doc
        // comment, ~line 2135), which DO persist a composed transform to a
        // real object field (Appearance+0xec) for reuse across the
        // per-particle loop, the routine computes and consumes its own
        // camera*rest composition ENTIRELY on the stack, once per dispatch,
        // with zero cross-frame or cross-call persistence in baseline's own
        // bytes.
        
        // CONCLUSION: like blocker (a), this is answered, not implemented as
        // a new field -- no persistent camera-rotation cache exists in
        // baseline for THIS function, so adding one here would be exactly the
        // "invent a field with no baseline backing" this project's rules
        // forbid. The two real, already-modeled pieces this function's own
        // camera math needs are already present: the live view transform via
        // GetDrawCameraTransform() (queried fresh every call, matching
        // baseline's own fresh-query-every-dispatch bytes) and the persistent
        // per-instance rest transform via GetNodeTransform(0)/NodeWorldXform_
        // (already on this class since the original port). No new field
        // added. A future wave writing FxPerpAppearance::DrawOrEnqueue
        // (particlefx_appearances.cpp) should call
        // MulTransform(GetDrawCameraTransform(), GetNodeTransform())
        // straight into a local buffer, exactly as baseline does, rather than
        // caching the result on the object.

        // Field types (7 leaves in particlefx_fields.h) override these; every
        // other leaf inherits the no-op below, matching what its old
        // FactoryName_ guard already did (and matching baseline's own
        // FLBeamField, whose real Begin/Apply genuinely are XOR EAX,EAX RET
        // no-ops — see particlefx_fields.h's FLBeamField for that citation).
        void NEURONCOMPONENTS_API BeginField(INeuronComponent* /*Appearance*/, float /*Dt*/) override {}
        void NEURONCOMPONENTS_API ApplyFieldToVel(float* /*PosVel6*/, float* /*Age*/, float /*Dt*/) override {}

        // wave30 (sonnet:particlefx-wave30-lane2, 2026-09-11): read-only
        // traversal entry point for THIS instance's own active-particle
        // list (InstanceListHead_/Tail_ below, added wave23/24). Added so a
        // future particlefx_common.cpp-owning wave can replace FillBeamRibbon's
        // documented O(pool-size) workaround (it currently walks the
        // entire shared FAppearancePool::UsedHead chain and filters by
        // `Particle->Appearance == Appearance`, since particlefx_genericfx.h
        // was out of wave28's file ownership) with a direct O(instance-
        // size) walk of exactly this list — see implementation tracker items
        // 131849/131852 for that workaround's own documentation and this
        // wave's comment there for the exact call-site swap.
        
        // Deliberately read-only and additive:
        //   - Returns a `const FPooledParticle*`, not the mutable
        //     InstanceListHead_ itself, so a caller cannot reassign this
        //     instance's own head pointer through the returned value.
        //   - FPooledParticle::Next/Prev (particlefx_types.h) are already
        //     plain public members with no accessor of their own — any
        //     external code that already compiles against FPooledParticle*
        //     (as FillBeamRibbon's current workaround already does, via
        //     Pool->UsedHead) can already walk ->Next once it holds a
        //     starting pointer, so this accessor does not expose anything
        //     that was not already externally walkable; it only supplies a
        //     narrower, correctly-scoped starting point than
        //     Pool->UsedHead.
        //   - Does not touch InstanceListHead_/InstanceListTail_'s existing
        //     private-to-this-class mutation path (still only ever written
        //     by AppendParticleNode/UnlinkParticleNode, called from EmitOne
        //     above and from the two wave23-friended pool functions) — this
        //     is a pure new getter, no existing behavior of EmitOne,
        //     IntegratePool, ApplyFieldsToPool, or ~FGenericFxObjectBase
        //     changes.
        //   - Intentionally NOT wired into FillBeamRibbon by this wave —
        //     particlefx_common.cpp is owned by a concurrent lane this wave
        //     (see this wave's own task scope); left for that future wave.
        const FPooledParticle* GetFirstInstanceParticle() const { return InstanceListHead_; }

        // baseline outer +4 / +8: the batch-list links the routine writes (prev, next) and
        // walks through [outer+8]. See FBatchList (particlefx_types.h).
        IGenericFx* BatchPrev_ = nullptr;
        IGenericFx* BatchNext_ = nullptr;

        // wave89 (sonnet:particlefx-wave89-lane1, 2026-09-15): read-only
        // accessor for Age_/Duration_ below -- see TickLifetime's own doc
        // comment above and Age_/Duration_'s own field comment for the full
        // baseline trace. Not part of IGenericFx -- a plain port-only helper.
        float GetLifetimeAge() const { return Age_; }
        float GetLifetimeDuration() const { return Duration_; }

        // the node-base half of every class's archetype copy constructor
        // (the routine / the routine / the routine, 18 callers). Duration_ =
        // [[Archetype+0x80]+0x48] (Node_LifeSpan); the own +0x7c is Released, the
        // archetype's copied and AddRef'd, +0x80 copied: the clone shares the archetype's
        // property set. The billboard set cache (node+0xf0, dust +0xe8) follows it, as
        // every appearance clone ctor copies it (basic the routine 0x06215e4b).
        void InitNodeFromArchetype(FGenericFxObjectBase* Archetype);

        // (RET 4), the appearance copy base all eight appearance clones call:
        // , the appearance set cache +0xa8 copied (PropertySet_ here), then the
        // field list (+0x98) assigned from the archetype's (CopyAssociatedFieldsFrom).
        void InitAppearanceFromArchetype(FGenericFxObjectBase* Archetype);

        // The composed world transform, node+0x4c (what GetNodeTransform(0) hands out).
        // I3DMathEngine FTransform layout: 3x3 row-major rotation, then translation.
        const float* GetNodeWorldXform() const { return NodeWorldXform_; }

        // node vtable slot 7 (+0x1c), shared by all 21 node vtables:
        // Out12 = GeneralInverse(Other's composed transform) * this node's composed
        // transform, i.e. this node expressed in Other's space. Other is an outer
        // object (its +0x40 GetNodeTransform(0) is called). Not an IGenericFx slot here:
        // the outer vtables do forward +0x44 to it (FxNode the branch), but the port's
        // +0x44 is TickLifetime and every known caller reaches this through the node
        // sub-object (emitter body the routine 0x06245c56, field Begin 0x06224219 ...).
        GENRESULT ComputeRelativeTransform(INeuronComponent* Other, float* Out12);

        // [[this+0x80]+0x24]: the property set's Node_Transform IAnimatedTransform (the
        // set constructor's default FxAnimatedTransform, filled in place by the routine
        // when the .ale has the property). No reference is added.
        virtual IAnimatedTransform* FindNodeAnimatedTransform() const { return PropertySet_->Transform; }

    protected:
        // Node slot +0x48: create this class's property set. The base body is FLBeamField's
        // (a base set under the field vtables 0x0624fda8 / 0x0624fcf0); every
        // other class, FxNode included (the routine), overrides it with its own creator.
        virtual void CreatePropertySet();
        // Store a freshly created set the way every creator does (e.g. sphere 0x0623bc8f..
        // Release the old +0x7c, store and AddRef the new, Release the
        // creation reference) and cache it (+0x80 and the class caches).
        void AdoptPropertySet(FFxNodePropertySet* Set);
        // Port-only: the class cache (baseline +0x9c / +0xa8 / +0xf0) of a leaf that keeps a
        // typed pointer next to PropertySet_; called whenever PropertySet_ changes.
        virtual void OnPropertySetChanged();

        // The particle list at node+0xd8 = {head +0xd8, tail +0xdc, pool +0xe0,
        // count +0xe4} is InstanceListHead_ / InstanceListTail_ / the leaf's pool /
        // LiveInstanceCount_. These two helpers are the list operations the four
        // billboard appearances (basic, rect, perp, oriented) share; baseline compiles
        // a copy per class.
        
        // SpawnListParticle: basic the routine, rect the routine + the routine,
        // perp the routine + the routine, oriented the routine. Returns the new
        // particle (the list tail) so perp can seed its axis.
        FPooledParticle* SpawnListParticle(FAppearancePool* Pool, const float* Six, const float& Age,
                                           const float& Duration);
        FPooledParticle* EraseListParticle(FAppearancePool* Pool, FPooledParticle* Particle);
        void EvictOldestListParticle(FAppearancePool* Pool);
        // UpdateListAppearance: the per-node update, basic the routine, rect
        // , perp the routine, oriented the routine, dust the routine.
        // FrameRamp is the descriptor ramp the class evaluates for a flag-8 frame.
        void UpdateListAppearance(FAlDrawContext& DrawContext, FAppearancePool* Pool, float Dt,
                                  INestedSingleRamp* FrameRamp);

        void ReleaseAssociatedFields();
        void DestroyListAppearance(FAppearancePool* Pool);
        void ReleaseRectFields();
        void DetachRectList();
        void ReleaseRectPropertySet();
        unsigned long ResolveLiveInstanceCount() const;

        // (UpdateInstanceBoundsSphere / InstanceBoundsSphere_, wave71, were removed
        // 2026-09-26: the routine is FLBeamAppearance::GetBoundingSphere, reached through
        // the beam's outer +0x8C thunk the routine.)

        unsigned long RefCount_;

        // Node-base +0x4 (the routine zeroes it), the node flags:
        //   0x1 -- lifetime over (TickLifetime, the branch: Age_ > Duration_),
        //   0x2 -- skip the Node_Transform animation (read by the compose the routine),
        //   0x4 -- the raw transform was set from outside (SetNodeTransform index 1),
        //   0x8 -- the composed transform was set from outside (SetNodeTransform index
        //          0); the next compose clears it and keeps the value.
        // The sign bit means "dead" (IsNodeDead, outer +0x1C); the emitter body
        // tests it on itself with `TEST EAX,EAX; JS` (0x06245c3d).
        unsigned long NodeFlags_;

        const char* PrimaryIid_;
        const char* FactoryName_;
        // baseline node +0x7c (the set's outer, AddRef'd) / +0x80 (the set): one pointer
        // holding one reference. Null until InitializeFromFactory or InitNodeFromArchetype.
        FFxNodePropertySet* PropertySet_;
        INeuronComponent* AssociatedAppearance_;
        INeuronComponent** AssociatedFields_;
        unsigned long FieldCount_;
        unsigned long FieldCapacity_;
        INeuronComponent* AttachParent_;
        INeuronComponent* FirstChild_;
        INeuronComponent* PrevSibling_;
        INeuronComponent* NextSibling_;
        unsigned long LiveInstanceCount_;

        // Node-base +0x1c: the raw (local) transform, identity from the routine. The
        // compose overwrites it with the evaluated Node_Transform while flag 0x4 is clear;
        // SetNodeTransform(1) sets it and flag 0x4.
        float NodeLocalXform_[12];
        // Node-base +0x4c: the composed (world) transform, identity from the routine.
        // Written by the compose the routine and by SetNodeTransform(0); read through
        // GetNodeTransform(0) and by every emitter / appearance / bounds consumer.
        float NodeWorldXform_[12];

        // Node-base +0x90, 1.0f from the routine (0x0623fc02); outer +0x84/+0x88.
        float LodInput_;
        // Node-base +0x94 / +0x95 / +0x96, zero from the routine; the family
        // constructors set one of them (see IGenericFx::GetFlagBit4). The appearance
        // and field leaves set theirs in this class's constructor from PrimaryIid_
        // (baseline sets them in the shared appearance init 0x0624439f and in each field
        // constructor), FFxEmitterBase sets +0x95.
        unsigned char bIsAppearanceNode_;
        unsigned char bIsEmitterNode_;
        unsigned char bIsFieldNode_;
        // Appearance node+0xb0 (outer +0xbc): how many emitters feed this appearance
        // (AddEmitterRef / ReleaseEmitterRef). Zero-initialised like the rest of the
        // appearance block.
        long EmitterRefCount_;

        // wave23 (sonnet:particlefx-wave23-lane1, 2026-09-11): this instance's
        // OWN active-particle list — every FPooledParticle this instance has
        // emitted that has not yet expired, threaded through the particle
        // struct's own (previously unused) Next/Prev fields
        // (particlefx_types.h). Rooted here, on the owning appearance instance,
        // NOT on FAppearancePool (which stays a purely shared block arena —
        // Items/FreeNodes/Capacity/FreeCount/UsedHead/UsedTail/FreeHead/
        // FreeTail, particlefx_types.h — its own Used/Free bookkeeping is a
        // physically separate FPoolLink*-typed list from this one and is
        // untouched by this change).
        
        // Matches baseline's confirmed per-owner-instance sentinel list: five
        // independently byte-verified call sites (the routine emit-and-link,
        // per-instance age/integrate, the routine unlink+free,
        // destructor erase-range, the routine per-instance
        // field-apply — implementation tracker items 132827/132872/132839/132921/
        // 132871 — plus the routine's independently-retraced parallel
        // lineage on a different concrete leaf class) all root this same
        // shape at a small fixed byte offset on the owning instance, not on
        // the shared pool. See implementation tracker item 132111's wave22 comment
        // for the complete trace and staged plan this field pair implements
        // (steps 1-7); EmitOne links a new particle in
        // (particlefx_genericfx.cpp), IntegratePool ages/unlinks-and-frees
        // expired ones, ApplyFieldsToPool walks it read/write per associated
        // field (both particlefx_common.cpp, both friended above).
        
        // wave24 (sonnet:particlefx-wave24-lane1, 2026-09-11): implementation tracker
        // item 132111 step 8 / item 132921 CLOSED — ~FGenericFxObjectBase()
        // (particlefx_genericfx.cpp) now erase-ranges this list (unlink + free
        // each remaining node back to PoolForAppearanceFactory(FactoryName_))
        // before any other teardown, mirroring the routine/the routine/
        // 's confirmed baseline order. See that destructor's own
        // doc comment for the full byte-level trace.
        FPooledParticle* InstanceListHead_;
        FPooledParticle* InstanceListTail_;

        // wave89 (sonnet:particlefx-wave89-lane1, 2026-09-15): backing store for
        // TickLifetime() above -- see IGenericFx::TickLifetime's own doc
        // comment (particlefx_types.h) for the full the branch byte trace.
        // baseline this+0x84 (Age_) and this+0x88 (Duration_) sit immediately
        // BEFORE AppearanceOuterTime_'s own confirmed this+0x8c below --
        // i.e. all three are consecutive dwords in baseline's real layout,
        // additional (not the original) evidence that Age_/Duration_
        // genuinely belong on this shared base class right next to it, not
        // on some unrelated per-leaf object.
        
        // seeds Age_ = 0 and Duration_ = 100.0f (0x42c80000);
        // (InitNodeFromArchetype) then copies the archetype's
        // Node_LifeSpan (hash 0xf27fde7d; descriptor default 3.0f from the routine,
        // FLT_MAX is only the table's max bound) into Duration_.
        float Age_;
        float Duration_;

        // Node +0x8c: the ramp Eval "OuterTime" every property evaluation passes
        // (emitter props in the routine, the compose, the mesh pose). the routine
        // zeroes it. CORRECTED 2026-09-26 (ALC-1): it does have a writer -- outer
        // +0x5C SetNodeOuterTime (cone the branch: FSTP [outer+0x90]), which
        // IFxEffect Slot6 (the branch) walks over every node.
        float AppearanceOuterTime_;

        // wave117 (sonnet:wave117:globals-final): backing store for
        // GetDustPrevCameraDistance/SetDustPrevCameraDistance above --
        // baseline's FLDustAppearance-only [this+0x12C] previous-frame
        // camera-distance field (see the accessor's doc comment in
        // particlefx_types.h for the full the routine trace). Defaults to
        // 0.0f, matching baseline's own zero-initialized object memory (no
        // explicit constructor write to this offset was found anywhere in
        // the module).
        float DustPrevCameraDistance_;

        // baseline node+0xb4 (the beam's +0xb8): the appearance's texture state,
        // at construction, the routine in every appearance destructor.
        // The port keeps it on the shared base; a node that never resolves a texture
        // keeps Handle -1, for which ReleaseHandle is a no-op.
        FFxTextureWrapper Texture_;

        // baseline node+0xf0 (dust +0xe8): the billboard set, i.e. PropertySet_ typed, for
        // the six billboard appearances; nullptr for every other node. Set by
        // OnPropertySetChanged of those six leaves.
        FAppearanceRampDescriptor* Descriptor_;
    };

    // RegisterComponent #15 (FxNode). A plain node has no field/emitter/appearance
    // behaviour, so every inherited hook is the baseline one. (#14 "FxNodePropertySet" is
    // not a node: its factory builds FxNode's own property set, FFxNodeOwnPropertySet --
    // wave 3's "same underlying object" reading was wrong, see particlefx_factory.cpp.)
    class FxNode final : public FGenericFxObjectBase
    {
    public:
        FxNode(const char* PrimaryIid, const char* FactoryName)
            : FGenericFxObjectBase(PrimaryIid, FactoryName)
        {
        }

        // (node +0x20), outer alias the branch: constant 0x190.
        int NEURONCOMPONENTS_API GetSortKey() override { return 0x190; }

        // (outer thunk 062026b0): the node (0xa0 bytes) built inline, then the
        // body of the routine inline (0x0623fd77..0x0623fdbd); *Out = it, 0.
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // (node slot +0x48): operator_new(0x54), the routine, inner vtable
        // , outer 0x0624f980, count 1, then the adoption tail.
        void CreatePropertySet() override;
    };

} // namespace ParticleFxImpl
