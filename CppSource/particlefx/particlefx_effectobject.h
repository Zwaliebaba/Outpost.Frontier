// Author: Giperion.

#pragma once

#include "particlefx_types.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// FFxEffectObject: the IFxEffect component (instantiated node tree bound to
// an FFxEffectArchetype).
namespace ParticleFxImpl
{
    class FFxEffectObject final : public IFxEffect
    {
    public:
        FFxEffectObject()
            : RefCount_(1), Archetype_(nullptr), TransformExtra_(nullptr), Nodes_(nullptr),
              NodeCount_(0), NodeCapacity_(0), RootNodeCount_(0)
        {
        }

        ~FFxEffectObject() noexcept(false);

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        // (particlefx, raw .txt:64408-6469f — read and
        // verified against baseline bytes this wave, not just the inspected C):
        // param_1[6] (this+0x18, TransformExtra_) is an override target. If set,
        // tail-call just that ONE node's SetTransform (vtable+0x3C) with (A,B) and
        // return its own result. Else walk every node in Nodes_[], skip any
        // vtable+0x1C-disabled one, call SetTransform(node,A,B) on the rest, and
        // FDUMP a "Failed to set node transform. Continuing." warning (code
        // 0x100002) per call that doesn't return GR_OK — the call's own result is
        // otherwise ignored and the loop always finishes returning GR_OK.
        // Slot3 never writes this+0x18. A is the node transform slot index (0 composed,
        // 1 raw -- the routine), B the 12-float source; ScriptPsys Tick passes (1, Xform).
        // A prior port's TransformExtra_=B store was removed 2026-09-23.
        GENRESULT NEURONCOMPONENTS_API Slot3(void* A, void* B) override;
        // this+0x18 (TransformExtra_) override target takes priority
        // (JMP straight to *TransformExtra_'s IFxNode+0x40 with (A,B)) — this half
        // was not previously ported (Slot4 always used Nodes_[0].Node). Else JMP
        // first list node IFxNode+0x40 (same stack, RET 0xC); no count or null test,
        // as in baseline (the port-only "return a cached effect transform" fallback
        // was removed 2026-09-26).
        GENRESULT NEURONCOMPONENTS_API Slot4(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot5(float Age) override;
        GENRESULT NEURONCOMPONENTS_API Slot6(float OuterTime) override;
        GENRESULT NEURONCOMPONENTS_API Slot7(void* Arg) override;
        GENRESULT NEURONCOMPONENTS_API Slot8(void* Arg) override;
        GENRESULT NEURONCOMPONENTS_API Slot9(void* Arg) override;
        // find list rec+8 == id, *out = rec[0]. Empty → -1.
        GENRESULT NEURONCOMPONENTS_API Slot10(void* IdRaw, void* Out) override;
        // empty list → AL=1 (kept). Non-empty: per-node flag
        // dispatch to +0x94/+0x98/+0x90, aborting (return 0) on flags==7 or
        // on any callback out-flag. See WalkNodesFlagged.
        GENRESULT NEURONCOMPONENTS_API Slot11(void* Arg) override;
        // per-node {Id,Node} enumeration callback. See WalkNodesEnumerate.
        GENRESULT NEURONCOMPONENTS_API Slot12(void* A, void* B) override;
        // analysis thiscall (this, effect, flags) RET 8. No in-DLL
        // caller of this vtable slot — keep stdcall Slot13(void*). flags!=7:
        // same +0x94/+0x98/+0x90 shape as Slot11 but never aborts, and
        // submits the node to IParticleFx on a set out-flag (DispatchFlagsToParticleFx).
        GENRESULT NEURONCOMPONENTS_API Slot13(void* Arg) override;
        // per-node call to node+0x88(node, Arg). See WalkNodes.
        GENRESULT NEURONCOMPONENTS_API Slot14(float LodInput) override;
        // combined bounding-sphere merge over node+0x8C
        // spheres (Ritter two-sphere merge). See MergeNodeBoundsSphere.
        GENRESULT NEURONCOMPONENTS_API Slot15(void* Arg) override;
        GENRESULT NEURONCOMPONENTS_API GetBoundComponent(void** Out) override;
        // thiscall (this, flag): Release +0x1c; flag&1 → delete.
        // Port: unbind only. Lifetime stays on Release (factory fail path).
        GENRESULT NEURONCOMPONENTS_API Slot17() override;

        GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) override;

        void BindArchetype(IFxEffectArchetype* Archetype);
        void Unbind();

    private:
        // empty list → 0. Else CALL [node+Off](node, arg) stdcall RET 8.
        // Slot 5 Off=0x64 = SetNodeAge (node+0x84 store, e.g. cone the branch).
        // Slot 6 (the branch) Off=0x5C = SetNodeOuterTime (node+0x8c).
        // Slot 14 (the branch) Off=0x88 = SetNodeLodInput (node+0x90).
        // , the routine, the routine and the routine are each a
        // single-offset walk (0x5C / 0x3C / 0x40 / 0x44); none also calls 0x64.

        // / 730 / 790: if node+0x1C AL==0, ParticleFx+Off(dt, node).
        GENRESULT WalkNodes(GENRESULT (NEURONCOMPONENTS_API IGenericFx::*Method)(float), float Value);
        GENRESULT WalkNodesParticleFx(GENRESULT (NEURONCOMPONENTS_API IParticleFx::*Method)(void*, void*),
                                   void* Arg);

