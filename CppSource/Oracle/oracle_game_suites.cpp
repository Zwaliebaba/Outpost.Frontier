// Oracle suites of work package P2.1, run by `Oracle.exe game-sim <fixture
// directory> <output directory>` over crates/game-sim/tests/fixtures/oracle
// (written by make_game_sim_fixtures.py). The Rust test
// crates/game-sim/tests/oracle_vectors.rs writes the same lines; the
// committed vectors are crates/game-sim/tests/vectors/.
//
// The suites run in a mode of their own so that nothing else in the process
// has used Common's string cache before string_cache.jsonl: the only string
// interned first is EquipDesc::CARGO_BAY_HP_NAME, by its static initialiser.
//
// CAsteroidCube::compute_asteroid_id is private and has no export this
// program can call; the Rust crate tests it on its own.

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <stdlib.h>
#include <string.h>

#include <map>
#include <string>
#include <vector>

#include "neuroncomponents.h"
#include "readfile.h"
#include "neuronruntime_core.h"
#include "common_ini.h"
#include "common_id.h"
#include "common_goods.h"
#include "common_casteroid.h"
#include "common_reputation.h"
#include "common_universe.h"
#include "common_cequip.h"   // CEGun::ComputeTimeToTgt, CNudgeEngine, CSteering (P2.4)
#include "common_csys.h"    // LookMatrix (P2.4)

// The NeuronRuntime globals Universe::Startup reads (see oracle_game_data.cpp).
namespace NeuronRuntime
{
    NEURONRUNTIME_API extern IFileSystem* Data;
    NEURONRUNTIME_API extern ICOManager* NeuronComponents;
}

using namespace oracle;

