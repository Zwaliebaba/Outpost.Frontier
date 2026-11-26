# Simulation oracle records

The files `Oracle.exe sim` writes (work package P2.3, decision O23,
[adr/0003](adr/0003-simulation-oracle.md); how to run it in
[reference-build.md](reference-build.md) section 10). The Rust replay and
scenario tests read them. The records are made from the original game data
and are never committed. Record format version 2 (the `version` of the
`start` record; version 2 is the single `psi` record per core and PSI
below).

## Encoding

Every `.jsonl` file holds one JSON object per line. The writer is the P0.5
one (`CppSource/OutpostFrontier/Client/oracle_json.h`):

| Kind | Written as |
| --- | --- |
| `u`, `i` | a JSON number: unsigned or signed 32-bit decimal |
| `b` | `true` or `false` |
| `f` | a JSON string of the `f32` bit pattern, 8 lower-case hex digits (`"3f800000"` is 1.0) |
| `d` | a JSON string of the `f64` bit pattern, 16 lower-case hex digits |
| `s` | a JSON string; bytes from 0x80 as `\u00XX`, `"` and `\` escaped |
| `f2`, `f3`, `f4`, `f9`, `f[]`, `f[12]` | an array of `f` (2, 3, 4, 9, any number or 12 elements) |
| `u3` | an array of three `u` |
| `{…}` | a nested object, its fields in the order listed |
| `x\|null` | the value, or `null` |

Fields appear in the order the tables list them; optional fields (in
brackets) are left out, not written as `null`. Every record but those of
`iclient.jsonl` starts with `rec`, its kind.

Conventions:

- **Ticks.** A run has `ticks` frames, numbered from 1. Tick 0 is the state
  after the objects are spawned, before any frame. The scenario's commands
  for tick *t* run before frame *t*; every PSI, call and event recorded
  with `tick` *t* happened during frame *t*; the `tick.jsonl` records of
  tick *t* describe the state after it.
- **Objects.** `obj` is the scenario's name for an object, `null` for any
  other object (a munition, a station that the scenario does not name).
  Ids are the game's object ids (`u`).
- **Orientation.** `orient` is a `Matrix` as `E[0][0], E[0][1], E[0][2],
  E[1][0], …, E[2][2]` (`f9`). A ship faces −Z: forward is minus the third
  column.
- **Quaternions.** `q` is IVP's `x, y, z, w`.

## `session.jsonl`

| `rec` | Fields |
| --- | --- |
| `start` | `version` u, `scenario` s (path), `data_root` s, `system` s, `system_id` u, `ticks` u, `frame` d (seconds), `stepping` s (`sp` or `mp`), `collisions` s (`sp` or `mp`; since the load-draw work, after `stepping`), `comms` s (`on` or `off`, the scenario's `comms`; since 2026-11-19), `seed` u (the scenario's: the first `srand`, before the load), `reseed` u (the seed of the seeding point: `--seed`'s, else `seed`; since the G2 seed ensemble, 2026-11-19), `clock` d (the fixed clock at tick 0), `objects` u, `commands` u |
| `server` | `data_path` s, `controller` u (SimClientStub's controller id), `system_id` u, `system_loaded` b, `reseed` u (the seed of the `srand` the harness calls again after the load, before the objects are spawned: the seeding point the Rust world matches, `--seed`'s when given; reference-build.md section 10, start-up step 5), `startup_seed` u (the seed `IServerImpl::Startup`'s `srand` took instead of `timeGetTime()`: the scenario's `seed`; since 2026-11-19), with `comms off` also `comms_clamped` b (Content's comm clamp was set) |
| `heartbeats` | `tick` u (0), `controllers` [{`flid` u, `priority` u, `module` s (the controller's DLL), `interval` f (s, −1 for none), `phase` f (the accumulator frame 1 starts from), `deferred` b}, …] in the server's list order: the heartbeat of every controller (`OracleSimControllerHeartbeats`; Content's GameSupport heartbeat is its game time), written before tick 0 (since 2026-11-19) |
| `spawn` | `obj` s, `kind` s (`ship` or `solar`); a ship: `arch` s, `arch_id` u, `loadout` s, `loadout_id` u, `result` i (`pub::SpaceObj::Create`), `id` u, [`state_graph` s, `state_graph_id` i, [`pilot` s, `pilot_found` b (the `pilot` option; its personality was submitted)], `submit_state` i]; with `faction`, before `arch`: `faction` s, `faction_result` i (`GetReputationGroup`), then when the group is known `faction_id` u, `vibe` i (the ship's `VibeGroupId`), `vibe_alloc` i, `vibe_affiliation` i (`pub::Reputation::Alloc`, `SetAffiliation`); a solar: `nickname` s, `id` u (`CreateID(nickname)`), `found` b; then (P2.5, the transport's collision geometry) a created ship's `part_map` [[`part` u, `instance` i], …] (`CObject::PartMap`) and `attached` [{`id` u, `connected` b, `kind` s (`non_physical`, `physical` or `none`), with `non_physical` also `hp_crc` u, `collisions` b}, …] (every `CAttachedEquip` and its attachment) |
| `player` | (P3.3, a scenario's `player` line; after the `spawn` records) `tick` u (0), `line` i, `session` u, `obj` s (the ship), `id` u (its id, 0 when it was not spawned), `joined` b (`OracleSimAddPlayer` answered true) |
| `command` | `tick` u, `line` i (in the scenario file), `obj` s, `cmd` s (the command as written), `ok` b, `result` i (what the called function returned; for a directive the `OP_RTYPE`), [`note` s, why it did not run] |
| `error`, `critical_warning`, `note` | `tick` u, `text` s (`note`: an object marked `psi` whose PSIs are not recorded, and why; once per object) |
| `fault` | `tick` u, `first_chance` b (from the vectored handler; false: the frame's `__except` filter, which ends the run with code 4), `code` s (hex), `code_name` s, `address` s (hex), `at` {`module` s\|null, `offset` s (hex, from the module base), `functions` s[] (the innermost inlined function first), `line` s (`file:line` under `CppSource/`, `""` when unknown)}, for an access violation `access` s (`read`, `write`, `execute`), `target` s (hex), `target_memory` s (`free`, `reserved`, `committed`, `committed, not readable`); `symbols` b (dbghelp started), `regs` {`eax` .. `ebp`: {`v` s, [`in` s (module+offset and symbol)], `words` s[4] and [`word0_in` s] when it points at readable memory (a vtable names the class), else `memory` s}, `esp` s, `eip` s, `eflags` s}, `esp_words` s[16], `stack` [{`module`, `offset`, `functions`, `line`}] (at most 48 frames; frame 0 is the faulting instruction, the others are resolved at their call) |
| `env` | (cross-runner reproducibility, owner 2026-11-19; just before `end`, after everything the run computes) `msc_full_ver` u, `msc_build` u (`_MSC_FULL_VER`, `_MSC_BUILD` of the compiler that built Oracle.exe; the solution builds every DLL with it), `cpu` {`vendor` s, `brand` s, `signature` s (CPUID leaf 1 EAX), `leaf1_ecx` s, `leaf1_edx` s, `leaf7_ebx` s (hex feature words), `logical` u}, `os` {`build` s, `ubr` u, `display_version` s}, `process_heap` s (hex), `stack_local` s (the address of a local of the writer, hex), `modules` [{`name` s, `loaded` b, [`path` s, `base` s (hex), `version` s (the file version)]}, …] (Oracle.exe, Server.dll, Common.dll, Content.dll, SimClientStub.dll and the CRT and system DLLs) |
| `content_tables` | (`dump content_tables`, P3.4b; P3.4c adds twelve tables) `lines` i: the lines `OracleDumpContentTables` wrote, −1 when a file could not be opened or Content.dll lacks the export |
| `content_systems` | (`dump content_systems`, P3.4d; after `system_loads`) `lines` i: the lines `OracleDumpContentSystems` wrote, −1 when a file could not be opened or Content.dll lacks the export |
| `mission_scripts` | (`dump mission_scripts`, P3.5a) `lines` i: the lines `OracleDumpMissionScripts` wrote, −1 when a file could not be opened, a mission was not made or Content.dll lacks the export |
| `mission_saves`, `mission_save_loads` | (`dump mission_saves` and `dump mission_save_loads`, P3.5b) `files` i: the files `OracleDumpMissionSaves` or `OracleLoadMissionSaves` wrote, −1 when a file could not be opened (for the loads, a port save in the out folder's `rust` folder is missing), a mission or the Story was not made or Content.dll lacks the export |
| `random_missions` | (`dump random_missions`, P3.7a) `lines` i: the lines `OracleDumpRandomMissions` wrote, −1 when a file could not be opened, the generator or a random-mission loader is missing or Content.dll lacks the export |
| `end` | `tick` u, `psi_total` u, `code` i (the exit code) |

## `tick.jsonl`

| `rec` | Fields |
| --- | --- |
| `frame` | `tick` u, `clock` d, `x87_cw` s, `sse_cw` s (`%08x` of `__control87_2`'s two words at the start of the frame, before the harness sets 53-bit precision again; at tick 0 after spawning), `psi_count` u (PSIs in the frame), `psi_dt` f[] (each PSI's `delta_time`); then (P2.5) `psi_mcheck` i[] (the environment's movement-check countdown `next_movement_check` at the start of each PSI) and `psi_ivp_seed` u[] (`IVP_FastRandomFloat`'s process-wide seed then): the load steps the environment and draws from the seed before tick 1, and the movement check decides which unit classifies its motion |
| `object` | `tick` u, `obj` s, `id` u, `alive` b; when alive: `pos` f3, `orient` f9, `vel` f3, `angvel` f3, `hp` f, `hp_max` f (`IObjInspect` of the server object: `get_position`, `get_orientation`, `get_velocity`, `get_angular_velocity`, `get_status`); then (P2.4, the scenario report) `power` f, `power_max` f (`CEqObj::Power`, `MaxPower`) and `shield` {`id` u, `hp` f, `hp_max` f, `online` b (`bOnline`), `functioning` b}\|null (the first `CEShield`: `GetHitPoints`, `GetMaxHitPoints`, `IsFunctioning`); a ship also: `throttle` f, `axis` f3, `nudge` f3, `strafe` i (`StrafeDir`), `cruise` b, `behavior` i (`CurrentBehaviorIndex`, −1 for none), `state_graph` i (−1 for none), `directive` u3 (`DirectiveStorage`), then (P2.5, the dogfight) `target` u\|null, `target_obj` (`IObjInspect::get_target`); then (P2.5, the strafe's bullets) `guns` [[`id` u, yaw current f, yaw desired f, pitch current f, pitch desired f, target point age f, yaw bound i, pitch bound i, lock target id u], …] (every `CEGun`'s turret joints, `TargetPointAge` and `GetTarget`) |

`alive` is false once the object is no longer in its system's object
database (destroyed, docked or removed).

Added by P2.5 (the guided-missile controller, owner 2026-11-18): one
`guided` record per tick, after the `object` records, for every guided
munition (`CObject` class 0xa7) whose simulation unit has run in a PSI
of the run, in id order: `tick` u, `id` u, `alive` b; when alive (found
by `CObject::Find(id, ClassGuided)`): `owner` u (`CProjectile::OwnerId`),
`pos` f3 (`WorldPosition`), `orient` f9 (`Orientation`), `vel` f3
(`get_velocity`), `angvel` f3 (`get_angular_velocity`, world space),
`lifetime` f, `seeker_elapsed` f, `target` u\|null and `target_obj`
(`get_target`), `sub_target` u. A munition that is gone gets one record
with `alive` false and no more. `scenario_replay` numbers them by id on
both sides (the launch order) and compares the launch tick, the last
tick alive and the trajectory bit for bit.

## `events.jsonl`

| `rec` | Fields | Source |
| --- | --- | --- |
| `collision` | `tick` u, `a` u, `a_obj` s\|null, `b` u, `b_obj` s\|null, `damage` f | `GameObject::on_collision` of object `a` (each side of a contact reports itself), with `Collision::CalcCollisionDamage`'s result |
| `munition_hit` | `tick` u, `target` u, `target_obj`, `shooter` u, `shooter_obj`, `munition` u (archetype id), `sub` u (sub-object hit) | `StarSystem::munition_collision` |
| `dock` | `tick` u, `ship` u, `ship_obj`, `dock` u, `dock_obj`, `system` u | the `DockComplete` controller message |
| `destroyed` | `tick` u, `id` u, `obj`, `killer` u, `killer_obj`, `kind` i (the destroy kind), `pos` f3 | the `ObjectDestroyed` controller message |
| `target` | `tick` u, `id` u, `obj`, `target` u\|null, `target_obj` | a scenario ship's `get_target` differs from the one of the previous tick's `tick.jsonl` record (P2.5) |

## `psi.jsonl`

The per-PSI free-flight records (record 0003 point 5) that
`crates/neuron-core/tests/sim_replay.rs` replays (point 7) through
`neuron_core::physics::replay`, whose `PsiRecord::to_line` writes the same
layout (without `ctl`) and whose unit tests round-trip it.

One `psi` record per core and PSI, in the order the PSIs run, for each
object marked `psi` in the scenario whose simulation unit ran in that PSI
(none while the unit is asleep). Since P2.5 also for every guided
munition whose unit (of one core) runs, with `obj` `guided`, its id and
no `ctl` (the motor's push is in `pushes`; the seeker's velocity changes
come between PSIs and are in `in`); `scenario_replay` compares their
`out` with the Rust missile's core after each tick. Not recorded, with a `note` in
`session.jsonl`: an object with a golem or remote-correction controller
(`CObject::GolemController`, +0x60, non-null; those beam their bodies and
are covered by scenario tests) and a core whose simulation unit holds more
than one core (the replay steps one core per unit).

### Recording points

All in `IVP_Environment_Simulate_Psi`
(`IVP/ivp_physics/ivp_environment.cpp:661`):

| Point in the C++ | Hook | Recorded |
| --- | --- | --- |
| before `simulate_sim_units_psi`, after the wake queue, the PSI listeners and the active values | `OnPsi` phase 0 | the PSI is counted (`psi`), the psi objects' cores are looked up |
| `IVP_Simulation_Unit_Process_One_Psi_Step` entry, before the core step (`ivp_simulation_unit.cpp`) | `OnSimUnitPsi` phase 0 | `dt`, `idt`, `time`, `limits`, `ctl` and `in` for each recorded core of the unit |
| every `IVP_Core::push_core_linear`, `push_core_angular_ws`, `push_core_angular_cs` into such a core while its unit runs (`ivp_core.cpp`) | `OnCorePush` | `pushes`, in call order |
| the end of the same `Process_One_Psi_Step` | `OnSimUnitPsi` phase 1 | `check`: the environment's movement-check countdown (`next_movement_check`) came to zero at this unit (`CooldownExpired`) |
| after `commit_all_calc_next_PSI_matrix`, before the hull managers and contacts | `OnPsi` phase 1 | `out`; the record is written |

`in` is taken at the unit's entry rather than before all units: the two
points see the same core state (another unit's controllers push only into
its own cores, and nothing else between the two points writes a core), but
the unit entry ties the record to the unit that steps the core, so a
sleeping core is not recorded, the push window and the movement check
belong to the same unit, and `in` is exactly what the Rust
`unit_psi_begin` starts from. `commit_all_calc_next_PSI_matrix` runs only
`calc_next_PSI_matrix` per core, so `out` is the state right after it.
Pushes outside the unit's run (between frames, from game code) are already
in `in` (`speed_change`, `rot_speed_change`) and are not listed.

### Fields

| Field | Kind | Source |
| --- | --- | --- |
| `rec` | s | `psi` |
| `tick` | u | |
| `psi` | u | PSI number in the run, from 1 |
| `obj`, `id` | s, u | |
| `dt` | f | `Environment->delta_time` (+0xac), the PSI length, s |
| `idt` | f | `Environment->inv_delta_time` (+0xb0), 1/s |
| `time` | f2 | `Environment->current_time` (base, offset), s |
| `limits` | f2 | `Environment->anomaly_limits`: `MaxVelocity` (m/s), `MaxAngularVelocityPerPsi` (rad) |
| [`ctl`] | {…} | a ship only: the controller inputs below, at the unit's entry |
| `in` | {core} | the core state at the unit's entry |
| `pushes` | [[s, f, f, f, f], …] | each push: the kind (`linear`, `angular_ws` or `angular_cs`, for `push_core_linear`, `push_core_angular_ws`, `push_core_angular_cs`), the direction or axis x, y, z (world space for the first two, core space for the third) and the magnitude argument |
| `check` | b | the movement check came to this unit in this PSI |
| `out` | {core} | the core state after `calc_next_PSI_matrix` |

A core state (`in` and `out`, the same fields). Offsets are `IVP_Core`'s
(`IVP/ivp_physics/ivp_core.h`); names in parentheses are the
reconstruction's where IVP's differ.

| Field | Kind | `IVP_Core` | Unit |
| --- | --- | --- | --- |
| `flags` | u | +0x00 `Flags` | — |
| `movement_state` | u | +0x60 `MovementState` | — |
| `unit_state` | u | `OwningSimUnit` (+0x15c) `->State`, the unit's flag word; 0 without a unit | — |
| `upper_limit_radius` | f | +0x04 `UpperLimitRadius` | m |
| `max_surface_deviation` | f | +0x08 (`SpeedLimit`) | m |
| `mass` | f | +0x20 `Mass` | kg |
| `inv_mass` | f | +0x40 `InvMass` | 1/kg |
| `inertia` | f3 | +0x14 `RotInertia` | kg m² |
| `inv_inertia` | f3 | +0x34 `InvRotInertia` | — |
| `rot_speed_damp` | f3 | +0x24 `RotSpeedDamp` | 1/s |
| `speed_damp` | f | +0x44 `SpeedDamp` | 1/s |
| `angular_limit` | f | +0x48 `DAngularLimitFactor` | 1/m |
| `spin_clip` | f3\|null | +0x4c `*SpinClipping`, `null` when the pointer is null | rad/s |
| `time_of_last_psi` | f2 | +0x68 `TimeOfLastPsi` (base, offset) | s |
| `i_delta_time` | f | +0x70 `IDeltaTime` | 1/s |
| `rot_speed_change` | f3 | +0x74 `RotSpeedChange` | rad/s, core space |
| `speed_change` | f3 | +0x84 `SpeedChange` | m/s, world |
| `rot_speed` | f3 | +0x94 `RotSpeed` | rad/s, core space |
| `speed` | f3 | +0xa4 `Speed` | m/s, world |
| `pos` | f3 | +0xb4 `PosWorldFCore` | m |
| `delta_world` | f3 | +0xc4 `DeltaWorldFCore` | m/s |
| `q` | f4 | +0xd4 `QWorldFCore` | — |
| `q_next` | f4 | +0xe4 `QWorldFCoreNext` | — |
| `m` | f[12] | +0xf4 `MWorldFCore`: rows 0, 1, 2 (three floats each, not the fourth), then `vv` | —, m |
| `rot_axis` | f3 | +0x134 rotation axis, world space (`RotAxisOS`) | — |
| `current_speed` | f | +0x144 (`Margin`) | m/s |
| `abs_omega` | f | +0x148 (`MaxSurfaceDeviation`) | rad/s |
| `surface_rot_speed` | f | +0x14c `max_surface_rot_speed` (`BoundingRadius`) | m/s |
| `time_of_last_motion` | f2 | +0x160 `TimeOfLastMotion` | s |
| `time_of_last_long_motion` | f2 | +0x168 `TimeOfLastLongMotion` | s |
| `q_last_motion` | f4 | +0x170 `QLastMotion` | — |
| `q_last_long_motion` | f4 | +0x180 `QLastLongMotion` | — |
| `pos_last_motion` | f3 | +0x190 `PosLastMotion` | m |
| `pos_last_long_motion` | f3 | +0x1a0 `PosLastLongMotion` | m |

`ctl` (the field record 0003 calls `controllers`; P2.4 replays it), from
the `CShip` and its equipment: `throttle` f, `axis` f3
(`Steering.AxisThrottle`), `steering_active` b, `nudge` f3
(`NudgeEngine.CurrentNudge`), `nudge_active` b, `strafe` i
(`StrafeEngine.CurrentStrafe`), `thrust_power` f, `max_thrust_power` f,
`angular_drag_factor` f, `engine` {…}\|null, `arch` {…}\|null.
`engine` (the first `CEEngine`): `cruise` b (`bCruiseActive`),
`cruise_charge_elapsed` f, `cruise_charge_rate` f, `last_wake_throttle` f,
and from `Archetype::Engine`: `max_force` f, `linear_drag` f, `power_usage`
f, `cruise_power_usage` f, `cruise_charge_time` f, `reverse_fraction` f.
`arch` (`Archetype::Ship`): `linear_drag` f, `angular_drag` f3,
`steering_torque` f3, `nudge_force` f, `strafe_force` f,
`strafe_power_usage` f. The P2.3 replay ignores it.

Added by P2.4 for game-sim's controller replay (after `arch`, so older
records stay readable): `power` f and `max_power` f (`CEqObj::Power`,
`MaxPower`; the engine draws from `power`), `orient` f9
(`CObject::Orientation`, the frame every controller pushes in), `zone_drag`
f\|null (the drag modifier of `ZoneAtA4`, which `get_linear_drag` uses while
cruising; `null` outside a zone), `steering_enabled` b, `nudge_enabled` b,
`strafe_enabled` b (`IsControllerEnabled` of the three ship controllers),
and `thrusters` [{…}, …], each `CEThruster` in traversal order (destroyed
ones too): `id` u, `functioning` b, `controller` b, `power_usage` f,
`max_force` f. The `engine` object adds, after `reverse_fraction`:
`active` b, `functioning` b (`CEEngine::IsFunctioning`), `controller` b.

Added by P2.5 (last): `order` s[], the unit's controller buckets in the
order the PSI runs them (`ControllerBuckets` from the last element; IVP
appends on registration, sorts by priority and, when the friction system
merges or splits units, rebuilds from the core's controller list walked
backwards): `ship`, `strafe`, `nudge`, `steering`, `engine`,
`thruster <id>`, or `other` (IVP's standard controller included).

`crates/game-sim/tests/controller_replay.rs` rebuilds the controllers from
`ctl` (the steering, nudge, strafe, thruster and engine controllers of
`game_sim::equip::flight`), runs them for the PSI in the recorded `order`
(without it IVP's order on registration: strafe, nudge, steering, the
thrusters from the last, the engine) and compares their pushes with
`pushes`: the same kinds, every float within 1e-6 relative to the larger
magnitude, in the same order. Records without the P2.4 fields use
enough power, the orientation of `in.m`, no zone, every controller
registered and no thrusters. Its summary is `rust-controller-replay.txt`.

### The replay

`neuron_core::physics::replay::replay` runs, on the `in` state: the core
step at `time`, the pushes in order, the standard controller (damping,
commit, zero gravity), the movement check when `check` is true, and
`calc_next_PSI_matrix` with `dt`, `idt` and `limits`. `flags`,
`movement_state`, `unit_state` and `spin_clip` must be equal; every other
float within 4e-6 relative to the larger magnitude (at least absolute).
The bound comes from the sines, cosines and arc cosines the port takes
from the Rust standard library (decision T11); it is tightened once the
first oracle runs show the actual spread. The test searches
`OUTPOST_ORACLE_DIR` recursively for `psi.jsonl` files and writes its
summary to `rust-sim-replay.txt` there.

## `calls.jsonl`

| `rec` | Fields |
| --- | --- |
| `collision_damage` | `tick` u, `source` u, `source_obj`, `source_sub` u, `target` u, `target_obj`, `target_sub` u, `own_vel` f3, `other_vel` f3, `speed` f, `other_mass` f, `own_mass` f, `damage` f. The call is `Collision::CalcCollisionDamage(speed, other_mass, own_mass)` (its parameters are named `DamageFactor, MaxSpeed, ActualSpeed`) and `damage` its result; `speed` is the relative speed from the two velocities |
| `munition_hit` | `tick` u, `target` u, `target_obj`, `shooter` u, `shooter_obj`, `munition` u, `sub` u, `hit_pos` f3, `damage` {`cause` i, `inflictor` u, `inflictor_owner` u, `destroyed` b, `count` i, `entries` [[`sub` u, `hp` f, `fate` i], …]}: the impact and the `DamageList` the target's `munition_hit` filled, before it is applied |
| `munition_hit` `state` (P2.4) | after `damage`: `hull_damage` f, `energy_damage` f, `weapon_type` u (the munition archetype); when the target is found (`CObject::Find` as a ship, else a solar): `hp` f, `hp_max` f, `power` f, `power_max` f, `owner` u, `armor` f\|null (the first `CEArmor`'s `hit_pts_scale`), `sub_kind` s (`group`, `equip`, `shield` or `root`); a group adds `sub_hp` f, `sub_hp_max` d, `sub_state` i, `sub_proxy` b (`IsRootHealthProxy`), `sub_linked_type` u; an item adds `sub_hp` f, `sub_hp_max` d, `sub_type` u, `sub_destroyed` b, and a shield `shield_offline_threshold` f and `shield_weapon_mod` d (`GetWeaponModifier` of the munition's weapon type). The target's state before the list is applied, which `crates/game-sim/tests/damage_replay.rs` rebuilds to replay `munition_hit` (summary `rust-damage-replay.txt`) |
| `behavior_in` | `tick` u, `seq` u (behaviour tick number, from 1), `id` u, `obj`, `dt` f, `behavior` i, `state_graph` i, `directive` u3, `suppressed` b (`TickSuppressedFlag`); when the owner is set: `pos` f3, `orient` f9, `vel` f3, `angvel` f3, `throttle` f, `axis` f3, `nudge` f3, `strafe` i, `cruise` b. Entry of `IBehaviorManager::UpdateBehaviorTick` |
| `behavior_out` | `tick` u, `seq` u, `id` u, `obj`, `behavior` i, `state_graph` i, `directive` u3. Every exit of the same call |
| `affect` | `tick` u, `beh` u (the `seq` of the behaviour tick in progress, 0 outside one), `src` s (`behavior`, `scenario` for the harness's own commands, `other`), `id` u, `obj`, `slot` i, `m` s, `i` i, `depth` i, `v` f[]\|null. One `IObjAffect` call on a ship: slot 0 `set_throttle` (`v` the throttle), 1 `set_axis_throttle` (`v` f3), 2 `set_nudge_vec` (`v` f3), 3 `set_strafe_dir` (`i` the `StrafeDir`), 13 `fire` (`EqObj::slot_34`: `i` the number of guns, `v` the target position or `null`), 16 `tractor` (P2.5 loot: `EqObj::slot_40`: `i` the tractor's equipment id, `v` [the target's id as the bits of the float, 0 for none]), 17 `gun_target` (P2.5 gun lock: `EqObj::slot_44`: `i` the equipment id, `v` [the target's id as the bits of the float, 0 for none; the sub-target]), 18 `set_target` (`i` the target's id, 0 for none; `v` [the sub-target]), 19 `activate_equip` (`EqObj::slot_4c`: `i` the equipment id, `v` [1 to activate, 0 to deactivate]), 20 `engage_cruise` (`Ship::slot_50`: `i` 1 to engage, 0 to leave; `v` [the second flag, 1 or 0]), 21 `set_thrusters` (`Ship::slot_54`: `i` 1 on, 0 off), and (P2.5 dock) 23 `request` (`Ship::slot_5c`: `i` the request type, 0 dock, 1 formation, 2 trade lane; `v` [the target's id, the second id, `bFlag`], each as the bits of a float), 24 `cancel_request` (`Ship::slot_60`: `i` the request type; `v` [the target's id, `Arg3`] as bits), 25 `go_tradelane` (`i` `bFastExit`; `v` [the entrance's id, the exit's id as bits, the speed]), 26 `stop_tradelane`. The answer the server gives during a request runs the requesting behaviour's `OnResult`, whose own calls are recorded at depth 0. `depth` (P2.5) is the number of these calls in progress around this one (`FOracleSimAffectScope`): 0 for a call the caller made, 1 or more for one an `IObjAffect` body makes in turn (`set_strafe_dir` and `slot_54` switch the engine on through `slot_4c`, `slot_50` switches the thrusters off through `slot_54`); records written before the field read as 0 |

The `affect` records between a `behavior_in` and its `behavior_out` with
the same `seq` are the commands that behaviour tick issued.

## `ai.jsonl`

The behaviour-tick recorder of work package P2.5 (record 0003 point 7):
for each `IBehaviorManager::UpdateBehaviorTick` the state of everything the
tick can read, so that a behaviour can be replayed in Rust against a world
snapshot instead of a live world. Written by
`CppSource/Oracle/oracle_sim_ai.cpp`, which `Oracle.exe sim` starts after
its own hooks (`OracleSimAiStart`); it chains itself in front of
`OnBehaviorTick`, so the `seq` of its records is the `seq` of the
`behavior_in` and `behavior_out` records of `calls.jsonl`, and the
`affect` records with that `beh` are the commands the tick issued. The
hooks it uses (`include/oracle_ai.h`) are inert outside the harness.

Per behaviour tick, in this order: `ai_in`; `personality` when the ship's
personality differs from the last one written for it (always on its first
tick); one `ai_obj` per object of the read set; then, while the tick runs,
each `ai_query` followed by an `ai_obj` for each object it found that has
no snapshot in this tick yet; finally `ai_out`. Objects are snapshotted
on entry: nothing moves during a behaviour tick (physics steps after it).

| `rec` | Fields |
| --- | --- |
| `ai_start` | `version` u (1), `near_radius` f (m), `rand_hooks` [s, …] (the DLLs whose `rand` import the recorder replaced: `Common.dll`, `Server.dll`, `Content.dll` when loaded) |
| `ai_in` | `tick` u, `seq` u, `id` u, `obj` s\|null, `dt` f, then the manager fields below, then `targets` [u, …] (the ids of the system manager's target list, `Sys->Timers.Targets`, built by the previous tick; read at the end of that tick, since an entry can name an object destroyed since), [`targets_gone` [u, …] (those of them no longer in the object database, or whose object is released: they are not snapshotted as `candidate` and not asked `can_point_at`)] |
| `personality` | `tick` u, `seq` u, `id` u, `obj`, `bytes` s: the 0x3d8 bytes of `Sys->PersonalityData` (`pub::AI::Personality`) as hex; `game_sim::ai::personality::Personality::from_bytes` reads them |
| `ai_obj` | `tick` u, `seq` u, `id` u, `obj`, `roles` [s, …], then either `fault` u (the SEH code; the read faulted) or the object fields below |
| `ai_query` | `tick` u, `seq` u, `kind` s (`ray` `PhySys::FindRayCollisions`, `ray_object` `FindRayIntersect`, `sphere` `FindSphereCollisions`), `system` u (0 for `ray_object`), `object` u\|null (the body `ray_object` tests), `from` f3, `to` f3 (the centre twice for a sphere), `radius` f (0 for rays), `max` i, `hits` [[`id` u, `point` f3, `normal` f3, `part` u], …] (a sphere hit is [`id`]) |
| `ai_out` | `tick` u, `seq` u, `id` u, `obj`, the manager fields below, `rand` [i, …] (every `rand()` value the game DLLs drew during the tick, in order), `rand_mod` [s, …] (the DLL that drew each) |
| `ai_answer` (P2.5 dock) | `tick` u, `seq` u (the behaviour tick in progress, 0 outside one), `kind` s, `id` u (the object asked), `obj`, `i` [i, …], `v` [f, …]\|null: a world answer the snapshot cannot give (`OnAnswer`, `include/oracle_ai.h`). `request`: `Ship::request_returned(RequestType, Response, Arg3)` before the callback runs, `i` [type, response, `Arg3`, a callback was pending]; `dock_hardpoints`: `get_dock_hardpoints(Index, …)` as it returns, `i` [index, result, the terminal type (-1 without the output), whether the mount, the two points and the distance were asked], `v` [the mount, point 1, point 2 (each a rotation row by row, then the translation), the distance], the caller's buffers after the call (an output it did not write keeps the caller's bytes); `lane_start`: `get_lane_start(Ring, OutLane, …)` asked of the ship `id`, `i` [the ring's id, the lane's id in, out, the result], `v` [`OutStart`, `OutDir`]; `go_tradelane`: `i` [the entrance's id, the exit's id, `bFastExit`, the result], `v` [the speed]; `gun_target`: `EqObj::slot_44` on the ship `id`, `i` [the equipment id, the target's id, the sub-target, `CEGun::SetTarget`'s answer (-1 when not called), the gun's lock target id after the call (the result when not called), the `CSimple` id given]; `zone_props`: `Universe::ISystem::GetSphereProperties` of the system `id`, `i` [the properties], `v` [the centre, the radius] |

Manager fields (`IBehaviorManager`, `include/common_iface.h`; read by
`OracleAiReadManager`): `behavior` i (`CurrentBehaviorIndex`),
`state_graph` i (`StateGraph->Id`, -1 for none), `directive` u3
(`DirectiveStorage`), `maneuver_mask` u (+0xB8), `lock` b (+0xBC),
`dirty` b (`BehaviorDirty`), `suppressed` b (`TickSuppressedFlag`),
`timers` f[5] (`BehaviorTimerA`, `BehaviorTimerB`, `BehaviorTimerC`,
`AttackOverrideTimer`, `AttackOverridePeriod`), `in_sights` u
(`InSightsTargetId`), `f118` i, `f11c` i, `f120` u (the fallback
behaviour), `score_w` f[21] and `score_b` i[21] (`BehaviorScores`, whose
stale entries the weighted draw can read; `game_sim::ai::state_graph::
choose_behavior` takes them), and (P2.5 goto and face) `pathfind`
{`timers` f[3] (`Timer1E0`, `Timer1E8`, `Timer20C`), `periods` f[3]
(`Period1E4`, `Period1EC`, `Period210`), `record` i (`Field_214`), `goal`
f3 (`Field_22C`), `cap` f (`Field_23C`), `kind` i (`Field_240`), `target`
f3 (`CachedTargetPosition`), `target_id` u (`Field_064`), `reaction` i
(`Reaction.State`), `probe_active` b and `probe_contact` b
(`Avoidance.Active`, `Avoidance.HasContact`)}\|null (`IBehaviorManager::
Pathfind`, `game_sim::ai::pathfind::PathfindManager`) and `thrust` i (the
thrust manager's flags: `bCruiseEngaged` 1, `bCruiseUnobstructed` 2,
`bEngineActive` 4, `bAutoLevel` 8, `bAutoAvoidance` 16). The pathfinder's
clocks start from three `rand()` draws of its constructor, outside any
behaviour tick, so a replay takes them from the record.

Added for the dogfight (P2.5 combat): `flags_1c8` i (`Flags1c8`), `sys`
{…}\|null (the system manager's combat state, `Sys`) and `face`
{…}\|null (the face behaviour's fields while it is the current
behaviour, 15). `sys` holds `targeting` (`Sys->Targeting`:
`attack_state` i, `aggression` f, `subtarget_dist_sq` f, `subtarget` u,
`forced` [u, …], `anchor` f3, `committed` b, `commit_timer` f,
`priority` i (`SelectionPriority`), `candidates_a` i, `candidates_b` i,
`selected` i, `selected_is_player` b, `targeted_enemies` i,
`over_clamp_kept` b, `over_clamp_dropped` b), `timers` (`Sys->Timers`
without the target list: `scanner` u, `decay_rate` f, `level` f, `timers`
[[b, f, f, f], …] (the eleven `SustainedThresholdTimer`s: active,
threshold, duration, accumulator), `decay_scale` f, `missiles` [u, …]
(the incoming-missile list, at most 16), `best_missile` u, `best_time` f)
and `guns` (`Sys->GunGroups`: `last_result` i, `target` u (0 none), `hold`
u, `aim_offset` f3, `aim_timer` f, `cruise_held` f, `scan_timer` f,
`scan_period` f, `slots` and `mines` [[u, b, i, b, i, f, f, f, f], …] (each
`FGunGroupSlot`: id, auto turret, seeker, fire tick, kind, burst, pause,
interval, phase), `params` f[19] (+0x103C to +0x1084 in field order,
`GunRange` last), `wide_cone` f, `fire_all` b, `enabled` b,
`line_of_fire` [[b, u, u, f, f, b, b], …] (the 16 cache slots: in use,
target, launcher, age, refresh timer, needs refresh, blocked) and `cm`
[f, b, [u, …]] (the countermeasure droppers' timer, state and ids)).
`face` holds `mode` i, `result` i, `dt` f, `flags` u, `flag29` u
(the `Behavior` prefix), `kind` i, `watched` u (0 none), `orient` f9,
`target` f3, `flag_1b4` u, `steer` f3, `flag_1c4` u, `flag_1c5` u,
`field_1c8` f, `field_1cc` f, `field_1d0` f, `flag_1d4` u and
`field_1d8` f (`FaceBehavior`'s fields of the same offsets).

Roles of an object: `self`; `target` (`get_target`); `formation_leader`,
`follow_leader`, `follower` (`get_formation_leader`, `get_follow_leader`,
`get_formation_followers`, at most 16); `dock_target` (`get_dock_target`
through `IObjDB::FindById`); `in_sights`; `candidate` (the target list);
`missile` (P2.5 combat: an incoming missile of the timer table's list,
`Sys->Timers.Missiles`, or its best candidate; the near walk's mask
leaves munitions out); `near` (every ship, solar and loot of the system
within `near_radius` of the ship's position, walked with
`IObjDB::GetNext`, mask `0x107ffdff`); `query` (found by a query of this tick). An object
has every role that applies, in that order.

Object fields, each the `IObjInspect` slot of `include/common_iface.h`
(`x|null` is null when the slot does not answer 0): `class` u
(`cobject()->ObjectClass`), `type` u\|null (`get_type`), `pos` f3,
`orient` f9, `vel` f3, `angvel` f3, `radius` f and `center` f3
(`get_physical_radius`), `extents` f[6]\|null (`get_archetype_extents`:
min, then max), `mass` f, `hp` f, `hp_max` f (`get_status`), `shield`
[f, f, b]\|null (current, maximum, active), `throttle` f\|null, `axis`
f3\|null, `nudge` f3\|null, `strafe` i\|null, `cruise` [b, b]\|null
(cruise, trade lane), `target` u\|null, `subtarget` u\|null,
`formation_leader` u\|null, `follow_leader` u\|null, `followers` [u, …],
(P2.5 escort) `formation_offset` f3\|null and `follow_offset` f3\|null
(`get_formation_offset`, `get_follow_offset`, which the formation and
follow behaviours read), `dock_target` u\|null, `base` u\|null, `rank` u\|null, `affiliation`
u\|null, `behavior_id` i, `toughness` i, `player` b, `dying` b,
`targetable` b, `attitude` [f, f]\|null (the ship's
`get_attitude_towards_symmetrical` toward this object and the reverse;
null for the ship itself), and (P2.5 goto and face) `engine`
[u, b, b]\|null (`find_equipment(&Id, 1, EquipTypeEngine)` and
`get_equipment_status`: the id, `EquipStatusFlagActive`,
`EquipStatusFlagDestroyed`; null without an engine), `thrusters` b\|null
(`are_thrusters_active`), `atmosphere` f\|null
(`get_atmosphere_range`) and `max_bank` f (`get_max_bank_angle`; 0 for
an object that is not a ship): what the thrust manager reads before it
commands the engine and the thrusters, and the bank limit of the steering
frame. These are the fields of
`game_sim::ai::world::ObjectView`, which a replay world fills from them.
Added for the dogfight (P2.5 combat): `targeted_enemies` i\|null
(`CShip::get_targeted_enemy_count`; null for an object that is not a
ship), and for the ship itself `lead` f3\|null (`CShip::
get_tgt_lead_fire_pos`; null when it answers false) and `equip` [{…}, …]:
each item `find_equipment` finds for the types `0x80` (guns and
launchers), `0x20` (mine droppers), `0x40` (countermeasure droppers) and
`0x100` (shields) and (P2.5 loot) `0x100000` (the tractor, whose `range` the loot behaviour reads), in that order, as {`id` u, `type` u (the mask that found
it), `status` [u, u] (`get_equipment_status`: `Flags`, `EquipFlags`) or
the failing result i, `munition`, `auto_turret`, `seeker`, `range`,
`target` u\|null (`get_equipment_val` of `EquipValMunitionKind`,
`EquipValAutoTurret`, `EquipValSeeker`, `EquipValRange`,
`EquipValTargetId`: the value's four bytes, null when the call fails),
`barrel` f[6]\|null (barrel 0's world position and direction, for a
launcher), `lead` f3\|null (`get_tgt_lead_fire_pos(id)`, a gun's lead on
its target; null when it fails), `pointing` [b, b, b] (`is_pointing_at` at π/60, the gun groups'
wide cone and missile cone angles) and `can_point` [[u, b], …] (an auto
turret's `can_point_at` toward the position of each object of the
snapshot but the ship, at 0.3: the scan asks it for the list it builds
during the tick, which the entry list need not hold yet) and, for an auto
turret, `turret` f[29] (what `CEGun::CanPointAt` reads: the yaw and the
pitch joint each as bound, `bLimited`, the engine's `Min` and `Max`, then
`GetTurretOrigin`, `TurretFrame` and the parent connector's orientation,
row-major; `scenario_replay` compares it and `can_point` with the Rust
world)}.

The `rand` values come from the counting wrapper of the rand() recorder
(`load_rand.jsonl` below), which sits in the import address table of each
game DLL: it calls the C runtime's own `rand`, so the sequence is the one
an unrecorded run draws, and the AI recorder keeps the value while a
behaviour tick is open. A replay feeds them back through
`game_sim::ai::world::RecordedRand` and checks that the port drew exactly
as many.

`crates/game-sim/tests/behavior_replay.rs` (reader in
`tests/support/ai_records.rs`) reruns every tick through
`game_sim::ai::manager::BehaviorManager`. It keeps one Rust manager per
object across the file, so what the records do not hold (the behaviours'
own fields but the face behaviour's, the thrust manager's caches)
carries over from the Rust side's previous tick; before each tick it sets
the manager fields of `ai_in` (with `flags_1c8`, `sys` and `face` where
the record has them), the target list and the personality, and builds a
world from the
`ai_obj` snapshot (object ids as handles, the ship's own state following
its commands, the ship's scan list from its next tick's `ai_in.targets`
(the list this tick built), the queries answered from `ai_query`, the
ship's equipment, lead and the ships' targeted-enemy counts from
`ai_obj`, `rand()` from `ai_out`'s values). The combat state of `ai_out`
(`flags_1c8`, `sys`, `face`) is compared too and reported on a line of
its own, with the equipment queries the snapshot cannot answer; a
difference or such a query fails the test like a mismatched tick (an
`is_pointing_at` at another angle, the lead of an item the snapshot has not). It compares the manager fields of `ai_out` bit for
bit (with `pathfind` and `thrust` where the record has them), the number
of `rand()` draws, and the commands with the `affect` records whose `beh`
is the tick's `seq` and whose `depth` is 0 (slots 0 to 3, 13, 16 to 21 and 23 to
26, the slots `affect` records; floats bit for bit). Before each tick it
also sets the `pathfind` and `thrust` fields of `ai_in`. The ship's motion
model (slots 50 to 64), which no record holds, comes from the scenario's
ship spawned into a `World` over `outdir/`; the snapshot's own state
follows the tick's commands as the `IObjAffect` bodies change it, nested
calls included. A tick whose behaviour on entry or
exit is not ported is counted as skipped by behaviour id, and so is a
mismatching tick whose graph row has a candidate that is not ported (its
poll is a stand-in). Its summary is `rust-behavior-replay.txt`.

Not recorded either: a loot container's contents (units, the AI flag, the
contents' class and good) and the ship's cargo hold, which the loot
behaviour reads only with a loot preference; Content parses
`loot_preference` case-sensitively (`LT_ALL`, `LT_COMMODITIES`, ...) and
the data's values are lower case, so every pilot of the data has none and
the replay world answers neither.

The own ship's `ai_obj` also has `hpmount` f[12]\|null (P2.5 dock:
`get_hardpoint("hpmount")`, the position, then the orientation row by
row). While the current behaviour is the dock (7), launch (8) or
instant-trade-lane (9) behaviour, the manager fields have `beh7`, `beh8`
or `beh9` {`i` [i, …], `v` [f, …]}, its own fields in this order, and the
objects it holds are snapshotted with the role `behavior`:

* `beh7` (`DockBehavior`): `i` [`ModeSelector0xc`, `UpdateResult10`,
  `Flags28`, `Flag29`, `ModeState320`, `StateFlags32c`, `Field330`,
  `ApproachClearedFlag334`, `Field43c`, `SubStateValue440`,
  `AbortFlag398`, `Flag399`, `Flag39a`, `Flag39b`, `CombatFlag344`, the
  watched object's id, `TargetShipPtr388`'s id, the proximity watcher's
  id, its `Flags14`, `SavedFlags18`, the pilot is set]; `v` [`Field338`,
  `Field33c`, `StateTimer340`, `Field408`, `MaxApproachSpeed46c`, the
  proximity watcher's near, far and ratio, `Facing38c`,
  `InterceptPoint39c`, `DestPoint3cc`, `SavedPosition3fc`, the
  `hpmount` offset at +0x430, `Field444` (three floats each),
  `Orient3a8`, `OrientMatrix3d8`, `Orient40c` (nine each)];
* `beh8` (`LaunchBehavior`): `i` [`ModeSelector0xc`, `UpdateResult10`,
  `Flags28`, `Flag29`, `ModeState164`, the base's id,
  `CachedSystemId170`, `TargetType174`, `DirectiveField178`, `Flag17c`,
  `Flag230`, `Flag231`]; `v` [`DeltaTime1c`, `Field180`, `Field184`,
  `Field188`, `Field18c`, `Field22c`, `Field190`, `Field19c`, `Field1cc`,
  `Field1fc`, `LaunchBasis`, `ApproachBasis2`, `ApproachBasis3`];
* `beh9` (`InstantTradeLaneBehavior`): `i` [`ModeSelector0xc`,
  `UpdateResult10`, `Flags28`, `Flag29`, `State30`, the entrance's id,
  the lane's id, `Flags54`, `Flag55`, `Flag56`]; `v` [`Field58`,
  `LaneStart3c`, `LaneEnd48`].

`behavior_replay` answers the dock hardpoints, the lane start, the
request answers, `go_tradelane`'s result, the zone probes and the
`hpmount` from these records (an answer outside a tick reaches the ship's
next tick: the directives submitted before it and the request's later
answer), restores the dock fields before each tick (its constructor leaves
most of them as the heap held them, D178), compares `beh7`, `beh8` and
`beh9` on exit (the launch's system id apart) and checks every recorded
zone probe against the Rust world's zones.

Not recorded: objects a behaviour looks up by an id it keeps in its own
state and that have none of the roles above (they are outside the near
radius and were not found by a query); the notification queue; the
behaviours' own state blocks. The first behaviour replays will show
whether the read set needs another role.

## `units.jsonl`

The simulation units of the first 120 PSIs, written by `OnSimUnitPsi`
(oracle_sim.cpp): one `units` record per PSI, `tick` u, `psi` u, `units`
[[id u, class u, …, check b], …] in the order the units ran, each unit's
cores as the id and `ObjectClass` of their first object (`CSimple::Id`, 0
for an object without one, such as a free part of class 1;
`OracleSimUnitCoreObject`), then whether the environment's movement-check
countdown came to the unit. Every unit decrements the countdown, so the
number and order of units decide when an object's motion is classified
(D88, D122). `scenario_replay` prints them around an object's first bit
difference.

## `unit_cores.jsonl`

The first core of every simulation unit of the first 12 PSIs, written by
`UnitCoreRecord` (oracle_sim.cpp): one `unit_core` record per unit and
PSI in the order the units ran, `tick` u, `psi` u, `i` u (the unit's
place in the PSI's run order), `id` u and `class` u as in `units.jsonl`,
`check` b, `in` (the core at the start of the unit's PSI, in the first
PSI only) and `out` (at its end, before `calc_next_PSI_matrix`), both in
the core layout of `psi.jsonl`. A unit whose object is a free part
(class 1) adds `orient` [f×9] and `pos` [f×3] (the object's frame) and,
with a golem, `golem_q` [f×4] and `golem_current_q` [f×4] (its
`TargetQuat` and `CurrentTargetQuat`, x y z w). `scenario_replay`
compares every unit core with the Rust world's bit for bit and reports
the first difference.

## `asteroids.jsonl`

The scenario system's asteroid fields after each frame (P2.4,
`oracle_sim_fields.cpp`), the fields numbered in their
`CAsteroidSystem::Fields` order (the system file's `[Asteroids]` order).
The server's `CmnAsteroid::Update` walks every ship of the system, so the
scenario ships fill the fields they fly in although the harness has no
player. Two records:

- `cube`: a cube that was not there after the last frame. `tick` u,
  `field` u, `id` u (`CubeId`), `pos` [f×3], `orient` [f×9] (the snap
  rotation), `empty` b, and `rocks` [{`obj_id` u (`CSimple::Id`),
  `asteroid_id` u, `pos` [f×3] (`WorldPosition`), `orient` [f×9],
  `mine` b, `instant_kill` b}, …] in the cube's `Asteroids` order.
- `cubes`: a field's cubes whenever their ids, states or timers changed.
  `tick` u, `field` u, `cubes` [{`id` u, `state` i (`CubeState`),
  `timer` f (`StateTimer`)}, …] in id order; a field left without cubes
  writes an empty list.
- `probe`: at ticks 0, 1 and every 150th, what the walk works with.
  `tick` u, `system` b (`CmnAsteroid::Find` found the system), `ships`
  [[f×3], …] (the scenario ships' positions), `fields` [{`zone` b,
  `cube_size` i, `fill_dist` i, `empty_freq` f, `arch` u (the `[Cube]`
  entries), `exclusions` b, `cubes` u, `near` [b, …] (`near_field` at each
  ship)}, …].

`field_replay` replays the walk from the ships' positions of `tick.jsonl`.

## `dock.jsonl`

The docking points (P2.4, the hooks `OnDockActivate` and `OnDockAnim` in
`Common/common_ceqobj.cpp`), one record per call in call order, each with
`tick` u, `seq` u (the call's number in the run), `obj_id` u and `obj` s
(the scenario name, `#<id>` for another object):