        // empty list → 1. Else per node (skipping a self match
        // against *TransformExtra_, then any node+0x1C-disabled node): flags==7
        // aborts (return 0) immediately; else each set bit in {1,2,4} calls
        // node+0x94/+0x98/+0x90(node, &outFlag) and aborts (return 0) if the
        // callee sets outFlag. Falling through the whole list returns 1.
        GENRESULT WalkNodesFlagged(unsigned long Flags);

        // , flags != 7: same +0x94/+0x98/+0x90 shape as
        // WalkNodesFlagged but never aborts; a set out-flag additionally
        // submits the node to IParticleFx::SubmitNode (+0x5C, the field).
        // flags==7 submits the enabled root prefix, then destroys all node
        // records and sets vector end=begin without freeing its capacity.
        GENRESULT DispatchFlagsToParticleFx(unsigned long Flags);

        // for each node build a {Id, Node} pair and invoke the
        // caller-supplied CDECL Callback(&pair, Context) until it returns
        // nonzero or the list is exhausted. Always returns GR_OK.
        GENRESULT WalkNodesEnumerate(void* Callback, void* Context);

        // merge the captured node range's float[4] spheres.
        // Distance uses 0x5f3759df and one Newton refinement. The float
        // stores, strict containment tests and unordered epsilon branch
        // are retained; no contributing sphere returns GR_GENERIC.
        GENRESULT MergeNodeBoundsSphere(void* Arg);

        // recursive max-duration walk over a sibling chain.
        // Per sibling: read its own duration via +0x68 (out-param accessor;
        // this call shape differs from IGenericFx::EmitOne's declared 3-arg
        // form at the same offset, so it is read through a local vtable
        // cast rather than the interface's virtual), recurse into its child
        // (+0x2C) for the subtree below it, and continue to the next
        // sibling (+0x34). Returns the max duration seen anywhere in the
        // chain/subtree.
        float ComputeMaxSubtreeDuration(INeuronComponent* FirstSibling);

        // / the routine per-node callback: calls
        // node+Off(node, &OutFlag) (OutFlag zeroed first) and returns
        // whatever the callee set. The call's own GENRESULT return value is
        // ignored by baseline in both slots.
        static unsigned char CallNodeFlagSlot(
            INeuronComponent* Node, GENRESULT (NEURONCOMPONENTS_API IGenericFx::*Query)(unsigned char*));

        // enumeration record built per node for Slot12's callback.
        struct FNodeIdPair
        {
            unsigned long Id;
            INeuronComponent* Node;
        };

        void InsertNodeRecords(unsigned long InsertIndex, unsigned long InsertCount,
                               const FFxEffectNodeRec& Record);
        FFxEffectNodeRec* InsertSingleNodeRecord(unsigned long InsertIndex,
                                                const FFxEffectNodeRec& Record);
        FFxEffectNodeRec* NodeRecordsEnd() const;
        GENRESULT InstantiateNodesFromArchetype(IFxEffectArchetype* InitialArchetype,
                                               const FFxEffectCreateDesc* Desc);
        INeuronComponent* FindNodeById(unsigned long Id) const;

        // root parent comes from the descriptor; the captured end is
        // reused after the association phase for the duration walk.
        FFxEffectNodeRec* AttachParticleFxTree(const FFxEffectCreateDesc* Desc);

        // after GetRec8Count (+0x30) / GetRec8 (+0x38):
        // rec8.A / rec8.B are node ids. QI A as IFxEmitter -> B as
        // IFxAppearance, then [emitter+0x9C](appearance), release both
        // temp interfaces. Else QI A as IFxAppearance -> B as IFxField,
        // then [appearance+0xAC](field), release both temp interfaces.
        // Else FDUMP line 0x17b. Wave-61 re-audit (byte-for-byte re-trace
        // of particlefx/particlefx): baseline
        // does NOT persist these pairs anywhere -- every QI'd interface is
        // released immediately after the single associate call, and
        // FFxEffectObject's own destructor (the routine/the routine,
        // particlefx/67278-67332) only ever releases
        // Archetype_ (+0x1c) and the node list (+0xc/+0x10) -- no second
        // array is ever walked or freed. A pre-wave-61 version of this port
        // invented a persistent Associates_ array (StoreRec8Assoc/
        // ClearAssociates) that AddRef'd both sides of every pair and held
        // them until the next rebind or teardown, plus a HintAppearanceCounts
        // helper that called the (real, elsewhere-verified) IGenericFx::
        // SetLiveInstanceCount with a guessed hint -- neither has any baseline
        // call site in this loop. Removed: this now matches baseline's exact
        // QI/use/release-once shape with no extra held references.
        void BindRec8Associates(IFxEffectArchetype* InitialArchetype);

        void ClearNodes();
        void DestroyNodeRecords();

        unsigned long RefCount_;
        IFxEffectArchetype* Archetype_;
        FFxEffectNodeRec* TransformExtra_;
        FFxEffectNodeRec* Nodes_;
        unsigned long NodeCount_;
        unsigned long NodeCapacity_;
        long RootNodeCount_;
    };

} // namespace ParticleFxImpl
