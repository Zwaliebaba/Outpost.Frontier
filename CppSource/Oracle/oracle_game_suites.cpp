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

#include <stdint.h>
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

    // NeuronRuntime::Data over the fixture directory (once), and x86math's
    // 3DMathEngine: ellipsoid and cylinder queries go through it
    // (common_zone.cpp ZoneMathEngine), and nothing else in this mode loads
    // it.
    bool GFixtureDataOpen = false;

    bool OpenFixtureData()
    {
        if (GFixtureDataOpen)
            return true;
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
            return false;
        }
        NeuronRuntime::Data = Fs;
        if (NeuronRuntime::NeuronComponents == nullptr)
            NeuronRuntime::NeuronComponents = NEURONCOMPONENTS_Acquire();
        if (!OracleLoadMathEngine())
        {
            fprintf(stderr, "game-sim: no 3DMathEngine for the zone queries\n");
            GFailed = true;
            return false;
        }
        GFixtureDataOpen = true;
        return true;
    }

    // Universe::Startup is reference-counted and loads nothing while a
    // universe is up (common_universe.cpp: GUniverseRefCount), so a suite
    // that starts its own universe first releases the previous suite's;
    // every suite then sees exactly its fixture, as the Rust tests do.
    bool GFixtureUniverseUp = false;

    void StartFixtureUniverse(const char* RelPath)
    {
        if (GFixtureUniverseUp)
            Universe::Shutdown();
        Universe::Startup(RelPath);
        GFixtureUniverseUp = true;
    }

    // zones/universe.ini through Universe::Startup with NeuronRuntime::Data
    // over the fixture directory, then every query of zone_points.list on
    // every zone of system zt, in its zone order.
    void ZoneSuite()
    {
        Suite Out("zones.jsonl");
        if (!OpenFixtureData())
            return;
        StartFixtureUniverse("zones\\universe.ini");

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
    std::vector<std::vector<float>> FloatRows(const char* Name)
    {
        std::vector<std::vector<float>> Rows;
        for (const std::string& Line : InputLines(Name))
        {
            std::vector<float> Row;
            for (const std::string& Word : Split(Line, ' '))
                Row.push_back(HexFloat(Word));
            Rows.push_back(Row);
        }
        return Rows;
    }

    Vector Vec3(const std::vector<float>& Row, size_t Offset)
    {
        Vector Out;
        Out.x = Row[Offset];
        Out.y = Row[Offset + 1];
        Out.z = Row[Offset + 2];
        return Out;
    }

    // Every query of zone_queries.jsonl on every zone of system zq of
    // zone_queries/universe.ini: ClassifyPoint and GetImmersion at the
    // points of zone_points.list, Intersect on the segments of
    // zone_segments.list, FrustumCull on zone_frustums.list and SphereCull
    // on zone_spheres.list (crates/game-sim/tests/oracle_vectors.rs
    // zone_queries).
    void ZoneQuerySuite()
    {
        Suite Out("zone_queries.jsonl");
        if (!OpenFixtureData())
            return;
        StartFixtureUniverse("zone_queries\\universe.ini");

        std::vector<Vector> Points;
        for (const std::string& Line : InputLines("zone_points.list"))
        {
            if (Line.compare(0, 2, "d ") == 0)
                continue;
            std::vector<float> Row;
            for (const std::string& Word : Split(Line, ' '))
                Row.push_back(HexFloat(Word));
            Points.push_back(Vec3(Row, 0));
        }
        const std::vector<std::vector<float>> Segments = FloatRows("zone_segments.list");
        std::vector<Geometry::Frustum> Frustums;
        for (const std::vector<float>& W : FloatRows("zone_frustums.list"))
        {
            Geometry::Frustum F;
            for (int Row = 0; Row < 3; ++Row)
            {
                for (int Col = 0; Col < 3; ++Col)
                    F.Xform.Rotation.E[Row][Col] = W[Row * 3 + Col];
            }
            F.Xform.Translation = Vec3(W, 9);
            F.AngleX = W[12];
            F.AngleY = W[13];
            F.Distance = W[14];
            Frustums.push_back(F);
        }
        std::vector<Geometry::Sphere> Spheres;
        for (const std::vector<float>& W : FloatRows("zone_spheres.list"))
        {
            Geometry::Sphere S;
            S.Center = Vec3(W, 0);
            S.Radius = W[3];
            Spheres.push_back(S);
        }

        for (const Universe::IZone* Zone = Universe::first_zone(CreateID("zq")); Zone != nullptr;
             Zone = Universe::next_zone(Zone))
        {
            FLine Head;
            Head.Add("zone", GameId(Zone->GetId()));
            Head.Add("shape", Format("%u", Zone->GetShape()));
            Out.Line(Head.Done());
            for (size_t Index = 0; Index < Points.size(); ++Index)
            {
                FLine L;
                L.Add("p", Format("%u", (unsigned int)Index));
                L.Add("classify", GameF((float)Zone->ClassifyPoint(Points[Index])));
                L.Add("immersion", GameF((float)Zone->GetImmersion(Points[Index])));
                Out.Line(L.Done());
            }
            for (size_t Index = 0; Index < Segments.size(); ++Index)
            {
                Vector Hit;
                Hit.x = 0.0f;
                Hit.y = 0.0f;
                Hit.z = 0.0f;
                const bool bHit = Zone->Intersect(Vec3(Segments[Index], 0), Vec3(Segments[Index], 3), Hit);
                FLine L;
                L.Add("seg", Format("%u", (unsigned int)Index));
                L.Add("hit", bHit ? "1" : "0");
                if (bHit)
                    L.Add("at", V3(Hit));
                Out.Line(L.Done());
            }
            std::string Culls;
            for (const Geometry::Frustum& F : Frustums)
                Culls += FrustumCull(F, Zone) ? "1" : "0";
            {
                FLine L;
                L.Add("frustums", Culls);
                Out.Line(L.Done());
            }
            Culls.clear();
            for (const Geometry::Sphere& S : Spheres)
                Culls += SphereCull(S, Zone) ? "1" : "0";
            {
                FLine L;
                L.Add("spheres", Culls);
                Out.Line(L.Done());
            }
        }
    }

    // The exclusion zones' properties (oracle_vectors.rs EXCLUSIONS).
    struct FExclusionSpec
    {
        const char* Name;
        unsigned char bBillboards;
        unsigned char bDynamic;
        float EmptyCubeFrequency;
        float BillboardCount;
    };

    const FExclusionSpec kExclusions[] = {
        {"x_a", 1, 0, -1.0f, -1.0f},     {"x_b", 0, 1, 0.5f, 30.0f},   {"x_c", 0, 0, -1.0f, 12.7f},
        {"x_far", 0, 0, 0.25f, -1.0f}, {"x_edge", 0, 1, -1.0f, 45.2f},
    };
    const int kExclusionCount = 5;
    const float kUntouched = 12345.0f;

    const Universe::IZone* ZoneNamed(const char* Name)
    {
        return Universe::get_zone(CreateID(Name));
    }

    // AsteroidExclusionZoneMgr and NebulaExclusionZoneMgr on the zones of
    // zq (oracle_vectors.rs exclusion_zones). The managers keep their zone
    // vectors private; the zones are reached through the pointers back()
    // returns after each push_back (they are heap blocks the vectors point
    // at, so a sort does not move them).
    void ExclusionSuite()
    {
        Suite Out("exclusion_zones.jsonl");
        if (!OpenFixtureData())
            return;
        StartFixtureUniverse("zone_queries\\universe.ini");

        struct FObserver
        {
            Vector Pos;
            std::vector<Vector> Queries;
        };
        std::vector<FObserver> Observers;
        for (const std::string& Line : InputLines("exclusion.list"))
        {
            std::vector<float> Row;
            for (const std::string& Word : Split(Line.substr(2), ' '))
                Row.push_back(HexFloat(Word));
            if (Line.compare(0, 2, "o ") == 0)
            {
                FObserver O;
                O.Pos = Vec3(Row, 0);
                Observers.push_back(O);
            }
            else if (!Observers.empty())
            {
                Observers.back().Queries.push_back(Vec3(Row, 0));
            }
        }

        const Universe::IZone* Parent = ZoneNamed("q_sphere");
        Exclusion::AsteroidExclusionZoneMgr Mgr;
        Exclusion::AsteroidExclusionZone* Zones[kExclusionCount];
        for (int Index = 0; Index < kExclusionCount; ++Index)
        {
            const FExclusionSpec& Spec = kExclusions[Index];
            const Universe::IZone* Zone = ZoneNamed(Spec.Name);
            Mgr.push_back(Zone, Parent);
            Exclusion::AsteroidExclusionZone* Back = Mgr.back();
            Zones[Index] = Back;
            const bool bInside = Back->set_zone(Zone, Parent);
            Back->bExcludeBillboards = Spec.bBillboards;
            Back->bExcludeDynamicAsteroids = Spec.bDynamic;
            Back->EmptyCubeFrequency = Spec.EmptyCubeFrequency;
            Back->BillboardCount = Spec.BillboardCount;
            FLine L;
            L.Add("push", GameId(Zone->GetId()));
            L.Add("inside", bInside ? "1" : "0");
            Out.Line(L.Done());
        }
        const int Max = Mgr.get_max_billboard_count();
        {
            FLine L;
            L.Add("max_billboards", Format("%d", Max));
            Out.Line(L.Done());
        }
        if (Max > 0)
            Mgr.normalize_billboard_density(Max);
        {
            std::string Densities;
            for (int Index = 0; Index < kExclusionCount; ++Index)
            {
                if (Index != 0)
                    Densities += ",";
                Densities += GameF(Zones[Index]->BillboardCount);
            }
            FLine L;
            L.Add("densities", Densities);
            Out.Line(L.Done());
        }
        for (int Pass = 0; Pass < 2; ++Pass)
        {
            if (Pass == 1)
            {
                Mgr.set_max_cliasteroid_exclusion_distance(300.0f);
                Mgr.set_max_dynamicasteroid_exclusion_distance(100.0f);
            }
            for (const FObserver& O : Observers)
            {
                Mgr.sort(&O.Pos);
                std::string Dist;
                for (int Index = 0; Index < kExclusionCount; ++Index)
                {
                    if (Index != 0)
                        Dist += ",";
                    Dist += GameF(Zones[Index]->MaxDistSq);
                }
                const Universe::IZone* BackZone = Mgr.back()->get_zone();
                FLine L;
                L.Add("observer", V3(O.Pos));
                L.Add("dist", Dist);
                L.Add("back", GameId(BackZone != nullptr ? BackZone->GetId() : 0u));
                Out.Line(L.Done());
                for (const Vector& Q : O.Queries)
                {
                    std::string Answers;
                    std::string Outs;
                    for (int Kind = 0; Kind < 5; ++Kind)
                    {
                        float Value = kUntouched;
                        const bool bHit = Mgr.is_excluded(Q, (Exclusion::ExcludeObjectType)Kind, &Value);
                        if (Kind != 0)
                        {
                            Answers += ",";
                            Outs += ",";
                        }
                        Answers += bHit ? "1" : "0";
                        Outs += GameF(Value);
                    }
                    FLine M;
                    M.Add("q", V3(Q));
                    M.Add("ex", Answers);
                    M.Add("out", Outs);
                    Out.Line(M.Done());
                }
            }
        }

        const Universe::IZone* Nebula = ZoneNamed("q_ellipsoid");
        Exclusion::NebulaExclusionZoneMgr Neb;
        const char* const kNebulaNames[] = {"x_d", "x_a", "x_b"};
        Exclusion::NebulaExclusionZone* NebZones[3];
        for (int Index = 0; Index < 3; ++Index)
        {
            const Universe::IZone* Zone = ZoneNamed(kNebulaNames[Index]);
            Neb.push_back(Zone, Nebula);
            Exclusion::NebulaExclusionZone* Back = Neb.back();
            NebZones[Index] = Back;
            if (Index == 0)
            {
                Back->set_zone_shell_name("nebula_shell");
                Back->set_fog_far(1500.0f);
            }
            FLine L;
            L.Add("nebula_push", GameId(Zone->GetId()));
            L.Add("shell", GameId(Back->get_zone_shell_name().Id));
            L.Add("fog", GameF(Back->get_fog_far()));
            Out.Line(L.Done());
        }
        for (const FObserver& O : Observers)
        {
            Neb.sort(&O.Pos);
            std::string Dist;
            for (int Index = 0; Index < 3; ++Index)
            {
                if (Index != 0)
                    Dist += ",";
                Dist += GameF(NebZones[Index]->MaxDistSq);
            }
            FLine L;
            L.Add("nebula_observer", V3(O.Pos));
            L.Add("dist", Dist);
            Out.Line(L.Done());
            for (const Vector& Q : O.Queries)
            {
                std::string In[2];
                for (int Near = 0; Near < 2; ++Near)
                {
                    Exclusion::NebulaExclusionZone* Found = nullptr;
                    float Value = kUntouched;
                    if (Neb.is_in_exclusion_zone(Q, &Found, &Value, Near != 0) && Found != nullptr)
                        In[Near] = GameId(Found->get_zone()->GetId()) + "," + GameF(Value);
                    else
                        In[Near] = "-," + GameF(Value);
                }
                const bool bNebula = Neb.is_excluded(Q, (Exclusion::ExcludeObjectType)4, nullptr);
                const bool bOther = Neb.is_excluded(Q, (Exclusion::ExcludeObjectType)0, nullptr);
                FLine M;
                M.Add("q", V3(Q));
                M.Add("excluded", std::string(bNebula ? "1" : "0") + "," + (bOther ? "1" : "0"));
                M.Add("in", In[0]);
                M.Add("in_near", In[1]);
                Out.Line(M.Done());
            }
        }
    }
}

