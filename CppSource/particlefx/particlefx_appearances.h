// Author: Giperion.

#pragma once

#include "particlefx_genericfx.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.

// The 8 "generic" appearance leaves (RegisterComponent names FxBasicAppearance/
// FxParticleAppearance/FxRectAppearance/FxPerpAppearance/FxMeshAppearance/
// FxOrientedAppearance/FLDustAppearance/FLBeamAppearance — implementation notes's
// RegisterComponent table, entries #26-#33).

// ALC-6 (2026-09-30): every appearance's properties live in its baseline property set
// (particlefx_types.h): the six billboards in an FAppearanceRampDescriptor behind
// FFxBillboardAppearance's table-driven slots, particle and mesh in their own sets. Each
// leaf creates its set in CreatePropertySet (node slot +0x48, run by
// InitializeFromFactory); clones share the archetype's (the routine).

// === Wave 17 (particlefx-wave17-lane1, 2026-09-10): Cluster A private
// particle-tracking list ===
// Waves 14-16 traced a real, unported baseline mechanism (implementation tracker
// /a070/a2e0/a360/a4a0/06239f80/06239f00/06238780/0623b410) but
// left its exact shape ambiguous ("a third, separate {Head,Tail,Pool} list"
// distinct from the one the routine populates). This wave resolved that:
// is dispatched through the appearance's OWN embedded
// sub-object vtable (the routine, assigned at object+0xC — see
// 's ctor), so its `this` parameter (called `param_1` in the
// analysis) is object+0xC, not the outer object. Its "+0xd8" field reference
// therefore resolves to object+0xC+0xD8 = object+0xE4 — the EXACT SAME
// {Head,Tail,Pool,RefCount} quad the routine/the routine populate at
// object+0xE4/0xE8/0xEC/0xF0. There is only ONE list, not three:
//   - Ctor (the routine/06215d80/0622bb10/06232950): zero-inits the quad,
//     Pool = the type's own already-shared FAppearancePool global
//     (the field/a04/a9c/b14 == this port's G_PoolRect/Basic/Oriented/
//     Perp — confirmed by matching the routine(&DAT_...,...,stride) init
//     call sites, same globals, same strides 0x30/0x30/0x30/0x3c).
//   - Spawn (the routine -> the routine): AllocPoolItem on that pool,
//     copy 6 floats (pos+vel) + Age/Duration/InvDuration (FPooledParticle
//     layout, stride 0x30/0x3c — confirmed, not kStrideParticle's 0x74),
//     `*(Appearance+0xE4) += 1` (the SAME quad's RefCount word, i.e. an
//     owner-refcount on `this`), link onto Head/Tail via the item's own
//     Next/Prev (FPooledParticle+0x0/+0x4 — unused by this port's
//     AllocPoolItem/FreePoolItem, confirmed free for this purpose).
//   - Erase (the routine/06239f00): unlink, `*(Appearance+0xE4) -= 1`,
//     FreePoolItem back to the SAME pool.
//   - Age/drive (the routine -> the routine): once per frame, walk the
//     list, Age+=Dt, Pos+=Vel*Dt while Age<=Duration, else erase.
// CONFIRMED PRESENT (byte-identical ctor pattern) on: FxRectAppearance
// (the routine), FxBasicAppearance (the routine), FxOrientedAppearance
// (the routine), FxPerpAppearance (the routine).
// CONFIRMED ABSENT on the other 4 (see each class below for its own
// evidence): FxMeshAppearance, FxParticleAppearance (both show an
// unrelated, smaller 3-word {link,?,Pool} ctor pattern with no RefCount —
// Cluster B's per-instance child-object pool, already documented in
// particlefx_genericfx.cpp above EmitOne, not this mechanism), FLDustAppearance
// and FLBeamAppearance (both have their OWN distinct, unrelated function at
// vtable slot 47 — neither calls the routine at all).

// CORRECTED 2026-09-26: wave17's own finding above -- one list -- was right, and
// the port then built two anyway: Pad47 spawned into a per-leaf PrivateHead_ list
// that no draw path walked, while DrawOrEnqueue drew InstanceListHead_, which
// nothing but the Beam spawned into. The private list, its helpers
// (PrivateListSpawn/Erase/Clear/Age) and OnAfterIntegrate are gone. The one list
// is FGenericFxObjectBase::InstanceListHead_/Tail_/LiveInstanceCount_ (node+0xd8/
// +0xdc/+0xe4), spawned by SpawnListParticle and aged by UpdateListAppearance from
// each leaf's own Pad5 (see particlefx_genericfx.cpp). Everything from the PORT NOTE
// down to the wave66 audit describes the removed mechanism and is kept only as
// history.

