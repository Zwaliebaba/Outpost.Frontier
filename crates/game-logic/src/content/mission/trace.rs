//! The Tier B mission trace (decision record 0008, question 9 and the
//! Verification section; work package P3.5c1): what the trigger engine
//! did, record by record, in the order it did it.
//!
//! The C++ side writes the same records through the oracle hook of
//! `Content/MissionScript.cpp` (`GOracleMissionTraceHook`), which
//! `Content.dll`'s `OracleDumpMissionTrace` (`Content/oracle_mission_trace.cpp`)
//! sets for the session `CppSource/Oracle/mission_trace.txt`. Both sides
//! write one JSON object per line (`mission.jsonl`):
//!
//! | `rec` | fields | where |
//! | --- | --- | --- |
//! | `message` | `tick`, `msg` | the session hands the mission a message |
//! | `activate` | `tick`, `trigger` | `ActivateMissionTrigger` sets a trigger active |
//! | `fire` | `tick`, `trigger`, `event` | a condition matched (`event` 4294967295 for the activation probe) |
//! | `action` | `tick`, `trigger`, `index` | an action of the firing trigger runs |
//! | `timer` | `tick`, `trigger`, `index` | a delayed action runs from the timer list |
//! | `rearm` | `tick`, `trigger` | a repeatable trigger is armed again |
//! | `deactivate` | `tick`, `trigger` | `DeactivateMissionTrigger` drops an active trigger |
//! | `state` | `tick`, `from`, `to` | `ApplyDesiredMissionState` applies a new state |
//! | `final` | `tick`, `active`, `pending` | the session's end: the active triggers in order and the pending activations |
//!
//! Ids are the `CreateID` values, written as unsigned decimal numbers; an
//! action is named by its trigger and its place in the trigger's list.
//! The comparison is exact, record by record, up to the first difference
//! (record 0008 question 9: the dispatch order to the first difference; the
//! session spawns nothing whose position an envelope would judge).

use std::fmt::Write as _;

/// One record of the trace.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TraceRecord {
    /// The session handed the mission message `msg`.
    Message {
        /// The session's tick.
        tick: u32,
        /// The message id.
        msg: u32,
    },
    /// A trigger became active.
    Activate {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
    },
    /// A trigger's condition matched `event`.
    Fire {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
        /// The event id (`u32::MAX` for the activation probe).
        event: u32,
    },
    /// The action at `index` of `trigger` ran in the trigger's pass.
    Action {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
        /// The action's place in the trigger's list.
        index: u32,
    },
    /// The action at `index` of `trigger` ran from the timer list.
    Timer {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
        /// The action's place in the trigger's list.
        index: u32,
    },
    /// A repeatable trigger was armed again.
    Rearm {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
    },
    /// An active trigger was deactivated.
    Deactivate {
        /// The session's tick.
        tick: u32,
        /// The trigger.
        trigger: u32,
    },
    /// The mission's state changed.
    State {
        /// The session's tick.
        tick: u32,
        /// The previous state.
        from: i32,
        /// The new state.
        to: i32,
    },
    /// The session's end.
    Final {
        /// The session's tick.
        tick: u32,
        /// The active triggers, in list order.
        active: Vec<u32>,
        /// The pending activations' targets, in key order.
        pending: Vec<u32>,
    },
}

impl TraceRecord {
    /// The record as one `mission.jsonl` line, without the newline, in the
    /// export's field order.
    #[must_use]
    pub fn to_json(&self) -> String {
        match self {
            Self::Message { tick, msg } => {
                format!(r#"{{"rec":"message","tick":{tick},"msg":{msg}}}"#)
            }
            Self::Activate { tick, trigger } => {
                format!(r#"{{"rec":"activate","tick":{tick},"trigger":{trigger}}}"#)
            }
            Self::Fire {
                tick,
                trigger,
                event,
            } => format!(r#"{{"rec":"fire","tick":{tick},"trigger":{trigger},"event":{event}}}"#),
            Self::Action {
                tick,
                trigger,
                index,
            } => format!(r#"{{"rec":"action","tick":{tick},"trigger":{trigger},"index":{index}}}"#),
            Self::Timer {
                tick,
                trigger,
                index,
            } => format!(r#"{{"rec":"timer","tick":{tick},"trigger":{trigger},"index":{index}}}"#),
            Self::Rearm { tick, trigger } => {
                format!(r#"{{"rec":"rearm","tick":{tick},"trigger":{trigger}}}"#)
            }
            Self::Deactivate { tick, trigger } => {
                format!(r#"{{"rec":"deactivate","tick":{tick},"trigger":{trigger}}}"#)
            }
            Self::State { tick, from, to } => {
                format!(r#"{{"rec":"state","tick":{tick},"from":{from},"to":{to}}}"#)
            }
            Self::Final {
                tick,
                active,
                pending,
            } => {
                let mut line = format!(r#"{{"rec":"final","tick":{tick},"active":["#);
                list(&mut line, active);
                line.push_str(r#"],"pending":["#);
                list(&mut line, pending);
                line.push_str("]}");
                line
            }
        }
    }
}

fn list(line: &mut String, ids: &[u32]) {
    for (i, id) in ids.iter().enumerate() {
        if i > 0 {
            line.push(',');
        }
        let _ = write!(line, "{id}");
    }
}

/// A mission's trace: off unless a session turns it on, so a game run
/// records nothing.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Trace {
    /// The session's tick, which every record carries.
    pub tick: u32,
    /// The records so far.
    pub records: Vec<TraceRecord>,
}

impl Trace {
    /// A trace from tick 0.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The trace as `mission.jsonl`'s text.
    #[must_use]
    pub fn to_jsonl(&self) -> String {
        let mut text = String::new();
        for record in &self.records {
            text.push_str(&record.to_json());
            text.push('\n');
        }
        text
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn records_print_as_the_export_writes_them() {
        assert_eq!(
            TraceRecord::Fire {
                tick: 3,
                trigger: 7,
                event: u32::MAX
            }
            .to_json(),
            r#"{"rec":"fire","tick":3,"trigger":7,"event":4294967295}"#
        );
        assert_eq!(
            TraceRecord::Final {
                tick: 9,
                active: vec![1, 2],
                pending: vec![]
            }
            .to_json(),
            r#"{"rec":"final","tick":9,"active":[1,2],"pending":[]}"#
        );
        assert_eq!(
            TraceRecord::State {
                tick: 1,
                from: 2,
                to: 0
            }
            .to_json(),
            r#"{"rec":"state","tick":1,"from":2,"to":0}"#
        );
    }
}
