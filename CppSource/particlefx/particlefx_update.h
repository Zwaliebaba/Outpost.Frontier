// Author: Giperion.

#pragma once

#include "particlefx_types.h"
#include "particlefx_common.h"
#include "particlefx_nodelibrary.h"

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// FParticleFx: the IParticleFx / IFxNodeLibrary component.
namespace ParticleFxImpl
{
    // fixed particle-draw render-state block + two-stream
    // vertex-format declaration applied to a render pipeline. baseline's own
    // three XREFs are FParticleFx::BeginDraw (0x06201b5e, the "real" call site
    // this port already wires below) plus FALBatchedMaterialFacet::Slot6
    // and FALMaterial::Slot8 (the branch/the branch in
    // particlefx_material.cpp) which call the exact same function against the
    // exact same the field (this port's G_RenderPipeline) with no other
    // arguments — not a distinct "property batch" on a different object.
    // Exposed here (out of particlefx_update.cpp's file-local anonymous
    // namespace) so particlefx_material.cpp can call the identical baseline
    // routine instead of re-implementing it.
    void ApplyParticleDrawRenderState();

    class FParticleFx final : public IParticleFx
    {
    public:
        // DLL-global callbacks set by the branch / the branch.
        using FnUpdateCallback = void(__cdecl*)(unsigned long, void*, unsigned long);
        using FnFilterCallback = unsigned long(__cdecl*)(void*, void*);

        FParticleFx()
            : Inner_(this), Controller_(&Inner_), Attaches_(nullptr), AttachCount_(0),
              AttachCapacity_(0), InitNode_(nullptr)
        {
            SavedRsAlphaTest_ = 0;
            SavedRsAlphaBlend_ = 0;
            SavedRsSrcBlend_ = 0;
            SavedRsDestBlend_ = 0;
            SavedRsColorWrite_ = 0;
            SavedRsCull_ = 0;
        }

        ~FParticleFx() noexcept(false);

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;

        void SetController(INeuronComponent* OuterUnknown)
        {
            Controller_ = OuterUnknown != nullptr ? OuterUnknown : &Inner_;
        }
        INeuronComponent* InnerUnknown() { return &Inner_; }

        GENRESULT NEURONCOMPONENTS_API LoadNodeLibrary(IFileSystem* Filesys) override;
        GENRESULT NEURONCOMPONENTS_API SaveNodeLibrary(IFileSystem* Filesys) override;
        unsigned long NEURONCOMPONENTS_API HashNodeName(const char* Name) override
        {
            return HashNameCrc(Name);
        }
        // RET 0x10: name, prototype, optional output hash.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot6(const char* Name, INeuronComponent* Node,
                                       unsigned long* OutHash) override;
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot7(unsigned long Hash) override;
        // enumerate node-lib entries, Callback(Node, Context),
        // stop at first nonzero result.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot8(FNodeLibraryEnumCallback Callback, void* Context) override;
        GENRESULT NEURONCOMPONENTS_API ClearNodeLibrary() override;
        // MakeUniqueName-style "#N" collision avoidance.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot10(void* Name, void* Out) override;
        // find prototype by hash, then CloneNode(Out).
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot11(unsigned long Hash, INeuronComponent** Out) override;
        // find CRC, write stored ptr, AddRef.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot12(unsigned long Hash, INeuronComponent** Out) override;