- `dock_activate`: `CEqObj::activate`. `index` u (as given), `on` b,
  `result` i (`ObjActivateResult`: 0 failed, 1 changed, 2 no change).
- `dock_anim`: `CEqObj::update_docking_animations` for a point with a
  channel, before the point is processed. `index` u, `active` b, `time` f
  (`GetStoredDuration`), `duration` f (`GetScriptDuration`), `last` f
  (`LastDuration`).

A solar runs `update_docking_animations` only while it is updated: near a
player, or in the server's `ListD4` while a dock request is queued.
`field_replay` makes the recorded activations and updates on the Rust
world and compares each point after its tick.

Content's dock queues (P3.6a) answer a dock request through
`pub::SpaceObj::Dock`, which `ai.jsonl` records as the ship's `ai_answer`
of kind `request`; their terminal states reach the docking points through
`pub::SpaceObj::Activate` in SpaceSupport's handler tail, which
`dock_activate` records. `dock_at_station` records the refusal (5, port
-1) of a ship Content does not know, which marks no terminal.
`game-logic/tests/scenario_hooks.rs` runs it through the server's hooks and
judges its `tick.jsonl` as the other scenarios.

A `content` ship (P3.6c: `content_follow`, `content_guide`, `content_flee`)
is made by Content.dll's `OracleSimCreateNpcShip` (`FSystemInfo::CreateNpcShip`)
instead of `pub::SpaceObj::Create`, so its `spawn` record in `session.jsonl`
has `content` true and the ship's `id`, its state graph and, with `pilot`, the
pilot's lookup, but no `submit_state` (the NPC ship's constructor submits its
state with the personality and both callbacks itself); with `flee`, `flee`
true and the `flee_distance` f. The ship's manager reports to Content, whose
reactions (the lead and tether events' timers on SpaceSupport, `LeadObject`,
`FleeRequest`) the scenario records do not show; `tick.jsonl` records the
ship as any other. `scenario_hooks` makes the ship through the server's
record (`npc_ships::create_in_system`) and judges `tick.jsonl` as for the
other scenarios.

