// Author: Giperion.

#include "object_prop.h"

#include "client_cliobj.h"
#include "engine_light.h"
#include "game_options.h"
#include "misc_util.h"

#include "NeuronRuntime.h"
#include "common_archetype.h"
#include "common_fuses.h"
#include "common_goods.h"
#include "common_id.h"
#include "common_newchar.h"
#include "common_universe.h"
#include "neuroncomponents.h"
#include "engine/sceneruntime.h"

#include <stdint.h>
#include <string.h>

namespace
{
    // .rdata 0x005cbc74 -- the source path ObjectProp.cpp hands FDUMP.
    char const kObjectPropCpp[] = "Client\\Gf\\ObjectProp.cpp";

    // The HardpointSummary the routine allocates (operator new(0x20), then the Common
    // constructor, then vptr 0x005cbd68). That vtable's only slot is the routine, the scalar
    // deleting destructor: ~HardpointSummary, then operator delete on flag bit 0.
    class FObjectPropHardpoints : public HardpointSummary
    {
    public:
        ~FObjectPropHardpoints() override {}
    };

    // (cdecl) -- the routine(Handle, 0, 0) on the companion unless Handle is -1.
    void RecomputeCompanionLod(Archetype::ICliObj* Companion, long Handle)
    {
        if (Handle == -1)
            return;
        Archetype::CliObjRecomputeLodPickExplicit(Companion, Handle, nullptr, false);
    }

    // The 0x30-byte frame the routine builds at [ESP+0xc] for Engine +0x9c: the 9-dword
    // orientation first (0x00446780..0x00446789 MOVSD.REP), then the position (+0x24..+0x2c).
    struct FInstanceMatrix
    {
        Matrix Orient;
        Vector Pos;
    };
    static_assert(sizeof(FInstanceMatrix) == 0x30, "engine instance matrix 0x30");

    // / the routine 0x00446ac3..0x00446b09: which options show a prop of Type.
    bool IsPropTypeShown(int Type)
    {
        if (Type == 0)
            return GetGameOption(kGameOptBackgroundGroup) >= 1;
        if (Type == 1)
            return GetGameOption(kGameOptObjectsGroup) != 0 && GetGameOption(kGameOptShips) != 0;
        return GetGameOption(kGameOptObjectsGroup) != 0;
    }
}

FPropData::FPropData()
    : Type(-1)
{
    // .
    Pos.x = 0.0f;
    Pos.y = 0.0f;
    Pos.z = 0.0f;
    MatrixSetIdentity(Orient);
}

FObjectProp::FObjectProp(FRoomRuntime* InOwner, FPropData const& Data)
    : FRoomObject(InOwner)
    , Instance(kRoomObjectNoInstance)
    , Type(0)
    , bDetached(0)
    , bRoomMain(0)
    , Hardpoints(nullptr)
{
    // . 0x00445f7f..0x00445f89: the routine on +0x4c; 0x00445f9c..0x00445fb7: the
    // +0x68 list head and the routine on +0x74 (the member constructors above).
    Scripts.Init();
    // +0x30 = -1, +0x48 = +0x49 = 0, the routine on +0x4c, +0x64 = 0.
    Scripts.Reset();
    // .
    Load(Data);
}

FObjectProp::~FObjectProp()
{
    // . 0x00446046..0x0044608a: unless +0x48, Engine +0x60 on every attached
    // instance, then on +0x30 -- when +0x30 is not -1.
    if (bDetached == 0 && Instance != kRoomObjectNoInstance)
    {
        std::list<void*>::iterator It = Attached.begin();
        for (; It != Attached.end(); ++It)
            NeuronRuntime::Engine->DestroyInstance(*It);
        NeuronRuntime::Engine->DestroyInstance(Instance);
    }
    // the routine on +0x4c.
    Scripts.Destroy();
    // the HardpointSummary through its vtable slot 0 with 1.
    if (Hardpoints != nullptr)
    {
        delete Hardpoints;
        Hardpoints = nullptr;
    }
    // the routine on +0x74, the +0x68 list, the thunk the routine on
    // +0x4c (a second pass over the table the call above already emptied -- only stores), the
    // +0x38 string, then the routine: the member and base destructors.
}