namespace
{
    std::vector<std::string> Split(const std::string& Line, char Separator)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        for (;;)
        {
            const size_t End = Line.find(Separator, Start);
            if (End == std::string::npos)
            {
                Out.push_back(Line.substr(Start));
                return Out;
            }
            Out.push_back(Line.substr(Start, End - Start));
            Start = End + 1;
        }
    }

    float HexFloat(const std::string& Text)
    {
        return FloatFromBits((unsigned int)strtoul(Text.c_str(), nullptr, 16));
    }

    void AsteroidIds()
    {
        Suite Out("asteroid_ids.jsonl");
        for (const std::string& Line : InputLines("asteroid.list"))
        {
            const std::vector<std::string> Parts = Split(Line, ' ');
            Vector Pos;
            Pos.x = HexFloat(Parts[0]);
            Pos.y = HexFloat(Parts[1]);
            Pos.z = HexFloat(Parts[2]);
            std::string Joined = Line;
            for (char& Ch : Joined)
            {
                if (Ch == ' ')
                    Ch = ',';
            }
            FLine L;
            L.Add("pos", Joined);
            L.Add("cube", Format("%08lx", CmnAsteroid::compute_cube_id(Pos)));
            Out.Line(L.Done());
        }
        const int Coords[] = {-1000, -300, -256, -1, 0, 1, 255, 256, 300, 1000, 2147483647, (-2147483647 - 1)};
        const int Sizes[] = {256, 1000, 1, -256};
        for (int Coord : Coords)
        {
            for (int Size : Sizes)
            {
                FLine L;
                L.Add("coord", Format("%d", Coord));
                L.Add("size", Format("%d", Size));
                L.Add("closest", Format("%d", CmnAsteroid::closest_cube_coord(Coord, Size, Size / 2)));
                Out.Line(L.Done());
            }
        }
    }

    void AsteroidOrient()
    {
        Suite Out("asteroid_orient.jsonl");
        for (const std::string& Line : InputLines("orient.list"))
        {
            const std::vector<std::string> Parts = Split(Line, ' ');
            const unsigned long Seed = strtoul(Parts[0].c_str(), nullptr, 10);
            const float Freq = HexFloat(Parts[1]);
            CmnAsteroid::AxisRotations Rot;
            for (int Index = 0; Index < 12; ++Index)
                Rot.Angles[Index / 4][Index % 4] = HexFloat(Parts[2 + Index]);
            Matrix M(1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f);
            const bool bFilled = CmnAsteroid::compute_cube_orientation(Seed, &M, Rot, Freq);
            std::string Words;
            for (int Row = 0; Row < 3; ++Row)
            {
                for (int Col = 0; Col < 3; ++Col)
                {
                    if (!Words.empty())
                        Words += ",";
                    Words += GameF(M.E[Row][Col]);
                }
            }
            FLine L;
            L.Add("seed", Format("%lu", Seed));
            L.Add("freq", Parts[1]);
            L.Add("filled", bFilled ? "1" : "0");
            L.Add("m", Words);
            Out.Line(L.Done());
        }
    }

    void StringCacheSuite()
    {
        Suite Out("string_cache.jsonl");
        std::map<const char*, int> First;
        int Index = 0;
        for (const std::string& Line : InputLines("strings.list"))
        {
            const size_t Space = Line.find(' ');
            const std::string Op = Line.substr(0, Space);
            const std::string Arg = Space == std::string::npos ? std::string() : Line.substr(Space + 1);
            FLine L;
            L.Add("op", Op);
            L.Add("in", Arg);
            const char* Result = nullptr;
            bool bHasResult = true;
            if (Op == "i" || Op == "t")
            {
                const Bytes Text = Unhex(Arg);
                Result = StringAlloc(CStr(Text), Op == "t");
            }
            else if (Op == "bay")
            {
                Result = EquipDesc::CARGO_BAY_HP_NAME.Value;
            }
            else
            {
                StringShutdown();
                bHasResult = false;
            }
            if (bHasResult)
            {
                int Seen = -1;
                std::map<const char*, int>::iterator Found = First.find(Result);
                if (Found != First.end())
                    Seen = Found->second;
                else if (Op != "bay")
                {
                    First[Result] = Index;
                    Seen = Index;
                }
                L.Add("out", Hex(Result, strlen(Result)));
                L.Add("first", Format("%d", Seen));
            }
            Out.Line(L.Done());
            ++Index;
        }
    }

    TString<16> Name16(const std::string& HexName)
    {
        TString<16> Name;
        Name.Length = 0;
        Name.Data[0] = '\0';
        if (HexName != "-")
        {
            const Bytes Text = Unhex(HexName);
            Name = CStr(Text);
        }
        return Name;
    }

    std::string Feeling(unsigned int From, unsigned int To)
    {
        float Value = 0.0f;
        const int Result = Reputation::get_feelings_towards(From, To, Value);
        if (Result != 0)
            return Format("%d,-", Result);
        return Format("%d,", Result) + GameF(Value);
    }

    struct FGroupIds : Reputation::RepGroupCB
    {
        std::vector<unsigned int> Ids;

        virtual char Execute(unsigned const* GroupId)
        {
            Ids.push_back(*GroupId);
            return 1;
        }
    };

    std::vector<unsigned int> GroupIds()
    {
        FGroupIds Collector;
        Reputation::enumerate(&Collector);
        return Collector.Ids;
    }

    std::string Nickname(unsigned int Id)
    {
        TString<16> Out;
        Out.Length = 0;
        Out.Data[0] = '\0';
        if (Reputation::get_nickname(Out, Id) != 0)
            return std::string();
        return Hex(Out.Data, strlen(Out.Data));
    }

    void ReputationSuite()
    {
        Suite Out("reputation.jsonl");
        Reputation::Shutdown();
        const bool bStarted = Reputation::Startup("groups.ini");
        {
            FLine L;
            L.Add("startup", bStarted ? "1" : "0");
            L.Add("count", Format("%u", Reputation::group_count()));
            Out.Line(L.Done());
        }
        const std::vector<unsigned int> Ids = GroupIds();
        for (unsigned int Id : Ids)
        {
            FLine L;
            L.Add("group", Format("%04x", Id));
            L.Add("nickname", Nickname(Id));
            L.Add("name", Format("%u", Reputation::get_name(Id)));
            L.Add("info", Format("%u", Reputation::get_info_card(Id)));
            L.Add("short", Format("%u", Reputation::get_short_name(Id)));
            Out.Line(L.Done());
        }
        {
            INI_Reader Reader;
            if (!Reader.open("feelings.ini", false))
            {
                fprintf(stderr, "game-sim: cannot open feelings.ini\n");
                GFailed = true;
                return;
            }
            Reputation::LoadFeelings(Reader);
        }
        for (unsigned int From : Ids)
        {
            for (unsigned int To : Ids)
            {
                FLine L;
                L.Add("from", Format("%04x", From));
                L.Add("to", Format("%04x", To));
                L.Add("feel", Feeling(From, To));
                Out.Line(L.Done());
            }
        }
        for (const std::string& Line : InputLines("reputation.list"))
        {
            const std::vector<std::string> Parts = Split(Line, ' ');
            FLine L;
            L.Add("op", Parts[0]);
            if (Parts[0] == "id")
            {
                L.Add("group", Format("%08x", Reputation::get_id(Name16(Parts[1]))));
            }
            else if (Parts[0] == "feel")
            {
                const unsigned int From = Reputation::get_id(Name16(Parts[1]));
                const unsigned int To = Reputation::get_id(Name16(Parts[2]));
                L.Add("feel", Feeling(From, To));
            }
            else if (Parts[0] == "affect")
            {
                const unsigned int From = Reputation::get_id(Name16(Parts[1]));
                const unsigned int To = Reputation::get_id(Name16(Parts[2]));
                const int Result = Reputation::affect_relation(From, To, HexFloat(Parts[3]));
                L.Add("result", Format("%d", Result));
                L.Add("feel", Feeling(From, To));
            }
            else if (Parts[0] == "story")
            {
                const unsigned int Id = MakeId(Name16(Parts[1]).Data);
                L.Add("story", Reputation::IsStoryFaction(Id) ? "1" : "0");
            }
            else
            {
                std::vector<Reputation::Relation> Relations;
                for (size_t Index = 2; Index < Parts.size(); ++Index)
                {
                    const size_t Colon = Parts[Index].find(':');
                    Reputation::Relation Relation;
                    Relation.GroupId = MakeId(Name16(Parts[Index].substr(0, Colon)).Data);
                    Relation.Feeling = HexFloat(Parts[Index].substr(Colon + 1));
                    Relations.push_back(Relation);
                }
                const TString<16> Name = Name16(Parts[1]);
                Reputation::set(Name, Relations.empty() ? nullptr : &Relations[0], (unsigned int)Relations.size());
                const unsigned int Id = MakeId(Name.Data);
                std::string Feelings;
                for (unsigned int To : GroupIds())
                {
                    if (!Feelings.empty())
                        Feelings += ";";
                    Feelings += Format("%04x:", To) + Feeling(Id, To);
                }
                L.Add("group", Format("%04x", Id));
                L.Add("nickname", Nickname(Id));
                L.Add("feelings", Feelings);
            }
            Out.Line(L.Done());
        }
    }
}

