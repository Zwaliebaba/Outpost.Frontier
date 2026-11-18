// MaterialAnimation.cpp of Shading.dll compiled into the oracle harness, so that its file-local
// ParseMaKeys can be called by oracle_shading.cpp (work package P1.7) and its file-local load
// group built by oracle_material.cpp (P4.5).

#include "oracle_support.h"

#include "../Shading/MaterialAnimation.cpp"

void OracleParseMaKeys(IFileSystem* FileSystem, FMaKeyBlock* Block)
{
    ParseMaKeys(FileSystem, Block);
}

// P4.5 (oracle_material.cpp): the per-child steps of LoadAnimationGroup after ParseMaKeys, on
// animation data the suite prepared (each with its creation reference): stored in the library's
// map and added to a new group, with the reference counting of 06ec6fe0..7060. Null for no data.
void* OracleAddAnimationGroup(FMaterialAnimation* Library, const std::vector<FImaData*>& Data)
{
    FMatAnimLoadGroup* Group = nullptr;
    for (size_t Index = 0; Index < Data.size(); ++Index)
    {
        FImaData* Item = Data[Index];
        if (Group == nullptr)
            Group = new FMatAnimLoadGroup();
        Library->StoreData(Item->NameCrc, Item);
        FMatAnimLoadEntry Entry;
        Entry.Crc = Item->NameCrc;
        Entry.Data = Item;
        Item->AddRef();
        Group->Entries.push_back(Entry);
        Item->AddRef();
        Item->Release();
        Item->Release();
    }
    return Group;
}
