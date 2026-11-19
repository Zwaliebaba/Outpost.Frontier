# 0003 — Replay and scenario oracle; G2 envelope method

Status: accepted by the owner on 2026-11-17 (P2.0); decision O23; implements decision O20. Revised 2026-11-19: points 9 and 10 replaced by the seed ensemble (owner, 2026-11-19); the 5 % bound on `ram_ship`'s first-contact damage and the outcome bounds of point 10 (median margin 0.44 × IQR) are approved by the owner.

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
determinism check: its spread is expected to stay 0 (points 11 to 13 made
the oracle repeat itself across processes and machines).

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
10. **Bounds (approved by the owner on 2026-11-19).** For each fight,
    object and metric, Rust's values over the seeds against the oracle's
    (the owner kept these tests with 60 seeds and set the median margin to
    the 1 % level for 60 seeds, 2026-11-19):
    - survival count: Fisher's exact test, two-sided, p ≥ 0.01;
    - kill tick, hits, collision events, hull, shield: the exact two-sample
      Kolmogorov–Smirnov test p ≥ 0.01, and |median(Rust) −
      median(oracle)| ≤ max(floor, 0.44 × the oracle's interquartile
      range), with the floors 30 ticks (1 s), 2 hits, 2 collision events
      (one contact), 50 hull points and 50 shield points (the floors apply
      where the oracle does not vary).

    Why these numbers. Both sides are reproducible (the Rust world on every
    machine; the oracle since points 11 to 13), so a verdict does not
    change from job to job; the bounds say how large a difference the
    seeds can show beyond chance, the seeds being a sample of the
    starting conditions. One level, 1 %, is used throughout. For the
    medians, the standard error of the difference of two medians of n is
    about 1.31 × IQR / √n (normal approximation: 0.30 × IQR for 20, 0.17 ×
    IQR for 60), so 0.75 × IQR is about 2.5 standard errors for 20 seeds
    (the 1 % level) and about 4.4 for 60. The owner therefore set the
    margin to 0.44 × IQR, about 2.5 standard errors at 60 seeds, so the
    median test keeps the 1 % level (2026-11-19). The levels were set before
    the comparison: the first job ran with 1 % and 1 × IQR (commit
    e82c02ea); the median margin was then tightened to 0.75 for 20 seeds
    and to 0.44 for 60, never widened. With 60 seeds the survival test accepts,
    for the oracle's measured counts, Rust counts of 53–60 (oracle 60 of
    60), 51–60 (59), 33–57 (47), 18–46 (32) and 4–29 (15). A fixed ±k would
    be tighter where the rate is near one half, at the cost of rejecting an
    identical distribution more often (±4 of 20: about 15 % of the time at
    a rate of one half); the owner may prefer it. The owner approved the
    bounds on 2026-11-19 (`ENVELOPES_APPROVED` in `scenario_replay`'s
    ensemble module), so a metric outside them fails the data job. The 5 % ram bound, the exact span,
    the same tick (`judge_fight` requires it for the fights since 2026-11-19)
    and `ram_ship`'s alive outcome fail now, per seed.

    **Measured; bounds approved by the owner on 2026-11-19** (data job run
    44566957771 at 6fd6ebf2: 60 seeds, the oracle with points 11 to 13 and
    the fights with `comms off`, the port with the IVP ray rules of D91 as
    amended on 2026-11-19). Every seed of every scenario equals the oracle
    bit for bit up to its first contact or impact (360 of 360), and the
    first contact or impact comes in the oracle's tick in every seed:
    `fly_to_point` none, `fire_at_target` tick 25, `ram_ship` 149,
    `dogfight` and `skirmish` 165–247 (median 193; the two share their
    seeds' first impacts), `strafe` 371–946 (median 417). `fire_at_target`
    and `ram_ship` draw no `rand()` after the seeding point, so their 60
    runs are identical on both sides; `fly_to_point` and the fights leave
    the first seed's run within the first 22 ticks (two strafe seeds at 403
    and 404) and
    stay exact. `ram_ship`'s first-contact damage: Rust 31.77 against 30.88
    in every seed, largest relative error 2.888 % (bound 5 %); both ships
    alive at the end in every seed on both sides. The pre-contact span and
    every first contact equal those of the job before (run 44560359142 at
    933b4a63, before points 11 to 13); the fights' outcomes after the first
    contact changed, as expected with `comms off`, and the Rust world's
    outcomes are the same in both jobs (0 of 660 changed). The fights,
    oracle against Rust, median [quartiles] (the number of kills where not
    all 60), the median difference against its margin (0.44 × IQR or the floor), and the KS
    statistic and p-value:

    | Scenario | Object | Metric | Oracle | Rust | Δ median / margin | KS D / p |
    | --- | --- | --- | --- | --- | --- | --- |
    | dogfight | police | survived | 59 of 60 | 60 of 60 | | Fisher 1.000 |
    | dogfight | police | hits | 35 [27.8, 43.2] | 35 [27.8, 44.2] | 0 / 6.8 | 0.05 / 1.000 |
    | dogfight | police | collisions | 1 [0.8, 1] | 1 [0, 1] | 0 / 2 | 0.07 / 1.000 |
    | dogfight | police | hull | 1300 [1300, 1300] | 1300 [1300, 1300] | 0 / 50 | 0.07 / 1.000 |
    | dogfight | police | shield | 291.2 [172.6, 428.6] | 362.8 [252.5, 509.1] | 71.6 / 112.6 | 0.23 / 0.076 |
    | dogfight | rogue | survived | 47 of 60 | 47 of 60 | | Fisher 1.000 |
    | dogfight | rogue | kill tick | 1484 [1356, 1690] (13) | 1484 [1303, 1615] (13) | 0 / 147 | 0.23 / 0.898 |
    | dogfight | rogue | hits | 118.5 [104.5, 135.2] | 121 [102.8, 137.2] | 2.5 / 13.5 | 0.10 / 0.928 |
    | dogfight | rogue | collisions | 1 [0, 1] | 1 [0, 1] | 0 / 2 | 0.02 / 1.000 |
    | dogfight | rogue | hull | 543.4 [80.8, 1024.7] | 584.2 [180.8, 1063.6] | 40.8 / 415.3 | 0.05 / 1.000 |
    | dogfight | rogue | shield | 119.5 [12.9, 127.6] | 119.8 [34, 129.1] | 0.3 / 50.5 | 0.13 / 0.665 |
    | skirmish | police | survived | 60 of 60 | 60 of 60 | | Fisher 1.000 |
    | skirmish | police | hits | 25 [16.8, 28] | 25.5 [18.8, 31] | 0.5 / 4.9 | 0.08 / 0.987 |
    | skirmish | police | collisions | 1 [0, 1] | 1 [0, 1] | 0 / 2 | 0.02 / 1.000 |
    | skirmish | police | hull | 1300 [1300, 1300] | 1300 [1300, 1300] | 0 / 50 | 0.02 / 1.000 |
    | skirmish | police | shield | 499.6 [367.3, 591.2] | 513.7 [412.3, 594.8] | 14 / 98.5 | 0.13 / 0.665 |
    | skirmish | rogue | survived | 15 of 60 | 16 of 60 | | Fisher 1.000 |
    | skirmish | rogue | kill tick | 1154 [916, 1267] (45) | 1162.5 [926.5, 1269] (44) | 8.5 / 154.4 | 0.06 / 1.000 |
    | skirmish | rogue | hits | 86 [68.5, 91.2] | 89.5 [79, 92] | 3.5 / 10 | 0.22 / 0.120 |
    | skirmish | rogue | collisions | 0 [0, 1] | 0 [0, 1] | 0 / 2 | 0.02 / 1.000 |
    | skirmish | rogue | hull | 0 [0, 3.1] | 0 [0, 27.7] | 0 / 50 | 0.08 / 0.987 |
    | skirmish | rogue | shield | 0 [0, 1.1] | 0 [0, 47.5] | 0 / 50 | 0.12 / 0.813 |
    | strafe | hauler | survived | 32 of 60 | 27 of 60 | | Fisher 0.465 |
    | strafe | hauler | kill tick | 1454.5 [1425.8, 1481.5] (28) | 1439 [1402, 1463] (33) | −15.5 / 30 | 0.17 / 0.678 |
    | strafe | hauler | hits | 118 [108.8, 128.5] | 111 [98, 124.2] | −7 / 8.7 | 0.25 / 0.047 |
    | strafe | hauler | collisions | 0 [0, 0] | 0 [0, 0] | 0 / 2 | 0.00 / 1.000 |
    | strafe | hauler | hull | 54 [0, 554] | 0 [0, 420.2] | −54 / 243.8 | 0.12 / 0.813 |
    | strafe | police | all | untouched in every seed on both sides | | | |

    Every fight metric is inside the approved bounds; the closest are
    `strafe`'s hauler hits (p = 0.047) and the dogfight police's shield
    (p = 0.076), and against the 0.44 × IQR margin `strafe`'s hauler hits
    (−7 against 8.7). Findings put to the owner:
    - Fixed on the way (2026-11-19): with 60 seeds four seeds of `dogfight`
      and `skirmish` (3893802793, 3175751809, 1295869954, 669242669) had
      the Rust world's first impact 1 to 10 ticks before the oracle's, a
      bullet passing through the rogue in the oracle; the port lacked IVP's
      OV sphere test of rays against every object (D91 amended, owner
      2026-11-19). `strafe`'s first impact 2 ticks late in 6 of 20 seeds
      was the phantom fuse's timing (D177 amended).
    - `strafe`'s hauler: its kill tick leaned early in Rust before the
      oracle ran with `comms off` (KS p = 0.007 in the 60-seed job at
      87d18a06, outside the bound). With the comm roll gone from the
      oracle's `rand()` sequence it is inside (p = 0.68, median −15.5
      ticks against a margin of 41.8); Rust kills it in 33 seeds and the
      oracle in 28 (Fisher p = 0.47), and with hits counted to the kill
      the hits are within the median margin (p = 0.047). Nothing is outside
      a bound; the hauler remains the closest case.
    - `ram_ship` after the first contact (reported, not bounded): the
      oracle's pilot touches the target again at ticks 156 and 157 (42.95
      and 48.33 m/s), so it records 6 collision events and each ship ends
      with shield 749.51; the Rust ships separate after the first contact
      (2 events, shield 794.26), in all 60 seeds. This is the contact
      solver (D87), and the outcome (both alive) agrees.