## `run_rand.jsonl`

Only with the environment variable `OUTPOST_ORACLE_RUN_RAND` set (the cross-runner
reproducibility work, owner 2026-11-19; `oracle_sim_rand.cpp`): every `rand()` draw of the
frames, after the load-time recorder stops, written when the run ends. One `draw` record per
draw in order: `tick` u, `i` u (its index in the CRT sequence of the seed, the generator
followed on from `load_rand.jsonl`), `v` i, `gap` u (draws the hooks did not see just
before it), `mod` s (the DLL whose import drew it), `stack` [s, …] (the caller first, as
`module+0xoffset`); then one `sym` record per address, `at` s, `fn` s, `line` s; then
`run_rand_end` with `recorded` u, `lost` b, `next_index` u.

## `load_rand.jsonl`

The load-time `rand()` draws (owner, 2026-11-18): every draw the game
DLLs make from the harness's `srand(seed)` until the first frame, with its
caller: a diagnostic of the draws the Rust world does not make yet (the
system's population, P3.6, checked at Gate G3; the comparison starts at
the seeding point after the load). Written by `CppSource/Oracle/oracle_sim_rand.cpp`,
whose wrapper replaces the `rand` import of every module bound to the
universal CRT's `rand` (`ucrtbase.dll`; system DLLs bound to `msvcrt.dll`
have a generator of their own and are left alone). It also feeds the AI
recorder. The harness sets the phase before each step of the start-up:
`controller` (`pub::Controller::Create` of the stub), `load`
(`OracleSimLoadSystem`: `StarSystem::load`, which ends with the
`SystemLoaded` message to Content's controllers), `after_load`, `spawn
<name>` for each scenario object (`pub::SpaceObj::Create` and, with `ai`,
`pub::AI::SubmitState`), `hooks` (the recorders start); `start` holds
nothing. Between `after_load` and the first `spawn` the harness calls
`srand(seed)` again (the seeding point); the `reseed` record marks it, and
the indices of the later draws count from 0 in the new sequence. The
records are written when the first frame starts.

| `rec` | Fields |
| --- | --- |
| `load_rand_start` | `version` u (1), `seed` u, `symbols` b (dbghelp started), `hooks` [s, …] (the modules whose import was replaced, in load order) |
| `draw` | `i` u (the draw's index in the CRT sequence of the seed, from 0), `phase` s, `mod` s (the module that called `rand`), `v` i (the value), `gap` u (draws just before this one the hooks did not see), `at` s (`<module>+0x<offset>` of the return address), `fn` s (the caller: the innermost inlined function first, `<` between the inline levels), `line` s (`file:line` of the call, paths from `CppSource/`; empty without line data), `stack` [s, …] (the callers' frames, each `<module>+0x<offset> <functions> (<file:line>)`, at most nine) |
| `reseed` | `seed` u (the seed of this `srand`: `--seed`'s when given, else the scenario's), `drawn` u (the index the generator had reached: the draws since the first `srand`), between the `draw` records of before and after it |
| `phase` | `phase` s, `first` u (index of its first draw, or the next index when it drew nothing), `count` u (indices it spans), `recorded` u (draws the hooks saw) |
| `load_rand_end` | `recorded` u, `unseen` u (the sum of the gaps), `lost` b (a value was not found within 10⁶ steps of the generator; the indices after it are counts of seen draws only), `next_index` u (the index of the first draw of frame 1) |

The gaps come from following the CRT's generator (`state = state × 214013 +
2531011`, value `(state >> 16) & 0x7fff`) from the seed: each recorded
value is looked up from the previous draw's state on, so a draw made
through a module that is not hooked shows as a gap before the next seen
one. The frames come from `StackWalk64` with the build's PDB files (the
release build omits frame pointers), the names and lines from
`SymFromAddr`, `SymFromInlineContext` and `SymGetLineFromInlineContext` at
the return address minus one. Without symbols only the frame-pointer
chain (`RtlCaptureStackBackTrace`) is walked and the functions are left
out.

`load_rand.txt`, beside it, is the summary the data job prints for each
scenario's first run: per phase the draws grouped by caller (the module
and the functions of the first four frames) in order of first draw with
their first index and count, then the runs of consecutive draws with the
same caller (the first 400). `crates/game-sim/tests/scenario_replay.rs`
reads the `phase` records and reports them beside the Rust world's draws
while it loads the system and spawns the scenario objects.

## SimClientStub files

`iclient.jsonl` has the layout of the P0.5 client's file
([reference-build.md](reference-build.md) section 8) with `tick` in place
of `frame`: `tick` u, `seq` u, `slot` i, `m` s, `a` {the arguments by
name}; a non-void slot adds a line with the same `seq` and `r`. Arguments:
scalars by value, strings, byte blocks as hex, `TextIds` as an array,
`CHAT_ID`, `Costume` and `Vector` as hex of their bytes, every other
structure or pointer as `"present"` (or `null`). The stub answers
`Startup` with `true`, `Update` with 1 and the queue queries with 0.

`controller.jsonl`: `rec` `msg`, `tick` u, `seq` u, `flid` u, `msg` i (the
`pub::Controller::EMessage` value), `a` {…}\|null. `a` decodes
`DockComplete` (`ship`, `dock`, `system`), `DockRequest` and `DockCancel`
(`ship`, `target`, `w2`), `ObjectDestroyed` (`id`, `killer`, `system`,
`owner`, `kind` i, `pos` f3), `ObjectDamaged`, `SystemLoaded`,
`SystemUnloaded` and `ControllerDestroyed` (`w0`), message 0x25 (`w0`,
`w1`, `w2`), `Heartbeat` (`elapsed` f); any other message is
`{"data":"present"}`.

`replication.jsonl` (P3.3, decision record 0007 point 11; only with
`dump replication`, from the players' join on): one line per call of the
slots below, written after its `iclient.jsonl` line (the same `seq`),
`tick` u, `seq` u, `client` u (the call's client id, the session), `m` s
(the method), `id` u (the object the packet is about; 0 for
`SendServerSetReputation`), then by method (`q` f4 is a `Quaternion` in
its member order w, x, y, z; archetypes are the full ids):

| `m` | Fields after `id` |
| --- | --- |
| `SendCommonUpdateObject` | `pos` f3, `q` f4, `time` f (`Field_0x20`, the observer's clock), `throttle` f (`Field_0x24`), `behavior` u (`Field_0x28`) |
| `SendCommonFireWeapon` | `target` f3, `guns` u[] (the hardpoints) |
| `SendCommonActivateEquip` | `equip` u, `on` b |
| `SendCommonActivateCruise` | `on` b, `arg` u (`Field_0x05`) |
| `SendCommonActivateThrusters` | `on` b |
| `SendCommonSetTarget` | `sub` u, `target` u, `target_sub` u |
| `SendCommonGoTradeLane` | `ring1` u, `ring2` u, `flag` b (`Field_0x0c`) |
| `SendCommonStopTradeLane` | `ring1` u, `ring2` u (`id` is the ship) |
| `SendCommonJettisonCargo` | `item` u, `count` u |
| `SendServerCreateSolar` | `arch` u, `pos` f3, `q` f4 |
| `SendServerCreateShip` | `arch` u, `pos` f3, `q` f4, `vel` f3, `angvel` f3, `hp` i, `throttle` f, `owner` u, `lane` u, `equip` i (the equipment list's length) |
| `SendServerCreateLoot` | `owner` u, `kind` u, `items` [{`id` u, `contents` u, `units` u, `container` u, `pos` f3}, …] (`id` is the first item's) |
| `SendServerCreateMine`, `SendServerCreateCounter` | `arch` u, `owner` u, `sub` u, `pos` f3, `q` f4, `vel` f3 |
| `SendServerCreateGuided` | `arch` u, `owner` u, `sub` u, `target` u, `target_sub` u, `pos` f3, `q` f4 |
| `SendServerSetGuidedTarget` | `target` u, `target_sub` u |
| `SendServerDestroyObject` | `kind` u (`Field_0x04`) |
| `SendServerDamageObject` | `destroyed` b, `cause` i, `inflictor` u, `entries` [[sub u, hit points f, fate i], …] |
| `SendServerActivateObject` | `on` u, `index` u |
| `SendServerSetReputation` | `vibe` i, `affiliation` u (`Field_0x08`) |
| `SendServerBurnFuse` | `source` u, `fuse` u, `sub` u, `at` f, `lifetime` f, `lit` u |
| `SendServerSystemSwitchOut` | `gate` u |
| `SendServerSystemSwitchIn` | `bits` u, `pos` f3, `q` f4 |
| `SendServerLand` | `dock` u, `base` u |
| `SendServerLaunch` | `dock` u, `index` u, `pos` f3, `q` f4 |

## `dispatch.jsonl`

The dispatch trace (record 0006 point 10; since P3.4a): every delivery
of `Server.dll`'s controller registry, from `IServerImpl::Startup` on,
written before the controller's `IDLL::HandleMessage` runs. The hook
(`GOracleSimHooks.OnControllerDeliver`, `FOracleSimDeliverScope`) sits at
the registry's three delivery sites: the broadcast and targeted arms of
`Server_ControllerDispatch`, and the heartbeat walk of `Server_TimerTick`.
A timer's `TimerExpired` is one of the first two (`Server_TimerFiring`
dispatches it). Every controller is listed, the harness's own included,
so a broadcast shows as one record per controller in list order.

| `rec` | Fields |
| --- | --- |
| `deliver` | `tick` u (0 for the start-up, before frame 1), `seq` u (from 0, in delivery order), `depth` i (the deliveries already in progress: 0 for one no handler made, 1 for one sent by a handler, and so on), `flid` u (the receiving controller), `msg` i (the `pub::Controller::EMessage` value), `path` s (`broadcast`, `send` or `heartbeat`), [`elapsed` f (`Heartbeat`: the whole periods delivered)], [`p1` u, `p2` u (`TimerExpired`: the timer's two words; `p1` is Content's counter value for Content's timers)] |

`crates/game-logic/tests/content_startup.rs` compares the heartbeats
Content's controllers get with the port's bus and reports the
`TimerExpired` words against Content's counter (P3.4a). The payloads of
the other messages are not decoded yet.

## Other files of a run

`ai.jsonl` (above) holds the behaviour ticks (P2.5), `load_rand.jsonl` and
`load_rand.txt` the load-time draws. `server.ini` is the server INI the run used (the master's with the
populators left out), `fdump.txt` every `FDUMP` message (`tick`, flags in
hex, text) and `metrics.txt` the plain-text input of `--runs`
(`oracle_sim.h`). None of them is a record for the tests.

## `content_<table>.txt` (`dump content_tables`)

Content's static tables after a multiplayer start-up (P3.4b, decision
record 0008 point 5; `Content/oracle_content_tables.cpp`), read by
`crates/game-logic/tests/content_tables.rs`. Plain text, one record per
line, fields `key=value` separated by one space: ids and unsigned values
decimal, signed values decimal, floats as eight hex digits of their bits,
strings as the hex of their bytes, lists comma-separated (empty for an empty
list). Maps in key order, vectors in their order.

| File | Line |
| --- | --- |
| `content_voice_lines.txt` | `voice msg duration` |
| `content_voice_properties.txt` | `voice flags permutations` (`nickname:count`, …) |
| `content_empathy.txt` | `group events` (4 floats) `rates` (`group:rate`, …) |
| `content_formations.txt` | `formation nickname slots` (`x:y:z:player`, …) |
| `content_pilots.txt` | `pilot refs` (inherit and the 16 references) `found personality` (the 0x3d8 bytes `GetPilotPersonality` fills over a default personality, as `OracleSimPilotPersonality` does; padding included, compared through `Personality::from_bytes`); then `blocks=<kind> ids` for each of the 16 block tables |
| `content_npc_ships.txt` | `npc_ship nickname ship loadout pilot state_graph level classes` |
| `content_factions.txt` | `faction npc_ships jump scan_cargo` (`archetype:level`, …) `scan_chance scan_announce market_goods assassinate legality singular formations` (`key:value`, …) `costumes` (`head,body,left,right,count,accessories…`, `;`-separated) `voices mc_costume msg_id_prefix` |
| `content_faction_names.txt` | `faction group female male last formation large_ship` (each `count:first-last,…`) `large_ship_desig short_name rank_desig thresholds name_ids` |
| `content_faction_groups.txt` | `group` |
| `content_ship_classes.txt`, `content_ship_mission_properties.txt` | `class` or `ship`, `members` |
| `content_loot.txt` | `loot drop_chance drop_properties` |
| `content_phantom_loot.txt` | `phantom toughness chance num_to_drop` |
| `content_gcs_exclusions.txt` | `voice scripts` |
| `content_toughness.txt` | `experience toughness` |
| `content_rank_difficulty.txt` | `stage difficulty` |
| `content_news.txt` | `news` (the index) `bases from to audio logo` (lower case, D785) `breaking autoselect icon category headline text` (the three `FmtStr`s flattened) |

P3.4c (record 0008 point 10) adds the other loaders' tables, after the
seventeen above, read by `crates/game-logic/tests/content_loaders.rs`. In
them `/`, `+`, `:` and `;` separate the fields of nested records, as each
row says. The bases are written from fresh records that take the live
records' factions and are then loaded (`FContentBase::Load(nullptr)`); no
room file is loaded at start-up, so no fixture finds its placement. The
specific NPCs are StoryLauncher's loop over `missions\specific_npc.ini`
into a local table (a multiplayer server has no StoryLauncher). Each system,
in id order, is read into a local `FSystemInfo` as `LoadFromReader` reads its
encounter parameters, zones and patrol paths; `Validate`, the `[System]` and
`[Object]` sections and the trade lanes are left out. Doubles are sixteen
hex digits of their bits.

| File | Line |
| --- | --- |
| `content_bases.txt` | `base msg_id_prefix diff local_faction vendor` (min,max) `npcs rooms` (counts) `factions` (`group:weight:npc+…:min/max/weight+…`, `;`-separated) |
| `content_base_npcs.txt` | `base npc` (the key) `costume voice name affiliation nickname` `missions` (`min/max/type`) `bribes` (`cost/group/text`) `knows` (`price/v1/v0/level`) `rumors` (`from/to/level/id+…` with all 8 ids) `rooms` |
| `content_base_rooms.txt` | `base room density fixtures` (`npc/placement/fidget/role`) |
| `content_specific_npcs.txt` | `npc` (the key) `costume voice name affiliation nickname` |
| `content_system_paths_all.txt`, `_gate`, `_hole` | `from to path` |
| `content_encounter_params.txt` | `system params` (the key) `nickname system_id zone reroll last difficulty formations` (count) `permutations` (`weight:index[*]+…`, `;`-separated) |
| `content_encounter_formations.txt` | `system params formation` (the index) `feeling reroll mask f0c value faction formation_id by_class behavior arrival` (nine digits) `distance times simultaneous f78 longevity ships` (`classes+…/npc_arch/pilot_job/min/max/make+…/offset`, `;`-separated) |
| `content_ship_line_factions.txt` | `system params formation ship factions` (`GetShipLineFactions`) `archs` (`group:arch+…` from `GetFactionShipLineArchs`, `;`-separated) |
| `content_zones.txt` | `system zone nickname path` (an index of the system's paths, −1 for none) `unpopulated tradelane lawful unlawful vignette density repop keys attack restrictions` (`count:type`) `f1c f30 additive battle relief encounters` (`nickname/zone/difficulty/chance/group:weight+…/class+…`, every ship line's classes, `;`-separated) |
| `content_patrol_paths.txt` | `system path` (the index) `label usage eligible loop points` (`x:y:z`) `zones` (`zone:step+…`) `objects` (`id:point+…`). The points are Tier B (decision T11): they are the zones' ends moved along the zone's orientation, which goes through `FSIN` and `FCOS`, so they are compared within game-sim's bound for the orientation (4 ulps or 1e-7); every other field is compared as text |

## `content_<table>.txt` (`dump content_systems`)

Content's system records, gate locks and `[mPlayer]` writer after a
multiplayer start-up whose `LoadGame` built the records and whose
`system_loads` loaded every system (P3.4d, decision record 0008 point 10;
`Content/oracle_content_systems.cpp`), read by
`crates/game-logic/tests/content_systems.rs`. The same plain text as
`dump content_tables`; records in `GSystemInfos`' key order, a record's
vectors in their order.

| File | Line |
| --- | --- |
| `content_systems.txt` | `system loads local_faction rpop zones paths jumps lanes lane_rings solar_npcs players mission_objects` (counts) |
| `content_system_jumps.txt` | `system object type pos` (`x,y,z`) `goto_system goto_object` |
| `content_system_lanes.txt` | `system lane` (the index) `start end` (`x,y,z`) `rotation` (9 floats, `,`-separated) `max_weight rings` (`id:x:y:z:reputation:` and the 9 rotation floats `:`-separated, `+`-separated) `candidates` (`ring index:max weight:zone@weight/…`, `+`-separated) |
| `content_solar_npcs.txt` | `system index object nickname base msg_id_prefix state_graph pilot` (0 or 1) `vibe affiliation` (the reputation handle's vibe and `pub::Reputation::GetAffiliation` of it now; `none` without a handle) |
| `content_gate_locks.txt` | `SaveContentGateLocks`' own text: `[locked_gates]`, `npc_locked_gate = id` lines, then `locked_gate = id` lines |
| `content_mplayer.txt` | `FContentPlayerState::Save`'s own text for a synthetic state of player 7 (every record kind and statistic), then the save of that text read back by `LoadContentPlayerState`; the first save also stays in `content_mplayer_first.txt` |

## `mission_<table>.txt` (`dump mission_scripts`)

The fourteen story missions as their parser leaves them (P3.5a, decision
record 0008 points 6 and 10; `Content/oracle_content_dump.cpp`), read
by `crates/game-logic/tests/mission_scripts.rs`. After the multiplayer
start-up (GameSupport's tables, `LoadGame`'s system records), player 7 is
registered in he01; for each mission in the order `Mission_01a`,
`Mission_01b`, `Mission_02` to `Mission_13` the mission dialogs are
emptied, `srand(0x4f524143)`, the mission is made by `CreateInstance` on
controller id `0x00ff0001` plus its index (no controller has it, so the
constructors' heartbeats draw nothing), its draws are counted (the next
four `rand()` values matched against the sequence from the seed), its
tables written and it is destroyed. Every line starts `mission=<name>`;
fields are `key=value` separated by one space, in the order of the C++
dumpers, which is the order of the Rust records' `fields()`
(`game-logic::content::mission::field`): unsigned and signed values in
decimal, flags as the byte's value, floats as the eight hex digits of
their bits, strings as the hex of their bytes; vectors, matrices (row by
row), transforms (rotation, then translation) and quaternions (W, X, Y, Z)
as their floats joined by `,`; a costume `head,body,left,right/count/
accessories`; lists joined by `,`, a pair `a:b`, a loadout line
`id:archetype:count:health:equipped:mission:hardpoint`, a
`Cnd_NPCSystem*` entry `label:system`. Maps are in key order, lists in
insertion order. P3.5a2 added the four tables of `[ObjList]` and
`[MetaBehavior]`; the fourteen of P3.5a1 are unchanged.

| File | Line |
| --- | --- |
| `mission_scripts.txt` | `draws` (−1 beyond 0x10000) `controller player player_state players heartbeat_interval reward title offer` (the mad libs) `previous_state mission_state field_0x2b0 campaign_slot_current campaign_slot_succeed story_cue field_0x2c0 field_0x2c4 field_0x2e8 elapsed_time triggers actions initially_active` (`id:id`) and the counts `ship_groups solars loot rand_encs npcs npc_ship_archs objectives dialogs dialog_costumes` |
| `mission_triggers.txt` | `trigger` (the map key) `state repeatable id events actions` (the count), then `cnd_trigger` and `cnd=` the factory name and every field of the condition, or `cnd=none` |
| `mission_actions.txt` | `trigger index act_trigger`, then `act=` the factory name and every field of the action |
| `mission_ship_groups.txt` | `group` (the index) and every field of the `[MsnShip]` / `[MsnFormation]` record (`system` the record's system id, 0 for none), then `ships` (the slot count) |
| `mission_ships.txt` | `group index` and every field of the slot (`cargo` as `good:count`) |
| `mission_solars.txt`, `mission_loot.txt` | `index` and every field of the record (the loot's `LootInfo` flattened; the solar's `name` its mad lib; the solar's personality is not written, `pilot` says whether `GetPilotPersonality` found it) |
| `mission_rand_encs.txt` | `index nickname enabled nickname_id formation_index npc_count params` (0 without parameters, else `GetType`: 2 SP, 3 TL) `system controller player attacker_rep pilot_id labels positions` (`x,y,z,weight`, `;`-separated) `ships_spawned formations_to_spawn num_forms_min num_forms_max formations` (`found:nickname:field_0x08:min:max:count:weight:` and the ship choices `classes/weight`, `;`-separated; formations `+`-separated) `target_ship_name goto_target_id target_object_id dock_target spawn_angle spawn_distance empty_pos_radius ship_name_counter`, then SP `activation_type activation_location activation_range activation_time_0 activation_time_1 cumulative_time` or TL `tl_attack_chance tl_attack_chance_read_in direction entry_ring exit_ring target_tradelane` |
| `mission_npcs.txt` | `key` and every field of the `[NPC]` record |
| `mission_npc_ship_archs.txt` | `key nickname ship_archetype loadout pilot state_graph npc_classes level` (the `[Mission] npc_ship_file`'s archetypes) |
| `mission_objectives.txt` | `player n type hidden nickname ids index state`, then by type: 1 `base_id`; 2 `marker` (five words); 4 `marker label`; 5 and 6 `rep_instance system_id marker` |
| `mission_dialogs.txt` | `dialog controller system speakers lines` (the dialogs of `GMissionDialogs` after the mission) |
| `mission_dialog_lines.txt` | `dialog speaker_key index` and every field of the line |
| `mission_costumes.txt` | `index costume` (the script's dialog costumes) |
| `mission_obj_lists.txt` | (P3.5a2) `list directives`: the mission object's `[ObjList]` map (+0x100) in key order, the key and its directive count |
| `mission_obj_list_directives.txt` | `list index factory type directive name` (the factory case 0 to 0x16 or 0x19, `GetType`, the `[ObjList]` key of the class, `TractorBeam` for case 0x19, and the line the loader kept), then the class's members and its op's fields (`game-logic::content::mission::obj_list`'s `fields()`): every op starts with `priority`; a goto op `kind destination field_1c words_20` (the twelve words from +0x20) `range field_54 field_58 field_59 cruise no_cruise field_5c field_60 field_64 field_68 field_6c`, the guide's goto and follow parts with `goto_` and `follow_` before each name, the embedded ops' own priorities not written; a name a class does not have is written empty (`GotoSpline`'s and `GotoVec`'s `target_name`, `Guide`'s `object_name`); `formation` says whether `FindFormation` found it; `InstantTradelane`'s `speed` is the bits of its third word |
| `mission_meta_behaviors.txt` | `list objects`: the `[MetaBehavior]` map (+0x114) in key order |
| `mission_meta_behavior_objects.txt` | `list index type class` (5 `MB_GOTOGUIDE`, 6 `MB_DOCKGUIDE`) `line priority first_position destination range target_name field_0x174 field_0x175 field_0x176`, and for the dock guide `dock_target_id`; the leg list (empty after the parse) and the ship (null) are not written |

## Mission saves (`dump mission_saves`, `dump mission_save_loads`)

The player-save sections of the fourteen story missions and of Story
(P3.5b, decision record 0008 points 6 and 10, Gate G3;
`Content/oracle_save_dump.cpp`), read by
`crates/game-logic/tests/mission_saves.rs`. After the multiplayer start-up
player 7 is registered in he01, and every mission is made as for
`dump mission_scripts` (dialogs emptied, `srand(0x4f524143)`,
`CreateInstance` on `0x00ff0001` plus its index). A save is the `PlayerSave` body of a script mission
without the message's gates (single player, the player's id; the session's
server is a multiplayer one): `\n[MissionState]\n`, `SaveMissionState`,
unless a trigger save is pending `SaveMission` and `SaveRandEncs` (no ship,
so no snapshot), then `\n[BStoryMissionDone]\n`. Files are written in
binary, with the `\n`s of the format strings. The six clamps are put back
after each mission.

The three states of each mission:

- `constructed`: right after the constructor.
- `advanced`: every trigger with a condition made active, in id order; the
  condition of the k-th (from 0) gets: `Cnd_Destroyed`, `Cnd_LootAcquired`,
  `Cnd_ProjHit`, `Cnd_ProjHitShipToLbl` count k + 1; `Cnd_DistShip`,
  `Cnd_DistVec` tick timer F[k], last distance F[k + 1]; `Cnd_DistVecLbl`
  tick timer F[k + 2]; `Cnd_TetherBroke` +0x90 F[k + 3], +0x88 F[k + 4];
  `Cnd_Timer` elapsed F[k + 5]; `Cnd_NPCSystemEnter` and `Exit` their ships
  as pending, the i-th with system S[(k + i) mod 3]; where F is the twelve
  floats `0.0078125, 0.0234375, 0.1, 123.456, -0.6, 31476.25, FLT_MAX,
  1e-7, 2500, 0.33333334, 16777216, -0` (indices mod 12) and S `7,
  16777217, 4000000007`. The script: story cue 7; the NN path active with
  ids 1234 and -5, position (1.5, -2.25, 100000.125), label
  `oracle_nn_target`, system 4000000007; three cut scenes
  `missions\oracle\rtc_{a,b,c}.thn` (b finished, a with `Field_0x95`); an
  ambient scene `scripts\oracle\ambi.thn` in room 0x1234 of base
  4000000007; the friendly-fire labels `oracle_ff_a`, `oracle_ff_b`. The
  mission object: two respawn ship records, a copy of the first `[MsnShip]`
  record with only its first ship (position (-31476.25, 150, 0.1),
  arrival 0, priority 3, 450 hit points, the first `[ObjList]`'s
  directives) and a copy of the last with only its first ship (arrival
  object 0x40000123, priority -2, the first `[MetaBehavior]`'s first object
  as its guide); one random-encounter respawn record (the first encounter's
  id or `CreateID("oracle_enc")`, 2 ships, priority 3, 450 hit points,
  translation (100.5, -0, 3000000), classes `17, 34` and none, names
  `oracle_rand_a`, `oracle_rand_b`, the first list's directives); the solar
  `oracle_solar`, the marks `oracle_mark_a`, `oracle_mark_b`, the
  invulnerables `oracle_inv_a` (12.5, 1) and `oracle_inv_b` (0.0234375, 0),
  the decloaked `oracle_cloak`, three attitudes (`oracle_vibe_a` to
  `Player` -0.6; reputation 17 to `oracle_vibe_b` 0.1; `oracle_vibe_c` to
  reputation 4000000007 -0); the first random encounter enabled with 3
  ships, the second disabled with 5; and the clamps attack 1, encounters 0,
  scan 1, GCS on, hostile 0, population 0.
- `pending`: a trigger save pending for the first trigger in id order.

| File | Content |
| --- | --- |
| `mission_saves/save_<mission>_<state>.ini` | (`dump mission_saves`) the save of the mission in that state |
| `mission_saves/story_constructed.ini` | (`dump mission_saves`) Story's `PlayerSave` right after its constructor for player 7 |
| `rust/save_<mission>_<state>.ini` | (written by the port before `dump mission_save_loads`, through the text-mode translation) the port's save in the three states, `restored` (the port's save after restoring the hand-made `crates/game-logic/tests/fixtures/mission_saves/raw.ini`) and `raw` (that fixture) |
| `rust/story_<n>.ini` | (written by the port) the port's Story save after restoring `story_<n>.ini` of the same fixtures, n = 0 to 5, in turn |
| `loaded_<mission>_<state>.txt` | (`dump mission_save_loads`) a new mission's restore of the port's save, the generator seeded before it: `script field_0x2ec initially_active` (`id:id`) `active pending_activations save_pending save_trigger nn_state draws`; per initially active trigger with a condition `cnd trigger=<id> text=` and its condition's `Save` output, then `|`; per ship of each respawn record `ship group index init_objectives init_meta guide directives`; `solar_reps=` (`,`-separated); per random encounter `rand_enc nickname` (hex) `enabled npc_count params`; then `=== save` and the save of the restored mission |
| `story_loaded_<n>.txt` | (`dump mission_save_loads`) one Story's `PlayerLoad` of `rust/story_<n>.ini`, n = 0 to 5 in turn, then its `PlayerSave` and `mission_number=` the player's |

## `rm_<table>.txt` (`dump random_missions`)

The random missions' tables, vignette tree and offers (P3.7a, decision
record 0008 point 8, Gate G3; `Content/oracle_random_missions.cpp`), read by
`crates/game-logic/tests/random_missions.rs`. After the multiplayer start-up
the tables and the tree are written as the generator object loaded them with
Content.dll. Then one case per base of the export's list (29 bases, the
last three those without a dock object, D954), per
`[BaseFaction]` of it with a `mission_type` and with faction properties and
names, per difficulty (0.2, 1.5, 8, 30, 85) and per seed (1, 0x4f524143):
`GRmObjectiveZones` emptied, player 7 put in the base's system,
`srand(seed)`, then one loader's `CreateMission` for {7, the base, the
faction, the difficulty}. Its draws since `FinalizeMission`'s
`srand(random_seed)` are counted (−1 when it did not run). The loader keeps
each mission until the next case's `CreateMission` releases it, so a target
name stays reserved until then. Fields are `key=value` separated by one
space: ids and unsigned values in decimal, signed values with their sign,
flags 0 or 1, floats the eight hex digits of their bits, strings and
flattened `FmtStr` values the hex of their bytes; lists joined by `,`,
sub-fields by `/`, `:` or `+`, records by `;`.

Every field is compared as text but the assassination target's position,
which `FillTargetShips` makes from the objective's position and
`random_inside_ring`'s offset: the position in the `target_ships` part of an
`rm_offers.txt` line and the `position` of an `rm_offer_ship_groups.txt` line
of the `target_ships` component. `random_inside_ring` turns its angle with
`FSIN` and `FCOS` (D879), so these are Tier B (decision T11) and compared
within the patrol points' bound (4 ulps or 1e-7,
`crates/game-logic/tests/support/mod.rs`). The summary
(`rust-random-missions.txt`) counts the lines equal only within the bound and
lists each of them; a bit-equal line counts as exact.

| File | Line |
| --- | --- |
| `rm_npc_rank_to_difficulty.txt` | `rank difficulties` |
| `rm_diff2money.txt` | `difficulty money` |
| `rm_bonus_loot.txt` | `drop_chance`, then per faction (`4294967295` for `all`) and record `faction index archetype num_to_drop difficulty weight` |
| `rm_solar_formations.txt` | `formation nickname type slots` (`mandatory/type/x,y,z/` the 9 orientation floats row by row) |
| `rm_killable_solars.txt` | `solar nickname type string_id any factions archetype loadout hitpoints difficulty zone_mask pilot_choices` |
| `rm_critical_loot.txt` | `loot why name archetype types any factions` |
| `rm_generator.txt` | `loaded linked root nodes leaves` |
| `rm_nodes.txt` | `node node_id nickname parents children implemented first_child weight difficulty windows` (`min:max`) `offer_groups hostile_groups zone_types failure reward` (`str_id:params`) `objectives` (`key/str_id:params`) `offer_texts comm_sequences` (counts) |
| `rm_offer_texts.txt` | `node index append entries` (`str_id/form/params`) |
| `rm_comm_sequences.txt` | `node index trigger target f08 delay lines` (`speaker/text`) |
| `rm_missions.txt` | `case base system group difficulty seed result` (`CreateMission`'s: 0 an offer, 1 no enemy group, 2 no offer, 3 no zone) `random_seed player offer_base offer_group enemy_group dest_sys zone pos reward rep_reward rep_hit path` (`T`/`F`) `offers draws serial` (`GRmShipGroupSerial`) `enemy_uses` (the enemy group's reserved pilot names and designations; −1,−1 without names) |
| `rm_offers.txt` | `case machine type system offer_group hostile_group difficulty reward rep_hit rep_reward offer_base mc_costume voice text reward_text failure_text target_first target_second target_gender target_affiliation general target_ships` (`active:position:f50:f54`; the position Tier B) `solars ships` (`active:position`) |
| `rm_offer_ship_groups.txt` | `case component` (`target_ships`, `ships`) `group f00 f01 f02 f04 f08 system position` (Tier B in the `target_ships` component) `f40 formation serial ships` |
| `rm_offer_ships.txt` | `case component group ship nickname random_name affiliation npc_ship_arch target_name d8 dc loot` (`kind/f100/critical name/archetype/units/system/hit_pts_scale/tractor/rel_pos/offset`) |
| `rm_offer_solars.txt` | `case solar nickname faction orientation position archetype loadout pilot` (`bPilot`; the personality is not written) `f4c8 arch type dest_sys mode invincible f4e0 hitpoints loot` |
| `rm_offer_objectives.txt` | `case component key type str_id params marker` (`system:value:position`) |
| `rm_offer_comms.txt` | `case conversation trigger delay ready lines` (`speaker/target/text`) |

`random_missions.rs` also compares the port's own `CreateMission` with the
same files (P3.7c, `data_dump_created_random_missions`, summary
`rust-created-random-missions.txt`): the port makes the cases itself, in the
export's order, and every field of the seven case files is its own (the zone,
the enemy group, the seed, the result); the rebuild from the oracle's values
above stays as it was. The rebuild takes the oracle's zone, enemy and seed,
so the dock position `CreateMission` searches from (D954) does not reach it;
the port's own creation is where it does. A case whose base is one of the
three bases without a dock object (`fr01_01_Base`, `he01_15_Base`,
`ve05_02_Base`; none is in the export's list) may differ only in the fields
that derive from the dock position, and each such case is listed under
"D954" in the summary (owner, 2026-11-25): the `rm_missions.txt` fields
`result enemy_group dest_sys zone pos path offers draws serial enemy_uses`;
the case's offer lines in number and every field but the query's
(`offer_group difficulty reward rep_hit rep_reward offer_base voice`) where
both have one; its other offer lines in number and every field. Every other
case and field is compared exactly, and a bit-equal case counts as exact.
A difference in the counters such a case leaves (the serial, the vignette
nicknames) reaches the next cases, which are compared exactly.

## `random_mission_saves/` (`dump random_mission_saves`) and `rmload_<table>.txt` (`dump random_mission_save_loads`)

The random missions' `[RandomMission]` save sections and the offers rebuilt
from them (P3.7c, decision record 0008 points 8 and 10, Gate G3;
`Content/oracle_random_missions.cpp`), read by
`crates/game-logic/tests/random_missions.rs`. After the multiplayer start-up,
for every base of the export's list that Content knows: `GRmObjectiveZones`
emptied, player 7 put in the base's system, the base loaded
(`FContentBase::Load`, which reads its `num_offers`), `srand(0x4f524143)`,
`BuildMissionVendorOffers` into a local visit, each vendor offer written and
saved, `ClearMissionVendorOffers`, the base unloaded; then per
`[BaseFaction]` with a `mission_type`, faction properties and names,
`srand(0x4f524143)` and `CreateBarMissionOffer` with the faction's first
mission type as the NPC's (`min`, `max`, type); a created offer is saved and
the loader released. Every save is then loaded in turn by a new loader for
player 7 (`Load`: every section, each with `FinalizeMission`), its draws
since the last `srand(random_seed)` counted, the loader released. The fields
are written as in `rm_<table>.txt`.

| File | Content |
| --- | --- |
| `random_mission_saves/rms_offers.txt` | per base `source=vendor_offers base count zones` (`GRmObjectiveZones`' size); per offer `save` (the save's number, −1 when not saved) `source` (`vendor`, `bar`) `base group created offer_id type`, then `mission=-`, or `random_seed player offer_base offer_group enemy_group dest_sys zone pos reward rep_reward rep_hit difficulty path offers serial zones` and the vendor description (`class=-` without an offer, else `class title description briefing base_id icon filter`) |
| `random_mission_saves/rmsave_<n>.ini` | the section a `DestroyMission` controller writes on `PlayerSave`: `[RandomMission]`, `type = DestroyMission`, `FRandomMissionData::Save` (binary: the `\n` the format strings write) |
| `random_mission_saves/rmsave_pair.ini` | `rmsave_0.ini` then `rmsave_1.ini` |
| `random_mission_saves/rmload_missions.txt` | per load `save loaded player offer_base offer_group enemy_group dest_sys zone pos reward rep_reward rep_hit difficulty jump_pref offer_ids random_seed path offers draws serial zones description enemy_uses` |
| `random_mission_saves/rmload_<table>.txt` | the rebuilt offers in the format of `rm_<table>.txt` (`offers`, `offer_ship_groups`, `offer_ships`, `offer_solars`, `offer_objectives`, `offer_comms`), the load's number as the case; the same Tier B fields as there |
| `rust/rmsave_<n>.ini`, `rust/rmsave_pair.ini` | (written by the port before `dump random_mission_save_loads`, through the text-mode translation) the port's saves of the same offers |
| `rmload_<table>.txt` | (`dump random_mission_save_loads`) the C++ loads of the port's saves, `n` = 0 up while a file opens, then the pair, in the same format |

Under D954 (owner, 2026-11-25) a base without a dock object may differ in
its `rms_offers.txt` lines in number and every field but `source` and
`base`, and in its saves in every key but `offer_base`, `offer_ids`,
`jump_pref`, `rep_reward` and `rep_hit`; each is listed under "D954" in the
summary. The loads compare exactly, because both sides load the same files.

## `mission.jsonl` (`dump mission_trace`)

The Tier B mission trace (P3.5c1, decision record 0008 question 9;
`Content/oracle_mission_trace.cpp`, the hook `GOracleMissionTraceHook` of
`Content/MissionScript.cpp`), read by `crates/game-logic/tests/mission_trace.rs`.
After the multiplayer start-up and the load of the session's system
(`RELAYSEVEN_system`): player 7 registered in that system without a ship,
`GMissionDialogs` emptied, `srand(0x4f524143)`, `Mission_01a` made by
`CreateInstance` on controller id `0x00ff0001`, which the bus does not know.
The session hands the mission `MissionStart` at tick 0, then at every tick
from 1 to 900 its scripted messages and a heartbeat of `1.0f / 30.0f`:
`SpaceEnter` (tick 2), `CommComplete` of `DX_M01_0000_FREEPORT_7_BRIDGE`
(200), `DX_M01_0005_Samura_SA_14` (260) and `DX_M01_0010_FREEPORT_7_BRIDGE`
(320), `SpaceExit` (600) and `BaseEnter` of `he01_01_Base` (620). One JSON
object per line, ids unsigned decimal:

| `rec` | Fields | Written |
| --- | --- | --- |
| `message` | `tick`, `msg` | before each message the session sends |
| `activate` | `tick`, `trigger` | `ActivateMissionTrigger` set the trigger `ACTIVE` |
| `fire` | `tick`, `trigger`, `event` | a condition matched; `event` 4294967295 is the activation probe |
| `action` | `tick`, `trigger`, `index` | before an action of the firing trigger runs, `index` its place in the list |
| `timer` | `tick`, `trigger`, `index` | before a delayed action runs from the timer list |
| `rearm` | `tick`, `trigger` | a repeatable trigger is `ACTIVE` again |
| `deactivate` | `tick`, `trigger` | `DeactivateMissionTrigger` erased an active entry |
| `state` | `tick`, `from`, `to` | `ApplyDesiredMissionState` applied a new state |
| `final` | `tick`, `active`, `pending` | the session's end: the active list in order, the pending activations' targets |

The comparison is exact, record by record; the summary
`rust-mission-trace.txt` gives the first divergence with the two records on
each side. The port's trace is kept as `rust-mission.jsonl`.

## `mission.jsonl` and `mission_objects.jsonl` (`mission <name> <session>`)

The Tier B mission session with a ship (P3.5c2, decision record 0008
question 9; `Content/oracle_mission_session.cpp`, the same hook), read by
`crates/game-logic/tests/mission_session.rs` with the run's `dispatch.jsonl`
and `session.jsonl`. A scenario run (`CppSource/Oracle/mission_session.txt`:
`RELAYSEVEN_system`, 900 ticks, one ship joined as player 1) whose
`mission Mission_01a 1` line makes, after the players join, player 1's
Content state with the ship as its ship object and the mission as Story
makes one (`ABOVE_NORMAL`, `MissionStart` through the bus at tick 0). The
session's scripted messages go to the mission alone at the start of their
tick's frame: `SpaceEnter` (tick 2) and the `CommComplete`s of
`DX_M01_0000_FREEPORT_7_BRIDGE` (200), `DX_M01_0005_Samura_SA_14` (260) and
`DX_M01_0010_FREEPORT_7_BRIDGE` (320). `session.jsonl` gains a `mission`
record (`tick`, `line`, `name`, `session`, `controller`) and, at the end, a
`mission_session` record (`records`).

`mission.jsonl` holds the records of `dump mission_trace` with a `message`
record before every delivery the bus makes to the mission (heartbeats and
broadcasts included), and in place of `final`, when the bus has torn the
mission down:

| `rec` | Fields | Written |
| --- | --- | --- |
| `gone` | `tick` | the session's end, the mission no longer listed |

`mission_objects.jsonl` holds, after each frame while the mission is
listed, one record per live mission NPC ship (the mission's map order),
spawned `[MsnSolar]` and spawned `[MsnLoot]` whose `GetLocation` succeeds:

| `rec` | Fields | Written |
| --- | --- | --- |
| `obj` | `tick` u, `kind` s (`npc`, `solar`, `loot`), `name` s (the nickname), `id` u, `pos` [u, u, u] (each coordinate's float bits) |

The comparison (`rust-mission-session.txt`): the trace exactly, record by
record, and the deliveries from the first one to the mission on as `(tick,
depth, controller, message)` with a heartbeat's elapsed time, a controller
named by its place in the bus's list (`session.jsonl`'s `heartbeats`
without the harness's controller of the `server` record); objects by kind
and nickname, ids not compared. Judged up to the first action that runs
without effect (D970) in the port's trace, at tick `J`: the trace through
that record, the deliveries before `J`, the objects within the gross bound
(500 m) through `J`; reported after (rule accepted by the owner on 2026-11-25). The port's records are kept as
`rust-mission-session.jsonl` and `rust-mission-objects.jsonl`. On `outdir/`, `J`
is tick 2: with P3.5c2 at `Act_SetVibe` (18 records), with P3.5c3 at
`Act_CallSceneScript` (30 records), a P3.5c4 action.
## `base.jsonl` (`dump base_session`)
The Tier B base session trace (P3.6e, decision record 0008 points 7 and 10;
`Content/oracle_base_session.cpp`, the hook `OracleBaseDbTrace` of
`Server/server_basedb.cpp`), read by `crates/game-logic/tests/base_session.rs`.
After the multiplayer start-up: player 7 registered in `he01_01_Base`'s
system without a ship or a server slot, a BaseSupport made by
`CreateInstance` on controller id `0x00ff0002` (the generic scripts emptied
first), `BaseDB::load_base` of the base. Ten steps, each from
`srand(0x4f524143 + step)`: `base_enter` (`BaseEnter`), `vendor_offers`
(`RefreshMissionVendorOffers`), one `room` per `[mRoom]` of the base
(`LocationEnter`, `LocationExit`), `bar` (`LocationEnter` of the
bartender's room), `select_bartender` (`GFObjSelect`), `accept_vendor`
(`MissionResponse` accepting the first vendor offer) and `base_exit`
(`LocationExit`, `BaseExit`). Each message goes to GameSupport's case for it,
then to the BaseSupport. One JSON object per line, ids unsigned decimal,
`data` lower-case hexadecimal:
| `step` | `step`, `name` | at a step's start |
| `message` | `step`, `msg` | before each message |
| `create` | `step`, `kind`, `handle`, `filter`, `data` | the base database made a record (`kind` `character`, `behavior`, `ambient`, `vendor` or `news`); `data` its properties as `send_update` marshals them |
| `destroy` | `step`, `kind`, `handle`, `found` | a destroy of a handle, `found` 1 when its map had it |
| `set_behavior` | `step`, `character`, `behavior`, `found` | `set_character_behavior` |
| `acceptance` | `step`, `handle`, `accepted`, `session`, `data` | a vendor offer's answer, `data` the marshalled `MissionVendorAcceptance` |
| `why_empty` | `step`, `client`, `reason` | `pub::GF::ReportWhyMissionVendorEmpty` |
| `state` | `step`, `draws`, `location`, `visit`, `rooms` | a step's end: the draws since its seeding (-1 beyond 0x10000 or after a reseed), the player's room, his visit (`null`, or `news` handles and `vendor` `[handle, offer id]`s) and the base's room states by id, each with `locks` `[player, placement, count]`, `characters` `[id, behaviour, uses, placement, role, player, talks, offer kind]` in list order and `refs` (character ids) |
The comparison is exact, record by record, to the first difference, but in
step `accept_vendor` past its `acceptance` record: the `DestroyMission`
controller the acceptance makes runs in the C++ and not in the port (D955),
so the rest of that step is compared and listed under "D955" in the summary
`rust-base-session.txt` without being judged (accepted by the owner on 2026-11-25 until P3.7b, which ports the `DestroyMission` constructor, makes the step exact and drops the exception). The port's trace is kept as
`rust-base.jsonl`.

## `spread.jsonl` (`--runs N`)

| `rec` | Fields |
| --- | --- |
| `runs` | `scenario` s, `runs` i, `completed` i, `exit_codes` [i, …] |
| `object` | `obj` s, `ticks` u, `alive_mismatch_ticks` u, then one object per metric, `pos`, `orient`, `vel`, `angvel`, `hp`, each {`max` d, `max_tick` u, `first_tick` u\|null, `final` d} |
| `event` | `kind` s, `obj` s, `present` u (runs with the event), `of` u (completed runs), `first_min` u, `first_max` u, `spread` u (ticks) |

For each object and tick the spread of a metric is its largest difference
between any two completed runs (the determinism check of record 0003's envelope method): the distance between
positions, velocities and angular velocities, the largest element
difference of the orientation matrices, the hit-point difference; ticks at
which the object is not alive in every run are counted in
`alive_mismatch_ticks` and left out. `max` is the largest spread over the
run and `max_tick` its tick, `first_tick` the first tick with a spread
above zero (the start of divergence), `final` the spread at the last tick.
An event's spread is the difference between its earliest and latest first
occurrence (`kind` `collision` keyed by object `a`, `munition_hit` by the
target, `dock` by the ship, `destroyed` by the object; other objects by
`#<id>`). `spread.txt` says the same in decimal.

## The seed ensemble (`--seed S`)

Gate G2's seed ensemble (record 0003 point 9, owner 2026-11-19): the data
job runs each scenario of `crates/game-sim/tests/fixtures/g2_ensemble.txt`
once per seed listed there, as `Oracle.exe sim <data root> <scenario>
<ensemble>\seed_<S>\<scenario> --seed S`. Each run's folder holds the
records of a single run; the job keeps only those the comparison reads
(`tick.jsonl`, `events.jsonl`, the `munition_hit` and `collision_damage`
records of `calls.jsonl`, the first 2,000 lines of `ai.jsonl`,
`session.jsonl`, `load_rand.txt`). A run records its seed in the `start`
and `server` records (`reseed`) and in `load_rand.jsonl`'s `reseed`
record. The Rust test (`scenario_replay`'s `g2_seed_ensemble`, with
`OUTPOST_ENSEMBLE_DIR` set to `<ensemble>`) writes `rust-ensemble.txt`
there: per scenario and seed the first contact or impact on both sides,
the tick at which the seed's run leaves the first seed's, each object's
outcome (survived, kill tick, munition hits taken, collision events, final
hull and shield) on both sides and the seed's judgement; then per object
and metric both sides' distributions (median, quartiles, range), the
median difference against its margin and the exact two-sample
Kolmogorov–Smirnov test (Fisher's exact test for the survival counts).
