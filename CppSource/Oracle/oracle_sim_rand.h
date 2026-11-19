// Oracle.exe sim: the rand() recorder (design/sim-records.md, `load_rand.jsonl`). One counting
// wrapper replaces the `rand` import of every loaded module that imports it (Common.dll,
// Server.dll, Content.dll, the engine DLLs); it calls the C runtime's own rand, so the random
// sequence of a run is unchanged. Two users: the behaviour-tick recorder (oracle_sim_ai.cpp),
// which keeps the values drawn during a behaviour tick, and the load-time recorder below,
// which records every draw from the harness's srand until the first frame with its caller.
//
// Everything here is inert unless Oracle.exe sim calls it.

#pragma once

#include <string>
#include <vector>

namespace oracle
{
    // Called for every draw with its value and the name of the DLL that drew it.
    typedef void (*FRandObserver)(int Value, const char* Module);

    // Installs the wrapper in every module loaded now that imports rand (again on a later
    // call for modules loaded since). Hooked receives the names of the hooked modules, in
    // load order.
    void OracleSimRandHook(std::vector<std::string>& Hooked);

    // The behaviour-tick recorder's observer (null for none).
    void OracleSimRandSetObserver(FRandObserver Observer);

    // The load-time recorder. Begin before the harness's srand(Seed): it installs the hooks,
    // starts the symbol handler and records every draw from then on, with the phase set by
    // Phase, the caller's module offset, function and source line, and a short stack. End
    // stops recording and writes <OutDir>\load_rand.jsonl and the summary load_rand.txt.
    // Begin returns false (and nothing is recorded) when the files cannot be written.
    bool OracleSimLoadRandBegin(std::string const& OutDir, unsigned int Seed);
    void OracleSimLoadRandPhase(std::string const& Phase);
    // The harness calls srand(Seed) again: the indices of the draws after it are counted in
    // the new sequence, from 0.
    void OracleSimLoadRandReseed(unsigned int Seed);
    void OracleSimLoadRandEnd();

    // The frame-time recorder (the cross-runner reproducibility work, owner 2026-11-19): with
    // OUTPOST_ORACLE_RUN_RAND set, Oracle.exe sim starts it after OracleSimLoadRandEnd. Every
    // draw from then on is kept with *Tick, its index in the CRT sequence (the generator is
    // followed on from the load-time recorder's state) and its caller's stack; End writes
    // <OutDir>\run_rand.jsonl (`draw` records, then one `sym` record per address).
    bool OracleSimRunRandBegin(std::string const& OutDir, unsigned int const* Tick);
    void OracleSimRunRandEnd();
}
