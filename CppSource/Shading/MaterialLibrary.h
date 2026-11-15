// Author: Giperion.

#pragma once

#include "ShadingInternal.h"

#include <map>

unsigned long GetMaterialGlobalRecordValue(unsigned long Index);
void ActivateMaterialGlobalRecord(unsigned long Index);
void InitializeMaterialGlobalRecords();

class FMaterialLibrary;

class FMaterialLibraryController final : public INeuronComponent
{
public:
    explicit FMaterialLibraryController(FMaterialLibrary* InOwner);
    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    FMaterialLibrary* Owner;
};

class FLibraryAggregate final : public IAggregateComponent
{
public:
    FLibraryAggregate();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API Initialize() override;

    class FMaterialLibrary* Owner;
};

class FMaterialLibrary final : public IMaterialLibrary
{
public:
    FMaterialLibrary();
    ~FMaterialLibrary();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    GENRESULT NEURONCOMPONENTS_API AddMapRule(int Unused, const char* Pattern, const char* TypeName) override;
    GENRESULT NEURONCOMPONENTS_API FindMapRule(int Key, const char* Pattern, void* Unused,
                                    char* OutType) override;
    GENRESULT NEURONCOMPONENTS_API RemapType(const char* Name, const char* Type, void* Unused,
                                  char* OutType) override;
    GENRESULT NEURONCOMPONENTS_API RemoveMapRule(int Key, const char* Pattern) override;
    GENRESULT NEURONCOMPONENTS_API AllocateGroup(unsigned long* OutId) override;
    GENRESULT NEURONCOMPONENTS_API LoadFromFileSystem(void* FileSystem, void* Extra) override;
    GENRESULT NEURONCOMPONENTS_API UnsupportedOperation(void* Arg1, void* Arg2) override;
    GENRESULT NEURONCOMPONENTS_API DeleteGroup(int GroupId) override;
    GENRESULT NEURONCOMPONENTS_API ApplyVerifyGroup(int GroupId, void* Context, void* Extra) override;
    GENRESULT NEURONCOMPONENTS_API ResolveTexturesGroup(int GroupId, float Delta) override;
    GENRESULT NEURONCOMPONENTS_API SetGroupName(int GroupId, const char* Name) override;
    GENRESULT NEURONCOMPONENTS_API GetGroupName(int GroupId, char* Buffer, unsigned long Size) override;
    GENRESULT NEURONCOMPONENTS_API FindOrCreate(const char* Name, const char* Type, INeuronComponent* AggregateOuter,
                                     IMaterial** Out) override;
    GENRESULT NEURONCOMPONENTS_API FindMaterialByCrc(void* Query, IMaterial** Out) override;
    GENRESULT NEURONCOMPONENTS_API AddMaterial(int GroupId, void* Record, IMaterial* Material) override;
    GENRESULT NEURONCOMPONENTS_API RemoveMaterial(unsigned long GroupId, void* Record) override;
    GENRESULT NEURONCOMPONENTS_API GetGroupCount(unsigned long* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API EnumGroup(unsigned long Index, unsigned long* OutId) override;
    GENRESULT NEURONCOMPONENTS_API GetGroupMaterialCount(unsigned long GroupId, int* OutCount) override;
    GENRESULT NEURONCOMPONENTS_API EnumGroupMaterial(unsigned long GroupId, unsigned long Index,
                                           IMaterial** Out) override;
    GENRESULT NEURONCOMPONENTS_API SetGlobalRecord(unsigned long Index,
                                         const unsigned long* Value) override;
    GENRESULT NEURONCOMPONENTS_API SetGlobalRecordValue(unsigned long Index,
                                              const unsigned long* Value) override;
    GENRESULT NEURONCOMPONENTS_API GetGlobalRecordValue(unsigned long Index,
                                              unsigned long* OutValue) override;
    FMaterialLibraryRecord* NEURONCOMPONENTS_API CreateAfterMap(const char* Name, const char* Type,
                                       unsigned char bRemap, INeuronComponent* AggregateOuter) override;
    FMaterialLibraryRecord* CreateGenericRecord(const char* Name, const char* Type,
                                       unsigned char bRemap, INeuronComponent* AggregateOuter);

    FLibraryAggregate Aggregate; // +4
    unsigned long RefCount;
    INeuronComponent* Outer;
    class FMaterialBatcher* OwnerBatcher;
    std::vector<FMaterialMapRule> MapRules;
    std::map<unsigned long, FCrcSlot> CrcMap;
    std::map<unsigned long, FMaterialGroup> Groups;
    unsigned long NextGroupId;
    char RemapScratch[64];
    FMaterialLibraryController Controller;
    INeuronComponent* ControllingUnknown; // standalone +44, distinct from base Outer+40

    GENRESULT ClearRuntime();
    GENRESULT InitializeBase(INeuronComponent* InOuter);
    GENRESULT InitializeFromDescriptor(const FMaterialAggregateDesc* Desc);
    GENRESULT ParseMaterialMap();
    const char* ResolveType(const char* Name, const char* Type);
    FCrcSlot* FindByCrc(unsigned long Crc);
    FMaterialGroup* FindGroup(unsigned long Id);
    void StoreCrc(unsigned long Crc, IMaterial* Material);
};
