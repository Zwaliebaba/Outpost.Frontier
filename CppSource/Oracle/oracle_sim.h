// Oracle.exe sim, the scenario and replay oracle (work package P2.3,
// design/adr/0003-simulation-oracle.md, design/reference-build.md section 10). Shared by
// oracle_sim.cpp (one run in this process) and oracle_sim_spread.cpp (--runs N).

#pragma once

#include <string>

namespace oracle
{
    // One run of the scenario in this process; its records go to OutDir. Returns the
    // process exit code (0 when the run completed). Ends the process itself after a run that
    // started the server (the server is not shut down), so it does not return then.
    int RunSimScenario(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir);

    // Runs the scenario Runs times, each in a fresh child process writing to
    // OutDir\run_NN, then writes spread.jsonl and spread.txt into OutDir from the runs'
    // metrics.txt files. Returns 0 when every run completed.
    int RunSimSpread(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir,
                     int Runs);

    // metrics.txt: one text line per tracked object and tick and one per event, written by
    // RunSimScenario for RunSimSpread only (the JSONL files are the records):
    //   T <tick> <object> <alive 0|1> <px> <py> <pz> <m00> .. <m22> <vx> <vy> <vz> <wx> <wy> <wz> <hp>
    //   E <kind> <object> <tick>
    // Numbers are decimal with 17 significant digits (%.17g), which round-trip a double.
}
