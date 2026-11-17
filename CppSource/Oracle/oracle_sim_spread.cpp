// Oracle.exe sim ... --runs N (work package P2.3, design/adr/0003-simulation-oracle.md point 9):
// the scenario N times, each in a fresh child process, then the run-to-run spread of every
// metric -- the largest difference between any two runs -- into spread.jsonl (record layout
// in design/sim-records.md) and spread.txt (the same in decimal, for reading).

#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "oracle_support.h"
#include "oracle_sim.h"
#include "../OutpostFrontier/Client/oracle_json.h"

namespace
{
    using oracle::Format;

    // One T line of metrics.txt.
    struct FState
    {
        bool bAlive = false;
        double V[19] = {};   // pos 3, orient 9, vel 3, angvel 3, hp
    };

    struct FRun
    {
        int ExitCode = -1;
        bool bRead = false;
        std::map<std::string, std::map<unsigned, FState>> States;   // object -> tick -> state
        std::map<std::string, unsigned> FirstEvent;                 // "<kind> <object>" -> tick
    };

    bool ReadMetrics(std::string const& Path, FRun& Run)
    {
        FILE* File = fopen(Path.c_str(), "rb");
        if (File == nullptr)
            return false;
        char Line[2048];
        while (fgets(Line, sizeof(Line), File) != nullptr)
        {
            char Kind[64] = {};
            char Name[256] = {};
            unsigned Tick = 0;
            if (Line[0] == 'T')
            {
                FState State;
                int Alive = 0;
                double* const V = State.V;
                int const Got = sscanf_s(Line,
                                         "T %u %255s %d %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg %lg "
                                         "%lg %lg %lg %lg",
                                         &Tick, Name, (unsigned)sizeof(Name), &Alive, &V[0], &V[1], &V[2], &V[3],
                                         &V[4], &V[5], &V[6], &V[7], &V[8], &V[9], &V[10], &V[11], &V[12],
                                         &V[13], &V[14], &V[15], &V[16], &V[17], &V[18]);
                if (Got != 22)
                    continue;
                State.bAlive = Alive != 0;
                Run.States[Name][Tick] = State;
            }
            else if (Line[0] == 'E')
            {
                if (sscanf_s(Line, "E %63s %255s %u", Kind, (unsigned)sizeof(Kind), Name, (unsigned)sizeof(Name),
                             &Tick) != 3)
                    continue;
                std::string const Key = std::string(Kind) + " " + Name;
                if (Run.FirstEvent.find(Key) == Run.FirstEvent.end())
                    Run.FirstEvent[Key] = Tick;
            }
        }
        fclose(File);
        return true;
    }

    double PairDistance(double const* A, double const* B)
    {
        double const X = A[0] - B[0];
        double const Y = A[1] - B[1];
        double const Z = A[2] - B[2];
        return sqrt(X * X + Y * Y + Z * Z);
    }

    double MaxAbs(double const* A, double const* B, int Count)
    {
        double Out = 0.0;
        for (int Index = 0; Index < Count; ++Index)
        {
            double const D = fabs(A[Index] - B[Index]);
            if (D > Out)
                Out = D;
        }
        return Out;
    }

    // The spread of one metric over the ticks: its largest value, the tick of it, the first
    // tick where it is above zero and its value at the last tick compared.
    struct FSpread
    {
        double Max = 0.0;
        unsigned MaxTick = 0;
        bool bDiverged = false;
        unsigned FirstTick = 0;
        double Final = 0.0;

        void Add(unsigned Tick, double Value)
        {
            if (Value > Max)
            {
                Max = Value;
                MaxTick = Tick;
            }
            if (Value > 0.0 && !bDiverged)
            {
                bDiverged = true;
                FirstTick = Tick;
            }
            Final = Value;
        }
    };

    enum EMetric
    {
        MetricPos,
        MetricOrient,
        MetricVel,
        MetricAngVel,
        MetricHp,
        kMetricCount
    };

    char const* const kMetricNames[kMetricCount] = {"pos", "orient", "vel", "angvel", "hp"};

