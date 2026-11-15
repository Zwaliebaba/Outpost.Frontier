// Author: Giperion.

#include "RendComp.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <new>
#include <utility>

// VMeshWire.cpp of baseline rendcomp.dll (06c2f4e0..06c308b0).

// The 0x18 AGGDESC the factory accepts (the branch).
struct FWireAggDesc
{
    unsigned long Size;
    const char* Name;
    INeuronComponent* Outer;       // +0x08
    INeuronComponent** InnerOut;   // +0x0C
    const char* Value;
    INeuronComponent* Parent;
};

// The per-archetype wrapper, 0xC bytes: {vtable the branch, VWireData blob, refcount}.
class FVmeshWireInstance : public IVMeshWire
{
public:
    FVwireDataHeader* Blob;     // +0x04 the whole VWireData leaf, owned
    unsigned long RefCount;     // +0x08

    // table {"1.11_IVMeshWire", 0}; hit -> AddRef, *Out = this, 0; miss -> 0, -3.
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
    {
        if (std::strcmp(Iids::VMeshWire, Iid) != 0)
        {
            *Out = nullptr;
            return GR_INTERFACE_UNSUPPORTED;
        }
        AddRef();
        *Out = (IVMeshWire*)this;
        return GR_OK;
    }

    // .
    unsigned long NEURONCOMPONENTS_API AddRef() override
    {
        RefCount += 1;
        return RefCount;
    }

    // decrement when above 0; at 0 pin the count at 1, free the blob and the
    // object (the base vtable 06c365cc is stored first; there is no destructor body), return 0.
    unsigned long NEURONCOMPONENTS_API Release() override
    {
        if (RefCount > 0)
            RefCount -= 1;
        if (RefCount != 0)
            return RefCount;
        RefCount = 1;
        delete[] (const unsigned char*)Blob;
        delete this;
        return 0;
    }

    // (thiscall in baseline).
    // formerly Slot3
    void* NEURONCOMPONENTS_API GetWireData() override
    {
        return Blob;
    }

    // formerly Slot4
    int NEURONCOMPONENTS_API Draw() override
    {
        FVmeshKey Key;
        Key.Name = nullptr;
        Key.Crc = Blob->Crc;
        FVmeshData* Data = VMeshDataFind(&Key);
        if (Data == nullptr)
            return (int)0xFFFFFFF8;
        FVmeshGroup* Group = Data->Group;
        if (Group == nullptr)
            return -1;
        if (VMeshGroupEnsureVb(Group) < 0)
            return -1;
        FVbListing Listing;
        Listing.VertexBuffer = Data->VertexBuffer;
        Listing.Fvf = Data->Fvf;
        Listing.BaseVertex = Data->BaseVertex;
        unsigned int StartIndex = 0;
        void* const ScratchIb = (void*)((intptr_t)-1);
        G_IndexBuffer->CopyIndices(ScratchIb, &StartIndex,
                                   (const unsigned short*)((const unsigned char*)Blob + sizeof(FVwireDataHeader)),
                                   Blob->Count);
        G_IndexBuffer->SelectIb(ScratchIb, Blob->IbBias + Listing.BaseVertex, 0, 0);
        IRPDraw* Draw = G_Draw;
        return Draw->DrawIndexedPrimitive(2, 0, Blob->NumVertices, StartIndex, Blob->Count);
    }

    // (thiscall in baseline): the VMeshData header for the blob's CRC, or 0.
    // formerly Slot5
    void* NEURONCOMPONENTS_API GetMeshData() override
    {
        FVmeshKey Key;
        Key.Name = nullptr;
        Key.Crc = Blob->Crc;
        FVmeshData* Data = VMeshDataFind(&Key);
        if (Data == nullptr)
            return nullptr;
        return Data->Header;
    }
};

// The VMeshWire engine component, 0x28 bytes: {vtable the branch, archetype map at +4
// (VC6 std::map<int, smart ptr>, 0x14), Outer +0x18, non-delegating unknown +0x1C}.
class FVMeshWire : public IEngineComponent
{
public:
    // +0x1C: vtable the branch.
    struct FInner : public INeuronComponent
    {
        unsigned long RefCount; // +0x20
        FVMeshWire* Owner;      // +0x24

        GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
        unsigned long NEURONCOMPONENTS_API AddRef() override;
        unsigned long NEURONCOMPONENTS_API Release() override;
    };

    // Stored instances retain their own references.
    std::map<int, FVmeshWireInstance*> Archetypes;
    INeuronComponent* Outer;        // +0x18 AGGDESC+8, or &Inner when not aggregated
    FInner Inner;

    // Releases every value, then drops the nodes (the map clear in the branch).
    void ClearArchetypes()
    {
        std::map<int, FVmeshWireInstance*>::iterator It = Archetypes.begin();
        while (It != Archetypes.end())
        {
            if (It->second != nullptr)
                It->second->Release();
            ++It;
        }
        Archetypes.clear();
    }

