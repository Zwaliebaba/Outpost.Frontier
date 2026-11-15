// Author: Giperion.

#pragma once

#include "particlefx_types.h"
#include <map>

// particlefx.dll — NEURONCOMPONENTS particle FX scaffolding.
// FFxEffectLibrary: the IFxEffectLibrary component (archetype registry, ALE
// load/save, effect instantiation).
namespace ParticleFxImpl
{
    class FFxEffectLibrary final : public IFxEffectLibrary
    {
    public:
        FFxEffectLibrary()
            : RefCount_(1), Entries_(new FArchetypeMap), MapOwner_(this, Entries_),
              Head_(nullptr), Tail_(nullptr), EntryCount_(0)
        {
        }

        ~FFxEffectLibrary() noexcept(false);

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override { return ++RefCount_; }
        unsigned long NEURONCOMPONENTS_API Release() override;

        GENRESULT NEURONCOMPONENTS_API BindParticleFx(IParticleFx* ParticleFx, IFxNodeLibrary* NodeLibrary) override;
        GENRESULT NEURONCOMPONENTS_API InsertArchetype(IFxEffectArchetype* Archetype,
                                            unsigned long* OutHash) override;
        GENRESULT NEURONCOMPONENTS_API RemoveByHash(unsigned long Hash) override;
        GENRESULT NEURONCOMPONENTS_API CreateEffect(void* /*Unused*/, void* Key, IFxEffect** Out) override;
        GENRESULT NEURONCOMPONENTS_API SaveAle(IFileSystem* Filesys) override;
        GENRESULT NEURONCOMPONENTS_API LoadAle(IFileSystem* Filesys) override;
        GENRESULT NEURONCOMPONENTS_API ClearEffects() override;
        GENRESULT NEURONCOMPONENTS_API EnumerateArchetypes(FEffectLibraryEnumCallback Callback, void* Context) override;
        GENRESULT NEURONCOMPONENTS_API FindByHash(unsigned long Hash, IFxEffectArchetype** Out) override;
        GENRESULT NEURONCOMPONENTS_API MakeUniqueName(const char* Name, const char** Out) override;
        GENRESULT NEURONCOMPONENTS_API RemapAllRec16FieldD(unsigned long OldId, unsigned long NewId) override;
        GENRESULT NEURONCOMPONENTS_API Slot14(void* /*Unused*/) override { return ClearEffects(); }

    private:
        struct FAleLibEntry
        {
            struct FArchetypeReference
            {
                FArchetypeReference() : Archetype(nullptr) {}
                explicit FArchetypeReference(IFxEffectArchetype* Value) : Archetype(Value)
                {
                    if (Archetype != nullptr)
                        Archetype->AddRef();
                }
                FArchetypeReference(const FArchetypeReference& Other)
                    : FArchetypeReference(Other.Archetype) {}
                ~FArchetypeReference() { Reset(); }
                FArchetypeReference& operator=(const FArchetypeReference&) = delete;
                void Reset()
                {
                    if (Archetype != nullptr)
                    {
                        Archetype->Release();
                        Archetype = nullptr;
                    }
                }
                IFxEffectArchetype* Archetype;
            };

            explicit FAleLibEntry(const FArchetypeReference& Value)
                : Next(nullptr), Prev(nullptr), Reference(Value) {}
            FAleLibEntry* Next;
            FAleLibEntry* Prev;
            FArchetypeReference Reference;
        };

        using FArchetypeMap = std::map<unsigned long, FAleLibEntry>;
        struct FMapStorageOwner
        {
            FMapStorageOwner(FFxEffectLibrary* InOwner, FArchetypeMap* InStorage)
                : Owner(InOwner), Storage(InStorage) {}
            ~FMapStorageOwner() noexcept(false);
            FMapStorageOwner(const FMapStorageOwner&) = delete;
            FMapStorageOwner& operator=(const FMapStorageOwner&) = delete;

            FFxEffectLibrary* Owner;
            FArchetypeMap* Storage;
        };

        unsigned char ContainsHash(unsigned long Hash) const;

        void ClearEffectList();
        void DetachEffectList();
        void ClearMapEntries(FArchetypeMap* Storage);
        void DestroyMapStorage(FArchetypeMap* Storage);

        unsigned long RefCount_;
        FArchetypeMap* Entries_;
        FMapStorageOwner MapOwner_;
        FAleLibEntry* Head_;
        FAleLibEntry* Tail_;
        unsigned long EntryCount_;
    };

} // namespace ParticleFxImpl