// PORT NOTE (fixed particlefx-wave20-lane1, 2026-09-11): the 4 classes below
// previously used DEDICATED heap allocation (new/delete per node) instead of
// carving items out of the shared per-type FAppearancePool arena the way
// baseline's the routine literally does, because sharing the real pool
// without safeguards would let this port's own IntegratePool (which walks
// every item reachable from Pool->UsedHead once per frame doing the
// IDENTICAL Age+=Dt / Pos+=Vel*Dt math as the routine) double-integrate a
// tracked particle or free it out from under the private list's own
// Head_/Tail_/Next/Prev pointers. Wave 19 (particlefx_common.h/.cpp) closed
// that gap with AllocPrivatePoolItem/FreePrivatePoolItem — they draw/return
// a slot from the SAME shared arena (G_PoolBasic/G_PoolRect/G_PoolOriented/
// G_PoolPerp below, matching baseline bytes) while excising the node from
// Pool->UsedHead on allocation so IntegratePool's sweep can never see or
// double-free it. This wave wired PrivateListSpawn/Erase/Clear/Age
// (particlefx_appearances.cpp) through those two functions instead of
// new/delete, passing each leaf's own already-shared pool global. baseline-
// accurate allocation source, no more dedicated heap traffic.

// SPAWN ENTRY POINT (WIRED UP wave 18, particlefx-wave18-lane1, 2026-09-10):
// baseline's only real trigger for inserting into this list is the routine,
// which lives at IGenericFx vtable slot 47 (+0xBC) — this port's `Pad47()`.
// Wave 17 assumed Pad47's real signature was (this, const float* Six, float
// Duration) — 2 args beyond `this` — but a byte-level re-check of
// found RET 0x10 (4 stack dwords, i.e. `this` + 3 args, not 2):
// analysis's own inspected prototype for the routine only lists 3 params
// (param_1/2/3) but its own RET count proves a 4th exists that analysis's
// stack-frame recovery missed. Tracing both args into the routine's field
// writes against this port's own FPooledParticle layout (Age@+0x24,
// Duration@+0x28, InvDuration@+0x2c — particlefx_types.h) resolved it: the
// ORIGINAL 3rd arg (param_3) lands at item+0x24 == Age, and the actual 4th
// arg (the one analysis dropped) is what the routine's `FLD 1.0 / FDIV`
// divides by and stores raw at item+0x28 == Duration. So the real signature
// is `Pad47(const float* Six, float Age, float Duration)` — baseline lets the
// spawning caller seed a non-zero initial Age, not just a Duration. Wave 17's
// SpawnPrivateInstance(Six, Duration) hardcoded Age=0.0f; this wave adds the
// Age parameter through Pad47 -> SpawnPrivateInstance -> PrivateListSpawn
// (particlefx_appearances.cpp) so a caller-supplied non-zero Age is no longer
// silently dropped. Pad47 is now overridden on the 4 leaves below to call
// SpawnPrivateInstance directly — the mechanism is reachable from real COM
// dispatch as of this wave.