    // range erase: every value in the range is released.
    void EraseArchetype(int Key)
    {
        std::map<int, FVmeshWireInstance*>::iterator It = Archetypes.find(Key);
        if (It == Archetypes.end())
            return;
        if (It->second != nullptr)
            It->second->Release();
        Archetypes.erase(It);
    }

    // / 06c307d0 / 06c307e0: forward to the controlling unknown.
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override
    {
        return Outer->QueryInterface(Iid, Out);
    }

    unsigned long NEURONCOMPONENTS_API AddRef() override
    {
        return Outer->AddRef();
    }

    unsigned long NEURONCOMPONENTS_API Release() override
    {
        return Outer->Release();
    }

    // .
    int NEURONCOMPONENTS_API Initialize() override
    {
        return 0;
    }

    int NEURONCOMPONENTS_API CreateArchetype(void* Archetype, void* FileSystem) override;
    int NEURONCOMPONENTS_API DuplicateArchetype(void* NewArchetype, void* OldArchetype) override;
    int NEURONCOMPONENTS_API DestroyArchetype(void* Archetype) override;
    int NEURONCOMPONENTS_API QueryArchetype(void* Archetype, const char* Name, void** Out) override;

    // XOR EAX,EAX; RET 0xC.
    int NEURONCOMPONENTS_API CreateInstance(void* /*Instance*/, void* /*Archetype*/) override
    {
        return 0;
    }

    // a bare RET 8 -- EAX is whatever the caller left; 0 here.
    int NEURONCOMPONENTS_API DestroyInstance(void* /*Instance*/) override
    {
        return 0;
    }

    // a bare RET 0xC -- EAX undefined; 0 here.
    int NEURONCOMPONENTS_API UpdateInstance(void* /*Instance*/, void* /*Arg*/) override
    {
        return 0;
    }

    int NEURONCOMPONENTS_API QueryInstance(void* Instance, const char* Iid, void** Out) override;

    // again (slot 12).
    int NEURONCOMPONENTS_API Notify(void* /*Arg*/) override
    {
        return 0;
    }
};

// table {"1.11_IEngineComponent", 0}, {"1.11_IAggregateComponent", 0}; a hit
// AddRefs through the main vtable (so the outer) and hands out the component; a miss writes 0
// and returns -3.
GENRESULT NEURONCOMPONENTS_API FVMeshWire::FInner::QueryInterface(const char* Iid, void** Out)
{
    if (std::strcmp(Iids::EngineComponent, Iid) != 0 &&
        std::strcmp(Iids::AggregateComponent, Iid) != 0)
    {
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }
    IEngineComponent* Face = (IEngineComponent*)Owner;
    Face->AddRef();
    *Out = Face;
    return GR_OK;
}

// .
unsigned long NEURONCOMPONENTS_API FVMeshWire::FInner::AddRef()
{
    RefCount += 1;
    return RefCount;
}

// decrement when above 0; at 0 pin the count at 1, clear the archetype map
// (releasing every wrapper), free the owner, return 0.
unsigned long NEURONCOMPONENTS_API FVMeshWire::FInner::Release()
{
    if (RefCount > 0)
        RefCount -= 1;
    if (RefCount != 0)
        return RefCount;
    RefCount = 1;
    if (Owner != nullptr)
    {
        Owner->ClearArchetypes();
        delete Owner;
    }
    return 0;
}

int NEURONCOMPONENTS_API FVMeshWire::CreateArchetype(void* Archetype, void* FileSystem)
{
    IFileSystem* File = (IFileSystem*)FileSystem;
    const int Result = File->OpenNode("VMeshWire");
    if (Result == 0)
        return Result;

    FFileSystemDesc Desc;
    std::memset(&Desc, 0, sizeof(Desc));
    Desc.ComponentName = "FileSystem";
    Desc.Access = 0x80000000;
    Desc.ShareMode = 1;
    Desc.Creation = 3;
    Desc.Flags = 0x8000080;
    Desc.Path = "VWireData";
    Desc.Size = 0x34;
    Desc.Field30 = 0xFFFFFFFF;
    void* Blob = nullptr;
    int bRead = 0;
    const int Handle = File->OpenByDesc(&Desc);
    if (Handle != -1)
    {
        const unsigned long Size = File->GetSize((unsigned long)Handle, 0);
        unsigned char* Buffer = new unsigned char[Size];
        unsigned long ReadCount = 0;
        bRead = File->Read((unsigned long)Handle, Buffer, Size, &ReadCount, 0);
        if (bRead != 0)
            Blob = Buffer;
        else
            delete[] Buffer;
        File->CloseFile((unsigned long)Handle);
    }
    if (bRead == 0)
    {
        RendCompFdumpAt(kFdumpError, G_FileVMeshWire, 0x87,
                        "couldnt load vmeshwire persistant wire_data; bad version?");
        return 0;
    }

    FVmeshWireInstance* Wire = new FVmeshWireInstance();
    Wire->RefCount = 1;
    Wire->Blob = (FVwireDataHeader*)Blob;
    if (Result >= 0)
    {
        const int Key = (int)((intptr_t)Archetype);
        std::pair<std::map<int, FVmeshWireInstance*>::iterator, bool> Inserted =
            Archetypes.insert(std::make_pair(Key, (FVmeshWireInstance*)nullptr));
        FVmeshWireInstance*& Slot = Inserted.first->second;
        if (Slot != Wire)
        {
            if (Slot != nullptr)
            {
                Slot->Release();
                Slot = nullptr;
            }
            Slot = Wire;
            Wire->AddRef();
        }
    }
    Wire->Release();
    File->OpenNode(".."); // the field = ".." (2E 2E 00)
    return Result;
}