namespace
{
    std::string ItemsLine(const char* Op, int Count, const FSpatialQueryResult& Result)
    {
        std::string Items;
        for (unsigned Index = 0; Index < Result.Count; ++Index)
        {
            if (Index != 0)
                Items += ",";
            Items += Format("%u", (unsigned int)(uintptr_t)Result.Items[Index]);
        }
        FLine L;
        L.Add("op", Op);
        L.Add("n", Format("%d", Count));
        L.Add("items", Items);
        return L.Done();
    }

    std::string ZoneText(const Universe::IZone* Zone)
    {
        return GameId(Zone != nullptr ? Zone->GetId() : 0u);
    }

    FSpatialEntry SpatialEntryFrom(const std::string& Object, const std::vector<std::string>& Words, size_t First)
    {
        FSpatialEntry Entry;
        Entry.Object = (void*)(uintptr_t)strtoul(Object.c_str(), nullptr, 10);
        Entry.Position.x = HexFloat(Words[First]);
        Entry.Position.y = HexFloat(Words[First + 1]);
        Entry.Position.z = HexFloat(Words[First + 2]);
        Entry.Radius = HexFloat(Words[First + 3]);
        return Entry;
    }

    // The script of spatial.list on NewSpatial's partitions and the zone
    // tree of system zq (oracle_vectors.rs spatial). The result buffer is a
    // fresh one per query.
    void SpatialSuite()
    {
        Suite Out("spatial.jsonl");
        if (!OpenFixtureData())
            return;
        StartFixtureUniverse("zone_queries\\universe.ini");
        const Universe::ISystem* Zq = Universe::get_system(CreateID("zq"));
        static FSpatialQueryResult Result;
        ISpatialPartition* Partition = nullptr;
        for (const std::string& Line : InputLines("spatial.list"))
        {
            const size_t Space = Line.find(' ');
            const std::string Op = Line.substr(0, Space);
            const std::string Rest = Space == std::string::npos ? std::string() : Line.substr(Space + 1);
            const std::vector<std::string> Words = Split(Rest, ' ');
            if (Op == "new")
            {
                FreeSpatial(Partition);
                Partition = NewSpatial(Rest.c_str());
                FLine L;
                L.Add("op", "new");
                L.Add("type", Rest);
                L.Add("made", Partition != nullptr ? "1" : "0");
                Out.Line(L.Done());
            }
            else if (Op == "add")
            {
                const FSpatialEntry Entry = SpatialEntryFrom(Words[0], Words, 1);
                if (Partition != nullptr)
                    Partition->Add(&Entry);
            }
            else if (Op == "array")
            {
                std::vector<FSpatialEntry> Entries;
                for (const std::string& Item : Words)
                {
                    const std::vector<std::string> Parts = Split(Item, ':');
                    Entries.push_back(SpatialEntryFrom(Parts[0], Parts, 1));
                }
                if (Partition != nullptr)
                    Partition->AddArray(Entries.data(), (unsigned)Entries.size());
            }
            else if (Op == "remove")
            {
                if (Partition != nullptr)
                    Partition->Remove((void*)(uintptr_t)strtoul(Rest.c_str(), nullptr, 10));
            }
            else if (Op == "clear")
            {
                if (Partition != nullptr)
                    Partition->Clear();
            }
            else if (Op == "sphere" || Op == "ray" || Op == "frustum")
            {
                std::vector<float> W;
                for (const std::string& Word : Words)
                    W.push_back(HexFloat(Word));
                Result.Capacity = 0x800;
                Result.Count = 0;
                int Count = 0;
                if (Partition != nullptr && Op == "sphere")
                {
                    Geometry::Sphere S;
                    S.Center = Vec3(W, 0);
                    S.Radius = W[3];
                    Count = Partition->QuerySphere(&S, &Result);
                }
                else if (Partition != nullptr && Op == "ray")
                {
                    FSpatialRay R;
                    R.Origin = Vec3(W, 0);
                    R.Direction = Vec3(W, 3);
                    R.Length = W[6];
                    Count = Partition->QueryRay(&R, &Result);
                }
                else if (Partition != nullptr)
                {
                    Geometry::Frustum F;
                    for (int Row = 0; Row < 3; ++Row)
                    {
                        for (int Col = 0; Col < 3; ++Col)
                            F.Xform.Rotation.E[Row][Col] = W[Row * 3 + Col];
                    }
                    F.Xform.Translation = Vec3(W, 9);
                    F.AngleX = W[12];
                    F.AngleY = W[13];
                    F.Distance = W[14];
                    Count = Partition->QueryFrustum(&F, &Result);
                }
                Out.Line(ItemsLine(Op.c_str(), Count, Result));
            }
            else
            {
                std::vector<float> W;
                for (const std::string& Word : Words)
                    W.push_back(HexFloat(Word));
                Geometry::Sphere S;
                S.Center = Vec3(W, 0);
                S.Radius = W[3];
                const Vector P = S.Center;
                FLine L;
                L.Add("op", "tree");
                L.Add("sphere_props", GameId(Zq->GetSphereProperties(&S)));
                L.Add("nested", GameId(Zq->GetNestedProperties(&S)));
                L.Add("smallest", ZoneText(Zq->GetSmallestZoneAt(&P)));
                L.Add("physical", ZoneText(Zq->GetSmallestPhysicalZoneAt(&P)));
                L.Add("with_props", ZoneText(Zq->GetSmallestZoneWithPropertiesAt(&P, 0x7fc0)));
                Out.Line(L.Done());
            }
        }
        FreeSpatial(Partition);
    }
}