void FObjectProp::Update(double DeltaSeconds)
{
    // . 0x00446a63..0x00446a7a: the routine on +0x74 with +0x64 while +0x14 is set.
    float const Delta = static_cast<float>(DeltaSeconds);
    if (Flag14 != 0)
        Flashlights.Update(Hardpoints, Delta);
    // Anim +0x6c (Instance, Delta).
    NeuronRuntime::Anim->MixerWalk(reinterpret_cast<unsigned long>(Instance), Delta);
    // Engine +0x6c (Instance, 2, Delta).
    NeuronRuntime::Engine->UpdateInstance(Instance, 2, AsOpaqueDword(Delta));
}

void* FObjectProp::GetEngineInstance()
{
    // MOV EAX,[ECX+0x30].
    return Instance;
}

void FObjectProp::DrawOpaque(ICamera* /*Camera*/)
{
    // .
    if (!IsPropTypeShown(Type))
        return;
    // .
    if (bDetached != 0)
        return;
    // a type-5 prop first casts its shadow (the routine with the
    // slot-6 instance, 0, 0).
    if (Type == 5)
    {
        RoomControlDrawShadow(Owner, reinterpret_cast<unsigned long>(GetEngineInstance()), 0,
                              0);
    }
    // the routine.
    RoomControlDrawObject(Owner, this);
}

void FObjectProp::DrawTranslucent(ICamera* Camera)
{
    // the same option gate, then the routine on +0x74 unless +0x48, when +0x14.
    if (!IsPropTypeShown(Type))
        return;
    if (bDetached != 0)
        return;
    if (Flag14 == 0)
        return;
    Flashlights.Draw(Camera);
}

void FObjectProp::Load(FPropData const& Data)
{
    // . 0x004464f0..0x00446516: +0x38 = the name, +0x34 = the type.
    Name = Data.Name;
    Type = Data.Type;
    char const* Template = Data.Template.c_str();

    Archetype::Root* Arch = nullptr;
    switch (Data.Type)
    {
    case 0:
    case 5:
    case 6:
    {
        // the PetalDB record of that category.
        unsigned const Id = ResolveTemplate(Template);
        PetalInterfaceDatabase::petal_record const* Record =
            PetalDB_get()->find_record(Data.Type, Id);
        LoadPetal(Record, Template);
        break;
    }
    case 1:
        Arch = Archetype::GetShip(ResolveTemplate(Template));
        break;
    case 2:
        Arch = Archetype::GetSolar(ResolveTemplate(Template));
        break;
    case 3:
        Arch = Archetype::GetAsteroid(ResolveTemplate(Template));
        break;
    case 4:
        Arch = Archetype::GetEquipment(ResolveTemplate(Template));
        break;
    default:
        break;
    }

    if (Data.Type >= 1 && Data.Type <= 4)
    {
        if (Arch == nullptr)
        {
            // / 0x004465f6 / 0x004466f3: the equipment case builds the severity with
            // (0x10000, 1), the others pass 0x100001 directly -- the same value.
            unsigned Severity = 0x100001;
            if (Data.Type == 4)
                PackNibbleByte(&Severity, 0x10000, 1);
            FDUMP(Severity, "%s(%d) : *** ERROR: No valid archetype for prop %s\n",
                  kObjectPropCpp, 0x69, Template);
        }
        else
        {
            // (0x004466a4..0x004466ec for solars and equipment):
            // +0x13 = the companion's envmap material is not empty, then the routine on the
            // archetype's mesh with the companion's material list, and the routine.
            Archetype::ICliObj* Companion = Arch->Companion;
            bEnvMap = Archetype::CliObjEnvmapMaterial(Companion)[0] != '\0' ? 1 : 0;
            long Handle = -1;
            Instance = MiscUtilLoadEngineInstance(Arch->DaArchetype.Value, NeuronRuntime::Data, &Scripts,
                                                  nullptr, Archetype::CliObjMaterialPaths(Companion),
                                                  &MaterialEntries, &Handle);
            RecomputeCompanionLod(Companion, Handle);
        }
    }

    // a ship with a loadout, while the "ship_loadouts" slider is on.
    if (Data.Type == 1 && Data.Loadout.length() != 0 && RtcSlider::is_enabled("ship_loadouts"))
        LoadLoadout(Data.Loadout.c_str());

    // the description's frame into the instance, then Engine +0x6c
    // (Instance, 0, 0).
    FInstanceMatrix Xform;
    Xform.Orient = Data.Orient;
    Xform.Pos = Data.Pos;
    NeuronRuntime::Engine->SetInstanceMatrix(Instance, &Xform);
    NeuronRuntime::Engine->UpdateInstance(Instance, 0, nullptr);
}

