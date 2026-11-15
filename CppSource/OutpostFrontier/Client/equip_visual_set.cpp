// Author: Giperion.

// The equipment-visual set -- see equip_visual_set.h. baseline OutpostFrontier.exe 0x00457d60..0x004582c0.

#include "equip_visual_set.h"

#include "misc_util.h"        // MiscUtilLoadEngineInstance (the routine)

#include "NeuronRuntime.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"
#include "common_archetype.h"
#include "common_goods.h"

#include <stdint.h>
#include <string.h>

FEquipVisualSet::FEquipVisualSet()
    : Summary(nullptr), Count(0), Instances(nullptr), ArchIds(nullptr)
{
    // the vector (+0x10..+0x1c) and the routine on +0x20 first, then the four
    // head dwords -- the member order of the initializer list above is only notation.
}

FEquipVisualSet::~FEquipVisualSet()
{
    // the routine, then the routine on +0x20 and operator delete on the vector
    // buffer -- the member destructors in reverse declaration order.
    Clear();
}

void FEquipVisualSet::ResetFields()
{
    // .
    Summary = nullptr;
    Count = 0;
    Instances = nullptr;
    ArchIds = nullptr;
}

void FEquipVisualSet::Clear()
{
    // . 0x00457e36..0x00457e58: the routine on every slot of the vector, then
    // the erase of [begin, end).
    for (size_t Index = 0; Index < MaterialEntries.size(); ++Index)
        Archetype::ReleaseMaterialEntrySlot(&MaterialEntries[Index]);
    MaterialEntries.clear();

    // IEngine slot 24 (+0x60) on every live item instance.
    for (int Index = 0; Index < Count; ++Index)
    {
        if (Instances[Index] != -1)
        {
            NeuronRuntime::Engine->DestroyInstance(reinterpret_cast<void*>(Instances[Index]));
            Instances[Index] = -1;
        }
        ArchIds[Index] = 0;
    }

    // the routine.
    FlashLights.Clear();

    // operator delete on both arrays, then the summary's deleting
    // destructor (vftable slot 0 with 1) when there is one.
    operator delete(Instances);
    operator delete(ArchIds);
    if (Summary != nullptr)
        delete Summary;

    Summary = nullptr;
    Count = 0;
    Instances = nullptr;
    ArchIds = nullptr;
}

long FEquipVisualSet::GetInstance(int Index) const
{
    // .
    return Instances[Index];
}

void FEquipVisualSet::Populate(long RootInstance)
{
    // . operator new(0x20) + the Common ctor (0x00457f48); baseline tests the
    // allocation and stores 0 on failure, which the throwing new of the port never returns.
    Summary = new HardpointSummary();
    Summary->analyze_instance(RootInstance);
    Count = Summary->get_count();
    Instances = static_cast<long*>(operator new(static_cast<size_t>(Count) * sizeof(long)));
    ArchIds = static_cast<unsigned*>(operator new(static_cast<size_t>(Count) * sizeof(unsigned)));
    for (int Index = 0; Index < Count; ++Index)
    {
        Instances[Index] = -1;
        ArchIds[Index] = 0;
    }
}

