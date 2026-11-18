// Oracle suites of work package P2.5 (flight AI, the parts without an object world), run by
// `Oracle.exe game-sim <fixture directory> <output directory>` after the P2.1 suites, over
// crates/game-sim/tests/fixtures/oracle (written by make_ai_fixtures.py). The Rust test
// crates/game-sim/tests/oracle_ai.rs writes the same lines; the committed vectors are
// crates/game-sim/tests/vectors/ai_*.jsonl.
//
// The AI helpers are internal to Common.dll; Common/oracle_ai.cpp exports thin wrappers
// (include/oracle_ai.h). They use the 3DMathEngine, so the suites load it first.
//
// Every float is written as its bit pattern (8 hex digits, a double 16), a NaN as one
// pattern (7fc00000, 7ff8000000000000) so that NaN payloads do not matter.

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "common_ini.h"
#include "common_iface.h"     // BehaviorTypes
#include "common_newchar.h"   // pub::StateGraph
#include "oracle_ai.h"

using namespace oracle;

// Defined below the suites; WriteStateGraphLines (oracle_game.h) is also used by the
// state_graph data dump (oracle_game_data.cpp).
std::vector<IStateGraph const*> LoadedStateGraphs();

// The navigation suites (oracle_ai_nav.cpp).
void AiNavSuite();
void AiNavTurnSuite();