// === Wave 66 (sonnet:particlefx-wave66-lane2, 2026-09-11): fresh skeptical
// re-audit applying post-wave-40 bug classes ===
// Re-derived, from raw bytes with no reliance on prior comments/statuses,
// the 3 specific bug classes this marathon has since cracked elsewhere:
//   1. Release()-reentrancy-guard (waves 59-63, missing in 7/9 classes
//      checked): neither this header nor particlefx_appearances.cpp declares
//      an OWN Release() override for any of the 8 classes here — all 8
//      inherit FGenericFxObjectBase::Release() (fixed wave63). Independently
//      verified this is baseline-correct, not an inherited gap, by tracing 2
//      of the 8 leaves' own COMPILED Release()s directly: the routine
//      (FxBasicAppearance's real vtable slot 2, the branch, single
//      XREF) and the routine (FxRectAppearance's, the branch, single
//      XREF) are BOTH byte-identical to the base class's current (post-
//      wave63) underflow-guard + reentrancy-guard pattern. baseline generates
//      a mechanically duplicated Release() per concrete leaf class (same
//      family already sampled via the routine for the wave63 fix itself);
//      this wave adds 2 more independent samples confirming that
//      generalization holds for this file's own classes specifically (see
//      implementation tracker comments on 0x06218630/0x0623a190). No override
//      needed — C++ virtual dispatch through the shared base already
//      reproduces baseline's per-class duplication exactly, since RefCount_
//      lives at a fixed base-class offset regardless of leaf type.
//   2. Calling-convention miscalibration (the FxCollideField/Oriented-
//      fStack_164 technique): re-derived the routine's (Pad47's baseline
//      entry point) raw x86 byte-by-byte from the .txt listing (not the
//      .c analysis, which undercounts its own stack args) — RET 0x10, 4
//      explicit stack dwords (this + Six-pointer + 2 float scalars) with
//      NO register-passed `this`, i.e. a plain STDMETHOD-style stack-only
//      call. Cross-traced both scalar's placement into the routine's own
//      field writes (item+0x24/+0x28) to confirm which stack slot is Age
//      vs. Duration. Result matches the existing Pad47(Six,Age,Duration)
//      signature and PrivateListSpawn/PrivateListErase/PrivateListAge
//      exactly, field for field — same conclusion wave40's own independent
//      fresh re-audit already reached (implementation tracker items 134776/134659/
//      134650/134661/134649, all dated 2026-09-11). No discrepancy found
//      across 2 independent re-derivations.
//   3. AddRef-on-accessor (waves 43-44, re-checked here for the wave57
//      fields specifically): neither EffectScaleRamp_ (FxMeshAppearance/
//      FxParticleAppearance) nor AxisOverrideRamp (Descriptor_, built by
//      the per-leaf descriptor builders) has any public accessor
//      declared anywhere in this file — both are opaque, private,
//      construction/teardown-only fields with no GetXxx() to leak a ref
//      through. Bug class does not apply; nothing to fix.
// No bugs found. No code changed this wave — every mechanism this file
// owns (Cluster A private list, Pad47, the 2 wave57 standalone ramps, the
// 2 wave55/56 AxisOverrideRamp ctors) re-verified byte-clean from scratch.

// === Wave 76 (sonnet:particlefx-wave76-lane1, 2026-09-11): ComposeAppearance-
// WorldTransform (the routine, particlefx_common.cpp/.h, implementation tracker item
// 135093) — exhaustive in-module call-site search, CLOSED NEGATIVE ===
// Wave 75 confirmed the real compose body and identified exactly 6 classes
// (all declared in THIS file — FLBeamAppearance/FLDustAppearance included;
// neither lives in particlefx_fields.cpp) whose own private "descriptor+4"
// helper installs the routine at vtable+0x18/+0x60, but left "wire it to a
// live call site" as future work. This wave searched for that call site and
// found there ISN'T one anywhere in particlefx.dll: each of the 6 classes' own
// vtable (the routine/c040/cac8/e160/e858/ee90) has exactly ONE XREF in
// the raw listing — the owning class's own constructor writing `*this =
// &PTR_FUN_...` at construction — and zero other references anywhere in the
// 160k-line analysis. A vtable pointer installed once and never read
// again is never locally dispatched. (The only 2 genuine direct CALLs to
// in the whole binary belong to FxMeshAppearance's
// PoseOneMeshExtra2 chain and FxParticleAppearance's EmitOne tail — neither
// is one of these 6 classes, both already separately covered.) Conclusion:
// this compose method is a pure COM-exposed callback for a caller OUTSIDE
// particlefx.dll (presumably a scene-graph/animation system in another module
// that QueryInterfaces the appearance or its aggregated descriptor+4
// sub-object) — there is no baseline call for these 6 leaves' own port methods
// to reproduce. Deliberately left UNWIRED rather than inventing a
// ParentXform12 source/call site with no baseline backing. See item 135093's
// own wave76 note for the full XREF-line citations. Not re-openable by a
// future wave without new evidence from a DIFFERENT module.
namespace ParticleFxImpl
{
    __declspec(noinline) double ForwardAppearanceSingleEval(
        INestedSingleRamp* Ramp, const float& OuterTime, const float& InnerTime);
    __declspec(noinline) unsigned long ForwardAppearanceColorEval(
        IRampColorRamp* Ramp, const float& OuterTime, const float& InnerTime);
    __declspec(noinline) float* ForwardAppearanceRgbEval(
        IRampColorRamp* Ramp, float* Out, const float& OuterTime, const float& InnerTime);
    __declspec(noinline) void ForwardAppearanceSinCos(const float& Angle, float* Output);

    // One case of a billboard set's kind 0x200 (+0x3c) or 0x202 (+0x40) getter: the hash
    // and the set field it hands out, AddRef'd.
    struct FBillboardRampSlot
    {
        unsigned long Hash;
        INestedSingleRamp* FAppearanceRampDescriptor::*Field;
    };