        GENRESULT NEURONCOMPONENTS_API CreateVertexBuffer() override;
        GENRESULT NEURONCOMPONENTS_API ReleaseVertexBuffer() override;
        GENRESULT NEURONCOMPONENTS_API Slot15(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API Slot16(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API DrawNodes(void* Cookie, void* StartNode) override;
        GENRESULT NEURONCOMPONENTS_API GetDrawCookie() override
        {
            return static_cast<GENRESULT>(G_DrawCookie);
        }
        GENRESULT NEURONCOMPONENTS_API Slot19(const char* Name, void* Unused) override;
        GENRESULT NEURONCOMPONENTS_API AttachNode(void* ParentRaw, void* ChildRaw) override;
        GENRESULT NEURONCOMPONENTS_API Slot21(void* A, void* B) override;
        GENRESULT NEURONCOMPONENTS_API DetachNode(void* Node) override;
        GENRESULT NEURONCOMPONENTS_API SubmitNode(void* Node) override;
        GENRESULT NEURONCOMPONENTS_API SubmitChildNodes(void* Node) override;
        GENRESULT NEURONCOMPONENTS_API GetRootNode(void** Out) override;
        // baseline's real stack-slot usage is (Out, SearchKey,
        // StartNode) — the first slot is written through as an out-pointer
        // and the third is never read — not the header's placeholder (A,B,C)
        // naming.
        GENRESULT NEURONCOMPONENTS_API FindNode(void* Out, void* SearchKey, void* StartNode) override;
        GENRESULT NEURONCOMPONENTS_API SetUpdateCallback(void* Fn) override;
        GENRESULT NEURONCOMPONENTS_API SetFilterCallback(void* Fn) override;

        GENRESULT NEURONCOMPONENTS_API BeginDraw() override;
        GENRESULT NEURONCOMPONENTS_API EndDraw() override;

        GENRESULT NEURONCOMPONENTS_API SetUseMaterialBatcher(unsigned char Value) override
        {
            G_Defaults.bUseMaterialBatcher = Value;
            return GR_OK;
        }
        GENRESULT NEURONCOMPONENTS_API GetUseMaterialBatcher() override
        {
            return static_cast<GENRESULT>(G_Defaults.bUseMaterialBatcher);
        }
        GENRESULT NEURONCOMPONENTS_API EnqueueAppearance(void* ListIndexRaw, void* AppearanceRaw) override;
        void* NEURONCOMPONENTS_API GetVertexBufferHandle() override
        {
            return G_VbHandle;
        }
        GENRESULT NEURONCOMPONENTS_API SetDrawFlags(void* Pair) override;
        GENRESULT NEURONCOMPONENTS_API CopyCounters(void* Out9Dwords) override;

        GENRESULT NEURONCOMPONENTS_API Initialize(AGGDESC* Desc) override;

        GENRESULT NEURONCOMPONENTS_API Slot38() override { return GR_OK; }
        GENRESULT NEURONCOMPONENTS_API GetFlagA() override
        {
            return static_cast<GENRESULT>(G_DrawFlagA);
        }
        GENRESULT NEURONCOMPONENTS_API GetFlagB() override
        {
            return static_cast<GENRESULT>(G_DrawFlagB);
        }

    private:
        class FInnerUnknown final : public INeuronComponent
        {
        public:
            explicit FInnerUnknown(FParticleFx* Owner) : RefCount_(1), Owner_(Owner) {}

            GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
            unsigned long NEURONCOMPONENTS_API AddRef() override;
            unsigned long NEURONCOMPONENTS_API Release() override;

        private:
            unsigned long RefCount_;
            FParticleFx* Owner_;
        };

        GENRESULT AcquirePipelineInterfaces(AGGDESC* Desc);
        void BindInitFxNode();
        INeuronComponent* FindNodeEntry(unsigned long Hash);
        GENRESULT InsertNodeByName(const char* Name, INeuronComponent* Node,
                                  unsigned long* OutHash = nullptr);
        GENRESULT RemoveNodeByHash(unsigned long Hash);
        void ClearNodeList();
        void ClearAttaches();
        // shared submit walk used by SubmitNode/SubmitChildNodes.
        void SubmitNodeChain(INeuronComponent* Node);

        FInnerUnknown Inner_;
        INeuronComponent* Controller_;
        FNodePrototypeRegistry NodeRegistry_;
        FParticleFxAttach* Attaches_;
        unsigned long AttachCount_;
        unsigned long AttachCapacity_;
        INeuronComponent* InitNode_;
        int SavedRsAlphaTest_;
        int SavedRsAlphaBlend_;
        int SavedRsSrcBlend_;
        int SavedRsDestBlend_;
        int SavedRsColorWrite_;
        int SavedRsCull_;
    };

} // namespace ParticleFxImpl
