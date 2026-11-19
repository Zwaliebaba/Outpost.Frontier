// Oracle.exe sim, the asteroid-field and docking-point recorders of work package P2.4
// (design/sim-records.md, `asteroids.jsonl` and `dock.jsonl`).
//
// asteroids.jsonl: after each frame, the scenario system's CmnAsteroid fields in their list
// order (CAsteroidSystem::Fields): a `cube` record for every cube that was not there after the
// last frame, with its rocks (CAsteroidCube::Asteroids), and a `cubes` record of a field
// whenever the ids, states or timers of its cubes changed. The server's CmnAsteroid::Update
// walks every ship of the system (there are no players in this harness), so the scenario ships
// fill the fields they fly in.
//
// dock.jsonl: the docking-point hooks of Common (CEqObj::activate and
// CEqObj::update_docking_animations), one record per call, numbered in call order.

#include <windows.h>

#include <stdio.h>
#include <string.h>

#include <map>
#include <string>

#include "oracle_sim_fields.h"
#include "../OutpostFrontier/Client/oracle_json.h"

#include "common_casteroid.h"  // CmnAsteroid::Find, CAsteroidField, CAsteroidCube
#include "common_cobject.h"    // CObject, CSimple, CAsteroid
#include "oracle_sim_hooks.h"

namespace
{
    FILE* GAsteroidFile = nullptr;
    FILE* GDockFile = nullptr;
    unsigned int GFieldSystemId = 0;
    unsigned int const* GFieldTick = nullptr;
    oracle::FSimFieldNameFn GFieldNameOf = nullptr;
    unsigned int GDockSeq = 0;

    // Per field index: the cube ids seen after the last frame and the last `cubes` line.
    std::map<unsigned int, std::map<unsigned long, int>> GSeenCubes;
    std::map<unsigned int, std::string> GLastCubes;

    void WriteLine(FILE* File, FOracleJson& J)
    {
        if (File == nullptr)
            return;
        fputs(J.Finish().c_str(), File);
        fputc('\n', File);
        fflush(File);
    }

    unsigned int TickNow()
    {
        return GFieldTick != nullptr ? *GFieldTick : 0u;
    }

    void VectorJ(FOracleJson& J, char const* Name, Vector const& Value)
    {
        float const Values[3] = {Value.x, Value.y, Value.z};
        J.FloatArray(Name, Values, 3);
    }

    void MatrixJ(FOracleJson& J, char const* Name, Matrix const& Value)
    {
        J.FloatArray(Name, &Value.E[0][0], 9);
    }

    unsigned int IdOf(CObject const* Object)
    {
        if (Object == nullptr || (static_cast<unsigned int>(Object->ObjectClass) & 0x3u) != 0x3u)
            return 0;
        return static_cast<CSimple const*>(Object)->Id;
    }

    void ObjectJ(FOracleJson& J, CObject const* Object)
    {
        unsigned int const Id = IdOf(Object);
        J.Uint("obj_id", Id);
        if (GFieldNameOf != nullptr)
            J.Str("obj", GFieldNameOf(Id).c_str());
    }

    void __cdecl OnDockActivate(CObject const* Object, unsigned Index, bool bOn, int Result)
    {
        FOracleJson J;
        J.Str("rec", "dock_activate");
        J.Uint("tick", TickNow());
        J.Uint("seq", ++GDockSeq);
        ObjectJ(J, Object);
        J.Uint("index", Index);
        J.Bool("on", bOn);
        J.Int("result", Result);
        WriteLine(GDockFile, J);
    }

    void __cdecl OnDockAnim(CObject const* Object, unsigned Index, bool bActive, float Duration,
                            float Position, float LastDuration)
    {
        FOracleJson J;
        J.Str("rec", "dock_anim");
        J.Uint("tick", TickNow());
        J.Uint("seq", ++GDockSeq);
        ObjectJ(J, Object);
        J.Uint("index", Index);
        J.Bool("active", bActive);
        J.Float("time", Duration);
        J.Float("duration", Position);
        J.Float("last", LastDuration);
        WriteLine(GDockFile, J);
    }