void FObjectProp::LoadPetal(void const* RecordPtr, char const* Template)
{
    // .
    PetalInterfaceDatabase::petal_record const* Record =
        static_cast<PetalInterfaceDatabase::petal_record const*>(RecordPtr);
    if (Record == nullptr)
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: No valid PetalDB archetype for prop %s\n",
              kObjectPropCpp, 0x86, Template);
        return;
    }

    // the record's extra material names, each as {name, -1} through
    // (CacheString::clear first, then the name stored over it).
    Archetype::FCliVc6Vector<Archetype::FCliMaterialPathPair> Extras;
    Extras.Cookie = 0;
    Extras.Begin = nullptr;
    Extras.End = nullptr;
    Extras.Capacity = nullptr;
    std::vector<char const*>::const_iterator It = Record->Extras.begin();
    for (; It != Record->Extras.end(); ++It)
    {
        CacheString Path;
        Path.clear();
        Path.Value = *It;
        Archetype::PushMaterialPathPair(&Extras, Path.Value, 0xffffffffu);
    }

    // the routine on the record's mesh (or "") into +0x30.
    char const* Mesh = Record->Mesh.c_str();
    Instance = MiscUtilLoadEngineInstance(Mesh, NeuronRuntime::Data, &Scripts, nullptr, &Extras,
                                          &MaterialEntries, nullptr);
    // the list's buffer.
    delete[] Extras.Begin;
}

unsigned FObjectProp::ResolveTemplate(char const* Template)
{
    // . 0x004464d9: an ordinary name is its CreateID.
    if (Template[0] != '$')
        return CreateID(Template);

    // the current base's slot for the indirection. baseline reads each
    // through BASE's vtable (+0x18 .. +0x50, getters of +0x30 .. +0x68, Common .rdata
    // ); the port's Universe::IBase publishes the same fields directly.
    Universe::IBase* Base = Universe::get_base(GetRoomRuntimeBaseId(Owner));
    if (_stricmp(Template, "$ship_sml_01") == 0)
        return Base->ShipSml[0];
    if (_stricmp(Template, "$ship_sml_02") == 0)
        return Base->ShipSml[1];
    if (_stricmp(Template, "$ship_sml_03") == 0)
        return Base->ShipSml[2];
    if (_stricmp(Template, "$ship_mdm_01") == 0)
        return Base->ShipMdm[0];
    if (_stricmp(Template, "$ship_mdm_02") == 0)
        return Base->ShipMdm[1];
    if (_stricmp(Template, "$ship_mdm_03") == 0)
        return Base->ShipMdm[2];
    if (_stricmp(Template, "$ship_lrg_01") == 0)
        return Base->ShipLrg[0];
    if (_stricmp(Template, "$ship_lrg_02") == 0)
        return Base->ShipLrg[1];
    if (_stricmp(Template, "$ship_lrg_03") == 0)
        return Base->ShipLrg[2];
    if (_stricmp(Template, "$terrain_tiny") == 0)
        return Base->TerrainTiny;
    if (_stricmp(Template, "$terrain_sml") == 0)
        return Base->TerrainSml;
    if (_stricmp(Template, "$terrain_mdm") == 0)
        return Base->TerrainMdm;
    if (_stricmp(Template, "$terrain_lrg") == 0)
        return Base->TerrainLrg;
    if (_stricmp(Template, "$terrain_dyna_01") == 0)
        return Base->TerrainDyna01;
    if (_stricmp(Template, "$terrain_dyna_02") == 0)
        return Base->TerrainDyna02;

    // .
    FDUMP(0x100001, "%s(%d) : *** ERROR: Invalid indirection template name %s in prop\n",
          kObjectPropCpp, 0xd8, Template);
    return 0;
}

