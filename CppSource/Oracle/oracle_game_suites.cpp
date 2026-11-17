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

    // zones/universe.ini through Universe::Startup with NeuronRuntime::Data
    // over the fixture directory, then every query of zone_points.list on
    // every zone of system zt, in its zone order.
    void ZoneSuite()
    {
        Suite Out("zones.jsonl");
        if (!OpenFixtureData())
            return;
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
        Universe::Startup("zone_queries\\universe.ini");

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
        Universe::Startup("zone_queries\\universe.ini");

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
    // A suite that faults ends the process without a word otherwise; the
    // guard names it and the exception, and the remaining suites still run.
    // No C++ object lives in the guarded frame.
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
}
