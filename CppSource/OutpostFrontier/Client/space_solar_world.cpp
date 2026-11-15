// Author: Giperion.

#include "space_solar_world.h"

#include "character_select_equip.h"
#include "client_cliobj.h" // Archetype::LoadMountedEquipmentCompanions / HookUpInstanceTreeMaterials
#include "client_gameobj.h"
#include "client_objdb.h"

#include "common_archetype.h"
#include "common_cobject.h"
#include "NeuronRuntime.h"
#include "engine/sceneruntime.h"
#include "render/rendcomp.h"

#include <cstdarg>
#include <cstdint>

#include <cstdio>
#include <cstring>
#include <new>
#include <windows.h>

// The anim-name list CSolar::ReadObj fills ("anim" keys) is CSolar::CreateParms::ContinualAnims,
// a std::list member: Parms' destructor frees it, as baseline's inlined ~CreateParms does on both
// exits (0x00548c95..0x00548d15 and 0x00548d1f..0x00548d80: the +0xe4 list, then the +0xac list
// and the +0x9c EquipDescVector).
bool SpawnWorldSolarFromObjectSection(INI_Reader& Reader)
{

    //   get_state → ReadObj → operator_new(0x208) the routine/the routine(2)
    //   → create(+0x168)=the routine → CSolar::init(ObjDb=the field)
    //   → the routine → the routine(gameobj,0) → CliObjLoad(1) → Release arch
    // Visual handle for neighbor: Solar->EngineInstanceId (IObjInspect::get_index).
    // Vibe::Verify in ReadObj is seeded by SolarReps::LoadObjectVibesFromSystemIni
    // during Universe::LoadSystemFile — not by ad-hoc EnsureExists.
    // is FClientSolarGameobj::ReadIni (client_gameobj.cpp).
    // Phase: LoadSpaceSceneSlice / connect — not inside BeginScene.

    // the reader's position at the section start, restored before the
    // client half re-reads the section (0x00548c73).
    INI_Reader::State SectionStart;
    Reader.get_state(SectionStart);

    // the stack CSolar::CreateParms at [ESP+0x20], built in baseline's
    // order -- the routine on the CSimple head (+0x00..+0x58), +0x5c = 0, +0x60 = -1,
    // on +0x64 (+0x64..+0x70 and the count +0x94), +0x98 = 0, the EquipDescVector
    // (+0x9c) and the collision-group list (+0xac) constructed, ID_String::clear on +0xb8, byte
    // +0xbc = 0, clear on +0xc0, bytes +0xc4 / +0xc5 = 0, +0xc8 = 0, clear on +0xcc, +0xd0..+0xe0
    // = 0, and the anim list (+0xe4, the routine). Value-initialisation zeroes what baseline
    // leaves unwritten (+0x74..+0x90); ReadObj fills the rest.
    CSolar::CreateParms Parms{};
    InitCreateParmsScratchbaseline(&Parms);
    Parms.DockingCount = 0;
    Parms.VibeId = 0xffffffffu;
    Parms.SpaceCostume.PartIds[0] = 0;
    Parms.SpaceCostume.PartIds[1] = 0;
    Parms.SpaceCostume.PartIds[2] = 0;
    Parms.SpaceCostume.PartIds[3] = 0;
    Parms.SpaceCostume.AccessoryCount = 0;
    Parms.VoiceId = 0;
    Parms.NicknameSrc.clear();
    Parms.VisitValue = 0;
    reinterpret_cast<ID_String&>(Parms.ParentNicknameId).clear();
    Parms.bDestructible = 0;
    Parms.bDynamic = 0;
    Parms.DestSystem = 0;
    reinterpret_cast<ID_String&>(Parms.DestGateId).clear();
    Parms.PrevRing = 0;
    Parms.NextRing = 0;
    Parms.TradelaneSpaceNameId = 0;
    Parms.AtmosphereRange = 0.0f;
    Parms.DockWithBaseId = 0;

    // ReadObj's bool is not looked at -- the archetype it returned
    // is. Parms.ObjDb (+0x04) is set to the field before that test.
    Archetype::Solar* OutArch = nullptr;
    CSolar::ReadObj(Reader, OutArch, Parms);
    Parms.ObjDb = GetClientObjDb();
    if (OutArch == nullptr)
    {
        // ~CreateParms, return false.
        return false;
    }

    // the archetype's vt+0x0c, Archetype::Root::load.
    OutArch->load();

    // operator new(0x208) + the solar gameobj constructor. baseline has
    // no failure path after the allocation (a null ESI is dereferenced at 0x00548bf6).
    FClientGameobj* const Go = ClientGameobj_CreateSolar();

    // Slot 90 returns nothing; baseline the routine (0x00548c03) goes on without a check.
    Go->create(OutArch);

    // the object (+0x10) narrowed to a CSolar by
    // (ObjectClass & 0x303) == 0x303, else null, then CSolar::init with no null test.
    CObject* const Obj = Go->Object;
    CSolar* const Solar =
        (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
            ? static_cast<CSolar*>(Obj)
            : nullptr;
    Solar->init(Parms);

    // the archetype's companion (+0x04) vt+0x20 Load(1), no null test.
    OutArch->Companion->Load(1);

    // the routine(&equipment, 1), then the routine(get_index()).
    Archetype::LoadMountedEquipmentCompanions(&Parms.Equipment, 1);
    Archetype::HookUpInstanceTreeMaterials(Go->get_index());

    // back to the section start, then the client half the routine
    // (ring / burn_color / dock_script / voice / jump_effect / star, then the nebula).
    Reader.set_state(SectionStart);
    static_cast<FClientSolarGameobj*>(Go)->ReadIni(Reader);

    ClientObjDb_RegisterGameobj(Go); // the routine(Go, 0)

    // Archetype::Root::Release; ~CreateParms follows, return true.
    OutArch->Release();
    return true;
}

void SolarSpheresApplyDetail(float Detail)
{
    // (cdecl). The whole computation stays on the x87 stack (extended precision),
    // so it is done in double here.

    // T = Detail + Detail - 1.0, then FCOM 1.0 / TEST AH,0x41 / JNZ: only a
    // T above 1.0 is replaced by 1.0; otherwise FCOM 0.0 / TEST AH,0x5 / JP: only a T below
    // 0.0 is replaced by 0.0 (an unordered T passes both).
    double T = static_cast<double>(Detail) + static_cast<double>(Detail) - 1.0;
    if (T > 1.0)
        T = 1.0;
    else if (T < 0.0)
        T = 0.0;

    // ftol(3.0 * T + 7.0) -> EBX, then ftol(3.5 - (T + T)) -> [ESP+0x10].
    int const Tessellation = static_cast<int>(3.0 * T + 7.0);
    int const Level = static_cast<int>(3.5 - (T + T));

    // the routine on bucket 2 (solars); walk ListNext (+0x14).
    int const SolarKind = 2;
    for (FClientGameobj* Go = ClientObjDb_GetKindHead(&SolarKind); Go != nullptr;
         Go = Go->ListNext)
    {
        // the object (+0x10) narrowed to a CSolar by
        // (ObjectClass & 0x303) == 0x303, else null, and its type byte (+0xe0) tested
        // against 7; zero -> next. Solar buckets only hold solars, so the null is never read.
        CObject* const Obj = Go->Object;
        CSolar* Solar = nullptr;
        if (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 0x303u) == 0x303u)
            Solar = static_cast<CSolar*>(Obj);
        if ((static_cast<unsigned char>(Solar->TypeBits) & 7u) == 0u)
            continue;

        // the same object narrowed to a CSimple ((ObjectClass & 3) == 3),
        // its engine instance (+0x04) queried for "1.11_ISphereMesh" through IEngine +0x78.
        CObject* Simple = nullptr;
        if (Obj != nullptr && (static_cast<unsigned>(Obj->ObjectClass) & 3u) == 3u)
            Simple = Obj;
        ISphereMesh* Sphere = nullptr;
        GENRESULT const Result = NeuronRuntime::Engine->QueryInstance(
            reinterpret_cast<void*>(static_cast<intptr_t>(Simple->EngineInstanceId)),
            Iids::SphereMesh, reinterpret_cast<void**>(&Sphere));
        if (Result < 0)
        {
            // a failed query still releases whatever came back.
            if (Sphere != nullptr)
                Sphere->Release();
            continue;
        }

        // Slot07(Tessellation), then Slot03(float(Level)) (FILD).
        Sphere->SetMaxTessellationLevel(Tessellation);
        Sphere->SetLodDistanceScale(static_cast<float>(Level));
        // .
        if (Sphere != nullptr)
            Sphere->Release();
    }
}