    // Metric M of two states (largest pairwise difference, as distance or element-wise).
    double Difference(int Metric, FState const& A, FState const& B)
    {
        switch (Metric)
        {
        case MetricPos:
            return PairDistance(A.V + 0, B.V + 0);
        case MetricOrient:
            return MaxAbs(A.V + 3, B.V + 3, 9);
        case MetricVel:
            return PairDistance(A.V + 12, B.V + 12);
        case MetricAngVel:
            return PairDistance(A.V + 15, B.V + 15);
        default:
            return fabs(A.V[18] - B.V[18]);
        }
    }

    std::string QuoteCommandArg(std::string const& Text)
    {
        return "\"" + Text + "\"";
    }

    int RunChild(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& RunDir)
    {
        char Exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, Exe, MAX_PATH);
        std::string Command = QuoteCommandArg(Exe) + " sim " + QuoteCommandArg(DataRoot) + " " +
                              QuoteCommandArg(ScenarioPath) + " " + QuoteCommandArg(RunDir);
        std::vector<char> Buffer(Command.begin(), Command.end());
        Buffer.push_back('\0');
        STARTUPINFOA Startup;
        memset(&Startup, 0, sizeof(Startup));
        Startup.cb = sizeof(Startup);
        PROCESS_INFORMATION Process;
        memset(&Process, 0, sizeof(Process));
        if (!CreateProcessA(Exe, Buffer.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &Startup, &Process))
        {
            fprintf(stderr, "sim: cannot start %s (%lu)\n", Exe, GetLastError());
            return -1;
        }
        WaitForSingleObject(Process.hProcess, INFINITE);
        DWORD Code = 0;
        GetExitCodeProcess(Process.hProcess, &Code);
        CloseHandle(Process.hThread);
        CloseHandle(Process.hProcess);
        return (int)Code;
    }
}