    // What one billboard set class (basic, rect, perp, oriented, dust, beam) knows about
    // its properties; the six sets share every slot body but these tables. Filled per
    // class in particlefx_appearances.cpp from the listing.
    struct FBillboardPropertyClass
    {
        // +0x04 / +0x08: the Flags (+0x60) bit table (FFxBillboardPropertySet::GetPropBool).
        const FDescriptorFlagBit* Flags;
        unsigned long FlagCount;
        // +0x3c: first match wins; no match -> the routine (Appearance_LODCurve).
        const FBillboardRampSlot* SingleRamps;
        unsigned long SingleRampCount;
        // +0x40: first match wins; no match -> -8.
        const FBillboardRampSlot* AnimatedRamps;
        unsigned long AnimatedRampCount;
        // +0x50: this hash -> LocalTransform (+0x8c) AddRef'd, any other -> the routine;
        // 0 = the class's +0x50 is the routine itself.
        unsigned long LocalTransformHash;
        // +0x54: the class table walked after the base one (the routine).
        const FNodePropertyRow* Rows;
    };

    // The six billboard set classes (FAppearanceRampDescriptor's layout): each baseline set
    // class's slots are the same bodies over its own tables (e.g. basic, outer 0x0624cd20 /
    // inner 0x0624cac8: +0x04 the routine, +0x08 the routine, +0x1c the routine, +0x20
    // , +0x30 the routine, +0x34 the routine, +0x38 the routine, +0x3c
    // , +0x40 the routine, +0x50 the routine, +0x54 the routine; the outer
    // +0x24 / +0x28 / +0x3c / +0x40 / +0x44 are own `MOV EAX,-8` bodies). Int (inner
    // +0x0c / +0x10) is -8 and float (+0x14 / +0x18) the base set's in all six.
    class FFxBillboardPropertySet final : public FAppearanceRampDescriptor
    {
    public:
        explicit FFxBillboardPropertySet(const FBillboardPropertyClass* PropertyClass);

