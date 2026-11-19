// Oracle.exe sim, the asteroid-field and docking-point recorders of work package P2.4
// (`asteroids.jsonl`, `dock.jsonl`; design/sim-records.md). Kept apart from oracle_sim.cpp:
// the run calls OracleSimFieldsStart once, after its own hooks are installed, and
// OracleSimFieldsTick after each frame's per-tick records.

#pragma once

#include <string>

namespace oracle
{
    // The scenario's name of an object id, "#<id>" for any other (oracle_sim.cpp's NameOf).
    typedef std::string (*FSimFieldNameFn)(unsigned int Id);

    // Opens <OutDir>\asteroids.jsonl and <OutDir>\dock.jsonl and installs the docking-point
    // hooks (GOracleSimHooks.OnDockAnim, OnDockActivate). SystemId is the scenario's system,
    // whose asteroid fields (CmnAsteroid::Find) the tick records walk; Tick points at the
    // run's tick counter. Returns false (and records nothing) when a file cannot be written.
    bool OracleSimFieldsStart(std::string const& OutDir, unsigned int SystemId,
                              unsigned int const* Tick, FSimFieldNameFn NameOf);

    // The asteroid records of the tick that just ran: every cube made since the last call,
    // with its rocks, and each field's cube states when they changed.
    void OracleSimFieldsTick();
}