namespace
{
    std::string V3(const Vector& Value)
    {
        return GameF(Value.x) + "," + GameF(Value.y) + "," + GameF(Value.z);
    }

    // zones/universe.ini through Universe::Startup with NeuronRuntime::Data
    // over the fixture directory, then every query of zone_points.list on
    // every zone of system zt, in its zone order.
    void ZoneSuite()
    {
        Suite Out("zones.jsonl");
        OracleRegisterReadFile();
        FFileSystemDesc Desc;
        memset(&Desc, 0, sizeof(Desc));
        Desc.Size = 0x34;
        Desc.ComponentName = "FileSystem";
        Desc.Implementation = "DOS";
        Desc.Path = GFixtureDir.c_str();
        Desc.Access = 0x80000000;
        Desc.ShareMode = 1;
        Desc.Creation = 3;
        Desc.Flags = 0x08000080;
        Desc.Field30 = 0xFFFFFFFF;
        IFileSystem* Fs = nullptr;
        if (NEURONCOMPONENTS_Acquire()->CreateInstance((AGGDESC*)&Desc, (void**)&Fs) != GR_OK || Fs == nullptr)
        {
            fprintf(stderr, "game-sim: no DOS file system over %s (ReadFile's factories?)\n", GFixtureDir.c_str());
            GFailed = true;
            return;
        }
        NeuronRuntime::Data = Fs;
        if (NeuronRuntime::NeuronComponents == nullptr)
            NeuronRuntime::NeuronComponents = NEURONCOMPONENTS_Acquire();
        // Ellipsoid and cylinder queries go through x86math's 3DMathEngine
        // (common_zone.cpp ZoneMathEngine), which nothing else in this mode
        // loads.
        if (!OracleLoadMathEngine())
        {
            fprintf(stderr, "game-sim: no 3DMathEngine for the zone queries\n");
            GFailed = true;
            return;
        }
        Universe::Startup("zones\\universe.ini");

        std::vector<std::string> PointText;
        std::vector<Vector> Points;
        std::vector<std::string> DirText;
        std::vector<Vector> Dirs;
        for (const std::string& Line : InputLines("zone_points.list"))
        {
            const bool bDir = Line.compare(0, 2, "d ") == 0;
            const std::string Text = bDir ? Line.substr(2) : Line;
            const std::vector<std::string> Parts = Split(Text, ' ');
            Vector V;
            V.x = HexFloat(Parts[0]);
            V.y = HexFloat(Parts[1]);
            V.z = HexFloat(Parts[2]);
            std::string Joined = Text;
            for (char& Ch : Joined)
            {
                if (Ch == ' ')
                    Ch = ',';
            }
            (bDir ? DirText : PointText).push_back(Joined);
            (bDir ? Dirs : Points).push_back(V);
        }

        for (const Universe::IZone* Zone = Universe::first_zone(CreateID("zt")); Zone != nullptr;
             Zone = Universe::next_zone(Zone))
        {
            FLine Head;
            Head.Add("zone", GameId(Zone->GetId()));
            Head.Add("shape", Format("%u", Zone->GetShape()));
            Head.Add("radius", GameF(Zone->GetRadius()));
            Out.Line(Head.Done());
            for (size_t Index = 0; Index < Points.size(); ++Index)
            {
                const Vector& P = Points[Index];
                FLine L;
                L.Add("p", PointText[Index]);
                L.Add("in", Format("%d,%d", Zone->InZone(P, 0.0f) ? 1 : 0, Zone->InZone(P, 10.0f) ? 1 : 0));
                L.Add("in_xz", Zone->InZoneXZ(P, 0.0f) ? "1" : "0");
                L.Add("dist", GameF((float)Zone->GetDistance(P)));
                L.Add("surface", V3(Zone->GetSurfacePoint(P)));
                Out.Line(L.Done());
            }
            for (size_t Index = 0; Index < Dirs.size(); ++Index)
            {
                FLine L;
                L.Add("d", DirText[Index]);
                L.Add("along", GameF((float)Zone->GetRadiusAlong(Dirs[Index])));
                Out.Line(L.Done());
            }
        }
    }
}