        GENRESULT NEURONCOMPONENTS_API GetPropBool(unsigned long Hash, unsigned char* Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropBool(unsigned long Hash, unsigned char Value) override;
        GENRESULT NEURONCOMPONENTS_API GetPropPair(unsigned long Hash, unsigned long* Out2) override;
        GENRESULT NEURONCOMPONENTS_API SetPropPair(unsigned long Hash, const unsigned long* In2) override;
        GENRESULT NEURONCOMPONENTS_API GetPropName(unsigned long Hash, const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API SetPropName(unsigned long Hash, const char* Text) override;
        GENRESULT NEURONCOMPONENTS_API GetPropColorRampRamp(unsigned long Hash, IRampColorRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropSingleRampRamp(unsigned long Hash,
                                                  INestedSingleRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropAnimatedSingleRamp(unsigned long Hash,
                                                      INestedSingleRamp** Out) override;
        GENRESULT NEURONCOMPONENTS_API GetPropTransform(unsigned long Hash, IAnimatedTransform** Out) override;
        int NEURONCOMPONENTS_API EnumerateProps(FNodePropertyRowCallback Callback, void* Context) override;

    private:
        const FBillboardPropertyClass* PropertyClass_;
    };

    // The six billboard appearances: the node part. Descriptor_ is PropertySet_ typed.
    class FFxBillboardAppearance : public FGenericFxObjectBase
    {
    public:
        FFxBillboardAppearance(const char* PrimaryIid, const char* FactoryName,
                               const FBillboardPropertyClass* PropertyClass);

    protected:
        void OnPropertySetChanged() override;
        // The six creators' shared body (see particlefx_appearances.cpp): a new set of this
        // class, filled by the class's builder, adopted.
        void AdoptBillboardSet(void (*Build)(FAppearanceRampDescriptor&));

    private:
        const FBillboardPropertyClass* PropertyClass_;
    };

    // Set the routine (create the routine), tables in particlefx_appearances.cpp.
    
    // List-driven (see file header): particles live on the node's one
    // InstanceListHead_/Tail_ list; GetBoundingSphere is the routine.
    class FxBasicAppearance final : public FFxBillboardAppearance
    {
    public:
        FxBasicAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxBasicAppearance() override;

        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;

        // draw now, or queue on batch list 0 when the material batcher is on.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;
        // Outer +0xA4, the branch -> the branch: the batch flush's per-appearance fill.
        GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() override;
        // Outer +0x9C the routine / +0xA0 the routine: batch begin, and end-and-draw.
        GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long Verts, unsigned long Indices) override;
        GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() override;

        // Outer +0x14 (the routine), +0x8C (the routine). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;

        // (outer thunk 06218df0).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;

    private:
        // (descriptor flag 2 set) and the routine (clear): one camera-space
        // quad / triangle per particle of this appearance's list into the open draw context.
        void FillQuadGeometry();
        void FillTriangleGeometry();
    };

    // Set: FFxParticleAppearancePropertySet (create the routine, set vtable 0x0624e790).
    
    // Cluster A (see file header): CONFIRMED ABSENT (particlefx-wave17-lane1,
    // 2026-09-10). Real ctor is the routine/the routine (operator_new
    // 0xD4, vtable the routine — the class-descriptor vtable
    // immediately following Particle's own factory vtable the branch
    // at CreateInstance the branch). It zeros only 3 words
    // (param_1[0x2d..0x2f], NOT 6) with Pool at the LAST word
    // (param_1[0x2f] = &the field — stride 0x74, confirmed == this
    // port's own kStrideParticle/G_PoolParticle). No RefCount word exists.
    // This matches Cluster B's per-instance child-object-pool shape (see
    // particlefx_genericfx.cpp above EmitOne: "this[0x2f] private pool /
    // this+0x2d list sentinel"), not Cluster A.
    
    // ALC-1 2026-09-26: the class is ported. Each "particle" is a 0x74 record from
    // G_PoolParticle on this node's list (baseline node+0xb4/+0xb8, pool +0xbc) that owns
    // two child EFFECTS created through IFxEffectLibrary::CreateEffect: the life effect
    // (ParticleApp_LifeName, +0x6c) at spawn and the death effect (ParticleApp_DeathName,
    // +0x70) once the record has aged out. The appearance draws nothing itself (outer
    // +0x18 the routine returns 0): the children are ordinary effects. Outer vtable
    // , node (this+0xc) vtable 0x0624e530.
    // A G_PoolParticle record (stride 0x74). the routine fills +0x08..+0x2c and
    // +0x6c/+0x70, the spawn the routine +0x30..+0x68.
    struct FParticleAppRecord
    {
        FPooledParticle Base;   // Next, Prev, owner node, Six, Age, Duration, 1/Duration
        // Raw spawn words; the complete field Apply contract exposes only +0x0c..+0x27.
        // Particle's own operations never consume these six seeded words.
        std::uint32_t UnusedSpawnWords_0x30[3];
        float BlendQuat[4];     // +0x3c, w x y z: the SmoothRotation slerp state
        std::uint32_t UnusedSpawnWords_0x4c[3];
        float VelQuat[4];       // +0x58, w x y z: orientation from the velocity
        float Blend;            // +0x68
        IFxEffect* LifeEffect;  // +0x6c
        IFxEffect* DeathEffect; // +0x70
    };
    static_assert(sizeof(FParticleAppRecord) == 0x74, "baseline particle record stride");

    class FxParticleAppearance final : public FGenericFxObjectBase
    {
    public:
        FxParticleAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxParticleAppearance() override;

        // Outer +0x10 the routine -> the routine: death effects, then the routine.
        GENRESULT NEURONCOMPONENTS_API ComposeNodeTransform(void* Unused, const float* Parent12) override;
        // Outer +0x14 the routine -> the routine: age, fields, records.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // Outer +0x5C the routine -> the routine / +0x88 the routine -> the routine.
        GENRESULT NEURONCOMPONENTS_API SetNodeOuterTime(float OuterTime) override;
        GENRESULT NEURONCOMPONENTS_API SetNodeLodInput(float LodInput) override;
        // Outer +0x8C the routine -> the routine: the children's bounds merged.
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;
        // Outer +0xBC the routine -> the routine: spawn one record + life effect.
        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;

        // (outer thunk 06231e10).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48: the routine. The set's slots (inner 0x0624e790) are
        // FFxParticleAppearancePropertySet's (particlefx_types.h).
        void CreatePropertySet() override;
        void OnPropertySetChanged() override;

    private:
        static void ForwardEffectFloat(IFxEffect* Effect,
                                       GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float),
                                       const float& Value);
        void ForwardChildFloat(GENRESULT (NEURONCOMPONENTS_API IFxEffect::*Method)(float),
                               const float& Value);

        FFxParticleAppearancePropertySet* ParticleProps() const
        {
            return ParticlePropertySet_;
        }

        // Node +0xc0: borrowed typed cache; clones read the source after callbacks.
        FFxParticleAppearancePropertySet* ParticlePropertySet_;

        // (node this, record, Dt): move, orient, hand the life effect its
        // transform.
        __declspec(noinline) void IntegrateRecord(FPooledParticle* Record, const float& Dt);
        // on the node's list: unlink, tear the record down (the routine,
        // inside FreePoolItem) and give it back to the pool. Returns the next record.
        FPooledParticle* EraseRecord(FPooledParticle* Record);
    };

    // Set the routine (create the routine).
    
    // List-driven (see file header). GetBoundingSphere is the routine, the side-length
    // member of the list-bounds family (ListBoundsSphere, particlefx_appearances.cpp): the
    // radius is the larger side ramp (+0x7c / +0x80, B > C ? B : C) times the
    // +0x78 ramp times the EffectScaleRamp value. (wave90's reading -- a discarded
    // SizeRamp and a node+0x8c scale -- is superseded by that trace.)
    class FxRectAppearance final : public FFxBillboardAppearance
    {
    public:
        FxRectAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxRectAppearance() noexcept(false) override;

        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;

        // draw now, or queue on batch list 1 when the material batcher is on.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;
        // Outer +0x9C the branch, +0xA0 the routine, +0xA4 the routine (-> the routine).
        GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long Verts, unsigned long Indices) override;
        GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() override;
        GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() override;

        // Outer +0x14 (the routine), +0x8C (the routine). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;
        // , outer +0xA8: fixed unsupported result, no side effects.
        GENRESULT NEURONCOMPONENTS_API Pad42() override { return static_cast<GENRESULT>(0xfffffff9); }

        // (outer thunk 0623ac80).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;

    private:
        // one node-local quad per particle into the open draw context.
        void FillGeometry();
    };

