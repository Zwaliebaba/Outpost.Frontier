// Oracle.exe sim, the run's environment (design/sim-records.md, session.jsonl `env`): the
// machine, the compiler and the runtime DLLs a run used, so that two runs that differ can be
// told apart by what they ran on (the cross-runner reproducibility work, owner 2026-11-19).
//
// It only reads: CPUID, the process's loaded modules and their version resources, the
// registry's Windows build. It allocates nothing on the game's heaps after Server.dll has
// started, so it cannot move what an uninitialised read would see.

#pragma once

#include <string>

namespace oracle
{
    // The `env` record as one finished session.jsonl line (no newline).
    std::string OracleSimEnvRecord();
}
