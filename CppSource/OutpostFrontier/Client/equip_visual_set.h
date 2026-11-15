// Author: Giperion.

#pragma once

// The equipment-visual set: the engine instances that show the equipment mounted on a ship's
// hardpoints (guns, thrusters, lights), plus the blinking lights of the light items. baseline
// OutpostFrontier.exe 0x00457d60..0x004582c0, no vtable and no RTTI, so the baseline class name is
// unknown; the name here is descriptive. operator new(0x2c) at 0x00585173 (ShipViewControl)
// and inside the other two owners (callers 0x004438xx / 0x004470xx .. 0x004478xx).

// Layout (ctor the routine):
//   +0x00 HardpointSummary* (operator new(0x20) in the routine; the exe's own copy of the
//         HardpointSummary vftable 0x005cbd68, whose slot 0 the routine is the plain
//         ~HardpointSummary + operator delete)
//   +0x04 hardpoint count
//   +0x08 long[count]      engine instance of the item on each hardpoint (-1 when empty)
//   +0x0c unsigned[count]  archetype id of that item (0 when empty)
//   +0x10 VC6 std::vector<FCliMaterialEntry*> (allocator byte, begin +0x14, end +0x18, cap +0x1c)
//         -- the material-library entries the routine hands over for the attached meshes
//   +0x20 FFlashLightSet (the routine)

#include "flash_light.h"
#include "client_cliobj.h"

#include <vector>

class EquipDescList;
class HardpointSummary;
struct ICamera;

namespace Archetype
{
    struct Equipment;
}

struct FEquipVisualSet
{
    // the four head fields zeroed, the vector and the flashlight set empty.
    FEquipVisualSet();
    // Clear(), then the flashlight set and the vector buffer.
    ~FEquipVisualSet();

    // zero the four head fields and nothing else (no release).
    void ResetFields();
    // release every material entry and empty the vector, destroy every item
    // instance, empty the flashlight set, free both arrays and delete the summary.
    void Clear();
    // (RET 4) -- Instances[Index], no range check.
    long GetInstance(int Index) const;
    // (RET 4) -- a new HardpointSummary analysed on RootInstance, and both
    // per-hardpoint arrays sized to its count (-1 / 0).
    void Populate(long RootInstance);
    // (RET 8) -- put Equip's visual attachment on hardpoint HpIndex: either
    // connect the attachment's HpChild hardpoint to it (NeuronRuntime::Hardpoint slot 6) or join it to
    // the hardpoint's frame (IEngine::CreateJoint). False when the hardpoint is taken, the good
    // is unknown or has no visual attachment.
    bool Attach(int HpIndex, Archetype::Equipment const* Equip);
    // (RET 4) -- destroy the instance on HpIndex (when there is one) and clear
    // its archetype id.
    void Detach(int HpIndex);
    // (RET 4) -- Attach every non-internal item of List whose hardpoint the
    // summary knows.
    void AttachAll(EquipDescList const* List);
    // (RET 4) -- a flashlight for every non-internal always-on light item of List
    // whose hardpoint the summary knows.
    void AddFlashLights(EquipDescList const* List);
    // JMP the routine on +0x20.
    void DrawFlashLights(ICamera* Camera);
    // (RET 4) -- the routine on +0x20 with the summary.
    void UpdateFlashLights(float Delta);

    HardpointSummary* Summary;                                  // +0x00
    int Count;                                                  // +0x04
    long* Instances;                                            // +0x08
    unsigned* ArchIds;                                          // +0x0c
    std::vector<Archetype::FCliMaterialEntry*> MaterialEntries; // +0x10
    FFlashLightSet FlashLights;                                 // +0x20

private:
    FEquipVisualSet(FEquipVisualSet const&) = delete;
    FEquipVisualSet& operator=(FEquipVisualSet const&) = delete;
};