namespace oracle
{
    int RunSimSpread(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir, int Runs)
    {
        MakeParentDirectories(OutDir + "\\spread.jsonl");
        std::vector<FRun> Results((size_t)Runs);
        int Completed = 0;
        for (int Index = 0; Index < Runs; ++Index)
        {
            std::string const RunDir = OutDir + Format("\\run_%02d", Index + 1);
            FRun& Run = Results[(size_t)Index];
            Run.ExitCode = RunChild(DataRoot, ScenarioPath, RunDir);
            Run.bRead = Run.ExitCode == 0 && ReadMetrics(RunDir + "\\metrics.txt", Run);
            if (Run.bRead)
                ++Completed;
            printf("sim run %d of %d: exit %d%s\n", Index + 1, Runs, Run.ExitCode, Run.bRead ? "" : " (no metrics)");
            fflush(stdout);
        }

        FILE* Json = fopen((OutDir + "\\spread.jsonl").c_str(), "wb");
        FILE* Text = fopen((OutDir + "\\spread.txt").c_str(), "wb");
        if (Json == nullptr || Text == nullptr)
        {
            fprintf(stderr, "sim: cannot write the spread files in %s\n", OutDir.c_str());
            if (Json != nullptr)
                fclose(Json);
            if (Text != nullptr)
                fclose(Text);
            return 3;
        }

        {
            FOracleJson J;
            J.Str("rec", "runs");
            J.Str("scenario", ScenarioPath.c_str());
            J.Int("runs", Runs);
            J.Int("completed", Completed);
            J.BeginArray("exit_codes");
            for (FRun const& Run : Results)
                J.Int(nullptr, Run.ExitCode);
            J.EndArray();
            fprintf(Json, "%s\n", J.Finish().c_str());
            fprintf(Text, "scenario %s\nruns %d, completed %d\n", ScenarioPath.c_str(), Runs, Completed);
        }

        std::vector<FRun const*> Good;
        for (FRun const& Run : Results)
        {
            if (Run.bRead)
                Good.push_back(&Run);
        }

        if (Good.size() >= 2)
        {
            // Objects and ticks of the first completed run; a tick missing in another run
            // counts as an alive mismatch.
            for (auto const& Object : Good[0]->States)
            {
                FSpread Spread[kMetricCount];
                unsigned AliveMismatch = 0;
                unsigned Ticks = 0;
                for (auto const& Tick : Object.second)
                {
                    ++Ticks;
                    double Worst[kMetricCount] = {};
                    bool bAllAlive = true;
                    bool bMismatch = false;
                    std::vector<FState const*> States;
                    for (FRun const* Run : Good)
                    {
                        auto const ObjectIt = Run->States.find(Object.first);
                        if (ObjectIt == Run->States.end())
                        {
                            bMismatch = true;
                            continue;
                        }
                        auto const TickIt = ObjectIt->second.find(Tick.first);
                        if (TickIt == ObjectIt->second.end())
                        {
                            bMismatch = true;
                            continue;
                        }
                        States.push_back(&TickIt->second);
                        bAllAlive = bAllAlive && TickIt->second.bAlive;
                    }
                    for (size_t A = 0; A < States.size(); ++A)
                    {
                        for (size_t B = A + 1; B < States.size(); ++B)
                        {
                            if (States[A]->bAlive != States[B]->bAlive)
                                bMismatch = true;
                        }
                    }
                    if (bMismatch)
                        ++AliveMismatch;
                    if (!bAllAlive)
                        continue;
                    for (size_t A = 0; A < States.size(); ++A)
                    {
                        for (size_t B = A + 1; B < States.size(); ++B)
                        {
                            for (int Metric = 0; Metric < kMetricCount; ++Metric)
                            {
                                double const D = Difference(Metric, *States[A], *States[B]);
                                if (D > Worst[Metric])
                                    Worst[Metric] = D;
                            }
                        }
                    }
                    for (int Metric = 0; Metric < kMetricCount; ++Metric)
                        Spread[Metric].Add(Tick.first, Worst[Metric]);
                }

                FOracleJson J;
                J.Str("rec", "object");
                J.Str("obj", Object.first.c_str());
                J.Uint("ticks", Ticks);
                J.Uint("alive_mismatch_ticks", AliveMismatch);
                fprintf(Text, "\nobject %s: %u ticks, %u with alive mismatch\n", Object.first.c_str(), Ticks,
                        AliveMismatch);
                for (int Metric = 0; Metric < kMetricCount; ++Metric)
                {
                    FSpread const& S = Spread[Metric];
                    J.BeginObject(kMetricNames[Metric]);
                    J.Double("max", S.Max);
                    J.Uint("max_tick", S.MaxTick);
                    if (S.bDiverged)
                        J.Uint("first_tick", S.FirstTick);
                    else
                        J.Null("first_tick");
                    J.Double("final", S.Final);
                    J.EndObject();
                    fprintf(Text, "  %-6s max %.9g at tick %u, final %.9g, first above zero %s\n",
                            kMetricNames[Metric], S.Max, S.MaxTick, S.Final,
                            S.bDiverged ? Format("at tick %u", S.FirstTick).c_str() : "never");
                }
                fprintf(Json, "%s\n", J.Finish().c_str());
            }

            std::set<std::string> Keys;
            for (FRun const* Run : Good)
            {
                for (auto const& Event : Run->FirstEvent)
                    Keys.insert(Event.first);
            }
            for (std::string const& Key : Keys)
            {
                unsigned Present = 0;
                unsigned First = 0;
                unsigned Last = 0;
                for (FRun const* Run : Good)
                {
                    auto const It = Run->FirstEvent.find(Key);
                    if (It == Run->FirstEvent.end())
                        continue;
                    if (Present == 0 || It->second < First)
                        First = It->second;
                    if (Present == 0 || It->second > Last)
                        Last = It->second;
                    ++Present;
                }
                size_t const Space = Key.find(' ');
                FOracleJson J;
                J.Str("rec", "event");
                J.Str("kind", Key.substr(0, Space).c_str());
                J.Str("obj", Key.substr(Space + 1).c_str());
                J.Uint("present", Present);
                J.Uint("of", (unsigned)Good.size());
                J.Uint("first_min", First);
                J.Uint("first_max", Last);
                J.Uint("spread", Last - First);
                fprintf(Json, "%s\n", J.Finish().c_str());
                fprintf(Text, "\nevent %s: in %u of %u runs, first at tick %u..%u (spread %u)\n", Key.c_str(), Present,
                        (unsigned)Good.size(), First, Last, Last - First);
            }
        }
        else
        {
            fprintf(Text, "fewer than two completed runs: no spread\n");
        }
        fclose(Json);
        fclose(Text);
        return Completed == Runs ? 0 : 1;
    }
}
