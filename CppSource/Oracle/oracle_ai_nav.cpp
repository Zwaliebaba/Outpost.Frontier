// Oracle suites of work package P2.5 (flight AI, navigation layer): the intercept and
// clearance queries (common_aiintercept.cpp) and the obstacle-avoidance helpers
// (common_obstacleavoidance.cpp), over crates/game-sim/tests/fixtures/oracle/ai_nav.list
// (written by make_ai_nav_fixtures.py). Run from RunAiSuites (oracle_ai.cpp). The Rust test
// crates/game-sim/tests/oracle_ai_nav.rs writes the same lines; the committed vectors are
// crates/game-sim/tests/vectors/ai_nav.jsonl and ai_nav_turn.jsonl.
//
// The helpers are internal to Common.dll; Common/oracle_ai.cpp exports wrappers over them
// that build an IObjInspect from each FOracleAiObject (include/oracle_ai.h).
//
// Every float is written as its bit pattern (8 hex digits, a double 16), a NaN as one
// pattern (7fc00000, 7ff8000000000000).

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_ai.h"

using namespace oracle;

void AiNavSuite();
void AiNavTurnSuite();

namespace
{
    std::vector<std::string> NavWords(const std::string& Line)
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

    std::string NavF(float Value)
    {
        if (Value != Value)
            return "7fc00000";
        return Format("%08x", FloatBits(Value));
    }

    std::string NavD(double Value)
    {
        if (Value != Value)
            return "7ff8000000000000";
        return Format("%016llx", DoubleBits(Value));
    }

    std::string NavFs(const float* Values, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += NavF(Values[Index]);
        }
        return Out;
    }

    static_assert(sizeof(FOracleAiObject) == 34 * 4, "34 words");

    // The 97 words of an ai_nav.list line (make_ai_nav_fixtures.py).
    struct FNav
    {
        FOracleAiObject A;
        FOracleAiObject B;
        float Point[3];
        float Threshold;
        float Cos;
        float Angular;
        float Linear;
        float Slots[3][7];
        unsigned int ExtraCheck;
    };
    static_assert(sizeof(FNav) == 97 * 4, "97 words");

    // A transform of 12 floats (rotation in row order, then the translation) from an object.
    void ObjectXform(const FOracleAiObject& O, float* Out)
    {
        memcpy(Out, O.Rotation, 9 * sizeof(float));
        memcpy(Out + 9, O.Position, 3 * sizeof(float));
    }

    template <typename FVisit>
    void ForEachNav(const char* Name, FVisit Visit)
    {
        int Case = 0;
        for (const std::string& Line : InputLines("ai_nav.list"))
        {
            const std::vector<std::string> Parts = NavWords(Line);
            if (Parts.size() != 97)
            {
                fprintf(stderr, "%s: an ai_nav.list line without 97 words\n", Name);
                GFailed = true;
                return;
            }
            unsigned int Raw[97];
            for (int Index = 0; Index < 97; ++Index)
                Raw[Index] = (unsigned int)strtoul(Parts[Index].c_str(), nullptr, 16);
            FNav N;
            memcpy(&N, Raw, sizeof(N));
            Visit(Case++, N);
        }
    }
}

void AiNavSuite()
{
    Suite Out("ai_nav.jsonl");
    ForEachNav("ai_nav", [&](int Case, const FNav& N) {
        FLine L;
        L.Add("in", Format("%d", Case));
        L.Add("lead", NavD(OracleAiLeadInterceptDistance(&N.A, &N.B)));
        L.Add("clear", NavD(OracleAiInterceptClearance(&N.A, &N.B, 0)));
        L.Add("range", Format("%d", OracleAiWithinInterceptRange(&N.A, &N.B, N.Threshold, 0)));
        L.Add("clear_if", NavD(OracleAiInterceptClearance(&N.A, &N.B, 1)));
        L.Add("range_if", Format("%d", OracleAiWithinInterceptRange(&N.A, &N.B, N.Threshold, 1)));
        L.Add("cone", Format("%d,%d", OracleAiPointOutsideHeadingCone(N.Point, &N.B, N.Cos),
                                     OracleAiPointOutsideHeadingCone(N.Point, nullptr, N.Cos)));
        L.Add("aligned", Format("%d,%d", OracleAiAlignedOutsideHeadingCone(&N.A, &N.B, N.Cos),
                                        OracleAiAlignedOutsideHeadingCone(&N.A, nullptr, N.Cos)));
        L.Add("radial", NavD(OracleAiSignedRadialClearance(&N.A, N.Point)));
        L.Add("ahead", Format("%d", OracleAiCoAlignedAndAhead(&N.B, &N.A, N.Cos)));
        L.Add("speed", NavD(OracleAiIfaceVectorMagnitude(&N.B)) + "," + NavD(OracleAiIfaceVectorMagnitude(nullptr)));

        float Xform[12];
        ObjectXform(N.B, Xform);
        float Corners[24];
        OracleAiBoxCorners(Xform, N.B.ExtentMin, N.B.ExtentMax, Corners);
        L.Add("corners", NavFs(Corners, 24));

        float Avoid[3] = {0.0f, 0.0f, 0.0f};
        OracleAiShapeBiasedAvoid(&N.A, &N.B, Avoid);
        float AvoidNull[3] = {0.0f, 0.0f, 0.0f};
        OracleAiShapeBiasedAvoid(&N.A, nullptr, AvoidNull);
        L.Add("avoid", NavFs(Avoid, 3) + "," + NavFs(AvoidNull, 3));

        L.Add("turn", NavD(OracleAiTurnCircleCost(&N.A, N.Point, N.Angular, N.Linear)) + "," +
                          NavD(OracleAiTurnCircleCost(&N.A, N.Point, 0.0f, 0.0f)));

        // The three slots first, the other 22 expired.
        float Slots[25 * 7];
        memset(Slots, 0, sizeof(Slots));
        memcpy(Slots, N.Slots, sizeof(N.Slots));
        float AXform[12];
        ObjectXform(N.A, AXform);
        float Average[3] = {0.0f, 0.0f, 0.0f};
        const int bAverage = OracleAiAverageActiveDirection(Slots, AXform, Average);
        L.Add("average", Format("%d,", bAverage) + NavFs(Average, 3));

        float Slot[7] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        const int bSlot = OracleAiResolveCandidateSlot(&N.B, Slot);
        L.Add("slot", Format("%d,", bSlot) + NavFs(Slot, 7));
        Out.Line(L.Done());
    });
}

// Tier B: NudgeObstaclePathPoint's last branch turns through RotateTowardBySlerp (FSIN, FCOS).
void AiNavTurnSuite()
{
    Suite Out("ai_nav_turn.jsonl");
    ForEachNav("ai_nav_turn", [&](int Case, const FNav& N) {
        FLine L;
        L.Add("in", Format("%d", Case));
        float Xform[12];
        ObjectXform(N.B, Xform);
        for (int Index = 0; Index < 2; ++Index)
        {
            const int bExtra = Index == 0 ? (int)N.ExtraCheck : 1 - (int)N.ExtraCheck;
            float Point[3] = {0.0f, 0.0f, 0.0f};
            const int Code = OracleAiNudgePathPoint(N.Slots[Index], N.Point, Xform, bExtra, Point);
            if (Code != 0)
            {
                Point[0] = 0.0f;
                Point[1] = 0.0f;
                Point[2] = 0.0f;
            }
            L.Add(Index == 0 ? "nudge0" : "nudge1", Format("%d,", Code) + NavFs(Point, 3));
        }
        Out.Line(L.Done());
    });
}
