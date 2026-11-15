// Author: Giperion.

#pragma once

#include "particlefx_types.h"
#include "particlefx_common.h"
#include <map>

// FParticleFx's embedded IFxNodeLibrary and the standalone FxNodeLibrary own
// separate instances of the same registry. An unsigned-key map owns each
// prototype; Head/Tail borrow the node's actual sibling links in insertion order.
// Modern std::map replaces only compiler storage, not linkage or reference calls.
// The independent baseline constructor/destructor chains are 0622a620/06202550
// and 06201010/06201190; shared registry operations start at 062273c0.
namespace ParticleFxImpl
{
    using ::IFxNodeLibrary;

    struct FParticleFxComponentReference
    {
        FParticleFxComponentReference() : Component(nullptr) {}
        explicit FParticleFxComponentReference(INeuronComponent* Value) : Component(Value)
        {
            if (Component != nullptr)
                Component->AddRef();
        }
        FParticleFxComponentReference(const FParticleFxComponentReference& Other)
            : FParticleFxComponentReference(Other.Component) {}
        ~FParticleFxComponentReference() { Reset(); }
        FParticleFxComponentReference& operator=(const FParticleFxComponentReference&) = delete;

        void Reset()
        {
            if (Component != nullptr)
            {
                Component->Release();
                Component = nullptr;
            }
        }

        INeuronComponent* Component;
    };

    class FNodePrototypeRegistry
    {
    public:
        FNodePrototypeRegistry()
            : Entries_(new FPrototypeMap), MapOwner_(this, Entries_),
              Count_(0), Head_(nullptr), Tail_(nullptr) {}
        ~FNodePrototypeRegistry() noexcept(false);
        FNodePrototypeRegistry(const FNodePrototypeRegistry&) = delete;
        FNodePrototypeRegistry& operator=(const FNodePrototypeRegistry&) = delete;

        INeuronComponent* Find(unsigned long Hash);
        GENRESULT FindWithRef(unsigned long Hash, INeuronComponent** Out);
        GENRESULT Insert(IFxNodeLibrary* Library, const char* Name, INeuronComponent* Node,
                         unsigned long* OutHash);
        GENRESULT Remove(unsigned long Hash);
        GENRESULT Enumerate(FNodeLibraryEnumCallback Callback, void* Context);
        GENRESULT Load(IFxNodeLibrary* Library, IFileSystem* Filesys);
        void Clear();
        void Destroy();
        unsigned long Count() const { return Count_; }
        const unsigned long* CountAddress() const { return &Count_; }
        INeuronComponent* First() const { return Head_; }

    private:
        INeuronComponent* UnlinkPrototype(INeuronComponent* Node);
        using FPrototypeMap = std::map<unsigned long, FParticleFxComponentReference>;
        struct FMapStorageOwner
        {
            FMapStorageOwner(FNodePrototypeRegistry* InOwner, FPrototypeMap* InStorage)
                : Owner(InOwner), Storage(InStorage) {}
            ~FMapStorageOwner() noexcept(false);
            FMapStorageOwner(const FMapStorageOwner&) = delete;
            FMapStorageOwner& operator=(const FMapStorageOwner&) = delete;

            FNodePrototypeRegistry* Owner;
            FPrototypeMap* Storage;
        };
        void DetachPrototypes();
        void ClearMapEntries(FPrototypeMap* Storage);
        void DestroyMapStorage(FPrototypeMap* Storage);
        FPrototypeMap* Entries_;
        FMapStorageOwner MapOwner_;
        unsigned long Count_;
        INeuronComponent* Head_;
        INeuronComponent* Tail_;
    };

    class FFxNodeLibrary final : public IFxNodeLibrary
    {
    public:
        FFxNodeLibrary()
            : RefCount_(1)
        {
        }

        ~FFxNodeLibrary() noexcept(false);

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

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
        // enumerate this object's own node-lib entries,
        // Callback(Node, Context) cdecl, stop at first nonzero result.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot8(FNodeLibraryEnumCallback Callback, void* Context) override;
        GENRESULT NEURONCOMPONENTS_API ClearNodeLibrary() override;
        // "#N" collision-avoidance MakeUniqueName, checked
        // against this object's own table (not FParticleFx's).
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot10(void* Name, void* Out) override;
        // find the prototype by hash, then CloneNode(Out).
        // Miss = GR_ALE_CREATE_FAIL (-8), output untouched.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot11(unsigned long Hash, INeuronComponent** Out) override;
        // find CRC in this object's own table, out = stored
        // node, AddRef.
        GENRESULT NEURONCOMPONENTS_API NodeLibSlot12(unsigned long Hash, INeuronComponent** Out) override;

    private:
        INeuronComponent* FindNodeEntry(unsigned long Hash);
        GENRESULT InsertNodeByName(const char* Name, INeuronComponent* Node,
                                  unsigned long* OutHash = nullptr);
        GENRESULT RemoveNodeByHash(unsigned long Hash);
        void ClearNodeList();

        unsigned long RefCount_;
        FNodePrototypeRegistry NodeRegistry_;
    };

} // namespace ParticleFxImpl
