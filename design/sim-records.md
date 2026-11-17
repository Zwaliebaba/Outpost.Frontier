# Simulation oracle records

The files `Oracle.exe sim` writes (work package P2.3, decision O23,
[adr/0003](adr/0003-simulation-oracle.md); how to run it in
[reference-build.md](reference-build.md) section 10). The Rust replay and
scenario tests read them. The records are made from the original game data
and are never committed. Record format version 1 (the `version` of the
`start` record).

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
| `f3`, `f4`, `f9`, `f[]` | an array of `f` (3, 4, 9 or any number of elements) |
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
| `start` | `version` u, `scenario` s (path), `data_root` s, `system` s, `system_id` u, `ticks` u, `frame` d (seconds), `stepping` s (`sp` or `mp`), `seed` u, `clock` d (the fixed clock at tick 0), `objects` u, `commands` u |
| `server` | `data_path` s, `controller` u (SimClientStub's controller id), `system_id` u, `system_loaded` b |
| `spawn` | `obj` s, `kind` s (`ship` or `solar`); a ship: `arch` s, `arch_id` u, `loadout` s, `loadout_id` u, `result` i (`pub::SpaceObj::Create`), `id` u, [`state_graph` s, `state_graph_id` i, `submit_state` i]; a solar: `nickname` s, `id` u (`CreateID(nickname)`), `found` b |
| `command` | `tick` u, `line` i (in the scenario file), `obj` s, `cmd` s (the command as written), `ok` b, `result` i (what the called function returned; for a directive the `OP_RTYPE`), [`note` s, why it did not run] |
| `error`, `critical_warning` | `tick` u, `text` s |
| `end` | `tick` u, `psi_total` u, `code` i (the exit code) |

## `tick.jsonl`

| `rec` | Fields |
| --- | --- |
| `frame` | `tick` u, `clock` d, `x87_cw` s, `sse_cw` s (`%08x` of `__control87_2`'s two words at the start of the frame, before the harness sets 53-bit precision again; at tick 0 after spawning), `psi_count` u (PSIs in the frame), `psi_dt` f[] (each PSI's `delta_time`) |
| `object` | `tick` u, `obj` s, `id` u, `alive` b; when alive: `pos` f3, `orient` f9, `vel` f3, `angvel` f3, `hp` f, `hp_max` f (`IObjInspect` of the server object: `get_position`, `get_orientation`, `get_velocity`, `get_angular_velocity`, `get_status`); a ship also: `throttle` f, `axis` f3, `nudge` f3, `strafe` i (`StrafeDir`), `cruise` b, `behavior` i (`CurrentBehaviorIndex`, −1 for none), `state_graph` i (−1 for none), `directive` u3 (`DirectiveStorage`) |

`alive` is false once the object is no longer in its system's object
database (destroyed, docked or removed).

## `events.jsonl`

| `rec` | Fields | Source |
| --- | --- | --- |
| `collision` | `tick` u, `a` u, `a_obj` s\|null, `b` u, `b_obj` s\|null, `damage` f | `GameObject::on_collision` of object `a` (each side of a contact reports itself), with `Collision::CalcCollisionDamage`'s result |
| `munition_hit` | `tick` u, `target` u, `target_obj`, `shooter` u, `shooter_obj`, `munition` u (archetype id), `sub` u (sub-object hit) | `StarSystem::munition_collision` |
| `dock` | `tick` u, `ship` u, `ship_obj`, `dock` u, `dock_obj`, `system` u | the `DockComplete` controller message |
| `destroyed` | `tick` u, `id` u, `obj`, `killer` u, `killer_obj`, `kind` i (the destroy kind), `pos` f3 | the `ObjectDestroyed` controller message |

## `psi.jsonl`

One record per PSI, phase and object marked `psi` in the scenario.

| Field | Kind | Source (`IVP_Core` unless noted) |
| --- | --- | --- |
| `rec` | s | `psi` |
| `tick` | u | |
| `psi` | u | PSI number in the run, from 1 |
| `phase` | s | `in`: before the PSI's simulation units (controllers, then `calc_next_PSI_matrix`); `out`: after `commit_all_calc_next_PSI_matrix`, before hull managers and contacts |
| `obj`, `id` | s, u | |
| `dt` | f | `Environment->delta_time`, the PSI length |
| `time` | f2 | `Environment->current_time` (base, offset) |
| `pos` | f3 | `PosWorldFCore` |
| `q` | f4 | `QWorldFCore` |
| `speed` | f3 | `Speed` (world) |
| `rot_speed` | f3 | `RotSpeed` (core space) |
| `speed_change` | f3 | `SpeedChange` |
| `rot_speed_change` | f3 | `RotSpeedChange` |
| `delta_world` | f3 | `DeltaWorldFCore` |
| `mass`, `inv_mass` | f, f | `Mass`, `InvMass` |
| `inertia`, `inv_inertia` | f3, f3 | `RotInertia`, `InvRotInertia` |
| `speed_damp` | f | `SpeedDamp` |
| `rot_speed_damp` | f3 | `RotSpeedDamp` |
| `upper_limit_radius` | f | `UpperLimitRadius` |
| `speed_limit` | f | `SpeedLimit` |
| `i_delta_time` | f | `IDeltaTime` |
| `time_of_last_psi` | f2 | `TimeOfLastPsi` |
| `movement_state` | u | `MovementState` |
| `flags` | u | `Flags` |
| `spin_clip` | f3\|null | `*SpinClipping` |
| [`ctl`] | {…} | phase `in` of a ship only: the controller inputs below |

`ctl`, from the `CShip` and its equipment: `throttle` f, `axis` f3
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
`strafe_power_usage` f.

## `calls.jsonl`

| `rec` | Fields |
| --- | --- |
| `collision_damage` | `tick` u, `source` u, `source_obj`, `source_sub` u, `target` u, `target_obj`, `target_sub` u, `own_vel` f3, `other_vel` f3, `speed` f, `other_mass` f, `own_mass` f, `damage` f. The call is `Collision::CalcCollisionDamage(speed, other_mass, own_mass)` (its parameters are named `DamageFactor, MaxSpeed, ActualSpeed`) and `damage` its result; `speed` is the relative speed from the two velocities |
| `munition_hit` | `tick` u, `target` u, `target_obj`, `shooter` u, `shooter_obj`, `munition` u, `sub` u, `hit_pos` f3, `damage` {`cause` i, `inflictor` u, `inflictor_owner` u, `destroyed` b, `count` i, `entries` [[`sub` u, `hp` f, `fate` i], …]}: the impact and the `DamageList` the target's `munition_hit` filled, before it is applied |
| `behavior_in` | `tick` u, `seq` u (behaviour tick number, from 1), `id` u, `obj`, `dt` f, `behavior` i, `state_graph` i, `directive` u3, `suppressed` b (`TickSuppressedFlag`); when the owner is set: `pos` f3, `orient` f9, `vel` f3, `angvel` f3, `throttle` f, `axis` f3, `nudge` f3, `strafe` i, `cruise` b. Entry of `IBehaviorManager::UpdateBehaviorTick` |
| `behavior_out` | `tick` u, `seq` u, `id` u, `obj`, `behavior` i, `state_graph` i, `directive` u3. Every exit of the same call |
| `affect` | `tick` u, `beh` u (the `seq` of the behaviour tick in progress, 0 outside one), `src` s (`behavior`, `scenario` for the harness's own commands, `other`), `id` u, `obj`, `slot` i, `m` s, `i` i, `v` f[]\|null. One `IObjAffect` call on a ship: slot 0 `set_throttle` (`v` the throttle), 1 `set_axis_throttle` (`v` f3), 2 `set_nudge_vec` (`v` f3), 3 `set_strafe_dir` (`i` the `StrafeDir`), 13 `fire` (`EqObj::slot_34`: `i` the number of guns, `v` the target position or `null`) |

The `affect` records between a `behavior_in` and its `behavior_out` with
the same `seq` are the commands that behaviour tick issued.

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

## Other files of a run

`server.ini` is the server INI the run used (the master's with the
populators left out), `fdump.txt` every `FDUMP` message (`tick`, flags in
hex, text) and `metrics.txt` the plain-text input of `--runs`
(`oracle_sim.h`). None of them is a record for the tests.

## `spread.jsonl` (`--runs N`)

| `rec` | Fields |
| --- | --- |
| `runs` | `scenario` s, `runs` i, `completed` i, `exit_codes` [i, …] |
| `object` | `obj` s, `ticks` u, `alive_mismatch_ticks` u, then one object per metric, `pos`, `orient`, `vel`, `angvel`, `hp`, each {`max` d, `max_tick` u, `first_tick` u\|null, `final` d} |
| `event` | `kind` s, `obj` s, `present` u (runs with the event), `of` u (completed runs), `first_min` u, `first_max` u, `spread` u (ticks) |

For each object and tick the spread of a metric is its largest difference
between any two completed runs (record 0003 point 9): the distance between
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

## Open: the replay layout of `psi.jsonl`

The P2.3 Rust replay test (`crates/neuron-core/tests/sim_replay.rs`) was written against the layout below,
the harness against the `psi.jsonl` section above. They differ in shape (one record per core and PSI with
`in_`/`out_` keys versus `in` and `out` records), encoding (strings versus numbers and arrays) and content
(the replay also needs the pushes, the movement check, the anomaly limits, the matrix, axis and motion
snapshots). They are being unified; until then neither side is final.

### Replay layout expected by the Rust test

The layout of the per-PSI free-flight records that `Oracle.exe sim`
writes (decision record [0003](adr/0003-simulation-oracle.md), point 5)
and that `crates/neuron-core/tests/sim_replay.rs` replays (point 7). The
Rust side is `neuron_core::physics::replay`, whose `PsiRecord::to_line`
writes exactly this layout and whose unit tests round-trip it.

### Files

One file `psi.jsonl` per scenario, anywhere below the oracle's output
folder (the test searches `OUTPOST_ORACLE_DIR` recursively). One line per
recorded core per PSI, in the order the PSIs run. The test writes its
summary to `rust-sim-replay.txt` in that folder.

### Encoding

As every oracle vector ([verification.md](verification.md)): one flat JSON
object per line, keys in the order below, every value a string. An `f32`
is 8 lower-case hex digits of its bit pattern; several floats are
concatenated without separators, in the order given. `u32` words are 8 hex
digits. Keys the test does not know are ignored, so the harness may add
fields (the controller inputs below).

### What is recorded, and where

A record is one core in one PSI of `IVP_Environment_Simulate_Psi`
(`IVP/ivp_physics/ivp_environment.cpp:661`). Record only cores without a
golem or remote-correction controller (`CObject+0x60` null); those beam
their bodies and are covered by scenario tests.

| Point in the C++ | Recorded |
| --- | --- |
| `IVP_Simulation_Unit_Process_One_Psi_Step` entry, before the core step (`ivp_simulation_unit.cpp:747`) | the `in_` state |
| the movement-check countdown of that unit (`*CooldownTimer == 0` after the decrement, `:779` / `:806`) | `check` |
| every `IVP_Core::push_core_linear`, `push_core_angular_ws`, `push_core_angular_cs` while `EventSim->sim_unit` is that unit (`ivp_core.cpp:1310-1338`) | `pushes`, in call order |
| after `IVP_Calc_Next_PSI_Solver::calc_next_PSI_matrix` for the core (`ivp_calc_next_psi_solver.cpp:118`) | the `out_` state |

### Keys

| Key | Value |
| --- | --- |
| `psi` | PSI counter in the run, decimal |
| `object` | the `CObject` id, 8 hex digits |
| `time` | the environment's `current_time` at the PSI: `time_base`, `time_offset` (2 floats) |
| `dt` | `delta_time` (+0xac), the PSI length, s |
| `idt` | `inv_delta_time` (+0xb0), 1/s |
| `check` | `1` when the movement check came to this unit in this PSI, else `0` |
| `limits` | `IVP_Anomaly_Limits` `MaxVelocity` (m/s), `MaxAngularVelocityPerPsi` (rad) |
| `in_…` | the core state below, before the PSI |
| `pushes` | the pushes, `;`-separated; each is `L`, `A` or `C` (linear, angular world space, angular core space) and 4 floats: the direction or axis x, y, z and the magnitude argument |
| `out_…` | the core state below, after `calc_next_PSI_matrix` |
| `controllers` (optional) | reserved for P2.4: the controller inputs of record 0003 point 5 (throttle, axis throttle, nudge, strafe, cruise state, power, archetype values). The P2.3 test ignores it |

The core state, each key once with the `in_` and once with the `out_`
prefix. Offsets are `IVP_Core`'s (`IVP/ivp_physics/ivp_core.h`); names in
parentheses are the reconstruction's.

| Key | `IVP_Core` | Floats | Unit |
| --- | --- | --- | --- |
| `flags` | +0x00 `Flags` | `u32` | — |
| `movement` | +0x60 `MovementState` | `u32` | — |
| `unit` | `OwningSimUnit->State` (+0x15c, then the unit's flag word) | `u32` | — |
| `ulr` | +0x04 `UpperLimitRadius` | 1 | m |
| `msd` | +0x08 max surface deviation (`SpeedLimit`) | 1 | m |
| `mass` | +0x20 `Mass` | 1 | kg |
| `inv_mass` | +0x40 `InvMass` | 1 | 1/kg |
| `inertia` | +0x14 `RotInertia` | 3 | kg m² |
| `inv_inertia` | +0x34 `InvRotInertia` | 3 | — |
| `rot_damp` | +0x24 `RotSpeedDamp` | 3 | 1/s |
| `speed_damp` | +0x44 `SpeedDamp` | 1 | 1/s |
| `angular_limit` | +0x48 `DAngularLimitFactor` | 1 | 1/m |
| `tlp` | +0x68 `TimeOfLastPsi` (base, offset) | 2 | s |
| `idt` | +0x70 `IDeltaTime` | 1 | 1/s |
| `rot_speed_change` | +0x74 `RotSpeedChange` | 3 | rad/s, core space |
| `speed_change` | +0x84 `SpeedChange` | 3 | m/s, world |
| `rot_speed` | +0x94 `RotSpeed` | 3 | rad/s, core space |
| `speed` | +0xa4 `Speed` | 3 | m/s, world |
| `pos` | +0xb4 `PosWorldFCore` | 3 | m |
| `delta` | +0xc4 `DeltaWorldFCore` | 3 | m/s |
| `q_last` | +0xd4 `QWorldFCore` (x, y, z, w) | 4 | — |
| `q_next` | +0xe4 `QWorldFCoreNext` (x, y, z, w) | 4 | — |
| `m` | +0xf4 `MWorldFCore`: rows 0, 1, 2 (3 floats each, not the 4th), then `vv` | 12 | —, m |
| `axis` | +0x134 rotation axis (`RotAxisOS`) | 3 | — |
| `current_speed` | +0x144 (`Margin`) | 1 | m/s |
| `abs_omega` | +0x148 (`MaxSurfaceDeviation`) | 1 | rad/s |
| `surface_rot_speed` | +0x14c (`BoundingRadius`) | 1 | m/s |
| `tlm` | +0x160 `TimeOfLastMotion` | 2 | s |
| `tllm` | +0x168 `TimeOfLastLongMotion` | 2 | s |
| `q_lm` | +0x170 `QLastMotion` | 4 | — |
| `q_llm` | +0x180 `QLastLongMotion` | 4 | — |
| `pos_lm` | +0x190 `PosLastMotion` | 3 | m |
| `pos_llm` | +0x1a0 `PosLastLongMotion` | 3 | m |
| `spin_clip` | +0x4c `SpinClipping`: `-` when null, else 3 floats | 3 | rad/s |

### The replay

`neuron_core::physics::replay::replay` runs, on the `in_` state: the core
step at `time`, the pushes in order, the standard controller (damping,
commit, zero gravity), the movement check when `check` is `1`, and
`calc_next_PSI_matrix` with `dt`, `idt` and `limits`. Flags, the movement
state and the unit word must be equal; every float within 4e-6 relative to
the larger magnitude (at least absolute). The bound comes from the sines,
cosines and arc cosines the port takes from the Rust standard library
(decision T11); it is tightened once the first oracle runs show the actual
spread.
