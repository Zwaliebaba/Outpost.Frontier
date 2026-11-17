// Oracle suite of work package P1.5: Timing::Delta::calc_dt and
// Timing::seconds (common_timing.cpp). Called from RunUtilSuites in
// oracle_util.cpp.
//
// The period comes from QueryPerformanceFrequency and lives in an
// anonymous-namespace global of Common.dll, so the suite cannot choose it
// through the export. Instead common_timing.cpp is compiled into this file,
// with its namespace renamed to OracleTiming (no clash with Common.dll's
// Timing) and COMMON_API empty, and the suite sets the frequency and period
// globals the way Timing::init does.

#include <windows.h>
#include <intrin.h>

#include <cmath>
#include <cstddef>
#include <stdio.h>

#include <string>

#include "oracle_support.h"

#include "neuroncomponents.h"
#include "common_exports.h"

#undef COMMON_API
#define COMMON_API
#define Timing OracleTiming
#include "../Common/common_timing.cpp"
#undef Timing

using namespace oracle;

// Each input line: frequency, start and stop counts, start and stop
// millisecond counts, the started and stopped flags.
void RunTimingSuite()
{
    Suite Out("timing_delta.jsonl");
    for (const std::string& Line : InputLines("util/timing.list"))
    {
        long long Frequency = 0;
        long long Start = 0;
        long long Stop = 0;
        unsigned long StartMs = 0;
        unsigned long StopMs = 0;
        int Started = 0;
        int Stopped = 0;
        if (sscanf(Line.c_str(), "%lld %lld %lld %lu %lu %d %d", &Frequency, &Start, &Stop, &StartMs, &StopMs,
                   &Started, &Stopped) != 7)
        {
            fprintf(stderr, "bad timing line %s\n", Line.c_str());
            GFailed = true;
            continue;
        }
        // Timing::init after a successful QueryPerformanceFrequency.
        g_timing_frequency = (double)Frequency;
        g_timing_period = 1.0 / g_timing_frequency;

        OracleTiming::Delta Delta;
        Delta.StartTicks = Start;
        Delta.StopTicks = Stop;
        Delta.StartTickCount = StartMs;
        Delta.StopTickCount = StopMs;
        Delta.bStarted = Started != 0;
        Delta.bStopped = Stopped != 0;
        const double Dt = Delta.calc_dt();
        const double Seconds = OracleTiming::seconds(Stop);
        Out.Line(Format("{\"in\":\"%s\",\"dt\":\"%016llx\",\"seconds\":\"%016llx\"}", Line.c_str(), DoubleBits(Dt),
                        DoubleBits(Seconds)));
    }
    g_timing_frequency = 0.0;
    g_timing_period = 0.0;
}