### Reproducibility (owner, 2026-11-19)

11. **The start-up seed** (a point-2 hook). `IServerImpl::Startup` seeded
    the generator with `timeGetTime()` before Content's controllers start;
    each `SetHeartbeatInterval` draws its heartbeat's starting phase from
    it, and GameSupport's 1 s heartbeat is Content's game time, which moves
    in whole heartbeats. Content's comm system (GCS) rolls `rand()` on a
    damage message once its category's interval (12 s by default) has
    passed since the last comm nearby (`IsGcsCommAllowed`), so the roll's
    tick depended on the wall clock, and every later draw of the AI with
    it: 5 of 120 G2 seed-ensemble runs ended differently in separate
    processes, always after the first contact (data runs 44494560931 and
    44500705507; probes 44523076483, 44526581221 and 44530545092 ruled out
    the machine, the CPU, the toolset and ASLR, and found the roll with a
    recorder of every frame-time draw). Under the harness's fixed clock
    the seed is the scenario's `seed`; the harness seeds again after
    `PlayerDB::init` (whose first start on a machine seeds from the clock
    once more). The heartbeat phases are recorded (`heartbeats`).
12. **Comms off in the fights.** The G2 fight scenarios (`dogfight`,
    `skirmish`, `strafe`) run with `comms off`: the harness sets Content's
    comm clamp, as a mission's `Act_GcsClamp` does, so `IsGcsCommAllowed`
    refuses before its roll. The Rust world has no comm system until P3.6;
    with comms on, the oracle's `rand()` sequence holds comm rolls the port
    never draws, and every draw after the first one compares unlike. This
    is harness configuration, not a divergence. `ram_ship` and
    `fire_at_target` keep comms on: their ships have no AI, so no draw
    follows a comm roll. The data job checks that a fight with comms on
    equals comms off before its first contact.
13. **The toolset.** The CI builds whose output is compared (the oracle
    and the resource DLLs in build.yml's oracle job, the oracle in
    data.yml) use one pinned MSVC toolset, `.github/msvc-toolset.txt`
    (v143 14.44.35207, the one toolset every image in use has); the plain
    reference build uses the image's default and logs it. The `env` record
    of every run names the compiler, the CPU, the OS build and the runtime
    DLLs.

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