void FObjectProp::LoadLoadout(char const* LoadoutName)
{
    // .
    Loadout::Map const* LoadoutMap = Loadout::Get(CreateID(LoadoutName));
    if (LoadoutMap == nullptr)
    {
        // .
        FDUMP(0x100001, "%s(%d) : *** ERROR: Failed to find loadout %s\n", kObjectPropCpp, 0x158,
              LoadoutName);
        return;
    }

    // +0x64 = a new summary, then analyze_instance(+0x30).
    Hardpoints = new FObjectPropHardpoints();
    Hardpoints->analyze_instance(reinterpret_cast<long>(Instance));

    // the routine on every item with +0x64.
    std::vector<EquipDesc>::const_iterator It = LoadoutMap->Items.begin();
    for (; It != LoadoutMap->Items.end(); ++It)
        AttachEquipment(&*It, Hardpoints);
}

void FObjectProp::AttachEquipment(void const* ItemPtr, HardpointSummary* Summary)
{
    // .
    EquipDesc const* Item = static_cast<EquipDesc const*>(ItemPtr);
    Archetype::Equipment* Arch = Archetype::GetEquipment(Item->get_arch_id());
    Archetype::ICliObj* Companion = Arch->Companion;

    // a light that is always on blinks on its hardpoint.
    if (Arch->get_class_type() == Archetype::AClassLight)
    {
        if (!static_cast<Archetype::Light*>(Arch)->bAlwaysOn)
            return;
        int const Index = Summary->find_name(Item->get_hardpoint().Value);
        if (Index == -1)
            return;
        Flashlights.Add(Companion, Index);
        return;
    }

    // anything else only when it is attached equipment.
    if (!Arch->is_attached())
        return;

    // the routine without a script table or handle, appended to +0x68.
    void* Child = MiscUtilLoadEngineInstance(Arch->DaArchetype.Value, NeuronRuntime::Data, nullptr,
                                             nullptr, Archetype::CliObjMaterialPaths(Companion),
                                             &MaterialEntries, nullptr);
    Attached.push_back(Child);

    // Engine +0x64 (the instance's archetype) into the routine, then
    // Engine +0x3c on it.
    void* ArchHandle = NeuronRuntime::Engine->GetInstanceArchetype(Child);
    Archetype::CliObjRecomputeLodPickExplicit(Companion, reinterpret_cast<long>(ArchHandle),
                                              nullptr, false);
    NeuronRuntime::Engine->ReleaseHandle(ArchHandle);

    // with both hardpoint names, connect the item's HP_child to the
    // loadout hardpoint (Hardpoint +0x18) when the summary knows that hardpoint.
    char const* HpName = Item->get_hardpoint().Value;
    char const* HpChild = static_cast<Archetype::AttachedEquipment*>(Arch)->HpChild.Value;
    if (HpName == nullptr || HpChild == nullptr)
        return;
    long const Parent = Summary->find_name_instance(HpName);
    if (Parent == -1)
        return;
    FHardpointKey ChildKey;
    ChildKey.Name = HpChild;
    ChildKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpChild);
    FHardpointKey ParentKey;
    ParentKey.Name = HpName;
    ParentKey.Crc = NEURONCOMPONENTS_CRC::GetCRC32(HpName);
    NeuronRuntime::Hardpoint->ConnectHardpoints(reinterpret_cast<void*>(Parent), &ParentKey, Child,
                                        &ChildKey);
}
