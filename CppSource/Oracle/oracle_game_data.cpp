// Data dumps of the game databases of Common (work package P2.1).
//
// Oracle.exe dump runs RunGameDataDump after the dumps of Phase 1. It points
// NeuronRuntime::Data at a ReadFile DOS file system over DATA (as
// NeuronRuntime::OpenData does), reads the [Data] list of
// EXE\OutpostFrontier.ini and dispatches each value to Common's own loader,
// in order, as DataConfig::startup does. DataConfig::startup itself is not
// called: it also runs loaders that belong to later work packages
// (bodyparts, costumes, PetalDB, WeaponModDB, NewCharDB, RtcSlider, the AI
// state graph), some of which need objects the harness does not create.
// GameData::load in crates/game-sim skips the same keys, so both sides
// intern the same strings in the same order.
//
// Each database is then written as one or more dumps of kind <kind>, the
// files listed in <output>\<kind>\files.txt; the Rust test
// crates/game-sim/tests/data_dump.rs renders the same lines
// (tests/support/game.rs). Nothing written here may be committed.

#include "oracle_support.h"

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
#include "common_physys.h"

// The one NeuronRuntime global the loaders read (NeuronRuntime.h declares it
// beside headers that need the DirectX 8 SDK).
namespace NeuronRuntime
{
    NEURONRUNTIME_API extern IFileSystem* Data;
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

    // The JSON text of a C string: printable ASCII except '"' and '\' as is,
    // every other byte as \u00xx, as crates/game-sim/tests/support/game.rs
    // writes it.
    std::string Text(const char* Value)
    {
        std::string Out;
        if (Value == nullptr)
            return Out;
        for (const unsigned char* P = (const unsigned char*)Value; *P != 0; ++P)
        {
            if (*P >= 0x20 && *P < 0x7f && *P != '"' && *P != '\\')
                Out += (char)*P;
            else
                Out += Format("\\u%04x", (unsigned int)*P);
        }
        return Out;
    }

    // --- The [Data] list ------------------------------------------------------

    // The value loop of DispatchDataValue (common_dataconfig.cpp) for the loaders
    // work package P2.1 ports, with the same GetFileSysPath calls.
    void DispatchDataValue(INI_Reader& Reader)
    {
        if (Reader.is_value("universe"))
        {
        }
        else if (Reader.is_value("constants"))
        {
            ReadConstants(Reader.get_value_string());
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

    // A kind with its files; Out is opened per file.
    class Kind
    {
    public:
        explicit Kind(const char* Name) : Name(Name), List(GOutputDir + "\\" + Name + "\\files.txt")
        {
        }

        std::string Path(const char* File)
        {
            List.Line(File);
            return DataOutputFile(Name, File);
        }

    private:
        const char* Name;
        Suite List;
    };

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
        Kind K("constants");
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
        static const char* const kKinds[] = {"constants"};
        for (const char* Name : kKinds)
            Suite List(GOutputDir + "\\" + Name + "\\files.txt");
        CloseGameData();
        return;
    }
    DumpConstants();
    CloseGameData();
    printf("dumped the game databases\n");
}
