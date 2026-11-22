//! The oracle scenarios run through the object world (decision O20,
//! closed loop; moved from `tests/scenario_replay.rs` into the library by
//! decision O45, so that `game-logic` runs the same scenarios through the
//! server's hooks).
//!
//! * [`Scenario`] and [`parse`]: the scenario files of
//!   `CppSource/Oracle/scenarios` ([`scenarios_dir`], [`scenario_files`]),
//!   and [`needs_ai`], why one cannot run yet, and [`needs_server`], why
//!   one runs only through the server's hooks.
//! * [`run`]: the driver. It spawns the scenario's objects in a world made
//!   with [`world_config`], applies its commands and steps the system
//!   through the given [`ServerHooks`](crate::object::world::ServerHooks)
//!   every tick, recording a [`Run`]: the per-tick [`State`]s, the munition
//!   [`Hit`]s, the collision [`Contact`]s, the guided munitions and the
//!   diagnostics the reports read. With
//!   [`NoServerHooks`](crate::object::world::NoServerHooks) it is the
//!   world's own [`World::step`](crate::object::World::step).
//!   [`run_with`] takes a [`ScenarioServer`] instead: its hooks, and the
//!   server's load of the system, its part once the objects are spawned
//!   and after each tick's step (P3.3's replication comparison).
//! * The oracle's records of a run: [`oracle_run`], [`oracle_ticks`],
//!   [`oracle_hits`], [`oracle_guided`], [`oracle_guided_cores`] and the
//!   [`AiInputs`] the AI ships take from it ([`oracle_ai_inputs`]); the
//!   readers they use are [`records`] (the JSON subset of `oracle_json.h`)
//!   and [`ai_records`] (`ai.jsonl`).
//! * The judges (record 0003 points 8 and 9): [`judge_exact`],
//!   [`judge_contact`] and [`judge_fight`], bit for bit
//!   ([`same_state`]) up to the first contact or munition impact
//!   ([`first_event`]); [`report_object_errors`] and [`report_guided`],
//!   which report and judge; and [`judge`], which applies all of them as
//!   `scenario_replay` does.

pub mod ai_records;
pub mod directives;
pub mod records;

mod driver;
mod file;
mod judge;
mod oracle;

pub use driver::{
    Contact, GuidedState, Hit, Run, ScenarioServer, ShieldState, State, TurretProbe, UNIT_PSIS,
    Unit, gun_text, run, run_with, world_config,
};
pub use file::{
    Cmd, ObjDesc, Scenario, needs_ai, needs_server, parse, scenario_files, scenarios_dir,
    spawn_orientation,
};
pub use judge::{
    EXACT_SCENARIOS, GROSS_POSITION, dist, first_event, first_event_of, fmt_contact, judge,
    judge_contact, judge_exact, judge_fight, report_guided, report_object_errors, same_state,
};
pub use oracle::{
    AiInputs, oracle_ai_inputs, oracle_guided, oracle_guided_cores, oracle_hits, oracle_run,
    oracle_ticks,
};
