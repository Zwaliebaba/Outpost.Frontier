# 0003 — Replay and scenario oracle; G2 envelope method

Status: proposed, 2026-11-17 (P2.0). Direction chosen by the owner on
2026-11-17 (decision O23); implements decision O20. The record waits for
the owner's approval.

## Context

Decision O20 checks Phase 2 in two layers: replay tests on the oracle's
recorded per-tick state, then closed-loop scenarios within envelopes the
owner approves, never tighter than the oracle's own run-to-run spread.
Decision O19 transliterates free flight from IVP. Facts from the C++
(paths relative to `CppSource/`):

- **Headless start-up.** The dedicated server is the model: it calls
  `NeuronRuntime::Startup` with no window and `NEURONCOMPONENTSsrv.ini`
  (`OutpostServer/OutpostServer.cpp:523`), which loads readfile,
  sceneruntime, x86math, rendcomp, system, SceneScript, dx8renderer and
  shading, no sound and no Deformable2. Common dereferences
  `NeuronRuntime::Engine`, `Anim` and `Data` unchecked, so the harness must
  run `Startup` (the console oracle does not today). Data loads through
  `DataConfig::startup` (`Common/common_dataconfig.cpp:161-217`).
- **Server obstacles.** `Server_LoadClientDll` needs a client-interface
  DLL and calls it without a null test (`Server/IServerImpl.cpp:2785-2791`,
  `:249`); `pub::SpaceObj::Create` needs a controller (Content.dll, or one
  made through `pub::Controller::Create`, `Server/server_pub.cpp:2495`,
  `:6315`); `Server_StepSystems` steps only systems with an active player
  (`IServerImpl.cpp:4081`); physics runs on the server only in multiplayer
  mode (`GbSinglePlayer` defaults to true, `Common/common_misc.cpp:902`).
- **Steps.** Each `IVP_Environment_SimulateDTime` call runs exactly one
  PSI, its length the step clamped to [0.005, 0.1] s
  (`IVP/ivp_physics/ivp_environment.cpp:1352-1359`, `:1444-1446`, `:1522`).
  So the PSI is not `1 / PHYSICAL_SIM_RATE`: in single player there is one
  PSI per frame, and in multiplayer one per 0.0999 s chunk. To be confirmed
  at run time (fact F16).
- **Free flight in one PSI** (`IVP_Environment_Simulate_Psi`,
  `ivp_environment.cpp:661-720`): core step and speed commit, the
  controllers (damping in the standard controller, `CEEngine`, `CSteering`,
  `CNudgeEngine`, `CStrafeEngine`, `CEThruster`, all through
  `push_core_linear` and `push_core_angular_ws`), then
  `calc_next_PSI_matrix` (spin clipping, anomaly limits, rotation and
  position; `ivp_calc_next_psi_solver.cpp:118-290`). It is separable from
  collision at function level.
- **AI** writes only through `IObjAffect` (`set_throttle`,
  `set_axis_throttle`, `set_nudge_vec`, `set_strafe_dir`, fire slots;
  `include/common_iface.h:649-689`) but reads the world around it
  (notification queue, state graph, targets, rays).
- **Damage.** `CalcCollisionDamage` is pure (`Common/common_physys.cpp:371-416`);
  weapon damage enters at `EqObj::munition_hit`
  (`Server/server_gameobject.cpp:2923-2985`).
- **Nondeterminism.** Uncontrollable: the pointer-keyed physics sync set
  and IVP's pointer-XOR pair hash. Controllable: `rand()` (one shared CRT),
  frame time, global time, x87 precision (IVP's point-soup builder sets
  64-bit and never restores it, `ivp_surface_builder_pointsoup.cpp:687`).
  Needs a hook: QPC reads in repair and damage timers
  (`server_gameobject.cpp:1014`, `:1408`, `:1588`).
- **Caveat.** The oracle is the reconstructed C++, not the original
  binary; some AI code is still marked unverified
  (`Common/common_behaviormanager.cpp:1517-1528`,
  `common_dockbehavior.cpp:1702`). Agreement with the oracle proves
  agreement with the reconstruction.

## Decision

### The harness

1. A new mode of the console oracle, `Oracle.exe sim <data root> <scenario>
   <out folder>`, starts like the dedicated server: `NeuronRuntime::Startup`
   with the server INI, `DataConfig::startup`, Server.dll and Content.dll
   as the controller, multiplayer mode set, and a stub client-interface DLL
   built with the oracle that accepts and logs every client call.
