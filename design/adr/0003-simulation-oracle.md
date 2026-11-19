# 0003 — Replay and scenario oracle; G2 envelope method

Status: accepted by the owner on 2026-11-17 (P2.0); decision O23; implements decision O20. Revised 2026-11-19: points 9 and 10 replaced by the seed ensemble (owner, 2026-11-19); the 5 % bound on `ram_ship`'s first-contact damage is approved, the outcome bounds of point 10 are proposed and await the owner.

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

### Envelope method (revision of 2026-11-19)

Points 9 and 10 replace the first method (N = 20 runs of one scenario,
bound `max(2 × spread, floor)`). The harness fixes the seed, the clock and
the frame time, and the runs turned out identical: `--runs 3` measured a
spread of 0 in all 130 metric lines and 23 events of the 17 scenarios
(data job of 2026-11-19), so that method collapsed to its floors and said
nothing after a contact, where the differences are the Rust port's against
the C++ (the contact solver, D87 and O19; the fuse's timing, D177). The
owner chose a seed ensemble instead (2026-11-19). `--runs` stays as a
determinism check of one machine: its spread is expected to stay 0. Between
machines the oracle is not quite reproducible after a contact (point 10).

9. **The seed ensemble.** Each Gate G2 scenario (`fly_to_point`,
   `fire_at_target`, `ram_ship`, `dogfight`, `skirmish`, `strafe`) runs
   once per seed of a fixed, committed list of 60 on both sides (20 in the
   first revision; 60 by the owner's decision of 2026-11-19)
   (`crates/game-sim/tests/fixtures/g2_ensemble.txt`: the default
   0x4f524143, then 0x4f524143 + k × 0x9e3779b9 mod 2³² for k = 1..59, so
   that no two seeds are close; the CRT's first draw is nearly linear in
   the seed). The seed replaces the one of the `srand` at the seeding point,
   after the system and Content's population have loaded (`Oracle.exe sim
   ... --seed S`, recorded as `reseed`; the Rust world's re-seeding point);
   the load-time draws keep the scenario's seed. Every seed's run is judged
   as the single run was:
   - bit for bit up to its first contact or munition impact (the first
     collision event, munition hit or spent guided munition): a difference
     before it is a gross divergence, and in every contact scenario the
     first contact or impact must come in the same tick on both sides;
   - `ram_ship`: the first contact's damage within **5 %** relative of the
     oracle's on each side (owner, 2026-11-19); after it only the outcome
     (which objects are alive);
   - the scenarios without contact (`fly_to_point`; `fire_at_target`, whose
     ships do not touch) keep the strict check: bit for bit to the first
     impact (the whole run without one), the impact in the same tick, the
     final position within the gross bound and the alive states equal.

   After the first contact the fights are compared by outcome statistics
   over the seeds, per object: the survival count, the kill tick, the
   munition hits taken and the collision events naming it up to the kill,
   the final hull and the final shield. A kill is the first tick the hull
   is 0 or less (owner, 2026-11-19), not the tick the object is removed:
   the server's death fuse keeps a ship at hull 0 in space for 75 ticks in
   the C++ (D179), where it can still be hit; a survivor is never killed.
10. **Bounds (proposed 2026-11-19, awaiting the owner).** For each fight,
    object and metric, Rust's values over the seeds against the oracle's
    (the owner kept these tests and levels with 60 seeds, 2026-11-19):
    - survival count: Fisher's exact test, two-sided, p ≥ 0.01;
    - kill tick, hits, collision events, hull, shield: the exact two-sample
      Kolmogorov–Smirnov test p ≥ 0.01, and |median(Rust) −
      median(oracle)| ≤ max(floor, 0.75 × the oracle's interquartile
      range), with the floors 30 ticks (1 s), 2 hits, 2 collision events
      (one contact), 50 hull points and 50 shield points (the floors apply
      where the oracle does not vary).

    Why these numbers. The Rust side is reproducible: the same seeds give
    the same runs on every machine (two data jobs, 0 of 220 per-object
    outcomes changed). The oracle is reproducible within one job but not
    quite between jobs: of its 120 runs, 5 ended differently in the second
    job (the dogfight with seeds 3358599207 and 464813110, the skirmish
    with 1478717352 and 225462782, the strafe with 2731971922, whose hauler
    survived in one job and died at tick 1485 in the other), each after
    its first contact, with the same first contact and the same exact span.
    This is the nondeterminism listed above (the pointer-keyed sync set and
    the pointer-XOR pair hash): addresses repeat between the processes of
    one machine, which is why `--runs 3` shows a spread of 0, and differ
    between machines. So the oracle's distributions move a little from job
    to job (the dogfight police's median shield 362.1 in one, 372.8 in the
    other), and the bounds are tests of a sample, not exact checks. One
    level, 1 %, is used throughout. For the medians, the standard error of
    the difference of two medians of 20 is about 0.30 × IQR (normal
    approximation), so 0.75 × IQR is about 2.5 standard errors, the same
    1 % level. The levels were set before the comparison: the first job
    ran with 1 % and 1 × IQR (commit e82c02ea); the median margin was then
    tightened to the 1 % level, not widened. With n = 20 the survival test
    is weak: for the oracle's measured counts it accepts Rust counts of
    14–20 (oracle 20 of 20), 8–20 (16), 1–16 (8), 2–17 (9) and 0–15 (6). A
    fixed ±4 would be tighter but would reject an identical distribution
    about 15 % of the time at a survival rate of one half; the owner may
    prefer it. The bounds are reported, not failed, until the owner
    approves them (`ENVELOPES_APPROVED` in `scenario_replay`'s ensemble
    module). The 5 % ram bound, the exact span, the same tick and
    `ram_ship`'s alive outcome fail now (per seed), the same tick of a
    fight once `judge_fight` requires it (the change that comes with the
    fix of `strafe`'s late first impact; until then a fight's later Rust
    first impact is reported).

    **Measured** (data job runs 44494560931 at e82c02ea and 44500705507 at
    5a99cf3e; the table is the second, with the bounds above). Every seed of
    every scenario equals the oracle bit for bit up to its first contact or
    impact (120 of 120, both jobs). The first contact or impact comes in
    the oracle's tick in every seed of `fly_to_point` (none),
    `fire_at_target` (tick 25), `ram_ship` (149), `dogfight` and
    `skirmish` (178–224, median 194), and in 14 of 20 seeds of `strafe`; in
    the other 6 the Rust world's comes 2 ticks later (seeds 1330790723,
    77536153, 3745876164, 3119248879, 2492621594, 1865994309).
    `fire_at_target` and `ram_ship` draw no `rand()` after the seeding
    point, so their 20 runs are identical on both sides; `fly_to_point` and
    the fights leave the first seed's run between ticks 1 and 22 (strafe
    once at 403) and stay exact. `ram_ship`'s first-contact damage: Rust
    31.77 against 30.88 in every seed, largest relative error 2.888 %; both
    ships alive at the end in every seed on both sides. The fights, oracle
    against Rust, median [quartiles] (the number of values when it is not
    20), the median difference against its margin, and the KS statistic and
    p-value:

    | Scenario | Object | Metric | Oracle | Rust | Δ median / margin | KS D / p |
    | --- | --- | --- | --- | --- | --- | --- |
    | dogfight | police | survived | 20 of 20 | 20 of 20 | | Fisher 1.000 |
    | dogfight | police | hits | 31 [26.5, 37.2] | 31.5 [22, 38.5] | 0.5 / 8.1 | 0.15 / 0.983 |
    | dogfight | police | collisions | 0.5 [0, 1] | 1 [0, 1] | 0.5 / 2 | 0.10 / 1.000 |
    | dogfight | police | hull | 1300 [1300, 1300] | 1300 [1300, 1300] | 0 / 50 | 0.10 / 1.000 |
    | dogfight | police | shield | 372.8 [282.8, 488.1] | 438.6 [317.1, 552.9] | 65.8 / 154 | 0.25 / 0.571 |
    | dogfight | rogue | survived | 16 of 20 | 15 of 20 | | Fisher 1.000 |
    | dogfight | rogue | kill tick | 1593 [1427.2, 1614.5] (4) | 1574 [1019, 1714] (5) | −19 / 140.4 | 0.40 / 0.746 |
    | dogfight | rogue | hits | 110.5 [102.2, 124.8] | 112 [101.8, 130.2] | 1.5 / 16.9 | 0.15 / 0.983 |
    | dogfight | rogue | collisions | 1 [1, 1] | 1 [1, 1] | 0 / 2 | 0.10 / 1.000 |
    | dogfight | rogue | hull | 682 [38.5, 938] | 421 [32.7, 1043.2] | −261 / 674.7 | 0.15 / 0.983 |
    | dogfight | rogue | shield | 123.4 [27.7, 129.3] | 122.9 [4.2, 129] | −0.5 / 76.2 | 0.20 / 0.832 |
    | skirmish | police | survived | 20 of 20 | 20 of 20 | | Fisher 1.000 |
    | skirmish | police | hits | 20 [12.8, 30.8] | 24.5 [17.8, 28.2] | 4.5 / 13.5 | 0.20 / 0.832 |
    | skirmish | police | collisions | 0 [0, 1] | 0.5 [0, 1] | 0.5 / 2 | 0.15 / 0.983 |
    | skirmish | police | hull | 1300 [1300, 1300] | 1300 [1300, 1300] | 0 / 50 | 0.05 / 1.000 |
    | skirmish | police | shield | 591.8 [403.7, 704.5] | 538.8 [400.9, 607.8] | −53 / 225.6 | 0.25 / 0.571 |
    | skirmish | rogue | survived | 6 of 20 | 7 of 20 | | Fisher 1.000 |
    | skirmish | rogue | kill tick | 910 [813.5, 1182.5] (14) | 1081 [939, 1173] (13) | 171 / 276.8 | 0.42 / 0.128 |
    | skirmish | rogue | hits | 76.5 [64.8, 90] | 81 [73, 91.2] | 4.5 / 18.9 | 0.25 / 0.571 |
    | skirmish | rogue | collisions | 1 [0, 1] | 0.5 [0, 1] | −0.5 / 2 | 0.10 / 1.000 |
    | skirmish | rogue | hull | 0 [0, 48.1] | 0 [0, 201.1] | 0 / 50 | 0.25 / 0.571 |
    | skirmish | rogue | shield | 0 [0, 32.6] | 0 [0, 121.7] | 0 / 50 | 0.20 / 0.832 |
    | strafe | hauler | survived | 8 of 20 | 6 of 20 | | Fisher 0.741 |
    | strafe | hauler | kill tick | 1466.5 [1425.5, 1480.8] (12) | 1438 [1147.8, 1456.5] (14) | −28.5 / 41.4 | 0.44 / 0.116 |
    | strafe | hauler | hits | 130 [114.8, 151] | 111 [93.2, 116] | −19 / 27.2 | 0.50 / 0.012 |
    | strafe | hauler | collisions | 0 [0, 0] | 0 [0, 0] | 0 / 2 | 0.00 / 1.000 |
    | strafe | hauler | hull | 0 [0, 355.8] | 0 [0, 103.2] | 0 / 266.8 | 0.15 / 0.983 |
    | strafe | police | all | untouched in every seed on both sides | | | |

    Every fight metric is inside the proposed bounds in both jobs; the
    closest is `strafe`'s hauler hits (p = 0.012 in both). Two systematic
    differences are put to the owner as findings, not absorbed by the
    bounds:
    - `strafe`: the 2-tick-late first impact in 6 seeds, and a consistent
      lean in the hauler's outcome: Rust kills it earlier (lower quartile
      of the kill tick 1148 against 1420–1426, earliest 1010 against 1387)
      after fewer hits (6 of Rust's 14 kills come at exactly 111 hits; the
      oracle's kills take 102–163), and it survives in 6 seeds against the
      oracle's 8–9. Each metric alone is inside the bounds; together they
      point at more hull damage per hit in Rust or a different damage path
      for the transport, to be explained with the fix of the late first
      impact.
    - `ram_ship` after the first contact (reported, not bounded): the
      oracle's pilot touches the target again at ticks 156 and 157 (42.95
      and 48.33 m/s), so it records 6 collision events and each ship ends
      with shield 749.51; the Rust ships separate after the first contact
      (2 events, shield 794.26), in all 20 seeds. This is the contact
      solver (D87), and the outcome (both alive) agrees.

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