bool FEquipVisualSet::Attach(int HpIndex, Archetype::Equipment const* Equip)
{
    // .
    if (Instances[HpIndex] != -1)
        return false;

    // the archetype id is recorded before the good is even looked up.
    unsigned const ArchId = Equip->Nickname.Id;
    ArchIds[HpIndex] = ArchId;
    Archetype::ICliObj* const Companion = Equip->Companion;

    GoodInfo const* const Good = GoodList::find_by_archetype(ArchId);
    if (Good == nullptr)
        return false;
    if (!Good->has_visual_attachment())
        return false;

    // the routine(attachment, NeuronRuntime::Data, 0, 0, companion +0x20,
    // &this+0x10, 0).
    Instances[HpIndex] = static_cast<long>(reinterpret_cast<intptr_t>(MiscUtilLoadEngineInstance(
        Good->AttachmentArchetype.Value, NeuronRuntime::Data, nullptr, nullptr,
        Archetype::CliObjMaterialPaths(Companion), &MaterialEntries, nullptr)));

    char const* const HpChild = Good->HpChild.Value;
    if (HpChild != nullptr)
    {
        // the attachment's HpChild hardpoint is connected to the
        // ship's hardpoint -- NeuronRuntime::Hardpoint slot 6 (parent instance, {name, crc},
        // attachment instance, {HpChild, crc}).
        FHardpointKey ChildKey;
        ChildKey.Name = HpChild;
        ChildKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpChild);
        FHardpointKey ParentKey;
        ParentKey.Name = Summary->get_name(HpIndex);
        ParentKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(ParentKey.Name);
        void* const Child = reinterpret_cast<void*>(Instances[HpIndex]);
        void* const Parent = reinterpret_cast<void*>(Summary->get_instance(HpIndex));
        NeuronRuntime::Hardpoint->ConnectHardpoints(Parent, &ParentKey, Child, &ChildKey);
        return true;
    }

    // a fixed joint (type 0) at the hardpoint's position and
    // orientation -- IEngine slot 47 (+0xbc) CreateJoint(parent, attachment, payload). The
    // payload is baseline's full 0x5C-byte joint block (ESP+0x20..0x7c, the top of the 0x6c frame;
    // SceneRuntime the routine copies all 0x17 dwords): 0x004580e0 writes [0] = 0, then [4..6] and
    // [7..15]. Dwords 1..3 and 16..22 are never written by baseline (stack garbage); the port
    // zeroes them.
    unsigned long Payload[G_EngineJointPayloadDwords];
    memset(Payload, 0, sizeof(Payload));
    HardpointInfo const& PosInfo = Summary->get_info(HpIndex);
    memcpy(&Payload[4], &PosInfo.Position, 3 * sizeof(float));
    HardpointInfo const& OrientInfo = Summary->get_info(HpIndex);
    memcpy(&Payload[7], &OrientInfo.Orient, 9 * sizeof(float));
    void* const Child = reinterpret_cast<void*>(Instances[HpIndex]);
    void* const Parent = reinterpret_cast<void*>(Summary->get_instance(HpIndex));
    NeuronRuntime::Engine->CreateJoint(Parent, Child, Payload);
    return true;
}

void FEquipVisualSet::Detach(int HpIndex)
{
    // .
    if (Instances[HpIndex] != -1)
    {
        NeuronRuntime::Engine->DestroyInstance(reinterpret_cast<void*>(Instances[HpIndex]));
        Instances[HpIndex] = -1;
    }
    ArchIds[HpIndex] = 0;
}

void FEquipVisualSet::AttachAll(EquipDescList const* List)
{
    // the list in its own order (node +0x08 is the EquipDesc).
    for (std::list<EquipDesc>::const_iterator It = List->Items.begin(); It != List->Items.end(); ++It)
    {
        EquipDesc const& Desc = *It;
        Archetype::Equipment* const Equip = Archetype::GetEquipment(Desc.ArchId);
        if (Desc.is_internal())
            continue;
        int const HpIndex = Summary->find_name(Desc.Hardpoint);
        if (HpIndex == -1)
            continue;
        Attach(HpIndex, Equip);
    }
}

void FEquipVisualSet::AddFlashLights(EquipDescList const* List)
{
    // .
    for (std::list<EquipDesc>::const_iterator It = List->Items.begin(); It != List->Items.end(); ++It)
    {
        EquipDesc const& Desc = *It;
        Archetype::Equipment* const Equip = Archetype::GetEquipment(Desc.ArchId);
        if (Desc.is_internal())
            continue;
        int const HpIndex = Summary->find_name(Desc.Hardpoint);
        if (HpIndex == -1)
            continue;
        // slot 4 get_class_type == 0x15 (Light) and always_on (+0x70).
        Archetype::ICliObj* const Companion = Equip->Companion;
        if (Equip->get_class_type() != Archetype::AClassLight)
            continue;
        if (!static_cast<Archetype::Light const*>(static_cast<Archetype::Root const*>(Equip))
                 ->bAlwaysOn)
            continue;
        FlashLights.Add(Companion, HpIndex);
    }
}

void FEquipVisualSet::DrawFlashLights(ICamera* Camera)
{
    // .
    FlashLights.Draw(Camera);
}

void FEquipVisualSet::UpdateFlashLights(float Delta)
{
    // .
    FlashLights.Update(Summary, Delta);
}
