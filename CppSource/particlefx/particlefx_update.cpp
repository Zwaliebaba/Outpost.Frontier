// Author: Giperion.

#include "particlefx_update.h"
#include "msvcrt_compat.h" // MsvcrtAtoi: baseline MSVCRT.DLL wraps mod 2^32, the UCRT saturates

#include "particlefx_common.h"
#include "particlefx_genericfx.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// Out-of-line method bodies for FParticleFx.
namespace ParticleFxImpl
{
    FParticleFx::FnUpdateCallback G_UpdateCallback = nullptr; 
    FParticleFx::FnFilterCallback G_FilterCallback = nullptr; 
    IParticleFx* G_InitializedParticleFx = nullptr; 

    namespace
    {
        // DrawNodes / the branch Slot15 / the branch Slot16
        // / the routine FindNode all walk the node tree the same way:
        // visit, then first child, else next sibling, else climb ancestors
        // (stopping at StartNode) looking for an uncle sibling. Shared here
        // so each gap only supplies its own per-node dispatch.
        template <typename VisitFn>
        void WalkNodePreOrder(INeuronComponent* StartNode, VisitFn&& Visit)
        {
            INeuronComponent* Node = StartNode;
            while (Node != nullptr)
            {
                if (!Visit(Node))
                    return;
                INeuronComponent* Next = CallGetFirstChild(Node); // GetFirstChild
                if (Next != nullptr)
                {
                    Node = Next;
                    continue;
                }
                if (Node == StartNode)
                {
                    Node = nullptr;
                    continue;
                }
                Next = CallGetNextSibling(Node); // GetNextSibling
                if (Next != nullptr)
                {
                    Node = Next;
                    continue;
                }
                INeuronComponent* Parent = CallGetAttachParent(Node);
                Node = nullptr;
                while (Parent != nullptr && Parent != StartNode)
                {
                    INeuronComponent* Sib = CallGetNextSibling(Parent);
                    Parent = CallGetAttachParent(Parent);
                    if (Sib != nullptr)
                    {
                        Node = Sib;
                        break;
                    }
                }
            }
        }

        // (particlefx): the 12-float parent transform the node
        // walk hands to every node's compose. Seeded to identity by the static initializer
        // (0x06255010 in the init table) and re-seeded by the branch.
        float G_NodeWalkXform[12] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f};

        // (particlefx, __thiscall this=IParticleFx, RET 8): for
        // Node and each following sibling (vt+0x34): vt+0x10 ComposeNodeTransform(A,
        // &G_NodeWalkXform). When the node has a first child (vt+0x2c): vt+0x40
        // GetNodeTransform(0, &p), save G, G = *p (12 dwords, REP MOVSD), recurse on
        // the child, restore G. The node's composed world transform REPLACES the parent
        // accumulator for its subtree. Node == nullptr returns at once (06201628 JZ).
        void PropagateNodeChainTransform(INeuronComponent* Node, void* A)
        {
            while (Node != nullptr)
            {
                IGenericFx* const Fx = AsFxNode(Node);
                Fx->ComposeNodeTransform(A, G_NodeWalkXform);
                INeuronComponent* const Child = Fx->GetFirstChild();
                if (Child != nullptr)
                {
                    float* World = nullptr;
                    Fx->GetNodeTransform(0, &World);
                    float Saved[12];
                    std::memcpy(Saved, G_NodeWalkXform, sizeof(Saved));
                    std::memcpy(G_NodeWalkXform, World, sizeof(G_NodeWalkXform));
                    PropagateNodeChainTransform(Child, A);
                    std::memcpy(G_NodeWalkXform, Saved, sizeof(G_NodeWalkXform));
                }
                Node = Fx->GetNextSibling();
            }
        }

