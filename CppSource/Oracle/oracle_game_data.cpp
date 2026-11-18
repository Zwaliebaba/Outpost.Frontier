// Data dumps of the game databases of Common (work package P2.1).
//
// Oracle.exe dump runs RunGameDataDump after the dumps of Phase 1. It points
// NeuronRuntime::Data at a ReadFile DOS file system over DATA (as
// NeuronRuntime::OpenData does), reads the [Data] list of
// EXE\OutpostFrontier.ini and dispatches each value to Common's own loader,
// in order, as DataConfig::startup does. DataConfig::startup itself is not
// called: it also loads the AI state graph (work package P2.5), and
// WeaponModDB_load is not exported. GameData::load in crates/game-sim
// dispatches the same keys, so both sides intern the same strings in the
// same order.
//
// Each database is then written as one or more dumps of kind <kind>, the
// files listed in <output>\<kind>\files.txt; the Rust test
// crates/game-sim/tests/data_dump.rs renders the same lines
// (tests/support/game.rs). Nothing written here may be committed.

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <string.h>

#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "neuronruntime_core.h"
#include "common_ini.h"
#include "common_id.h"
#include "common_archetype.h"
#include "common_goods.h"
#include "common_misc.h"
#include "common_csys.h"
#include "common_reputation.h"
#include "common_universe.h"
#include "common_fuses.h"
#include "common_newchar.h"   // pub::StateGraph (the state_graph dump, P2.5)
#include "oracle_ai.h"        // OracleAiFreeStateGraph

#include <algorithm>
#include <set>

// The PhySys constants and collision functions of common_physys.h, declared
// here because that header pulls in the IVP headers.
namespace PhySys
{
    COMMON_API extern float PHYSICAL_SIM_RATE;
    COMMON_API extern int MAX_SPAWNED_MINDIST_COUNT;
    COMMON_API extern float ANOM_LIMITS_MAX_VELOCITY;
    COMMON_API extern float ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI;
    COMMON_API extern double MATERIAL_FRICTION;
    COMMON_API extern double MATERIAL_ELASTICITY;
    COMMON_API extern double MIN_TIME_BETWEEN_COLLISIONS;
    COMMON_API extern float RMGR_LOOK_AHEAD_TIME_WORLD;
    COMMON_API extern float RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD;
    COMMON_API extern float RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD;
    COMMON_API extern float RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD;
    COMMON_API extern float RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD;
    COMMON_API extern float RMGR_LOOK_AHEAD_TIME_INTRA;
    COMMON_API extern float RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA;
    COMMON_API extern float RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA;
    COMMON_API extern float RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA;
    COMMON_API extern float RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA;
    COMMON_API extern float DEFAULT_LINEAR_DAMPING;
    COMMON_API extern Vector DEFAULT_ANGULAR_DAMPING;
    COMMON_API extern float GOLEM_CHILD_MASS;
    COMMON_API extern float GOLEM_CHILD_LINEAR_DAMP;
    COMMON_API extern Vector GOLEM_CHILD_ANGULAR_DAMP;
    COMMON_API extern float GOLEM_MAX_DELTA_POSITION;
    COMMON_API extern float GOLEM_DELTA_ORIENTATION;
    COMMON_API extern float GOLEM_MAX_TORQUE;
    COMMON_API extern float GOLEM_FORCE_FACTOR;
    COMMON_API extern float GOLEM_DAMP_FACTOR;
    COMMON_API extern float GOLEM_TORQUE_FACTOR;
    COMMON_API extern float GOLEM_ANGULAR_DAMP_FACTOR;
    COMMON_API extern float RC_MAX_DELTA_POSITION;
    COMMON_API extern float RC_MAX_DELTA_ORIENTATION;
    COMMON_API extern Vector GOLEM_MAX_TRANSLATION_FORCE;
}

namespace Collision
{
    COMMON_API float __cdecl GetImpactDamageFromType(unsigned TypeId);
    COMMON_API float __cdecl CalcCollisionDamage(unsigned TypeId, Vector const& RelVel, Vector const& Normal);
}

// The NeuronRuntime globals the loaders read (NeuronRuntime.h declares them
// beside headers that need the DirectX 8 SDK): the data file system, and the
// component manager Universe::Startup creates its directory file system with
// (without a null test).
namespace NeuronRuntime
{
    NEURONRUNTIME_API extern IFileSystem* Data;
    NEURONRUNTIME_API extern ICOManager* NeuronComponents;
}