namespace
{
    // --- The base records of common_baseproperties.cpp (base_props.jsonl) -------

    FmtStr MakeFmt(unsigned int MadLib, bool bInt, unsigned int IntValue, bool bBase, unsigned int BaseId)
    {
        FmtStr Out;
        Out.begin_mad_lib(MadLib);
        if (bInt)
            Out.append_int(IntValue);
        if (bBase)
            Out.append_base(BaseId);
        Out.end_mad_lib();
        return Out;
    }

    std::string MarshalHex(const MarshalBuffer& Buffer)
    {
        const int Count = Buffer.get_num_written();
        return Count == 0 ? std::string() : Hex(Buffer.get_buffer(), (size_t)Count);
    }

    // Marshals Value, reads it back into a fresh record and marshals that
    // (oracle_vectors.rs props_case).
    template <typename T>
    void PropsCase(Suite& Out, const char* Record, int Case, const T& Value)
    {
        static unsigned char First[0x1000];
        static unsigned char Second[0x1000];
        MarshalBuffer W;
        W.use_write_buffer(First, (int)sizeof(First));
        Value.marshal(&W);
        MarshalBuffer R;
        R.use_read_buffer(First, W.get_num_written());
        T Back;
        Back.unmarshal(&R);
        MarshalBuffer W2;
        W2.use_write_buffer(Second, (int)sizeof(Second));
        Back.marshal(&W2);
        FLine L;
        L.Add("record", Record);
        L.Add("case", Format("%d", Case));
        L.Add("out", MarshalHex(W));
        L.Add("back", MarshalHex(W2));
        Out.Line(L.Done());
    }

