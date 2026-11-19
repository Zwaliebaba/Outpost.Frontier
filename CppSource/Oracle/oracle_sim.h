// Oracle.exe sim, the scenario and replay oracle (work package P2.3,
// design/adr/0003-simulation-oracle.md, design/reference-build.md section 10). Shared by
// oracle_sim.cpp (one run in this process) and oracle_sim_spread.cpp (--runs N).
//
//   Oracle.exe sim <data root> <scenario file> <out folder> [--runs N] [--seed S]

#pragma once

#include <string>

namespace oracle
{
    // The options after the three paths.
    struct FSimOptions
    {
        // --runs N: the scenario N times in fresh processes (RunSimSpread); 0 for one run here.
        int Runs = 0;
        // --seed S (Gate G2's seed ensemble, design/adr/0003-simulation-oracle.md point 9):
        // the seed of the srand the harness calls again once the system and Content's
        // population have loaded (start-up step 5), in place of the scenario's. The first
        // srand, before the load, keeps the scenario's seed: the load-time draws stay as they
        // are.
        bool bReseed = false;
        unsigned int Reseed = 0;
    };

    // One run of the scenario in this process; its records go to OutDir. Returns the
    // process exit code (0 when the run completed). Ends the process itself after a run that
    // started the server (the server is not shut down), so it does not return then.
    int RunSimScenario(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir,
                       FSimOptions const& Options);

    // Runs the scenario Options.Runs times, each in a fresh child process writing to
    // OutDir\run_NN with the same --seed, then writes spread.jsonl and spread.txt into
    // OutDir from the runs' metrics.txt files. Returns 0 when every run completed.
    int RunSimSpread(std::string const& DataRoot, std::string const& ScenarioPath, std::string const& OutDir,
                     FSimOptions const& Options);

    // metrics.txt: one text line per tracked object and tick and one per event, written by
    // RunSimScenario for RunSimSpread only (the JSONL files are the records):
    //   T <tick> <object> <alive 0|1> <px> <py> <pz> <m00> .. <m22> <vx> <vy> <vz> <wx> <wy> <wz> <hp>
    //   E <kind> <object> <tick>
    // Numbers are decimal with 17 significant digits (%.17g), which round-trip a double.
}