    // Set the routine (create the routine).
    
    // List-driven (see file header); GetBoundingSphere is the routine.
    
    // wave68 (sonnet:particlefx-wave68-lane1, 2026-09-11): real override of
    // the new port-only IGenericFx::DrawOrEnqueue slot (particlefx_types.h),
    // reproducing the routine's own per-instance draw dispatch (analysis-
    // tracker item 134306). wave69 (sonnet:particlefx-wave69-lane2,
    // 2026-09-11) closed the two pieces wave68 left open: the
    // material-batcher gate (the field == already-ported
    // G_Defaults.bUseMaterialBatcher, IParticleFx::SetUseMaterialBatcher/
    // GetUseMaterialBatcher) and the DIP Begin/Fill/End/state/Draw
    // lifecycle (confirmed fully self-contained inside baseline's own
    // function, wired via this port's existing AlDrawContextBegin/End/Draw).
    // See the doc comment on this override (particlefx_appearances.cpp) for
    // the full byte trace. DrawOrEnqueue is IGenericFx +0x18 (outer thunk
    // @0624e9e8), reached from the IParticleFx draw walk the branch.
    class FxPerpAppearance final : public FFxBillboardAppearance
    {
    public:
        FxPerpAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxPerpAppearance() override;

        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;
        // draw now, or queue on batch list 2 when the material batcher is on.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;
        // Outer +0x9C the routine, +0xA0 the routine, +0xA4 the routine (-> the routine).
        GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long Verts, unsigned long Indices) override;
        GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() override;
        GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() override;

        // Outer +0x14 (the routine), +0x8C (the routine). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;

        // (outer thunk 06235730).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;

    private:
        // one node-local quad per particle into the open draw context.
        void FillGeometry();
    };

    // Set: FFxMeshAppearancePropertySet (the routine, create the routine, set vtable
    // ). Overrides the node compose (the routine)
    // to pose each instance's IFxMesh after composing its own transform; that
    // is the only place baseline poses the meshes.
    
    // Cluster A (see file header): CONFIRMED ABSENT (particlefx-wave17-lane1,
    // 2026-09-10). Real ctor is the routine/the routine (operator_new
    // 0xD4, vtable the routine — the class-descriptor vtable following
    // Mesh's factory vtable the branch at CreateInstance
    // ). Same reduced 3-word {link,?,Pool} pattern as
    // FxParticleAppearance above (param_1[0x2d..0x2f], Pool =
    // param_1[0x2f] = &the field, stride 0x34, == this port's
    // G_PoolMesh) — no RefCount word. Cluster B shape, not Cluster A.
    class FxMeshAppearance final : public FGenericFxObjectBase
    {
    public:
        FxMeshAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxMeshAppearance() override;

        // (vtable 0x0624dd90 +0x68): the mesh emit-one. Asks the bound
        // IFxMeshFactory (the field) for the particle's IFxMesh; see the body.
        GENRESULT NEURONCOMPONENTS_API EmitOne(const float* Six, unsigned long Extra,
                                    float Duration) override;
        // Outer +0xBC is the routine, the this+0xc thunk into the routine -- the
        // body EmitOne above holds. Until 2026-09-26 +0xBC fell through to the
        // base default, so emitters never reached the mesh spawn.
        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override
        {
            unsigned long AgeBits = 0;
            std::memcpy(&AgeBits, &Age, sizeof(AgeBits));
            return EmitOne(Six, AgeBits, Duration);
        }
        // Outer +0x14 -> the routine. See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;