namespace
{
    std::vector<std::string> Words(const std::string& Line)
    {
        std::vector<std::string> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            const size_t End = Line.find(' ', Start);
            if (End == std::string::npos)
            {
                Out.push_back(Line.substr(Start));
                break;
            }
            Out.push_back(Line.substr(Start, End - Start));
            Start = End + 1;
        }
        return Out;
    }

    float HexF(const std::string& Text)
    {
        return FloatFromBits((unsigned int)strtoul(Text.c_str(), nullptr, 16));
    }

    std::string F(float Value)
    {
        if (Value != Value)
            return "7fc00000";
        return Format("%08x", FloatBits(Value));
    }

    std::string D(double Value)
    {
        if (Value != Value)
            return "7ff8000000000000";
        return Format("%016llx", DoubleBits(Value));
    }

    std::string Fs(const float* Values, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += F(Values[Index]);
        }
        return Out;
    }

    void Copy(float* Out, const float* In, int Count)
    {
        for (int Index = 0; Index < Count; ++Index)
            Out[Index] = In[Index];
    }

    // The 66 inputs of an ai_geometry.list line (make_ai_fixtures.py).
    struct FGeometry
    {
        float Origin[3];
        float Dir[3];
        float Center[3];
        float Radius;
        float XformA[12];
        float BoxA[6];
        float XformB[12];
        float BoxB[6];
        float Capsule[7];
        float Point[3];
        float A1[3];
        float B0[3];
        float B1[3];
        float RadiusB;
    };
    static_assert(sizeof(FGeometry) == 66 * sizeof(float), "66 floats");

    void GeometrySuite()
    {
        Suite Out("ai_raycast.jsonl");
        int Case = 0;
        for (const std::string& Line : InputLines("ai_geometry.list"))
        {
            const std::vector<std::string> Parts = Words(Line);
            if (Parts.size() != 66)
            {
                fprintf(stderr, "ai_geometry.list: a line without 66 values\n");
                GFailed = true;
                return;
            }
            float Raw[66];
            for (int Index = 0; Index < 66; ++Index)
                Raw[Index] = HexF(Parts[Index]);
            FGeometry G;
            memcpy(&G, Raw, sizeof(G));

            FLine L;
            L.Add("in", Format("%d", Case++));

            float T = -2.0f;
            const int bSphere = OracleAiRaySphere(G.Origin, G.Dir, G.Center, G.Radius, &T);
            L.Add("sphere", Format("%d,", bSphere) + F(T));

            float TMin = 0.0f;
            float TMax = 3.4028235e+38f;
            const int bAabb = OracleAiRayAabb(G.Origin, G.Dir, G.Center, &TMin, &TMax);
            L.Add("aabb", Format("%d,", bAabb) + F(TMin) + "," + F(TMax));

            // The box: the centre, the rows of transform A's rotation as the axes, box A's
            // first three values as the half extents.
            float Box[15];
            Copy(Box, G.Center, 3);
            Copy(Box + 3, G.XformA, 9);
            Copy(Box + 12, G.BoxA, 3);
            float Points[6];
            Copy(Points, G.Point, 3);
            Copy(Points + 3, G.Point, 3);
            int HitType = -1;
            const int bBox = OracleAiRayOrientedBox(G.Origin, G.Dir, Box, &HitType, Points);
            L.Add("obox", Format("%d,%d,", bBox, HitType) + Fs(Points, 6));

            float Nearest[3];
            Copy(Nearest, G.Point, 3);
            int bInside = -1;
            const int bNearest = OracleAiRayTransformedBoxNearest(G.Origin, G.Dir, G.XformA, G.BoxA, Nearest, &bInside);
            L.Add("nearest", Format("%d,%d,", bNearest, bInside) + Fs(Nearest, 3));

            L.Add("boxes", Format("%d", OracleAiBoxesIntersect(G.XformA, G.BoxA, G.XformB, G.BoxB)));

            float Dir[3];
            Copy(Dir, G.Dir, 3);
            float Basis1[3];
            float Basis2[3];
            OracleAiPerpendicularBasis(Dir, 0, Basis1, Basis2);
            L.Add("basis", Fs(Dir, 3) + "," + Fs(Basis1, 3) + "," + Fs(Basis2, 3));
            Copy(Dir, G.Dir, 3);
            OracleAiPerpendicularBasis(Dir, 1, Basis1, Basis2);
            L.Add("basis_n", Fs(Basis1, 3) + "," + Fs(Basis2, 3));

            float Ts[2] = {0.0f, 0.0f};
            const int Count = OracleAiCapsuleIntersect(G.Origin, G.Dir, G.Capsule, Ts);
            std::string Capsule = Format("%d", Count);
            for (int Index = 0; Index < Count && Index < 2; ++Index)
                Capsule += "," + F(Ts[Index]);
            L.Add("capsule", Capsule);

            float First[3] = {0.0f, 0.0f, 0.0f};
            const int bFirst = OracleAiCapsuleFirstHit(G.Origin, G.Dir, G.Capsule, First);
            L.Add("first", Format("%d,", bFirst) + Fs(First, 3));

            L.Add("in_capsule", Format("%d", OracleAiPointInCapsule(G.Point, G.Capsule)));
            L.Add("dot", D(OracleAiClampedDot3(G.Dir, G.Point)));

            float SweptT = -2.0f;
            const int bSwept = OracleAiSweptSphere(G.Radius, G.Origin, G.A1, G.RadiusB, G.B0, G.B1, &SweptT);
            L.Add("swept", Format("%d,", bSwept) + F(SweptT));
            Out.Line(L.Done());
        }
    }

    // The 28 floats of an ai_steer.list line after the seed.
    struct FSteer
    {
        float Reference[9];
        float Forward[3];
        float Hint[3];
        float Threshold;
        float Angle;
        float MatB[9];
        float Spread;
        float Bank;
    };
    static_assert(sizeof(FSteer) == 28 * sizeof(float), "28 floats");

    void SteerSuite()
    {
        // ai_steer.jsonl holds what needs no trigonometry; ai_turn.jsonl what goes through
        // FSIN, FCOS, _CIacos or _CIasin (Tier B on the Rust side, decision T11).
        Suite Out("ai_steer.jsonl");
        Suite Turn("ai_turn.jsonl");
        for (const std::string& Line : InputLines("ai_steer.list"))
        {
            const std::vector<std::string> Parts = Words(Line);
            if (Parts.size() != 29)
            {
                fprintf(stderr, "ai_steer.list: a line without a seed and 28 values\n");
                GFailed = true;
                return;
            }
            const unsigned int Seed = (unsigned int)strtoul(Parts[0].c_str(), nullptr, 10);
            float Raw[28];
            for (int Index = 0; Index < 28; ++Index)
                Raw[Index] = HexF(Parts[1 + Index]);
            FSteer S;
            memcpy(&S, Raw, sizeof(S));

            FLine L;
            L.Add("seed", Format("%u", Seed));

            float M[9];
            OracleAiOrientationBasis(S.Reference, S.Forward, S.Hint, M);
            L.Add("basis_hint", Fs(M, 9));
            OracleAiOrientationBasis(S.Reference, S.Forward, nullptr, M);
            L.Add("basis", Fs(M, 9));

            Copy(M, S.Reference, 9);
            int bOk = OracleAiPerpendicularFrame(S.Forward, S.Threshold, S.Hint, M);
            L.Add("perp", Format("%d,", bOk) + Fs(M, 9));
            Copy(M, S.Reference, 9);
            bOk = OracleAiPerpendicularFrame(S.Forward, S.Threshold, nullptr, M);
            L.Add("perp_default", Format("%d,", bOk) + Fs(M, 9));

            OracleAiSteeringFrame(S.Reference, S.Forward, S.Hint, M);
            L.Add("steering", Fs(M, 9));

            float V[3];
            OracleAiTransformVectorByMatrix(S.Reference, S.Hint, V);
            L.Add("xform", Fs(V, 3));

            srand(Seed);
            OracleAiRandomVector(S.Angle, V);
            L.Add("random", Fs(V, 3));
            Out.Line(L.Done());

            FLine T;
            T.Add("seed", Format("%u", Seed));
            OracleAiRotateTowardBySlerp(S.Forward, S.Hint, S.Angle, V);
            T.Add("slerp", Fs(V, 3));

            const float Angle = OracleAiAngularThrottle(S.Reference, S.MatB, V);
            T.Add("throttle", Fs(V, 3) + "," + F(Angle));

            srand(Seed);
            OracleAiRandomSpreadFrame(S.Reference, S.Spread, S.Bank, M);
            T.Add("spread", Fs(M, 9));
            Turn.Line(T.Done());
        }
    }

    std::string Lower(const std::string& Text)
    {
        std::string Out = Text;
        for (char& Ch : Out)
        {
            if (Ch >= 'A' && Ch <= 'Z')
                Ch = (char)(Ch - 'A' + 'a');
        }
        return Out;
    }

    void StateGraphSuite()
    {
        Suite Out("ai_state_graph.jsonl");
        for (const std::string& File : InputLines("ai_state_graph.list"))
        {
            const std::string Path = "ai_state_graph\\" + File;
            OracleAiLoadStateGraph(Path.c_str());
            FLine Head;
            Head.Add("file", File);
            Head.Add("graphs", Format("%u", (unsigned int)LoadedStateGraphs().size()));
            Out.Line(Head.Done());
            WriteStateGraphLines(Out);
        }
        // Leave nothing loaded for later suites.
        OracleAiFreeStateGraph();
    }

    void BehaviorNameSuite()
    {
        Suite Out("ai_behavior_names.jsonl");
        for (int Id = -1; Id <= 20; ++Id)
        {
            FLine L;
            L.Add("id", Format("%d", Id));
            L.Add("name", GameText(BehaviorTypes::get_behavior_name(Id)));
            Out.Line(L.Done());
        }
        for (const std::string& Name : InputLines("ai_behavior_names.list"))
        {
            FLine L;
            L.Add("name", GameText(Name.c_str()));
            L.Add("id", Format("%d", BehaviorTypes::get_behavior_id(Name.c_str())));
            Out.Line(L.Done());
        }
    }

    void PersonalitySuite()
    {
        Suite Out("ai_personality.jsonl");
        unsigned char Bytes[0x3d8];
        OracleAiDefaultPersonality(Bytes);
        {
            FLine L;
            L.Add("default", Hex(Bytes, sizeof(Bytes)));
            Out.Line(L.Done());
        }
        for (int Type = -1; Type <= 6; ++Type)
        {
            FLine L;
            L.Add("toughness", Format("%d", Type));
            L.Add("ratio", F(OracleAiToughnessRatio(Type)));
            Out.Line(L.Done());
        }
        for (int Job = -1; Job <= 3; ++Job)
        {
            FLine L;
            L.Add("job", Format("%d", Job));
            L.Add("range", D(OracleAiRangeThresholdValue(Job)));
            Out.Line(L.Done());
        }
    }

    // A suite that faults ends the process without a word otherwise (see
    // oracle_game_suites.cpp); no C++ object lives in the guarded frame.
    typedef void (*FAiSuite)();

    DWORD GuardedAiSuiteCall(FAiSuite Run)
    {
        __try
        {
            Run();
            return 0;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return GetExceptionCode();
        }
    }

    void GuardedAiSuite(const char* Name, FAiSuite Run)
    {
        const DWORD Code = GuardedAiSuiteCall(Run);
        fflush(stdout);
        if (Code != 0)
        {
            fprintf(stderr, "game-sim %s: exception %08lx\n", Name, static_cast<unsigned long>(Code));
            GFailed = true;
        }
    }
}