using namespace oracle;

namespace
{
    IFileSystem* GPreviousData = nullptr;

    // NeuronRuntime::Data over <dump root>\DATA (NeuronRuntime::OpenData's descriptor).
    bool OpenGameData()
    {
        const std::string Root = GFixtureDir + "\\DATA";
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = "DOS";
        Desc.Path = Root.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK || Fs == nullptr)
        {
            fprintf(stderr, "game data: no DOS file system over %s\n", Root.c_str());
            GFailed = true;
            return false;
        }
        GPreviousData = NeuronRuntime::Data;
        NeuronRuntime::Data = Fs;
        if (NeuronRuntime::NeuronComponents == nullptr)
            NeuronRuntime::NeuronComponents = NEURONCOMPONENTS_Acquire();
        return true;
    }

    void CloseGameData()
    {
        if (NeuronRuntime::Data != nullptr)
            NeuronRuntime::Data->Release();
        NeuronRuntime::Data = GPreviousData;
        GPreviousData = nullptr;
    }

    std::string F(float Value)
    {
        return Format("%08x", FloatBits(Value));
    }

    std::string D(double Value)
    {
        return Format("%016llx", DoubleBits(Value));
    }

    std::string V(const Vector& Value)
    {
        return F(Value.x) + "," + F(Value.y) + "," + F(Value.z);
    }

    // --- The [Data] list ------------------------------------------------------

    // Nickname ids the dumps cannot enumerate through an export: the [Explosion]
    // sections (the first nickname, as Archetype::LoadExplosion takes it), the
    // [Motor] sections (the last nickname, as MotorData::read keeps it) and the
    // [Loadout] sections (the first nickname, as Loadout::Load takes it) of every
    // file the list dispatches to those loaders.
    std::set<unsigned int> GExplosionIds;
    std::set<unsigned int> GMotorIds;
    std::set<unsigned int> GLoadoutIds;
    // The [ObjectTable] records of the PetalDB file: room 0, cart 5, prop 6.
    std::set<unsigned int> GPetalIds[7];
    // The set and set_less names of the [RtcSlider] sections of the RtcSlider
    // files (render_character.rs rtc_names).
    std::set<unsigned int> GRtcNames;
    // The [Data] universe value (DumpAsteroidFields).
    std::string GUniversePath;

    void ScanRtcNames(const char* FullPath)
    {
        INI_Reader Reader;
        if (!Reader.open(FullPath, false))
            return;
        while (Reader.read_header())
        {
            if (!Reader.is_header("RtcSlider"))
                continue;
            while (Reader.read_value())
            {
                if (Reader.is_value("set") || Reader.is_value("set_less"))
                    GRtcNames.insert(CreateID(Reader.get_value_string(0)));
            }
        }
    }

    void ScanPetalIds(const char* FullPath)
    {
        INI_Reader Reader;
        if (!Reader.open(FullPath, false))
            return;
        while (Reader.read_header())
        {
            if (!Reader.is_header("ObjectTable"))
                continue;
            while (Reader.read_value())
            {
                int Category = -1;
                if (Reader.is_value("room"))
                    Category = 0;
                else if (Reader.is_value("cart"))
                    Category = 5;
                else if (Reader.is_value("prop"))
                    Category = 6;
                if (Category >= 0)
                    GPetalIds[Category].insert(CreateID(Reader.get_value_string(0)));
            }
        }
    }

    void ScanIds(const char* FullPath)
    {
        INI_Reader Reader;
        if (!Reader.open(FullPath, false))
            return;
        while (Reader.read_header())
        {
            const bool bExplosion = Reader.is_header("Explosion");
            const bool bMotor = Reader.is_header("Motor");
            const bool bLoadout = Reader.is_header("Loadout");
            if (!bExplosion && !bMotor && !bLoadout)
                continue;
            bool bFound = false;
            unsigned int Id = 0;
            while (Reader.read_value())
            {
                if (!Reader.is_value("nickname"))
                    continue;
                if (bFound && !bMotor)
                    continue;
                Id = CreateID(Reader.get_value_string());
                bFound = true;
            }
            if (!bFound)
                continue;
            if (bExplosion)
                GExplosionIds.insert(Id);
            else if (bMotor)
                GMotorIds.insert(Id);
            else
                GLoadoutIds.insert(Id);
        }
    }

    // Archetype::LoadEquipment ends with Munition::LoadBeamArchIdx, which asks
    // NeuronRuntime::Engine for a compound archetype. The harness starts no
    // engine, so that call faults after the file is loaded; the fault is caught
    // here and the records stay. No C++ object lives in this frame.
    int GuardedLoadEquipment(const char* Path)
    {
        __try
        {
            return Archetype::LoadEquipment(Path, false, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return -1;
        }
    }

    // DispatchDataValue (common_dataconfig.cpp), with the same GetFileSysPath
    // calls and no factory. WeaponModDB_load is not exported, so that key is
    // skipped here; it interns nothing, so the other databases do not depend
    // on it.
    void DispatchDataValue(INI_Reader& Reader)
    {
        char FullPath[0x104];
        if (Reader.is_value("universe"))
        {
            GUniversePath = Reader.get_value_string();
            Universe::Startup(Reader.get_value_string());
        }
        else if (Reader.is_value("constants"))
        {
            ReadConstants(Reader.get_value_string());
        }
        else if (Reader.is_value("equipment"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanIds(FullPath);
            GuardedLoadEquipment(FullPath);
        }
        else if (Reader.is_value("solar"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanIds(FullPath);
            Archetype::LoadSolar(FullPath, false, nullptr);
        }
        else if (Reader.is_value("ships"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanIds(FullPath);
            Archetype::LoadShips(FullPath, false, nullptr);
        }
        else if (Reader.is_value("asteroids"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            Archetype::LoadAsteroids(FullPath, nullptr);
        }
        else if (Reader.is_value("loadouts"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanIds(FullPath);
            Loadout::Load(FullPath, false);
        }
        else if (Reader.is_value("bodyparts"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            LoadBodypartDescriptions(FullPath);
        }
        else if (Reader.is_value("costumes"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            LoadCostumeDescriptions(FullPath);
        }
        else if (Reader.is_value("bases"))
        {
            BaseDataList_load();
        }
        else if (Reader.is_value("goods"))
        {
            GoodList_load(Reader.get_value_string());
        }
        else if (Reader.is_value("markets"))
        {
            BaseDataList_load_market_data(Reader.get_value_string());
        }
        else if (Reader.is_value("PetalDB"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanPetalIds(FullPath);
            PetalDB_load(Reader.get_value_string());
        }
        else if (Reader.is_value("fuses"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            FuseDB::AddFuseINIFile(FullPath);
        }
        else if (Reader.is_value("Groups"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            Reputation::Startup(FullPath);
        }
        else if (Reader.is_value("explosions"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanIds(FullPath);
            Archetype::LoadSimples(FullPath, false, nullptr);
        }
        else if (Reader.is_value("NewCharDB"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            NewChar::TheDB->Load(FullPath);
        }
        else if (Reader.is_value("RtcSlider"))
        {
            GetFileSysPath(FullPath, Reader.get_value_string(), nullptr);
            ScanRtcNames(FullPath);
            RtcSlider::load(FullPath);
        }
    }

    bool LoadDataList()
    {
        INI_Reader Reader;
        if (!Reader.open("EXE\\OutpostFrontier.ini", false))
            return false;
        while (Reader.read_header())
        {
            if (!Reader.is_header("Data"))
                continue;
            while (Reader.read_value())
                DispatchDataValue(Reader);
        }
        return true;
    }

    // --- Dumps ------------------------------------------------------------------

    void FloatLine(Suite& Out, const char* Key, float Value)
    {
        Out.Line(Format("{\"key\":\"%s\",\"f\":\"%s\"}", Key, F(Value).c_str()));
    }

    void IntLine(Suite& Out, const char* Key, int Value)
    {
        Out.Line(Format("{\"key\":\"%s\",\"i\":\"%d\"}", Key, Value));
    }

    void IdLine(Suite& Out, const char* Key, unsigned int Value)
    {
        Out.Line(Format("{\"key\":\"%s\",\"id\":\"%08x\"}", Key, Value));
    }

    void DoubleLine(Suite& Out, const char* Key, double Value)
    {
        Out.Line(Format("{\"key\":\"%s\",\"d\":\"%s\"}", Key, D(Value).c_str()));
    }

    void VectorLine(Suite& Out, const char* Key, const Vector& Value)
    {
        Out.Line(Format("{\"key\":\"%s\",\"v\":\"%s\"}", Key, V(Value).c_str()));
    }

    // Kind "constants": every exported global ReadConstants fills, then per
    // OBJECT_TYPE name the collision damage of Collision::Load's table.
    void DumpConstants()
    {
        FGameKind K("constants");
        Suite Out(K.Path("constants"));
        FloatLine(Out, "COLLISION_DAMAGE_FACTOR", COLLISION_DAMAGE_FACTOR);
        FloatLine(Out, "LOOT_OWNER_SAFE_TIME", LOOT_OWNER_SAFE_TIME);
        FloatLine(Out, "LOOT_UNSEEN_LIFE_TIME", LOOT_UNSEEN_LIFE_TIME);
        FloatLine(Out, "LOOT_UNSEEN_RADIUS", LOOT_UNSEEN_RADIUS);
        FloatLine(Out, "MUSIC_CROSS_FADE_DELAY", MUSIC_CROSS_FADE_DELAY);
        FloatLine(Out, "JETTISONED_CARGO_VELOCITY", JETTISONED_CARGO_VELOCITY);
        IdLine(Out, "SND_CARGO_JETTISONED", SND_CARGO_JETTISONED.Id);
        FloatLine(Out, "MUZZLE_CONE_ANGLE", MUZZLE_CONE_ANGLE);
        IdLine(Out, "FIRE_FAILED_SOUND", FIRE_FAILED_SOUND.Id);
        FloatLine(Out, "FIRE_FAILED_DELAY", FIRE_FAILED_DELAY);
        FloatLine(Out, "PLAYER_COLLISION_GROUP_HIT_PTS_SCALE", PLAYER_COLLISION_GROUP_HIT_PTS_SCALE);
        FloatLine(Out, "PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE", PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE);
        IntLine(Out, "MAX_PLAYER_AMMO", MAX_PLAYER_AMMO);
        FloatLine(Out, "CRUISING_SPEED", EngineEquipConsts::CRUISING_SPEED);
        FloatLine(Out, "CRUISE_ACCEL_TIME", EngineEquipConsts::CRUISE_ACCEL_TIME);
        FloatLine(Out, "CRUISE_DRAG", EngineEquipConsts::CRUISE_DRAG);
        FloatLine(Out, "CRUISE_DISRUPT_TIME", EngineEquipConsts::CRUISE_DISRUPT_TIME);
        FloatLine(Out, "MAX_DELTA_FX_THROTTLE", EngineEquipConsts::MAX_DELTA_FX_THROTTLE);
        FloatLine(Out, "MAX_ENGINE_FX_THROTTLE", EngineEquipConsts::MAX_ENGINE_FX_THROTTLE);
        FloatLine(Out, "THROTTLE_STEADY_TIME", EngineEquipConsts::THROTTLE_STEADY_TIME);
        FloatLine(Out, "THROTTLE_ATTEN_MOD_RANGE", EngineEquipConsts::THROTTLE_ATTEN_MOD_RANGE);
        FloatLine(Out, "DELTA_THROTTLE_ATTEN_MOD_CHANGING", EngineEquipConsts::DELTA_THROTTLE_ATTEN_MOD_CHANGING);
        FloatLine(Out, "DELTA_THROTTLE_ATTEN_MOD_STEADY", EngineEquipConsts::DELTA_THROTTLE_ATTEN_MOD_STEADY);
        FloatLine(Out, "CRUISE_STEADY_TIME", EngineEquipConsts::CRUISE_STEADY_TIME);
        FloatLine(Out, "DELTA_CRUISE_ATTEN_MOD_STEADY", EngineEquipConsts::DELTA_CRUISE_ATTEN_MOD_STEADY);
        FloatLine(Out, "CRUISE_ATTEN_MOD_RANGE", EngineEquipConsts::CRUISE_ATTEN_MOD_RANGE);
        FloatLine(Out, "HULL_DAMAGE_FACTOR", ShieldEquipConsts::HULL_DAMAGE_FACTOR);
        IdLine(Out, "INTERIOR_SOUND_NAME", ThrusterEquipConsts::INTERIOR_SOUND_NAME.Id);
        IdLine(Out, "EXTERIOR_SOUND_NAME", ThrusterEquipConsts::EXTERIOR_SOUND_NAME.Id);
        FloatLine(Out, "MAX_VOLUME_FORCE", ThrusterEquipConsts::MAX_VOLUME_FORCE);
        FloatLine(Out, "MIN_VOLUME_FORCE", ThrusterEquipConsts::MIN_VOLUME_FORCE);
        FloatLine(Out, "INSIDE_CONE_ANGLE", ThrusterEquipConsts::INSIDE_CONE_ANGLE);
        FloatLine(Out, "OUTSIDE_CONE_ANGLE", ThrusterEquipConsts::OUTSIDE_CONE_ANGLE);
        FloatLine(Out, "OUTSIDE_CONE_ATTENUATION", ThrusterEquipConsts::OUTSIDE_CONE_ATTENUATION);
        FloatLine(Out, "PHYSICAL_SIM_RATE", PhySys::PHYSICAL_SIM_RATE);
        IntLine(Out, "MAX_SPAWNED_MINDIST_COUNT", PhySys::MAX_SPAWNED_MINDIST_COUNT);
        FloatLine(Out, "ANOM_LIMITS_MAX_VELOCITY", PhySys::ANOM_LIMITS_MAX_VELOCITY);
        FloatLine(Out, "ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI", PhySys::ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI);
        DoubleLine(Out, "MATERIAL_FRICTION", PhySys::MATERIAL_FRICTION);
        DoubleLine(Out, "MATERIAL_ELASTICITY", PhySys::MATERIAL_ELASTICITY);
        DoubleLine(Out, "MIN_TIME_BETWEEN_COLLISIONS", PhySys::MIN_TIME_BETWEEN_COLLISIONS);
        FloatLine(Out, "RMGR_LOOK_AHEAD_TIME_WORLD", PhySys::RMGR_LOOK_AHEAD_TIME_WORLD);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD", PhySys::RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD", PhySys::RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD", PhySys::RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD", PhySys::RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD);
        FloatLine(Out, "RMGR_LOOK_AHEAD_TIME_INTRA", PhySys::RMGR_LOOK_AHEAD_TIME_INTRA);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA", PhySys::RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA", PhySys::RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA", PhySys::RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA);
        FloatLine(Out, "RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA", PhySys::RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA);
        FloatLine(Out, "DEFAULT_LINEAR_DAMPING", PhySys::DEFAULT_LINEAR_DAMPING);
        VectorLine(Out, "DEFAULT_ANGULAR_DAMPING", PhySys::DEFAULT_ANGULAR_DAMPING);
        FloatLine(Out, "GOLEM_CHILD_MASS", PhySys::GOLEM_CHILD_MASS);
        FloatLine(Out, "GOLEM_CHILD_LINEAR_DAMP", PhySys::GOLEM_CHILD_LINEAR_DAMP);
        VectorLine(Out, "GOLEM_CHILD_ANGULAR_DAMP", PhySys::GOLEM_CHILD_ANGULAR_DAMP);
        FloatLine(Out, "GOLEM_MAX_DELTA_POSITION", PhySys::GOLEM_MAX_DELTA_POSITION);
        FloatLine(Out, "GOLEM_DELTA_ORIENTATION", PhySys::GOLEM_DELTA_ORIENTATION);
        VectorLine(Out, "GOLEM_MAX_TRANSLATION_FORCE", PhySys::GOLEM_MAX_TRANSLATION_FORCE);
        FloatLine(Out, "GOLEM_MAX_TORQUE", PhySys::GOLEM_MAX_TORQUE);
        FloatLine(Out, "GOLEM_FORCE_FACTOR", PhySys::GOLEM_FORCE_FACTOR);
        FloatLine(Out, "GOLEM_DAMP_FACTOR", PhySys::GOLEM_DAMP_FACTOR);
        FloatLine(Out, "GOLEM_TORQUE_FACTOR", PhySys::GOLEM_TORQUE_FACTOR);
        FloatLine(Out, "GOLEM_ANGULAR_DAMP_FACTOR", PhySys::GOLEM_ANGULAR_DAMP_FACTOR);
        FloatLine(Out, "RC_MAX_DELTA_POSITION", PhySys::RC_MAX_DELTA_POSITION);
        FloatLine(Out, "RC_MAX_DELTA_ORIENTATION", PhySys::RC_MAX_DELTA_ORIENTATION);
        FloatLine(Out, "COMM_PLAYER_FAR_DIST", COMM_PLAYER_FAR_DIST);
        FloatLine(Out, "COMM_PLAYER_FAR_DIST_ATTEN", COMM_PLAYER_FAR_DIST_ATTEN);
        FloatLine(Out, "CHATTER_MAX_DIST", CHATTER_MAX_DIST);
        FloatLine(Out, "CHATTER_MAX_DIST_ATTEN", CHATTER_MAX_DIST_ATTEN);
        FloatLine(Out, "CHATTER_START_ATTEN", CHATTER_START_ATTEN);
        IntLine(Out, "COMM_CONFLICT_PRIORITY_CUTOFF", COMM_CONFLICT_PRIORITY_CUTOFF);
        FloatLine(Out, "WALLA_MAX_DIST", WALLA_MAX_DIST);
        FloatLine(Out, "WALLA_MAX_DIST_ATTEN", WALLA_MAX_DIST_ATTEN);
        FloatLine(Out, "WALLA_START_ATTEN", WALLA_START_ATTEN);
        IntLine(Out, "WALLA_PRIORITY_CUTOFF", WALLA_PRIORITY_CUTOFF);

        // The OBJECT_TYPE names of DecodeType's table (common_archetype.cpp), in its order.
        static const char* const kTypes[] = {
            "NONE", "MOON", "PLANET", "SUN", "ASTEROID", "BLACKHOLE", "SATELLITE", "DOCKING_RING",
            "JUMP_GATE", "JUMP_HOLE", "WEAPONS_PLATFORM", "DESTROYABLE_DEPOT", "NON_TARGETABLE",
            "MISSION_SATELLITE", "AIRLOCK_GATE", "TRADELANE_RING", "STATION", "WAYPOINT", "FIGHTER",
            "FREIGHTER", "GUNBOAT", "CRUISER", "TRANSPORT", "CAPITAL", "MINING", "GUIDED", "MINE",
            "LOOT", "BULLET",
        };
        Vector Unit;
        Unit.x = 1.0f;
        Unit.y = 0.0f;
        Unit.z = 0.0f;
        for (const char* Name : kTypes)
        {
            const unsigned int Bits = DecodeType(Name);
            Out.Line(Format("{\"collision\":\"%s\",\"damage\":\"%s\",\"calc\":\"%s\"}", Name,
                            F(Collision::GetImpactDamageFromType(Bits)).c_str(),
                            F(Collision::CalcCollisionDamage(Bits, Unit, Unit)).c_str()));
        }
    }
}

void RunGameDataDump(const std::vector<std::string>& Files)
{
    (void)Files;
    if (!OpenGameData())
        return;
    if (!LoadDataList())
    {
        // No engine INI under the dump root (the synthetic tree of build.yml):
        // every kind lists no files, as the Rust test does.
        printf("game data: no EXE\\OutpostFrontier.ini, nothing loaded\n");
        static const char* const kKinds[] = {"constants", "goods",    "loadouts",   "reputation", "archetypes",
                                             "universe",  "bases",    "fuses",      "characters", "vibes",
                                             "asteroid_fields", "state_graph"};
        for (const char* Name : kKinds)
            FGameKind Empty(Name);
        CloseGameData();
        return;
    }
    DumpConstants();
    {
        FGameKind K("goods");
        DumpGoods(K);
    }
    {
        FGameKind K("loadouts");
        DumpLoadouts(K, GLoadoutIds);
    }
    {
        FGameKind K("reputation");
        DumpReputation(K);
    }
    {
        FGameKind K("archetypes");
        DumpArchetypes(K, GMotorIds, GExplosionIds);
    }
    {
        FGameKind K("universe");
        DumpUniverse(K);
    }
    {
        FGameKind K("bases");
        DumpBaseData(K);
    }
    {
        FGameKind K("fuses");
        DumpFuses(K, GPetalIds);
    }
    {
        FGameKind K("characters");
        DumpCharacters(K, GRtcNames);
    }
    {
        FGameKind K("vibes");
        DumpVibes(K);
    }
    {
        FGameKind K("asteroid_fields");
        DumpAsteroidFields(K, GUniversePath);
    }
    {
        // P2.5: AI\state_graph.db through pub::StateGraph::refresh_state_graph, which
        // resolves the path through NeuronRuntime::Data as the game's loader does.
        FGameKind K("state_graph");
        pub::StateGraph::refresh_state_graph();
        Suite Out(K.Path("state_graph"));
        WriteStateGraphLines(Out);
        OracleAiFreeStateGraph();
    }
    CloseGameData();
    printf("dumped the game databases\n");
}