    void BasePropsSuite()
    {
        Suite Out("base_props.jsonl");
        {
            MissionVendorProperties Default;
            PropsCase(Out, "mission_vendor", 0, Default);
            MissionVendorProperties P;
            P.Key = 7;
            P.BaseId = 0x11223344u;
            P.TargetId = 5;
            P.SystemId = 0xabcdef01u;
            P.MissionClass = 3;
            P.Title = MakeFmt(1001, true, 42, false, 0);
            P.Description = MakeFmt(1002, false, 0, true, 0x1234);
            P.Briefing = MakeFmt(0, false, 0, false, 0);
            P.IconTextureId = 99;
            PropsCase(Out, "mission_vendor", 1, P);
        }
        {
            MissionVendorAcceptance Default;
            PropsCase(Out, "acceptance", 0, Default);
            MissionVendorAcceptance P;
            P.Key = 2;
            P.TargetId = 3;
            P.bAccepted = true;
            P.Reason = MakeFmt(55, true, 1, false, 0);
            PropsCase(Out, "acceptance", 1, P);
        }
        {
            NewsBroadcastProperties Default;
            PropsCase(Out, "news", 0, Default);
            NewsBroadcastProperties P;
            P.Key = 1;
            P.BaseId = 2;
            P.bFlag1 = true;
            P.bFlag2 = false;
            P.Category = 4;
            P.Headline = MakeFmt(2001, false, 0, false, 0);
            P.Body = MakeFmt(2002, true, 3, true, 9);
            P.Logo = MakeFmt(0, false, 0, false, 0);
            malloc_strcpy(&P.LogoTexture, "news_logo");
            P.LogoTextureId = 0x9988;
            PropsCase(Out, "news", 1, P);
        }
        {
            CharacterProperties Default;
            PropsCase(Out, "character", 0, Default);
            CharacterProperties P;
            P.Key = 1;
            P.SessionId = 2;
            P.RoomId = 3;
            P.NameIds = 4;
            P.FactionId = 5;
            malloc_wstrcpy(&P.NameWide, L"Ab");
            malloc_strcpy(&P.NameAnsi, "place");
            P.CharType = 6;
            P.Posture = 7;
            memset(&P.Cost, 0, sizeof(P.Cost));
            P.Cost.PartIds[0] = 11;
            P.Cost.PartIds[1] = 12;
            P.Cost.PartIds[2] = 13;
            P.Cost.PartIds[3] = 14;
            P.Cost.AccessoryIds[0] = 21;
            P.Cost.AccessoryIds[1] = 22;
            P.Cost.AccessoryCount = 2;
            malloc_strcpy(&P.Script, "script.thn");
            P.BehaviorId = 8;
            P.VoiceId = 9;
            PropsCase(Out, "character", 1, P);
        }
        {
            AmbientScriptProperties Default;
            PropsCase(Out, "ambient", 0, Default);
            AmbientScriptProperties P;
            P.Key = 4;
            P.RoomId = 5;
            P.ScriptId = 6;
            P.bFlag = false;
            malloc_strcpy(&P.Name, "amb");
            PropsCase(Out, "ambient", 1, P);
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
    GuardedGameSimSuite("zone_queries", ZoneQuerySuite);
    GuardedGameSimSuite("exclusion_zones", ExclusionSuite);
    GuardedGameSimSuite("spatial", SpatialSuite);
    GuardedGameSimSuite("base_props", BasePropsSuite);
    GuardedGameSimSuite("object_math", ObjectMathSuite);
    // The last suite's universe is released under the same guard, so a fault
    // in the teardown is reported like a suite's.
    GuardedGameSimSuite("universe_shutdown", []() {
        if (GFixtureUniverseUp)
        {
            Universe::Shutdown();
            GFixtureUniverseUp = false;
        }
    });
}