namespace
{
    // A suite that faults ends the process without a word otherwise; the
    // guard names it and the exception, and the remaining suites still run.
    // No C++ object lives in the guarded frame.
    // Work package P2.4: LookMatrix, CEGun::ComputeTimeToTgt, and the change-thresholded
    // setters CNudgeEngine::SetNudgeVec and CSteering::SetAxisThrottle on fresh objects
    // (their wakeup() has no core to wake). The Rust test is object_math in
    // crates/game-sim/tests/oracle_vectors.rs.
    std::string FloatWords(const float* Values, int Count)
    {
        std::string Words;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Words += ",";
            Words += GameF(Values[Index]);
        }
        return Words;
    }

    void ObjectMathSuite()
    {
        Suite Out("object_math.jsonl");
        for (const std::string& Line : InputLines("object_math.list"))
        {
            const std::vector<std::string> Parts = Split(Line, ' ');
            std::string Args;
            for (size_t Index = 1; Index < Parts.size(); ++Index)
            {
                if (Index != 1)
                    Args += ",";
                Args += Parts[Index];
            }
            Vector V;
            V.x = HexFloat(Parts[1]);
            V.y = HexFloat(Parts[2]);
            V.z = HexFloat(Parts[3]);
            std::string Result;
            if (Parts[0] == "look")
            {
                const Matrix M = LookMatrix(V);
                Result = FloatWords(&M.E[0][0], 9);
            }
            else if (Parts[0] == "ttt")
            {
                Vector Vel;
                Vel.x = HexFloat(Parts[4]);
                Vel.y = HexFloat(Parts[5]);
                Vel.z = HexFloat(Parts[6]);
                float Time = 0.0f;
                if (CEGun::ComputeTimeToTgt(V, Vel, HexFloat(Parts[7]), Time))
                    Result = "1," + GameF(Time);
                else
                    Result = "0";
            }
            else if (Parts[0] == "nudge")
            {
                CNudgeEngine Nudge;
                Nudge.SetNudgeVec(V);
                const Vector After = Nudge.GetNudgeVec();
                const float Words[3] = {After.x, After.y, After.z};
                Result = FloatWords(Words, 3);
            }
            else if (Parts[0] == "axis")
            {
                CSteering Steering;
                Steering.SetAxisThrottle(V);
                const Vector After = Steering.GetAxisThrottle();
                const float Words[3] = {After.x, After.y, After.z};
                Result = FloatWords(Words, 3);
            }
            else
            {
                continue;
            }
            FLine L;
            L.Add("kind", Parts[0]);
            L.Add("in", Args);
            L.Add("out", Result);
            Out.Line(L.Done());
        }
    }

    typedef void (*FGameSimSuite)();

    DWORD GuardedGameSimSuiteCall(FGameSimSuite Suite)
    {
        __try
        {
            Suite();
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }

    void GuardedGameSimSuite(const char* Name, FGameSimSuite Suite)
    {
        const DWORD Code = GuardedGameSimSuiteCall(Suite);
        fflush(stdout);
        if (Code != 0)
        {
            fprintf(stderr, "game-sim %s: exception %08lx\n", Name, static_cast<unsigned long>(Code));
            GFailed = true;
        }
    }
}

void RunGameSimSuites()
{
    GuardedGameSimSuite("asteroid_ids", AsteroidIds);
    GuardedGameSimSuite("asteroid_orient", AsteroidOrient);
    GuardedGameSimSuite("string_cache", StringCacheSuite);
    GuardedGameSimSuite("reputation", ReputationSuite);
    GuardedGameSimSuite("zones", ZoneSuite);
    GuardedGameSimSuite("object_math", ObjectMathSuite);
}
