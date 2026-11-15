// Author: Giperion.

#include "gf_scenescript_component.h"

#include "NeuronRuntime.h"
#include "common_goods.h"   // HardpointSummary
#include "common_misc.h"    // malloc_strcpy / malloc_strfree
#include "gf_script_object.h"

#include <cstdint>
#include <cstring>
#include <new>

namespace
{
    char const* const GScriptBaseFile = "Client\\Gf\\ScriptBase.cpp";

    // The QueryInterface table at 0x005cca20: {IID, facet offset}, null-terminated. An offset
    // with the high bit set would name a field holding the facet pointer (the branch
    // JNS); none of baseline's nine entries uses it.
    struct FSceneScriptFacetEntry
    {
        char const* Iid;
        unsigned long Offset;
    };

    FSceneScriptFacetEntry const G_SceneScriptFacetTable[] = {
        {Iids::CompoundControl, 0x00},     {Iids::SoundControl, 0x04},
        {Iids::IKControl, 0x08},           {Iids::GeoTransformControl, 0x0c},
        {Iids::CameraControl, 0x10},       {Iids::D3DLightControl, 0x14},
        {Iids::PSysControl, 0x18},         {Iids::FogControl, 0x1c},
        {Iids::ReverbControl, 0x20},       {nullptr, 0},
    };

    long InstanceHandle(void* Instance)
    {
        return static_cast<long>(reinterpret_cast<intptr_t>(Instance));
    }

    void* InstancePointer(long Instance)
    {
        return reinterpret_cast<void*>(static_cast<intptr_t>(Instance));
    }
}

// . 0x0044fc25..0x0044fca0: +0x24 -1, +0x28 1, the nine vptrs, 0xc dwords of the
// frame to +0x2c, +0x5c Owner, +0x60 Desc+0x00, malloc_strcpy(+0x64, Desc+0x38), +0x68 the low
// bit of Desc+0x04. Name is not cleared first: malloc_strcpy overwrites it.
FSceneScriptComponent::FSceneScriptComponent(Csys const& ScriptFrame, FGfScript* InOwner,
                                 FSceneScriptEntitySnap const* Desc)
    : EntityIndex(-1),
      RefCount(1),
      Frame(ScriptFrame),
      Owner(InOwner),
      Type(Desc->Type)
{
    malloc_strcpy(&Name, Desc->Name);
    bReference = static_cast<unsigned char>(Desc->Flags & 1);
}

// / the routine.
FSceneScriptComponent::~FSceneScriptComponent()
{
    malloc_strfree(&Name);
}

// (RET 0xc).
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::QueryInterface(const char* Iid, void** Out)
{
    unsigned char* Base = reinterpret_cast<unsigned char*>(static_cast<ICompoundControl*>(this));
    int Index = 0;
    for (; G_SceneScriptFacetTable[Index].Iid != nullptr; ++Index)
    {
        if (std::strcmp(G_SceneScriptFacetTable[Index].Iid, Iid) != 0)
            continue;

        unsigned long const Offset = G_SceneScriptFacetTable[Index].Offset;
        INeuronComponent* Facet = nullptr;
        if ((Offset & 0x80000000u) != 0)
            Facet = *reinterpret_cast<INeuronComponent**>(Base + (Offset & 0x7fffffffu));
        else
            Facet = reinterpret_cast<INeuronComponent*>(Base + Offset);
        Facet->AddRef();
        *Out = Facet;
        return GR_OK;
    }
    *Out = nullptr;
    return GR_INTERFACE_UNSUPPORTED;
}

// .
unsigned long NEURONCOMPONENTS_API FSceneScriptComponent::AddRef()
{
    ++RefCount;
    return RefCount;
}

// . 0x00450197 `TEST EAX,EAX; JBE` -- a count of 0 is not decremented. At zero the
// count goes back to 1 and the routine (operator delete) frees the block: the destructor does
// NOT run, so the name copy (and a derived class's resources) is never released on this path.
// The scalar deleting destructor (slot 13) is the path that does run it.
unsigned long NEURONCOMPONENTS_API FSceneScriptComponent::Release()
{
    if (RefCount > 0)
        --RefCount;
    if (RefCount != 0)
        return RefCount;
    RefCount = 1;
    ::operator delete(static_cast<void*>(this));
    return 0;
}