        // (particlefx) / the branch Slot16:
        // per-node vtbl+0x14(A) then vtbl+0x20(&flag). When flag bit0 is set,
        // baseline calls **ParticleFx+0x5C SubmitNode(node)** (EBX/this = ParticleFx,
        // PUSH node then ParticleFx) — NOT A->+0x5C. A is the dt float* from
        // IFxEffect Slot8; the prior port's CallVt0(A, 0x5C) followed a
        // analysis misread and would dispatch on the float pointer.
        void WalkNodeChainFlagDispatch(FParticleFx* ParticleFx, INeuronComponent* Node, void* A)
        {
            while (Node != nullptr)
            {
                INeuronComponent* Next = CallGetNextSibling(Node);
                // +0x14 per-node update (A carries the dt float bits), then
                // +0x20 GetNodeFlags: baseline stores the whole node+4 dword.
                float Dt = 0.0f;
                std::memcpy(&Dt, &A, sizeof(float));
                AsFxNode(Node)->Pad5(Dt);
                unsigned long Flag = 0;
                AsFxNode(Node)->GetNodeFlags(&Flag);
                if ((Flag & 1) != 0)
                {
                    ParticleFx->SubmitNode(Node);
                }
                else
                {
                    INeuronComponent* Child = CallGetFirstChild(Node);
                    if (Child != nullptr)
                        WalkNodeChainFlagDispatch(ParticleFx, Child, A);
                }
                Node = Next;
            }
        }
    } // namespace

    // fixed particle-draw render-state and texture-stage
    // block that baseline applies on the render
    // pipeline. Called from baseline FParticleFx::BeginDraw at 0x06201b5e,
    // right after the six CallGetRenderState saves and before the
    // draw-counter reset (confirmed by walking the listing around that
    // call site: the six GetRenderState reads at 0x06201ae4-0x06201b58
    // save exactly SavedRsAlphaTest_/ColorWrite_/Cull_/AlphaBlend_/
    // SrcBlend_/DestBlend_ in that order, then CALL the routine, then
    // the the field..06257904 nine-dword counter zeroing that matches
    // BeginDraw's G_DrawCounters reset below). Two other XREFs
    // (the branch, the branch) are FALBatchedMaterialFacet::Slot6 and
    // FALMaterial::Slot8 (particlefx_material.cpp) -- real vtable methods on
    // the separate FALMaterial component, not pad slots as an earlier pass
    // assumed; they call this identical function with no other arguments,
    // so it is exposed via particlefx_update.h (declared, not anonymous-
    // namespace-local) for that file to call directly instead of
    // duplicating it.
    
    // Literal state ids/values and texture-stage (stage, type, value)
    // triples match the listing; baseline never
    // reverses any of this at EndDraw (its epilogue is only the six
    // SetRenderState restores, already ported below), so no EndDraw
    // change is needed.
    void ApplyParticleDrawRenderState()
    {
        G_RenderPipeline->SetRenderState(D3DRS_LIGHTING, 0); // color write mask: off
        G_RenderPipeline->SetRenderState(D3DRS_ZWRITEENABLE, 0); // cull mode: none
        G_RenderPipeline->SetRenderState(D3DRS_ALPHABLENDENABLE, 1); // alpha blend: on
        G_RenderPipeline->SetRenderState(D3DRS_CULLMODE, 1); // alpha test: on

        G_RenderPipeline->SetTextureStageState(0, 1, 4);
        G_RenderPipeline->SetTextureStageState(0, 2, 2);
        G_RenderPipeline->SetTextureStageState(0, 3, 0);
        G_RenderPipeline->SetTextureStageState(0, 4, 4);
        G_RenderPipeline->SetTextureStageState(0, 5, 2);
        G_RenderPipeline->SetTextureStageState(0, 6, 0);
        G_RenderPipeline->SetTextureStageState(0, 0x0B, 0);
        G_RenderPipeline->SetTextureStageState(0, 0x0D, 3);
        G_RenderPipeline->SetTextureStageState(0, 0x0E, 3);
        G_RenderPipeline->SetTextureStageState(1, 1, 1);
        G_RenderPipeline->SetTextureStageState(1, 4, 1);

        G_RenderPipeline->SetTextureStageTexture(1, nullptr);
    }

    FParticleFx::~FParticleFx() noexcept(false)
    {
        struct FInitNodeOwner
        {
            FParticleFx* Owner;

            void Release()
            {
                FParticleFx* Current = Owner;
                Owner = nullptr;
                if (Current->InitNode_ != nullptr)
                {
                    Current->InitNode_->Release();
                    Current->InitNode_ = nullptr;
                }
            }

            ~FInitNodeOwner() noexcept(false)
            {
                if (Owner != nullptr)
                    Release();
            }
        } InitNodeOwner{this};

        // submit the InitNode child chain before releasing
        // the node or node-library storage. SubmitNodeChain preserves the
        // cached sibling, child-first tagging, global callback and detach.
        SubmitNodeChain(AsFxNode(InitNode_)->GetFirstChild());
        InitNodeOwner.Release();
        ClearAttaches();
        NodeRegistry_.Destroy();
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::FInnerUnknown::QueryInterface(const char* Iid, void** Out)
    {
        // resolve against the owner, then AddRef the selected
        // delegating interface before publishing it.
        if (IidEqual(Iid, Iids::ParticleFx) || IidEqual(Iid, Iids::FxNodeLibrary) ||
            IidEqual(Iid, Iids::NeuronComponent))
        {
            IParticleFx* const Interface = static_cast<IParticleFx*>(Owner_);
            Interface->AddRef();
            *Out = Interface;
            return GR_OK;
        }
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }

    unsigned long NEURONCOMPONENTS_API FParticleFx::FInnerUnknown::AddRef()
    {
        return ++RefCount_;
    }

    unsigned long NEURONCOMPONENTS_API FParticleFx::FInnerUnknown::Release()
    {
        if (RefCount_ != 0)
            --RefCount_;
        const unsigned long Next = RefCount_;
        if (Next == 0)
        {
            RefCount_ = 1;
            if (Owner_ != nullptr)
                delete Owner_;
            return 0;
        }
        return Next;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::QueryInterface(const char* Iid, void** Out)
    {
        return Controller_->QueryInterface(Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API FParticleFx::AddRef()
    {
        return Controller_->AddRef();
    }

    unsigned long NEURONCOMPONENTS_API FParticleFx::Release()
    {
        return Controller_->Release();
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::LoadNodeLibrary(IFileSystem* Filesys)
    {
        return NodeRegistry_.Load(this, Filesys);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SaveNodeLibrary(IFileSystem* Filesys)
    {
        FAleChunk Chunk;
        Chunk.Filesys = Filesys;
        Chunk.DeleteNode(kNodeLibName);
        Chunk.OpenNode(kNodeLibName);
        Chunk.OpenChunk(Filesys, kNodeLibName, 1);
        const unsigned long VersionBits = kAleVersion11Bits;
        Chunk.WriteBytes(&VersionBits, 4);
        Chunk.WriteBytes(NodeRegistry_.CountAddress(), 4);
        INeuronComponent* Node = NodeRegistry_.First();
        while (Node != nullptr)
        {
            WriteNodeLibProperties(&Chunk, Node);
            Node = AsFxNode(Node)->GetNextSibling();
        }
        Chunk.OpenNode(kAleLeaveNode);
        Chunk.CloseChunk();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot6(const char* Name, INeuronComponent* Node,
                                                       unsigned long* OutHash)
    {
        return InsertNodeByName(Name, Node, OutHash);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot7(unsigned long Hash)
    {
        return RemoveNodeByHash(Hash);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot8(FNodeLibraryEnumCallback Callback, void* Context)
    {
        return NodeRegistry_.Enumerate(Callback, Context);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::ClearNodeLibrary()
    {
        ClearNodeList();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot10(void* NameRaw, void* OutRaw)
    {
        // borrowed original name or the independent the field buffer.
        const char* CurrentName = static_cast<const char*>(NameRaw);
        const char** Out = static_cast<const char**>(OutRaw);
        char Base[256];
        char Ext[32];
        int Suffix = 0;
        while (FindNodeEntry(HashNodeName(CurrentName)) != nullptr)
        {
            if (Suffix == 0)
            {
                std::strcpy(Base, CurrentName);
                char* HashMark = std::strrchr(Base, '#');
                if (HashMark != nullptr)
                {
                    *HashMark = '\0';
                    Suffix = MsvcrtAtoi(HashMark + 1);
                }
                const char* Dot = std::strrchr(CurrentName, '.');
                if (Dot != nullptr)
                    std::strcpy(Ext, Dot);
                else
                    Ext[0] = '\0';
            }
            unsigned int SuffixBits;
            std::memcpy(&SuffixBits, &Suffix, sizeof(SuffixBits));
            ++SuffixBits;
            std::memcpy(&Suffix, &SuffixBits, sizeof(Suffix));
            std::sprintf(G_NodeLibraryResolvedName, "%s#%d%s", Base, Suffix, Ext);
            CurrentName = G_NodeLibraryResolvedName;
        }
        *Out = CurrentName;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot11(unsigned long Hash, INeuronComponent** Out)
    {
        INeuronComponent* Prototype = FindNodeEntry(Hash);
        if (Prototype == nullptr)
            return GR_ALE_CREATE_FAIL;
        return InstantiateFromNodeLibPrototype(Prototype, Out);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::NodeLibSlot12(unsigned long Hash, INeuronComponent** Out)
    {
        return NodeRegistry_.FindWithRef(Hash, Out);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::CreateVertexBuffer()
    {
        // the entry null test and post-call global reloads differ.
        IRPVertexBuffer* VertexBuffer = static_cast<IRPVertexBuffer*>(G_RpVertexBuffer);
        if (VertexBuffer == nullptr)
        {
            G_VbHandle = nullptr;
            return GR_OK;
        }
        if (G_VbHandle != nullptr)
        {
            VertexBuffer->DestroyVb(&G_VbHandle);
            VertexBuffer = static_cast<IRPVertexBuffer*>(G_RpVertexBuffer);
            G_VbHandle = nullptr;
        }
        const GENRESULT Result = VertexBuffer->CreateVb(
            kParticleFxVbFvf, G_MaxVertices, reinterpret_cast<unsigned int**>(&G_VbHandle), 1);
        if (Result < 0)
        {
            FdumpParticleFxError(0x43, kMsgCreateVb);
            VertexBuffer = static_cast<IRPVertexBuffer*>(G_RpVertexBuffer);
            if (VertexBuffer != nullptr)
                VertexBuffer->DestroyVb(&G_VbHandle);
            G_VbHandle = nullptr;
        }
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::ReleaseVertexBuffer()
    {
        IRPVertexBuffer* const VertexBuffer = static_cast<IRPVertexBuffer*>(G_RpVertexBuffer);
        if (VertexBuffer != nullptr)
            VertexBuffer->DestroyVb(&G_VbHandle);
        G_VbHandle = nullptr;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::Slot15(void* A, void* B)
    {
        // (particlefx, RET 0xC): Start = B, or the field
        // (G_RootNode) when B is null -- no null test after that, baseline dereferences it.
        // G_NodeWalkXform = identity (1.0 at [0]/[4]/[8], zeros elsewhere, 062016d8..
        // ); Start vt+0x10 ComposeNodeTransform(A, G); Start vt+0x40
        // GetNodeTransform(0, &p); G = *p (NOT restored afterwards). Then the loop at
        // over Start's children is the routine's body inlined, so it is the
        // same PropagateNodeChainTransform on the first child. Start's own siblings are
        // not visited. Returns 0.
        INeuronComponent* StartNode = static_cast<INeuronComponent*>(B);
        if (StartNode == nullptr)
            StartNode = G_RootNode;
        static const float kIdentityXform12[12] = {1.f, 0.f, 0.f, 0.f, 1.f, 0.f,
                                                   0.f, 0.f, 1.f, 0.f, 0.f, 0.f};
        std::memcpy(G_NodeWalkXform, kIdentityXform12, sizeof(G_NodeWalkXform));
        IGenericFx* const StartFx = AsFxNode(StartNode);
        StartFx->ComposeNodeTransform(A, G_NodeWalkXform);
        float* World = nullptr;
        StartFx->GetNodeTransform(0, &World);
        std::memcpy(G_NodeWalkXform, World, sizeof(G_NodeWalkXform));
        PropagateNodeChainTransform(StartFx->GetFirstChild(), A);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::Slot16(void* A, void* B)
    {
        // B (root when null) + descendants via the routine.
        // Flag bit0 → SubmitNode(this, node), not A->+0x5C (see helper).
        INeuronComponent* StartNode = static_cast<INeuronComponent*>(B);
        if (StartNode == nullptr)
            StartNode = G_RootNode;
        if (StartNode == nullptr)
            return GR_OK;

        float Dt = 0.0f;
        std::memcpy(&Dt, &A, sizeof(float));
        AsFxNode(StartNode)->Pad5(Dt);
        unsigned long Flag = 0;
        AsFxNode(StartNode)->GetNodeFlags(&Flag);
        if ((Flag & 1) != 0)
        {
            SubmitNode(StartNode);
            return GR_OK;
        }
        INeuronComponent* Child = CallGetFirstChild(StartNode);
        if (Child != nullptr)
            WalkNodeChainFlagDispatch(this, Child, A);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::DrawNodes(void* Cookie, void* StartNode)
    {
        // successful child/sibling probes are called again;
        // ancestor traversal reads the parent even after finding a sibling.
        if (!G_bInDraw)
        {
            FdumpParticleFxWarning(0x12C, kMsgDrawWithoutBegin);
            return GR_GENERIC;
        }
        G_DrawCookie = static_cast<unsigned long>(reinterpret_cast<unsigned int>(Cookie));
        INeuronComponent* Start = static_cast<INeuronComponent*>(StartNode);
        if (Start == nullptr)
            Start = G_RootNode;
        INeuronComponent* Node = Start;
        while (Node != nullptr)
        {
            AsFxNode(Node)->DrawOrEnqueue();
            if (AsFxNode(Node)->GetFirstChild() != nullptr)
                Node = AsFxNode(Node)->GetFirstChild();
            else if (Node == Start)
                Node = nullptr;
            else if (AsFxNode(Node)->GetNextSibling() != nullptr)
                Node = AsFxNode(Node)->GetNextSibling();
            else
            {
                INeuronComponent* Parent = AsFxNode(Node)->GetAttachParent();
                Node = nullptr;
                while (Parent != Start)
                {
                    Node = AsFxNode(Parent)->GetNextSibling();
                    Parent = AsFxNode(Parent)->GetAttachParent();
                    if (Node != nullptr)
                        break;
                }
            }
        }
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::Slot19(const char* Name, void* /*Unused*/)
    {
        // initializes a local FxNode and names it. baseline retains
        // its creation reference, ignores argument two, and publishes no output.
        FxNode* const Node = new (std::nothrow) FxNode(Iids::FxNode, kFxNodeFactory);
        Node->InitializeFromFactory(nullptr);
        Node->SetInstanceName(Name);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::AttachNode(void* ParentRaw, void* ChildRaw)
    {
        // [child+0x28]; then the routine; AddRef parent+child.
        INeuronComponent* Child = static_cast<INeuronComponent*>(ChildRaw);
        if (Child == nullptr)
            return GR_GENERIC;
        if (CallGetAttachParent(Child) != nullptr)
            return GR_GENERIC;
        INeuronComponent* Parent = static_cast<INeuronComponent*>(ParentRaw);
        LinkNodeTree(Parent, Child);
        if (AttachCount_ == AttachCapacity_)
        {
            const unsigned long Capacity = AttachCapacity_ == 0 ? 8 : AttachCapacity_ * 2;
            FParticleFxAttach* Next = new (std::nothrow) FParticleFxAttach[Capacity];
            if (Next == nullptr)
                return GR_OUT_OF_MEMORY;
            if (Attaches_ != nullptr)
            {
                std::memcpy(Next, Attaches_, sizeof(FParticleFxAttach) * AttachCount_);
                delete[] Attaches_;
            }
            Attaches_ = Next;
            AttachCapacity_ = Capacity;
        }
        Attaches_[AttachCount_].Parent = Parent;
        Attaches_[AttachCount_].Child = Child;
        if (Parent != nullptr)
            Parent->AddRef();
        Child->AddRef();
        AttachCount_ += 1;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::Slot21(void* A, void* B)
    {
        // filter the parent before attaching the child.
        unsigned long FilterResult = 0;
        if (G_FilterCallback != nullptr)
            FilterResult = G_FilterCallback(A, B);
        return AttachNode((void*)FilterResult, B);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::DetachNode(void* Node)
    {
        INeuronComponent* Child = static_cast<INeuronComponent*>(Node);
        INeuronComponent* OriginalParent = AsFxNode(Child)->GetAttachParent();
        if (OriginalParent == nullptr)
            return GR_GENERIC;

        if (AsFxNode(Child)->GetPrevSibling() != nullptr)
        {
            INeuronComponent* Prev = AsFxNode(Child)->GetPrevSibling();
            INeuronComponent* Next = AsFxNode(Child)->GetNextSibling();
            AsFxNode(Prev)->SetNextSibling(Next);
        }
        if (AsFxNode(Child)->GetNextSibling() != nullptr)
        {
            INeuronComponent* Next = AsFxNode(Child)->GetNextSibling();
            INeuronComponent* Prev = AsFxNode(Child)->GetPrevSibling();
            AsFxNode(Next)->SetPrevSibling(Prev);
        }
        INeuronComponent* ParentAgain = AsFxNode(Child)->GetAttachParent();
        if (ParentAgain != nullptr)
        {
            if (AsFxNode(ParentAgain)->GetFirstChild() == Child)
                AsFxNode(ParentAgain)->SetFirstChild(AsFxNode(Child)->GetNextSibling());
        }
        else
            G_RootNode = AsFxNode(Child)->GetNextSibling();
        AsFxNode(Child)->SetParent(nullptr);
        AsFxNode(Child)->SetNextSibling(nullptr);
        AsFxNode(Child)->SetPrevSibling(nullptr);

        // Remove only the port's private attachment bookkeeping; baseline releases
        // the original node-reported parent, including directly linked roots.
        unsigned long Index = 0;
        for (Index = 0; Index < AttachCount_; ++Index)
        {
            if (Attaches_[Index].Child != Child)
                continue;
            unsigned long Tail = Index;
            for (Tail = Index; Tail + 1 < AttachCount_; ++Tail)
                Attaches_[Tail] = Attaches_[Tail + 1];
            AttachCount_ -= 1;
            break;
        }
        OriginalParent->Release();
        Child->Release();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SubmitNode(void* Node)
    {
        // submit the node's children first (SubmitChildNodes),
        // tag the node itself via its own +0x20/+0x24 pair (out-param read,
        // OR 0x80000000 sentinel bit, dispatch back), fire the update
        // callback if one is installed, then detach the node from the tree.
        // baseline always returns 0.
        INeuronComponent* NodePtr = static_cast<INeuronComponent*>(Node);
        SubmitChildNodes(NodePtr);
        unsigned long Flag = 0;
        AsFxNode(NodePtr)->GetNodeFlags(&Flag);
        Flag |= 0x80000000u;
        AsFxNode(NodePtr)->SetNodeFlags(Flag);
        if (G_UpdateCallback != nullptr)
            G_UpdateCallback(0, NodePtr, 0);
        DetachNode(NodePtr);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SubmitChildNodes(void* Node)
    {
        // submit every child of Node depth-first via the
        // shared SubmitNodeChain walk (matches baseline's the routine).
        INeuronComponent* NodePtr = static_cast<INeuronComponent*>(Node);
        INeuronComponent* Child = AsFxNode(NodePtr)->GetFirstChild();
        if (Child != nullptr)
            SubmitNodeChain(Child);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::GetRootNode(void** Out)
    {
        if (Out == nullptr)
            return GR_GENERIC;
        *Out = G_RootNode;
        if (G_RootNode == nullptr)
            return GR_OK;
        G_RootNode->AddRef();
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::FindNode(void* Out, void* SearchKey, void* StartNode)
    {
        // pre-order search over the node tree (same
        // child/sibling/parent-climb walk as DrawNodes/Slot15/16). Each
        // visited node's own +0x74 getter (out-param form, not a 0-arg Pad)
        // is compared against the caller-supplied key; the first match is
        // written to *Out. StartNode selects the search root (G_RootNode
        // when null).
        INeuronComponent** OutPtr = static_cast<INeuronComponent**>(Out);
        INeuronComponent* Start = static_cast<INeuronComponent*>(StartNode);
        if (Start == nullptr)
            Start = G_RootNode;
        if (Start == nullptr)
            return static_cast<GENRESULT>(0xfffffff8);

        bool bFound = false;
        WalkNodePreOrder(Start, [&](INeuronComponent* Node) {
            unsigned long Key = 0;
            AsFxNode(Node)->GetInstanceNameHash(&Key);
            if (Key == reinterpret_cast<unsigned long>(SearchKey))
            {
                if (OutPtr != nullptr)
                    *OutPtr = Node;
                bFound = true;
                return false; // stop the walk
            }
            return true;
        });
        return bFound ? GR_OK : static_cast<GENRESULT>(0xfffffff8);
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SetUpdateCallback(void* Fn)
    {
        // raw pointer store (the field in baseline).
        G_UpdateCallback = reinterpret_cast<FnUpdateCallback>(Fn);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SetFilterCallback(void* Fn)
    {
        // raw pointer store (the field in baseline).
        G_FilterCallback = reinterpret_cast<FnFilterCallback>(Fn);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::BeginDraw()
    {
        if (G_bInDraw)
        {
            FdumpParticleFxWarning(0x151, kMsgBeginWithoutEnd);
            return GR_GENERIC;
        }
        G_bInDraw = true;
        if (static_cast<IRPVertexBuffer*>(G_RpVertexBuffer)->SelectVb(G_VbHandle) < 0)
        {
            if (G_RpVertexBuffer == nullptr)
            {
                G_VbHandle = nullptr;
                return GR_GENERIC;
            }
            if (G_VbHandle != nullptr)
            {
                static_cast<IRPVertexBuffer*>(G_RpVertexBuffer)->DestroyVb(&G_VbHandle);
                G_VbHandle = nullptr;
            }
            if (static_cast<IRPVertexBuffer*>(G_RpVertexBuffer)->CreateVb(
                    kParticleFxVbFvf, G_MaxVertices,
                    reinterpret_cast<unsigned int**>(&G_VbHandle), 1) < 0)
            {
                FdumpParticleFxError(0x43, kMsgCreateVb);
                if (G_RpVertexBuffer != nullptr)
                    static_cast<IRPVertexBuffer*>(G_RpVertexBuffer)->DestroyVb(&G_VbHandle);
                G_VbHandle = nullptr;
                return GR_GENERIC;
            }
        }
        // with the batcher on, zero the four batch lists (the field, stride
        // 0xc) before the render-state saves. EndDraw's flush does not clear them.
        if (G_Defaults.bUseMaterialBatcher != 0)
            ClearBatchLists();
        G_RenderPipeline->GetRenderState(D3DRS_CULLMODE, &SavedRsAlphaTest_);
        G_RenderPipeline->GetRenderState(D3DRS_LIGHTING, &SavedRsColorWrite_);
        G_RenderPipeline->GetRenderState(D3DRS_ZWRITEENABLE, &SavedRsCull_);
        G_RenderPipeline->GetRenderState(D3DRS_ALPHABLENDENABLE, &SavedRsAlphaBlend_);
        G_RenderPipeline->GetRenderState(D3DRS_SRCBLEND, &SavedRsSrcBlend_);
        G_RenderPipeline->GetRenderState(D3DRS_DESTBLEND, &SavedRsDestBlend_);
        ApplyParticleDrawRenderState();
        unsigned long Index = 0;
        for (Index = 0; Index < 9; ++Index)
            G_DrawCounters[Index] = 0;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::EndDraw()
    {
        if (!G_bInDraw)
        {
            FdumpParticleFxError(0x180, kMsgEndWithoutBegin);
            return GR_GENERIC;
        }
        const unsigned char UseMaterialBatcher = G_Defaults.bUseMaterialBatcher;
        G_bInDraw = false;
        if (UseMaterialBatcher != 0)
            FlushAppearanceBatches();
        G_RenderPipeline->SetRenderState(D3DRS_CULLMODE, SavedRsAlphaTest_);
        G_RenderPipeline->SetRenderState(D3DRS_LIGHTING, SavedRsColorWrite_);
        G_RenderPipeline->SetRenderState(D3DRS_ZWRITEENABLE, SavedRsCull_);
        G_RenderPipeline->SetRenderState(D3DRS_ALPHABLENDENABLE, SavedRsAlphaBlend_);
        G_RenderPipeline->SetRenderState(D3DRS_SRCBLEND, SavedRsSrcBlend_);
        G_RenderPipeline->SetRenderState(D3DRS_DESTBLEND, SavedRsDestBlend_);
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::EnqueueAppearance(void* ListIndexRaw, void* AppearanceRaw)
    {
        const unsigned long ListIndex =
            static_cast<unsigned long>(reinterpret_cast<unsigned int>(ListIndexRaw));
        return EnqueueAppearanceToList(ListIndex, static_cast<INeuronComponent*>(AppearanceRaw));
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::SetDrawFlags(void* Pair)
    {
        const unsigned char* Bytes = static_cast<const unsigned char*>(Pair);
        G_DrawFlagA = Bytes[0] != 0 ? 1 : 0;
        G_DrawFlagB = Bytes[1] != 0 ? 1 : 0;
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::CopyCounters(void* Out9Dwords)
    {
        unsigned char* const OutBytes = static_cast<unsigned char*>(Out9Dwords);
        for (unsigned long Index = 0; Index < 9; ++Index)
        {
            const unsigned long Word = G_DrawCounters[Index];
            std::memcpy(OutBytes + Index * sizeof(Word), &Word, sizeof(Word));
        }
        return GR_OK;
    }

    GENRESULT NEURONCOMPONENTS_API FParticleFx::Initialize(AGGDESC* Desc)
    {
        const GENRESULT Qi = AcquirePipelineInterfaces(Desc);
        if (Qi != GR_OK)
            return Qi;
        BindInitFxNode();
        return GR_OK;
    }

    GENRESULT FParticleFx::AcquirePipelineInterfaces(AGGDESC* Desc)
    {
        G_PipelineOuter = reinterpret_cast<FAGGDESC14*>(Desc)->OuterUnknown;
        G_InitializedParticleFx = this;
        GENRESULT Result = G_PipelineOuter->QueryInterface(
            Iids::RenderPipeline8B, reinterpret_cast<void**>(&G_RenderPipeline));
        if (Result < 0)
        {
            FdumpParticleFxError(0x75, kMsgAcquirePipeline);
            return GR_GENERIC;
        }
        G_RenderPipeline->Release();

        Result = G_PipelineOuter->QueryInterface(
            Iids::TextureLibraryA, reinterpret_cast<void**>(&G_TextureLibrary));
        if (Result < 0)
        {
            FdumpParticleFxError(0x7C, kMsgAcquireTexLib);
            return GR_GENERIC;
        }
        G_TextureLibrary->Release();

        Result = G_PipelineOuter->QueryInterface(
            Iids::RPVertexBuffer, reinterpret_cast<void**>(&G_RpVertexBuffer));
        if (Result < 0)
        {
            FdumpParticleFxError(0x8D, kMsgAcquireVb);
            return GR_GENERIC;
        }
        G_RpVertexBuffer->Release();

        Result = G_PipelineOuter->QueryInterface(
            Iids::RPIndexBuffer, reinterpret_cast<void**>(&G_RpIndexBuffer));
        if (Result < 0)
        {
            FdumpParticleFxError(0x94, kMsgAcquireIb);
            return GR_GENERIC;
        }
        G_RpIndexBuffer->Release();

        Result = G_PipelineOuter->QueryInterface(
            Iids::RPDraw, reinterpret_cast<void**>(&G_RpDraw));
        if (Result < 0)
        {
            FdumpParticleFxError(0x9B, kMsgAcquireDraw);
            return GR_GENERIC;
        }
        G_RpDraw->Release();
        return GR_OK;
    }

    void FParticleFx::BindInitFxNode()
    {
        if (InitNode_ != nullptr)
        {
            InitNode_->Release();
            InitNode_ = nullptr;
        }
        AGGDESC Desc;
        Desc.size = 8;
        Desc.interface_name = kFxNodeFactory;
        NEURONCOMPONENTS_Acquire()->CreateInstance(&Desc, reinterpret_cast<void**>(&InitNode_));
        // kInitNodeBoundBits is 0x7F7FFFFF, the same FLT_MAX literal baseline pushes at
        // . Slot +0x6C takes a float; the constant is kept in its bit form
        // because that is how the baseline listing spells it.
        float InitBound = 0.0f;
        std::memcpy(&InitBound, &kInitNodeBoundBits, sizeof(float));
        AsFxNode(InitNode_)->SetNodeDuration(InitBound);
        if (G_RootNode == nullptr)
        {
            G_RootNode = InitNode_;
            AsFxNode(InitNode_)->SetParent(nullptr);
            AsFxNode(InitNode_)->SetPrevSibling(nullptr);
            AsFxNode(InitNode_)->SetNextSibling(nullptr);
            return;
        }
        // (0x06201530-0x0620160a, cross-checked against the raw
        // .txt listing byte-for-byte): when G_RootNode already exists, baseline
        // does NOT run this through the generic sort-key-ordered LinkNodeTree
        // insertion (the routine, used by AttachNode). It sets InitNode_'s
        // parent to G_RootNode directly, then either becomes the root's first
        // child (root has none yet) or is appended as the new LAST sibling of
        // whatever child chain the root already has (walks +0x34
        // GetNextSibling to the end, links after it) -- unconditionally, with
        // no GetSortKey comparison. No AddRef anywhere in this internal
        // bootstrap path (matches baseline; this is not the AttachNode-tracked
        // pairing so it isn't added to Attaches_ either). Previously this
        // called the sort-key LinkNodeTree helper, which only matched baseline
        // when G_RootNode had no children yet -- wrong once a second/Nth
        // Initialize() call re-bound InitNode_ under an already-populated
        // root.
        AsFxNode(InitNode_)->SetParent(G_RootNode);
        if (AsFxNode(G_RootNode)->GetFirstChild() == nullptr)
        {
            AsFxNode(G_RootNode)->SetFirstChild(InitNode_);
            return;
        }
        INeuronComponent* Last = AsFxNode(G_RootNode)->GetFirstChild();
        while (AsFxNode(Last)->GetNextSibling() != nullptr)
        {
            Last = AsFxNode(Last)->GetNextSibling();
        }
        AsFxNode(Last)->SetNextSibling(InitNode_);
        AsFxNode(InitNode_)->SetPrevSibling(Last);
        AsFxNode(InitNode_)->SetNextSibling(nullptr);
    }

    INeuronComponent* FParticleFx::FindNodeEntry(unsigned long Hash)
    {
        return NodeRegistry_.Find(Hash);
    }

    // Shared unsigned-key ownership map and borrowed prototype sibling list.
    GENRESULT FParticleFx::InsertNodeByName(const char* Name, INeuronComponent* Node,
                                             unsigned long* OutHash)
    {
        return NodeRegistry_.Insert(this, Name, Node, OutHash);
    }

    GENRESULT FParticleFx::RemoveNodeByHash(unsigned long Hash)
    {
        return NodeRegistry_.Remove(Hash);
    }

    void FParticleFx::ClearNodeList()
    {
        NodeRegistry_.Clear();
    }

    void FParticleFx::ClearAttaches()
    {
        delete[] Attaches_;
        Attaches_ = nullptr;
        AttachCount_ = 0;
        AttachCapacity_ = 0;
    }

    void FParticleFx::SubmitNodeChain(INeuronComponent* Node)
    {
        // shared by SubmitNode/SubmitChildNodes. Depth-first —
        // a node's own children are submitted before the node itself — over
        // a sibling chain, caching each node's next sibling before tagging
        // and detaching it (detach unlinks the node, which would otherwise
        // invalidate the sibling pointer for continued iteration).
        while (Node != nullptr)
        {
            INeuronComponent* Next = CallGetNextSibling(Node); // GetNextSibling
            INeuronComponent* Child = CallGetFirstChild(Node); // GetFirstChild
            if (Child != nullptr)
                SubmitNodeChain(Child);
            unsigned long Flag = 0;
            AsFxNode(Node)->GetNodeFlags(&Flag);
            Flag |= 0x80000000u;
            AsFxNode(Node)->SetNodeFlags(Flag);
            if (G_UpdateCallback != nullptr)
                G_UpdateCallback(0, Node, 0);
            DetachNode(Node);
            Node = Next;
        }
    }

} // namespace ParticleFxImpl