        // (node vtable 0x0624dd90 slot 2; outer thunk the routine,
        // this+0xc): the routine, then pose every instance's IFxMesh.
        GENRESULT NEURONCOMPONENTS_API ComposeNodeTransform(void* Unused, const float* Parent12) override;
        // Outer +0x18 the routine -> the routine: each instance's IFxMesh draws itself.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;

        // (outer thunk 06226ed0).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

        // (inner +0x34), raw center/radius output.
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;

    protected:
        // Node slot +0x48: the routine. The set's slots (inner 0x0624dff0) are
        // FFxMeshAppearancePropertySet's (particlefx_types.h).
        void CreatePropertySet() override;

    private:
        FFxMeshAppearancePropertySet* MeshProps() const
        {
            return MeshPropertySet_;
        }

        FFxMeshAppearancePropertySet* MeshPropertySet_; // inner +0xc0, non-owning cache.
    };

    // Set the routine (create the routine).
    
    // List-driven (see file header); GetBoundingSphere is the routine. (wave108
    // concluded this class had no bounds function because it found no FSQRT in the
    // class's range. None of the five copies uses FSQRT: the distance is the
    // reciprocal square root the routine times LenSq.)
    class FxOrientedAppearance final : public FFxBillboardAppearance
    {
    public:
        FxOrientedAppearance(const char* PrimaryIid, const char* FactoryName);
        ~FxOrientedAppearance() override;

        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;

        // one function that draws every particle of the node immediately
        // (oriented has no material-batcher branch -- the four the routine call sites
        // are basic/rect/perp/beam). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;
        // Outer +0x14 (the routine), +0x8C (the routine via the routine). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;

        GENRESULT NEURONCOMPONENTS_API GetQueuedVertCount(unsigned long* Out) override;

        GENRESULT NEURONCOMPONENTS_API GetQueuedIndexCount(unsigned long* Out) override;

        // (outer thunk 0622ea00).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;
    };

    // FLDustAppearance. CORRECTED 2026-09-26 (ALC-1): the outer ctor installs
    // (0x0620f651) with the node vtable the routine at outer+0xc
    // (node ctor the routine). Outer +0x14 the routine -> node the routine (update),
    // outer +0x18 the routine -> node +0x10 the routine (draw), outer +0xBC
    // (spawn), outer +0xD0 / +0xD4 the routine / the routine (count * 4 /
    // count * 6). The earlier wave108 reading (slot 47 = the routine at 0x0624c0fc)
    // indexed the wrong table: the routine is 0x58 bytes long and 0x0624c0fc is
    // node-vtable the routine + 0x64.
    // Node fields beyond the shared list layout (node+0xd8 head, +0xdc tail, +0xe0 pool,
    // +0xe4 count, +0xe8 descriptor), all set by the routine:
    //   +0xec  12 floats, the view transform of the last draw (ctor: identity);
    //   +0x11c the last update's Dt (ctor: 0.05, 0x3d4ccccd);
    //   +0x120 the camera position of the last draw, +0x12c its view-space step length
    //          (ctor: 0).
    // Particles carry their previous view-space position at +0x30 (pool stride 0x3C).
    class FLDustAppearance final : public FFxBillboardAppearance
    {
    public:
        FLDustAppearance(const char* PrimaryIid, const char* FactoryName);

        // Outer +0x14 -> the routine, the per-node update. See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // Outer +0xBC the routine, the spawn. See the .cpp.
        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;
        // Outer +0x18 -> the routine: draws every particle at once, in view space (no
        // batcher branch). See the .cpp.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;

        GENRESULT NEURONCOMPONENTS_API GetQueuedVertCount(unsigned long* Out) override;

        GENRESULT NEURONCOMPONENTS_API GetQueuedIndexCount(unsigned long* Out) override;

        // (outer thunk 06212270).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;

    private:
        float ViewXform_[12];    // node+0xec
        float LastDt_;           // node+0x11c
        float LastCameraPos_[3]; // node+0x120
        float LastCameraStep_;   // node+0x12c
    };

    // Set the routine (create the routine).
    
    // (+0xBC), node twin the branch: evict the pool's oldest used
    // item when full, allocate/fill through the routine, prepend to the owning
    // node's list, then toggle the ribbon parity byte. This list is the same one
    // used by update and draw; there is no separate private particle list.
    
