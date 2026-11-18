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
| `start` | `version` u, `scenario` s (path), `data_root` s, `system` s, `system_id` u, `ticks` u, `frame` d (seconds), `stepping` s (`sp` or `mp`), `seed` u, `clock` d (the fixed clock at tick 0), `objects` u, `commands` u |
| `server` | `data_path` s, `controller` u (SimClientStub's controller id), `system_id` u, `system_loaded` b |
| `spawn` | `obj` s, `kind` s (`ship` or `solar`); a ship: `arch` s, `arch_id` u, `loadout` s, `loadout_id` u, `result` i (`pub::SpaceObj::Create`), `id` u, [`state_graph` s, `state_graph_id` i, `submit_state` i]; a solar: `nickname` s, `id` u (`CreateID(nickname)`), `found` b |
| `command` | `tick` u, `line` i (in the scenario file), `obj` s, `cmd` s (the command as written), `ok` b, `result` i (what the called function returned; for a directive the `OP_RTYPE`), [`note` s, why it did not run] |
| `error`, `critical_warning`, `note` | `tick` u, `text` s (`note`: an object marked `psi` whose PSIs are not recorded, and why; once per object) |
| `end` | `tick` u, `psi_total` u, `code` i (the exit code) |

## `tick.jsonl`

| `rec` | Fields |
| --- | --- |
| `frame` | `tick` u, `clock` d, `x87_cw` s, `sse_cw` s (`%08x` of `__control87_2`'s two words at the start of the frame, before the harness sets 53-bit precision again; at tick 0 after spawning), `psi_count` u (PSIs in the frame), `psi_dt` f[] (each PSI's `delta_time`) |
| `object` | `tick` u, `obj` s, `id` u, `alive` b; when alive: `pos` f3, `orient` f9, `vel` f3, `angvel` f3, `hp` f, `hp_max` f (`IObjInspect` of the server object: `get_position`, `get_orientation`, `get_velocity`, `get_angular_velocity`, `get_status`); then (P2.4, the scenario report) `power` f, `power_max` f (`CEqObj::Power`, `MaxPower`) and `shield` {`id` u, `hp` f, `hp_max` f, `online` b (`bOnline`), `functioning` b}\|null (the first `CEShield`: `GetHitPoints`, `GetMaxHitPoints`, `IsFunctioning`); a ship also: `throttle` f, `axis` f3, `nudge` f3, `strafe` i (`StrafeDir`), `cruise` b, `behavior` i (`CurrentBehaviorIndex`, −1 for none), `state_graph` i (−1 for none), `directive` u3 (`DirectiveStorage`) |

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

The per-PSI free-flight records (record 0003 point 5) that
`crates/neuron-core/tests/sim_replay.rs` replays (point 7) through
`neuron_core::physics::replay`, whose `PsiRecord::to_line` writes the same
layout (without `ctl`) and whose unit tests round-trip it.

One `psi` record per core and PSI, in the order the PSIs run, for each
object marked `psi` in the scenario whose simulation unit ran in that PSI
(none while the unit is asleep). Not recorded, with a `note` in
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

`crates/game-sim/tests/controller_replay.rs` rebuilds the controllers from
`ctl` (the steering, nudge, strafe, thruster and engine controllers of
`game_sim::equip::flight`), runs them for the PSI in IVP's order (the last
registered first: strafe, nudge, steering, the thrusters from the last, the
engine) and compares their pushes with `pushes`: the same kinds, every
float within 1e-6 relative to the larger magnitude; pushes that only
differ in order are counted but pass. Records without the P2.4 fields use
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
| `affect` | `tick` u, `beh` u (the `seq` of the behaviour tick in progress, 0 outside one), `src` s (`behavior`, `scenario` for the harness's own commands, `other`), `id` u, `obj`, `slot` i, `m` s, `i` i, `depth` i, `v` f[]\|null. One `IObjAffect` call on a ship: slot 0 `set_throttle` (`v` the throttle), 1 `set_axis_throttle` (`v` f3), 2 `set_nudge_vec` (`v` f3), 3 `set_strafe_dir` (`i` the `StrafeDir`), 13 `fire` (`EqObj::slot_34`: `i` the number of guns, `v` the target position or `null`), 18 `set_target` (`i` the target's id, 0 for none; `v` [the sub-target]), 19 `activate_equip` (`EqObj::slot_4c`: `i` the equipment id, `v` [1 to activate, 0 to deactivate]), 20 `engage_cruise` (`Ship::slot_50`: `i` 1 to engage, 0 to leave; `v` [the second flag, 1 or 0]), 21 `set_thrusters` (`Ship::slot_54`: `i` 1 on, 0 off). `depth` (P2.5) is the number of these calls in progress around this one (`FOracleSimAffectScope`): 0 for a call the caller made, 1 or more for one an `IObjAffect` body makes in turn (`set_strafe_dir` and `slot_54` switch the engine on through `slot_4c`, `slot_50` switches the thrusters off through `slot_54`); records written before the field read as 0 |

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
| `ai_in` | `tick` u, `seq` u, `id` u, `obj` s\|null, `dt` f, then the manager fields below, then `targets` [u, …] (the ids of the system manager's target list, `Sys->Timers.Targets`, built by the previous tick) |
| `personality` | `tick` u, `seq` u, `id` u, `obj`, `bytes` s: the 0x3d8 bytes of `Sys->PersonalityData` (`pub::AI::Personality`) as hex; `game_sim::ai::personality::Personality::from_bytes` reads them |
| `ai_obj` | `tick` u, `seq` u, `id` u, `obj`, `roles` [s, …], then either `fault` u (the SEH code; the read faulted) or the object fields below |
| `ai_query` | `tick` u, `seq` u, `kind` s (`ray` `PhySys::FindRayCollisions`, `ray_object` `FindRayIntersect`, `sphere` `FindSphereCollisions`), `system` u (0 for `ray_object`), `object` u\|null (the body `ray_object` tests), `from` f3, `to` f3 (the centre twice for a sphere), `radius` f (0 for rays), `max` i, `hits` [[`id` u, `point` f3, `normal` f3, `part` u], …] (a sphere hit is [`id`]) |
| `ai_out` | `tick` u, `seq` u, `id` u, `obj`, the manager fields below, `rand` [i, …] (every `rand()` value the game DLLs drew during the tick, in order), `rand_mod` [s, …] (the DLL that drew each) |

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

Roles of an object: `self`; `target` (`get_target`); `formation_leader`,
`follow_leader`, `follower` (`get_formation_leader`, `get_follow_leader`,
`get_formation_followers`, at most 16); `dock_target` (`get_dock_target`
through `IObjDB::FindById`); `in_sights`; `candidate` (the target list);
`near` (every ship, solar and loot of the system within `near_radius` of
the ship's position, walked with `IObjDB::GetNext`, mask `0x107ffdff`);
`query` (found by a query of this tick). An object has every role that
applies, in that order.

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
`dock_target` u\|null, `base` u\|null, `rank` u\|null, `affiliation`
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

The `rand` values come from a counting wrapper that the recorder puts in
the import address table of each game DLL: it calls the C runtime's own
`rand`, so the sequence is the one an unrecorded run draws, and records
the value while a behaviour tick is open. A replay feeds them back through
`game_sim::ai::world::RecordedRand` and checks that the port drew exactly
as many.

`crates/game-sim/tests/behavior_replay.rs` (reader in
`tests/support/ai_records.rs`) reruns every tick through
`game_sim::ai::manager::BehaviorManager`. It keeps one Rust manager per
object across the file, so what the records do not hold (the behaviours'
own fields, the thrust manager's caches) carries over from the Rust
side's previous tick; before each tick it sets the manager fields of
`ai_in`, the target list and the personality, and builds a world from the
`ai_obj` snapshot (object ids as handles, the ship's own state following
its commands, the ship's scan list from its next tick's `ai_in.targets`
(the list this tick built), the queries answered from `ai_query`, `rand()` from
`ai_out`'s values). It compares the manager fields of `ai_out` bit for
bit (with `pathfind` and `thrust` where the record has them), the number
of `rand()` draws, and the commands with the `affect` records whose `beh`
is the tick's `seq` and whose `depth` is 0 (slots 0 to 3, 13 and 18 to
21, the slots `affect` records; floats bit for bit). Before each tick it
also sets the `pathfind` and `thrust` fields of `ai_in`. The ship's motion
model (slots 50 to 64), which no record holds, comes from the scenario's
ship spawned into a `World` over `outdir/`; the snapshot's own state
follows the tick's commands as the `IObjAffect` bodies change it, nested
calls included. A tick whose behaviour on entry or
exit is not ported is counted as skipped by behaviour id, and so is a
mismatching tick whose graph row has a candidate that is not ported (its
poll is a stand-in). Its summary is `rust-behavior-replay.txt`.

Not recorded: objects a behaviour looks up by an id it keeps in its own
state and that have none of the roles above (they are outside the near
radius and were not found by a query); the notification queue; the
behaviours' own state blocks. The first behaviour replays will show
whether the read set needs another role.

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

`ai.jsonl` (above) holds the behaviour ticks (P2.5). `server.ini` is the server INI the run used (the master's with the
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
