# Simulation oracle records: free flight per PSI

The layout of the per-PSI free-flight records that `Oracle.exe sim`
writes (decision record [0003](adr/0003-simulation-oracle.md), point 5)
and that `crates/neuron-core/tests/sim_replay.rs` replays (point 7). The
Rust side is `neuron_core::physics::replay`, whose `PsiRecord::to_line`
writes exactly this layout and whose unit tests round-trip it.

## Files

One file `psi.jsonl` per scenario, anywhere below the oracle's output
folder (the test searches `OUTPOST_ORACLE_DIR` recursively). One line per
recorded core per PSI, in the order the PSIs run. The test writes its
summary to `rust-sim-replay.txt` in that folder.

## Encoding

As every oracle vector ([verification.md](verification.md)): one flat JSON
object per line, keys in the order below, every value a string. An `f32`
is 8 lower-case hex digits of its bit pattern; several floats are
concatenated without separators, in the order given. `u32` words are 8 hex
digits. Keys the test does not know are ignored, so the harness may add
fields (the controller inputs below).

## What is recorded, and where

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

## Keys

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

## The replay

`neuron_core::physics::replay::replay` runs, on the `in_` state: the core
step at `time`, the pushes in order, the standard controller (damping,
commit, zero gravity), the movement check when `check` is `1`, and
`calc_next_PSI_matrix` with `dt`, `idt` and `limits`. Flags, the movement
state and the unit word must be equal; every float within 4e-6 relative to
the larger magnitude (at least absolute). The bound comes from the sines,
cosines and arc cosines the port takes from the Rust standard library
(decision T11); it is tightened once the first oracle runs show the actual
spread.