std::vector<IStateGraph const*> LoadedStateGraphs()
{
    std::vector<IStateGraph const*> Graphs;
    for (int Id = 0;; ++Id)
    {
        IStateGraph const* const Graph = pub::StateGraph::get_state_graph_internal(Id);
        if (Graph == nullptr)
            break;
        Graphs.push_back(Graph);
    }
    return Graphs;
}

void WriteStateGraphLines(oracle::Suite& Out)
{
    const std::vector<IStateGraph const*> Graphs = LoadedStateGraphs();
    for (IStateGraph const* Graph : Graphs)
    {
        FLine L;
        L.Add("id", Format("%d", Graph->Id));
        L.Add("type", Format("%d", Graph->Type));
        L.Add("name", GameText(Graph->Name));
        L.Add("w", Fs(&Graph->Weights[0][0], 21 * 21));
        Out.Line(L.Done());
    }
    for (IStateGraph const* Graph : Graphs)
    {
        const std::string Name = Graph->Name;
        const std::string LowerName = Lower(Name);
        FLine L;
        L.Add("find", GameText(Name.c_str()));
        L.Add("leader", Format("%d", pub::StateGraph::get_state_graph(Name.c_str(), pub::StateGraph::LEADER)));
        L.Add("escort", Format("%d", pub::StateGraph::get_state_graph(Name.c_str(), pub::StateGraph::ESCORT)));
        L.Add("lower_leader", Format("%d", pub::StateGraph::get_state_graph(LowerName.c_str(), pub::StateGraph::LEADER)));
        L.Add("by_id_leader", Format("%d", pub::StateGraph::get_state_graph(Graph->Id, pub::StateGraph::LEADER)));
        L.Add("by_id_escort", Format("%d", pub::StateGraph::get_state_graph(Graph->Id, pub::StateGraph::ESCORT)));
        Out.Line(L.Done());
    }
}

void RunAiSuites()
{
    // The steering helpers call the 3DMathEngine (Acquire3DMathEngine).
    if (!OracleLoadMathEngine())
    {
        fprintf(stderr, "game-sim: no 3DMathEngine for the AI suites\n");
        GFailed = true;
        return;
    }
    GuardedAiSuite("ai_raycast", GeometrySuite);
    GuardedAiSuite("ai_steer", SteerSuite);
    GuardedAiSuite("ai_state_graph", StateGraphSuite);
    GuardedAiSuite("ai_behavior_names", BehaviorNameSuite);
    GuardedAiSuite("ai_personality", PersonalitySuite);
    GuardedAiSuite("ai_nav", AiNavSuite);
    GuardedAiSuite("ai_nav_turn", AiNavTurnSuite);
}