    void CubeRecord(unsigned int FieldIndex, CmnAsteroid::CAsteroidCube const& Cube)
    {
        FOracleJson J;
        J.Str("rec", "cube");
        J.Uint("tick", TickNow());
        J.Uint("field", FieldIndex);
        J.Uint("id", static_cast<unsigned int>(Cube.CubeId));
        VectorJ(J, "pos", Cube.Pos);
        MatrixJ(J, "orient", Cube.Orient);
        J.Bool("empty", Cube.bEmpty);
        J.BeginArray("rocks");
        for (std::list<CObject*>::const_iterator It = Cube.Asteroids.begin(); It != Cube.Asteroids.end();
             ++It)
        {
            CAsteroid const* const Rock = static_cast<CAsteroid const*>(*It);
            J.BeginObject(nullptr);
            J.Uint("obj_id", Rock->Id);
            J.Uint("asteroid_id", static_cast<unsigned int>(Rock->AsteroidId));
            VectorJ(J, "pos", Rock->WorldPosition);
            MatrixJ(J, "orient", Rock->Orientation);
            J.Bool("mine", Rock->bMine);
            J.Bool("instant_kill", Rock->bInstantKill);
            J.EndObject();
        }
        J.EndArray();
        WriteLine(GAsteroidFile, J);
    }
}

namespace oracle
{
    bool OracleSimFieldsStart(std::string const& OutDir, unsigned int SystemId, unsigned int const* Tick,
                              FSimFieldNameFn NameOf)
    {
        GAsteroidFile = fopen((OutDir + "\\asteroids.jsonl").c_str(), "wb");
        GDockFile = fopen((OutDir + "\\dock.jsonl").c_str(), "wb");
        if (GAsteroidFile == nullptr || GDockFile == nullptr)
        {
            if (GAsteroidFile != nullptr)
                fclose(GAsteroidFile);
            if (GDockFile != nullptr)
                fclose(GDockFile);
            GAsteroidFile = nullptr;
            GDockFile = nullptr;
            return false;
        }
        GFieldSystemId = SystemId;
        GFieldTick = Tick;
        GFieldNameOf = NameOf;
        GOracleSimHooks.OnDockActivate = OnDockActivate;
        GOracleSimHooks.OnDockAnim = OnDockAnim;
        return true;
    }

    void OracleSimFieldsTick()
    {
        if (GAsteroidFile == nullptr)
            return;
        CmnAsteroid::CAsteroidSystem* const System = CmnAsteroid::Find(GFieldSystemId);
        if (System == nullptr)
            return;
        unsigned int FieldIndex = 0;
        for (std::list<CmnAsteroid::CAsteroidField*>::const_iterator Field = System->Fields.begin();
             Field != System->Fields.end(); ++Field, ++FieldIndex)
        {
            std::map<unsigned long, int>& Seen = GSeenCubes[FieldIndex];
            std::map<unsigned long, int> Now;
            FOracleJson Summary;
            Summary.Str("rec", "cubes");
            Summary.Uint("tick", TickNow());
            Summary.Uint("field", FieldIndex);
            Summary.BeginArray("cubes");
            std::string Key;
            for (std::map<unsigned long, CmnAsteroid::CAsteroidCube>::const_iterator It = (*Field)->Cubes.begin();
                 It != (*Field)->Cubes.end(); ++It)
            {
                CmnAsteroid::CAsteroidCube const& Cube = It->second;
                Now[It->first] = 1;
                if (Seen.find(It->first) == Seen.end())
                    CubeRecord(FieldIndex, Cube);
                Summary.BeginObject(nullptr);
                Summary.Uint("id", static_cast<unsigned int>(Cube.CubeId));
                Summary.Int("state", static_cast<int>(Cube.State));
                Summary.Float("timer", Cube.StateTimer);
                Summary.EndObject();
                unsigned int TimerBits = 0;
                memcpy(&TimerBits, &Cube.StateTimer, sizeof(TimerBits));
                char Buffer[48];
                sprintf(Buffer, "%08lx:%d:%08x;", static_cast<unsigned long>(It->first),
                        static_cast<int>(Cube.State), TimerBits);
                Key += Buffer;
            }
            Summary.EndArray();
            Seen.swap(Now);
            std::string& Last = GLastCubes[FieldIndex];
            if (Key != Last)
            {
                Last = Key;
                WriteLine(GAsteroidFile, Summary);
            }
        }
    }
}
