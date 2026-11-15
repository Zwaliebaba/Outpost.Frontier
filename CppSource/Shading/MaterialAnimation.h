// Author: Giperion.

#pragma once

#include "ShadingInternal.h"

// Implementations of the animation interfaces declared in render/shading.h.

class FImaData final : public IImaData
{
public:
    FImaData();
    ~FImaData();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    FMaKeyBlock* NEURONCOMPONENTS_API KeyBlock() override;
    void NEURONCOMPONENTS_API CrcOut(unsigned long* Out) override;

    unsigned long NameCrc; // +4
    FMaKeyBlock Keys;      // +8
    unsigned long RefCount; // +18
};

static_assert(offsetof(FImaData, NameCrc) == 0x04, "IMAData CRC +4");
static_assert(offsetof(FImaData, Keys) == 0x08, "IMAData keys +8");
static_assert(offsetof(FImaData, RefCount) == 0x18, "IMAData ref +18");

class FImaInstance final : public IImaInstance
{
public:
    FImaInstance();
    ~FImaInstance();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    void AttachData(FImaData* NewData);
    FMaKeyBlock* NEURONCOMPONENTS_API GetKeyBlock() override;
    void NEURONCOMPONENTS_API ResetTime() override;
    void NEURONCOMPONENTS_API Tick(float Dt) override;
    unsigned char NEURONCOMPONENTS_API FillUvMatrix(float* Out) override;

    FImaData* Data;        // +4
    float Time;            // +8
    unsigned long Frame;   // +C
    FImaInstance* Next;    // +10 the field walk
    FImaInstance* Prev;    // +14
    unsigned long RefCount; // +18
};

static_assert(offsetof(FImaInstance, Data) == 0x04, "IMAInstance data +4");
static_assert(offsetof(FImaInstance, Time) == 0x08, "IMAInstance time +8");
static_assert(offsetof(FImaInstance, Frame) == 0x0C, "IMAInstance frame +C");
static_assert(offsetof(FImaInstance, Next) == 0x10, "IMAInstance next +10");
static_assert(offsetof(FImaInstance, Prev) == 0x14, "IMAInstance prev +14");
static_assert(offsetof(FImaInstance, RefCount) == 0x18, "IMAInstance ref +18");

class FMaterialAnimation;

// baseline non-delegating IUnknown at MaterialAnimation+0x20 (the branch / the branch).
// Factory writes this to AGGDESC InnerOut when Outer is set — not the Aggregate face at +4.
class FMaterialAnimationController final : public INeuronComponent
{
public:
    explicit FMaterialAnimationController(FMaterialAnimation* InOwner);

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;

    FMaterialAnimation* Owner;
};

class FMatAnimAggregate final : public IAggregateComponent
{
public:
    FMatAnimAggregate();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    GENRESULT NEURONCOMPONENTS_API Initialize() override;

    FMaterialAnimation* Owner;
};

struct FCrcAnimSlot
{
    unsigned long Crc;
    FImaData* Data;
};

class FMaterialAnimation final : public IMatAnimLibrary
{
public:
    FMaterialAnimation();
    ~FMaterialAnimation();

    GENRESULT NEURONCOMPONENTS_API QueryInterface(const char* Iid, void** Out) override;
    unsigned long NEURONCOMPONENTS_API AddRef() override;
    unsigned long NEURONCOMPONENTS_API Release() override;
    void* NEURONCOMPONENTS_API LoadAnimationGroup(void* FileSystem) override;
    GENRESULT NEURONCOMPONENTS_API UnloadAnimationGroup(void* Arg) override;
    GENRESULT NEURONCOMPONENTS_API CreateAnimationInstance(int Unused, void* Query, IImaInstance** OutInstance) override;
    GENRESULT NEURONCOMPONENTS_API TickAnimationInstances(int Filter, float Dt) override;

    FMatAnimAggregate Aggregate; // +4
    unsigned long RefCount;
    INeuronComponent* Outer; // baseline controlling unknown at +0x1C
    FMaterialAnimationController Controller; // baseline non-delegating unknown @ +0x20
    std::vector<FCrcAnimSlot> CrcMap;

    FImaData* FindData(unsigned long Crc) const;
    void StoreData(unsigned long Crc, FImaData* Data);
};