// (RET 0xC): look up the OLD archetype; on a hit the NEW key shares its wrapper
// (the previous value released, the shared one AddRef'd). The blob is not copied. EAX is not
// set on either path in baseline; 0 here.
int NEURONCOMPONENTS_API FVMeshWire::DuplicateArchetype(void* NewArchetype, void* OldArchetype)
{
    const int OldKey = (int)((intptr_t)OldArchetype);
    std::map<int, FVmeshWireInstance*>::iterator Found = Archetypes.find(OldKey);
    if (Found == Archetypes.end())
        return 0;
    FVmeshWireInstance* Shared = Found->second;
    const int NewKey = (int)((intptr_t)NewArchetype);
    std::pair<std::map<int, FVmeshWireInstance*>::iterator, bool> Inserted =
        Archetypes.insert(std::make_pair(NewKey, (FVmeshWireInstance*)nullptr));
    FVmeshWireInstance*& Slot = Inserted.first->second;
    if (Slot != nullptr)
    {
        Slot->Release();
        Slot = nullptr;
    }
    Slot = Shared;
    if (Shared != nullptr)
        Shared->AddRef();
    return 0;
}

// (RET 8): a present key is erased (its wrapper released). EAX is not set in
// baseline; 0 here.
int NEURONCOMPONENTS_API FVMeshWire::DestroyArchetype(void* Archetype)
{
    EraseArchetype((int)((intptr_t)Archetype));
    return 0;
}

// (RET 0x10): only "1.11_IVMeshWire" (16-byte REPE CMPSB, an exact match); a
// wrong IID or an unknown archetype returns -1 with Out untouched. A hit AddRefs the wrapper.
int NEURONCOMPONENTS_API FVMeshWire::QueryArchetype(void* Archetype, const char* Name, void** Out)
{
    if (std::strcmp(Name, Iids::VMeshWire) != 0)
        return -1;
    std::map<int, FVmeshWireInstance*>::iterator Found =
        Archetypes.find((int)((intptr_t)Archetype));
    if (Found == Archetypes.end())
        return -1;
    Found->second->AddRef();
    *Out = Found->second;
    return 0;
}

// (RET 0x10): the instance's archetype (IEngine +0x64, -1 -> -1), the handle
// released again (+0x3C), then this component's own QueryArchetype through the vtable.
int NEURONCOMPONENTS_API FVMeshWire::QueryInstance(void* Instance, const char* Iid, void** Out)
{
    void* Archetype = G_Engine->GetInstanceArchetype(Instance);
    if (Archetype == (void*)((intptr_t)-1))
        return -1;
    G_Engine->ReleaseHandle(Archetype);
    return QueryArchetype(Archetype, Iid, Out);
}

// (factory vtable slot 3, RET 0xC). Only a 0x18 AGGDESC whose name matches the
// factory's; otherwise *Out = 0, -3. Allocation failure -> *Out = 0, -4. Outer = AGGDESC+8;
// when set the inner goes to *(AGGDESC+0xC), otherwise the object is its own outer.
GENRESULT NEURONCOMPONENTS_API VMeshWireFactoryCreate(FNeuronComponentsFactory* This, AGGDESC* Desc, void** Out)
{
    FWireAggDesc* Extended = (FWireAggDesc*)Desc;
    if (Extended->Size != 0x18 || std::strcmp(Extended->Name, This->Name) != 0)
    {
        *Out = nullptr;
        return GR_INTERFACE_UNSUPPORTED;
    }
    FVMeshWire* Object = new (std::nothrow) FVMeshWire();
    if (Object == nullptr)
    {
        *Out = nullptr;
        return GR_OUT_OF_MEMORY;
    }
    Object->Inner.RefCount = 1;
    Object->Inner.Owner = Object;
    Object->Outer = Extended->Outer;
    if (Object->Outer == nullptr)
        Object->Outer = &Object->Inner;
    else
        *Extended->InnerOut = &Object->Inner;
    *Out = (IEngineComponent*)Object;
    return GR_OK;
}