// --- defaults -------------------------------------------------------------------------------
// MOV EAX,0xfffffff9; RET 8. the branch / fcd0 / fce0 / fcf0: the same answer
// with RET 4 / 0x10 / 0xc / 0x14. the branch: XOR EAX,EAX; RET 8.

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StartMotion(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StopMotion(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetFloorHeight(float) { return GR_OK; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::AdvanceTime(int) { return GR_OK; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::ConnectHardpoints(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::DisconnectHardpoints(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetHardpointXform(int, char const*, float*)
{
    return kSceneScriptSlotNotImplemented;
}

// *(dword*)Out = 0; RET 8.
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetFloorHeight(float* OutHeight)
{
    *OutHeight = 0.0f;
    return GR_OK;
}

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetTransform(float const*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetTransform(float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StartSound(void*, int) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StopSound(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetAttenuation(float) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetPan(float) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetAttenuation(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetPan(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetAudioProps(float const*, int) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetAudioProps(float*, int) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StartIk(void*, float*, float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StopIk(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::UpdateIk(void*, float*, float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetPosition(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetOrientation(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetPosition(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetOrientation(float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetFov(const float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::CameraSlot4(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetAspect(const float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetNear(const float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetFar(const float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetFov(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::CameraSlot9(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetAspect(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetNear(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetFar(float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetOn(int) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetLight(void const*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetDirection(float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetOn(int*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetLight(void*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetDirection(float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::Start() { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::StopEffect() { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::Tick(int) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetSparam(float) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetSparam(float*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetOn(unsigned char) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetOn(unsigned char*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetColor(int const*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetColor(int*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetRange(float, float, float) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetRangeValue(const float*, const float*, const float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetRange(float*, float*, float*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetTableMode(unsigned char, unsigned int)
{
    return kSceneScriptSlotNotImplemented;
}
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetTableMode(unsigned char*, unsigned int*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetPacked(unsigned char const*) { return kSceneScriptSlotNotImplemented; }
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetPacked(unsigned char*) { return kSceneScriptSlotNotImplemented; }

GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::SetReverb(unsigned int, float, float, float)
{
    return kSceneScriptSlotNotImplemented;
}
GENRESULT NEURONCOMPONENTS_API FSceneScriptComponent::GetReverb(float*, float*, float*, float*)
{
    return kSceneScriptSlotNotImplemented;
}

// --- the identity tail's base bodies --------------------------------------------------------
void FSceneScriptComponent::Stop() {}
void FSceneScriptComponent::SetSpawnTable(FSceneScriptComponent**, int) {}
bool FSceneScriptComponent::FindActor(char const*, void**, void**) { return false; }
void FSceneScriptComponent::DrawEffects(ICamera*) {}
void FSceneScriptComponent::SetScene(void*) {}
void FSceneScriptComponent::SetFlags(unsigned char, unsigned char, unsigned char, unsigned char) {}
void FSceneScriptComponent::RefreshSound() {}
bool FSceneScriptComponent::IsBackgroundSound() { return false; }

// . 0x00450070..0x004500be: a reference entity never matches; otherwise the type must
// be equal and the name equal under the inlined exact strcmp.
bool FSceneScriptComponent::MatchesEntity(int InType, char const* InName) const
{
    if (bReference != 0)
        return false;
    if (Type != InType)
        return false;
    return std::strcmp(Name, InName) == 0;
}

// . The two summaries are locals with destructors (EH states 0 / 1, unwind
// ), built A then B; both are analyzed, keyed and searched A then B, and destroy()ed
// A then B before the destructors run (B, then A).
void FSceneScriptComponent::ConnectEntityHardpoints(int IndexA, char const* HpA, int IndexB,
                                              char const* HpB)
{
    FSceneScriptComponent* EntityA = Owner->Spawned[IndexA];
    FSceneScriptComponent* EntityB = Owner->Spawned[IndexB];
    if (EntityA == nullptr || EntityB == nullptr)
        return;

    HardpointSummary SummaryA;
    HardpointSummary SummaryB;
    SummaryA.analyze_instance(InstanceHandle(EntityA->GetEngineInstance()));
    SummaryB.analyze_instance(InstanceHandle(EntityB->GetEngineInstance()));

    FHardpointKey KeyA;
    KeyA.Name = HpA;
    KeyA.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpA);
    FHardpointKey KeyB;
    KeyB.Name = HpB;
    KeyB.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpB);

    long const InstanceA = SummaryA.find_name_instance(HpA);
    long const InstanceB = SummaryB.find_name_instance(HpB);
    if (NeuronRuntime::Hardpoint->ConnectHardpoints(InstancePointer(InstanceB), &KeyB,
                                            InstancePointer(InstanceA), &KeyA) != 0)
    {
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: Cannot connect hardpoint %s on entity %s to hardpoint %s on "
              "entity %s",
              GScriptBaseFile, 0x5e, HpB, EntityB->Name, HpA, EntityA->Name);
    }

    SummaryA.destroy();
    SummaryB.destroy();
}

// . Same shape; the joint goes away through NeuronRuntime::Engine slot 48 (+0xc0) with
// (instance B, instance A), and anything but 1 is reported.
void FSceneScriptComponent::DisconnectEntityHardpoints(int IndexA, char const* HpA, int IndexB,
                                                 char const* HpB)
{
    FSceneScriptComponent* EntityA = Owner->Spawned[IndexA];
    FSceneScriptComponent* EntityB = Owner->Spawned[IndexB];
    if (EntityA == nullptr || EntityB == nullptr)
        return;

    HardpointSummary SummaryA;
    HardpointSummary SummaryB;
    SummaryA.analyze_instance(InstanceHandle(EntityA->GetEngineInstance()));
    SummaryB.analyze_instance(InstanceHandle(EntityB->GetEngineInstance()));

    long const InstanceA = SummaryA.find_name_instance(HpA);
    long const InstanceB = SummaryB.find_name_instance(HpB);
    if (NeuronRuntime::Engine->DestroyJoint(InstancePointer(InstanceB), InstancePointer(InstanceA)) != 1)
    {
        FDUMP(0x100001,
              "%s(%d) : *** ERROR: Cannot disconnect hardpoint %s on entity %s from hardpoint %s "
              "on entity %s",
              GScriptBaseFile, 0x81, HpB, EntityB->Name, HpA, EntityA->Name);
    }

    SummaryA.destroy();
    SummaryB.destroy();
}
