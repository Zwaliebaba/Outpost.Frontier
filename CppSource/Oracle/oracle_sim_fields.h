// Oracle.exe sim, the asteroid-field and docking-point recorders of work package P2.4
// (`asteroids.jsonl`, `dock.jsonl`; design/sim-records.md). Kept apart from oracle_sim.cpp:
// the run calls OracleSimFieldsStart once, after its own hooks are installed, and
// OracleSimFieldsTick after each frame's per-tick records.

#pragma once

#include <string>
#include <vector>

class Vector;

namespace oracle
{
    // The scenario's name of an object id, "#<id>" for any other (oracle_sim.cpp's NameOf).
    typedef std::string (*FSimFieldNameFn)(unsigned int Id);
    // The positions of the scenario's live ships (oracle_sim.cpp's ShipPositions).
    typedef std::vector<Vector> (*FSimShipPositionsFn)();

    // Opens <OutDir>\asteroids.jsonl and <OutDir>\dock.jsonl and installs the docking-point
    // hooks (GOracleSimHooks.OnDockAnim, OnDockActivate). SystemId is the scenario's system,
    // whose asteroid fields (CmnAsteroid::Find) the tick records walk; Tick points at the
    // run's tick counter; Ships gives the positions the `probe` records test the fields at.
    // Returns false (and records nothing) when a file cannot be written.
    bool OracleSimFieldsStart(std::string const& OutDir, unsigned int SystemId,
                              unsigned int const* Tick, FSimFieldNameFn NameOf,
                              FSimShipPositionsFn Ships);

    // The asteroid records of the tick that just ran: every cube made since the last call,
    // with its rocks, and each field's cube states when they changed; at ticks 0, 1 and every
    // 150th a `probe` of the fields (their parameters, and near_field at each ship).
    void OracleSimFieldsTick();
}
