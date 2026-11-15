// Author: Giperion.

#pragma once

// Gf::ObjectProp -- Client\Gf\ObjectProp.cpp (OutpostFrontier.exe
// ). A room prop: one engine instance loaded from a PetalDB record or an
// archetype, optionally with the ship loadout's equipment attached and its lights blinking.
// RoomControl builds one per RoomData+0x158 record (the routine); the SceneScript spawn the routine
// builds one per COMPOUND entity. Vtable 0x005cbc0c, sizeof 0x80 (operator new(0x80) at
// and 0x0044e7ab).

#include "anim_script_set.h"
#include "common_csys.h"
#include "flash_light.h"
#include "room_control.h"

#include <list>
#include <string>

class HardpointSummary;

// The prop description the routine / the routine fill on the stack and the routine reads
// (sizeof 0x64; three VC6 strings at +0x00 / +0x10 / +0x54).
struct FPropData
{
    // empty strings, Type -1, Pos 0, Orient identity.
    FPropData();
    // the three strings, last to first.

    std::string Template;   // +0x00 the archetype / PetalDB nickname (or a "$..." indirection)
    std::string Name;       // +0x10 the entity name (the routine finds props by it)
    int Type;               // +0x20 0/5/6 PetalDB category, 1 ship, 2 solar, 3 asteroid, 4 equip
    Vector Pos;             // +0x24
    Matrix Orient;          // +0x30
    std::string Loadout;    // +0x54 only read for Type 1
};

class FObjectProp : public FRoomObject
{
public:
    // (thiscall, RET 8).
    FObjectProp(FRoomRuntime* InOwner, FPropData const& Data);
    // (the routine is the scalar deleting destructor).
    ~FObjectProp() override;

    void Update(double DeltaSeconds) override;      // 5  the routine
    void* GetEngineInstance() override;             // 6  the branch: +0x30
    void DrawOpaque(ICamera* Camera) override;      // 7  the routine
    void DrawTranslucent(ICamera* Camera) override; // 8  the routine

    // RoomControl's reads (the routine 0x0044a2a3 / the routine 0x0044e506 read +0x58, the
    // script table's +0x0c; the routine 0x00449566 tests +0x49).
    unsigned long GetMainScript() const { return Scripts.Script; }
    char const* GetName() const { return Name.c_str(); }

    void* Instance;                                // +0x30 -1 until a mesh loaded
    int Type;                                      // +0x34 FPropData::Type
    std::string Name;                              // +0x38 FPropData::Name
    unsigned char bDetached;                       // +0x48 0; gates the draws and the release
    unsigned char bRoomMain;                       // +0x49 RoomPropInfo+0x3f (the routine)
    FAnimScriptSet Scripts;                        // +0x4c the routine / the routine
    HardpointSummary* Hardpoints;                  // +0x64 the routine, when a loadout loaded
    std::list<void*> Attached;                     // +0x68 loadout equipment instances
    FFlashLightSet Flashlights;                    // +0x74 the routine

private:
    // (thiscall, RET 4) -- load the mesh the description names, place it.
    void Load(FPropData const& Data);
    // (thiscall, RET 8) -- load a PetalDB record's mesh with its extra materials.
    void LoadPetal(void const* Record, char const* Template);
    // (thiscall, RET 4) -- the nickname id of Template; "$..." names one of the
    // current base's ship / terrain nicknames.
    unsigned ResolveTemplate(char const* Template);
    // (thiscall, RET 4) -- attach a ship loadout's equipment.
    void LoadLoadout(char const* LoadoutName);
    // (thiscall, RET 8) -- attach one loadout item.
    void AttachEquipment(void const* Item, HardpointSummary* Summary);
};