2. Oracle instrumentation (permitted by the Phase 0 rules) adds hooks, each
   inert unless the harness enables it: step systems without an active
   player; a fixed frame time; `srand` with a fixed seed after start-up;
   53-bit x87 precision set again after loading and before every tick; a
   fixed clock for the QPC reads in timers; and recorders (below). Each hook
   is a few lines; together about ten sites in Common, IVP and Server.
3. Scenarios are text files beside the harness: system, objects to spawn
   (archetype, loadout, position, orientation, velocity), commands at given
   ticks (directive, throttle, fire, dock request), the number of ticks, and
   the stepping mode (single-player single step or multiplayer
   accumulator). The first set: free flight at fixed throttle, turn to a
   heading, fly to a point, dock at a station, fire at a target, ram an
   asteroid.

### Recorded data

4. **Per tick:** each object's position, orientation, linear and angular
   velocity, hit points and AI state; events (collisions with damage,
   munition hits, docks, deaths).
5. **Per PSI (free flight):** for selected objects, the IVP core state in
   and out (`PosWorldFCore`, `QWorldFCore`, speed and rotation speed, their
   changes, inverse mass and inertia, damping, movement state, spin
   clipping, PSI time) and the controller inputs (throttle, axis throttle,
   nudge, strafe, cruise state, power, archetype values).
6. **Per call (replay):** the inputs and outputs of
   `CalcCollisionDamage`, of `munition_hit` (impact in, damage list out),
   and of each `UpdateBehaviorTick` (the ship's state, its directive and the
   commands it issued through `IObjAffect`).

### Tests on the Rust side

7. **Replay tests** (Tier B, decision O20): one PSI of free flight from the
   recorded core and controller state; damage functions from recorded
   inputs; AI command output from recorded inputs where the behaviour reads
   only the recorded state. Behaviours that read the wider world are
   replayed from a recorded world snapshot of the objects they query, added
   behaviour by behaviour in P2.5.
8. **Scenario tests** run the same scenario file through the Rust world
   and compare with the oracle within the envelopes.

### Envelope method (numbers later)

9. For each scenario and metric (position error over time, orientation
   error, time to an event, damage dealt, end state), the oracle runs the
   scenario N = 20 times in fresh processes. The measured spread is the
   largest difference between any two runs. The bound is
   `max(k × spread, floor)` with k = 2, and a floor per metric (for example
   0.01 m position, 1 tick event time, 0 for discrete outcomes such as
   "docked"). A metric whose spread grows without bound over the run (chaotic
   contact) is compared only up to the first divergence point the oracle
   itself shows, and after that by outcome.
10. The numbers are filled in from harness runs in a later revision of this
    record and approved by the owner before Gate G2 is judged.

## Consequences

- The per-PSI free-flight records (point 5) have a fixed layout, written
  down in [sim-records.md](../sim-records.md) by P2.3: one `psi.jsonl` per
  scenario, one line per core per PSI with the core state in and out
  (field names, `IVP_Core` offsets, units, floats as hex bit patterns), the
  movement-check flag and the pushes of the force controllers in call
  order. The controller inputs travel in an optional `controllers` field
  that P2.4 defines. `neuron-core`'s `sim_replay` test reads them.
- The harness reuses the real server object wrappers and Content's
  controller, so the oracle exercises the reference code paths; the cost is
  the stub client DLL and the hooks.
- The single-player and multiplayer stepping modes are both covered, which
  O19's free flight must reproduce.
- Fact F16 (one PSI per step) and fact F14 (one shared world) can be
  confirmed by the first harness runs.
- About 2,500 to 4,000 lines of C++ in `CppSource/Oracle/` and the hooks;
  built in CI by the Reference build and the oracle job, run in CI on the
  original data like the data dumps.
- Risk: the server INI loads the Direct3D 8 renderer DLL; no device is made
  in headless use, but CI must confirm it loads on the runner.

## Options rejected

- **A driver of its own over Common** (own object database and `IObjRW`
  wrappers, no Server.dll). Fewer hooks in Server, but 115 inspection slots
  and the wrapper logic rewritten in the harness, which would then not be
  the reference.
- **Waiting for measurements before deciding the envelope method.** P2.0
  would stay open until the harness runs; the method does not depend on the
  numbers.
