// Oracle suite of work package P2.5 (the system manager's combat parts without an object
// world): the sustained-threshold timer, a gun slot's timing draws and one UpdateTimers step
// of a gun slot and a line-of-fire cache slot. Run from RunAiSuites (oracle_ai.cpp) over
// ai_combat.list (written by make_ai_fixtures.py); the Rust test is the ai_combat test of
// crates/game-sim/tests/oracle_ai.rs and the committed vector
// crates/game-sim/tests/vectors/ai_combat.jsonl.
//
// Every float is written as its bit pattern (8 hex digits).

#include <windows.h>

#include "oracle_support.h"
#include "oracle_game.h"

#include <stdlib.h>
#include <string.h>

#include <string>
#include <vector>

#include "oracle_ai.h"

using namespace oracle;

namespace
{
    // The 28 values of an ai_combat.list line (make_ai_fixtures.py).
    const size_t kCombatValues = 28;

    std::string CombatF(float Value)
    {
        if (Value != Value)
            return "7fc00000";
        return Format("%08x", FloatBits(Value));
    }

    std::string CombatFs(const float* Values, int Count)
    {
        std::string Out;
        for (int Index = 0; Index < Count; ++Index)
        {
            if (Index != 0)
                Out += ",";
            Out += CombatF(Values[Index]);
        }
        return Out;
    }

    std::vector<float> CombatFloats(const std::string& Line)
    {
        std::vector<float> Out;
        size_t Start = 0;
        while (Start <= Line.size())
        {
            size_t End = Line.find(' ', Start);
            if (End == std::string::npos)
                End = Line.size();
            const std::string Word = Line.substr(Start, End - Start);
            Out.push_back(FloatFromBits((unsigned int)strtoul(Word.c_str(), nullptr, 16)));
            Start = End + 1;
        }
        return Out;
    }
}

void AiCombatSuite()
{
    Suite Out("ai_combat.jsonl");
    for (const std::string& Line : InputLines("ai_combat.list"))
    {
        const std::vector<float> V = CombatFloats(Line);
        if (V.size() != kCombatValues)
        {
            fprintf(stderr, "ai_combat.list: a line without 28 values\n");
            GFailed = true;
            return;
        }
        const unsigned int Seed = (unsigned int)V[0];
        const int Kind = (int)V[1];
        const int bAutoTurret = (int)V[2];
        const float Dt = V[3];
        const float Level = V[4];

        FLine L;
        L.Add("seed", Format("%u", Seed));

        float Timer[2];
        const int bTimer = OracleAiThresholdTimer(&V[16], Dt, Level, Timer);
        L.Add("timer", Format("%d,", bTimer) + CombatFs(Timer, 2));

        float Timing[4];
        srand(Seed);
        OracleAiGunSlotTiming(&V[5], Kind, bAutoTurret, Timing);
        L.Add("timing", CombatFs(Timing, 4));

        float Step[7];
        OracleAiGunGroupStep(Kind, &V[20], V[24], &V[25], Dt, Step);
        L.Add("step", CombatFs(Step, 7));
        Out.Line(L.Done());
    }
}
