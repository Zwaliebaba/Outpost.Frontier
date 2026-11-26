// Oracle.exe sim, the behaviour-tick recorder of work package P2.5 (`ai.jsonl`,
// design/sim-records.md; record 0003 point 7). Kept apart from oracle_sim.cpp: the run calls
// OracleSimAiStart once, after its own hooks are installed, and the recorder chains itself
// in front of them.

#pragma once

#include <string>

namespace oracle
{
    // The scenario's name of an object id, "#<id>" for any other (oracle_sim.cpp's NameOf).
    typedef std::string (*FSimNameFn)(unsigned int Id);

    // Opens <OutDir>\ai.jsonl and installs the recorder: it wraps
    // GOracleSimHooks.OnBehaviorTick (so it must run after that is set), sets
    // GOracleAiHooks.OnQuery and observes the rand() recorder (oracle_sim_rand.h), whose
    // wrapper replaces the `rand` import of every game DLL. Tick points at the run's
    // tick counter. Returns false (and records nothing) when the file cannot be written.
    bool OracleSimAiStart(std::string const& OutDir, unsigned int const* Tick, FSimNameFn NameOf);

    // P3.6d: from here on every directive a behaviour manager hears through
    // pub::AI::SubmitDirective (GOracleAiHooks.OnDirective) is recorded as an `ai_answer` of
    // kind `directive`; those before OracleSimAiStart are written right after `ai_start`.
    void OracleSimAiDirectivesBegin();
    // Brackets the harness's own scenario directives, which are not recorded (the replay
    // takes them from the scenario file).
    void OracleSimAiHarnessDirective(bool bBegin);
}