    // Draw: the routine (batch list 3), outer +0x9C/+0xA0/+0xA4 through the
    // node vtable the routine (+0x4c the branch, +0x50 the branch,
    // +0x54 the branch), draw state the routine, counts +0x7c the branch /
    // +0x80 the branch. See the .cpp.
    // Outer +0x64 on this vtable (the branch + 0x64 = the branch,
    // `FLD [ESP+8]; FSTP [outer+0x90]`) is the node Age store: the appearance
    // node sits at outer+0xC, so outer+0x90 = node+0x84. The shared inline
    // SetNodeAge covers it; the former Pad25 override and its
    // EffectOuterScale_ field (the same dword under a guessed name) were
    // removed 2026-09-26.
    class FLBeamAppearance final : public FFxBillboardAppearance
    {
    public:
        FLBeamAppearance(const char* PrimaryIid, const char* FactoryName);

        GENRESULT NEURONCOMPONENTS_API Pad47(const float* Six, float Age, float Duration) override;

        // wave124 (sonnet:wave124:beampool, 2026-09-15): real override of the
        // IGenericFx::Pad5 slot, reproducing baseline the routine (implementation tracker
        // item 132227): the appearance per-node update on outer +0x14, run by
        // the IParticleFx Slot16 walk every frame. `*(outer+0x90) += Dt` is Age_.
        GENRESULT NEURONCOMPONENTS_API Pad5(float Dt) override;
        // Outer +0x8C the routine (`ADD [ESP+4],0xc; JMP`) -> the routine. See the .cpp.
        GENRESULT NEURONCOMPONENTS_API GetBoundingSphere(float* OutSphere) override;

        // draw now, or queue on batch list 3 when the material batcher is on.
        GENRESULT NEURONCOMPONENTS_API DrawOrEnqueue() override;
        // / the branch / the branch (-> the routine).
        GENRESULT NEURONCOMPONENTS_API BeginBatchedDraw(unsigned long Verts, unsigned long Indices) override;
        GENRESULT NEURONCOMPONENTS_API EndBatchedDraw() override;
        GENRESULT NEURONCOMPONENTS_API FillBatchedGeometry() override;
        // / the branch: 5 verts / 0x18 indices per particle, one more
        // unit when descriptor flag 0x100 is set.
        GENRESULT NEURONCOMPONENTS_API GetQueuedVertCount(unsigned long* Out) override;
        GENRESULT NEURONCOMPONENTS_API GetQueuedIndexCount(unsigned long* Out) override;

        // (outer thunk 0620df10).
        GENRESULT NEURONCOMPONENTS_API CloneNode(INeuronComponent** Out) override;

    protected:
        // Node slot +0x48 the routine: the set the routine.
        void CreatePropertySet() override;

    private:
        // 's stack struct at [ESP+0x6c], handed to the routine / the routine
        // as ECX: the previous ring's four vertex indices (Idx[0] == 0xffff until the first
        // ring), the ring counter, the node, the ribbon basis MulTransform(camera vt+0x10,
        // node+0x4c) and the previous ring's centre.
        struct FBeamRingState
        {
            unsigned short Idx[4];  // +0x00
            long Counter;           // +0x08
            FLBeamAppearance* Node; // +0x0c
            float Xform[12];        // +0x10
            float PrevPos[3];       // +0x40
        };

        // blend, texture, fill mode (scoped) and the node's view transform.
        void ApplyDrawState();
        // the ribbon over the particle list (newest first).
        void FillRibbon();
        // half width, {R, G, B, A} and the ring's V for particle P.
        void EvalRingParams(const FBeamRingState& State, const FPooledParticle* P, float* OutHalfWidth,
                            float* OutRgba, float* OutV);
        // The callers' packing of that {R, G, B, A} into a diffuse colour.
        static unsigned long PackRingColor(const float* Rgba);
        // one ring of four vertices at Pos, joined to the previous ring.
        void EmitRing(FBeamRingState& State, const FPooledParticle* P, const float* Pos);
        // The join both of them inline once Idx[0] != 0xffff: flag 2 -> a centre vertex and
        // eight triangles (the routine order), else two quads (the routine order).
        void JoinRing(const FBeamRingState& State, const float* Pos, unsigned long Diffuse,
                      const unsigned short* NewIdx);

        // node+0xe1: cleared by the node ctor the routine, flipped by every spawn
        // (the routine / 0x06205606); the routine / the routine pick V0 or V1 by
        // comparing it with the ring counter's low bit.
        bool bRibbonParity_;
    };

} // namespace ParticleFxImpl
