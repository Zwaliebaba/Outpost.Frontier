//! The `[Trigger]` sections (TU21, `Content/MissionScript.cpp`
//! `LoadTrigger`, `:240-297`, and the `[Trigger]` branch of
//! `FMissionScript::LoadMission`, `:299-334`).
//!
//! A trigger is its state, one condition, its actions, the message ids its
//! condition listens to, the repeatable flag and its id (record 0008 point
//! 6). The run time (activation, the trigger pass) is P3.5c's.

use std::collections::BTreeMap;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::MissionScript;
use super::action::{self, Action};
use super::condition::{self, Condition};
use crate::server::Cx;

/// `G_TriggerStateActive` (`ACTIVE`).
pub const STATE_ACTIVE: i32 = 0;
/// `G_TriggerStateInactive` (`INACTIVE`), a new trigger's state.
pub const STATE_INACTIVE: i32 = 1;
/// `G_TriggerStateComplete` (`COMPLETE`).
pub const STATE_COMPLETE: i32 = 2;

/// One trigger (`FTrigger`, `MissionScript.h:48-57`).
#[derive(Clone, Debug, PartialEq)]
pub struct Trigger {
    /// `State` (inactive).
    pub state: i32,
    /// `Condition`: the last `Cnd_*` line's; an earlier one is dropped, as
    /// the C++ leaks it.
    pub condition: Option<Condition>,
    /// `Actions`, in line order.
    pub actions: Vec<Action>,
    /// `EventIds`: the listen ids of every `Cnd_*` line, in line order (a
    /// second condition adds its ids after the first's).
    pub events: Vec<u32>,
    /// `bRepeatable`.
    pub repeatable: bool,
    /// `Id`: `CreateID(nickname)`.
    pub id: u32,
}

impl Trigger {
    /// The stack trigger of the `[Trigger]` branch: inactive, no
    /// condition, empty lists, not repeatable.
    #[must_use]
    pub fn new(id: u32) -> Self {
        Self {
            state: STATE_INACTIVE,
            condition: None,
            actions: Vec::new(),
            events: Vec::new(),
            repeatable: false,
            id,
        }
    }
}

/// `LoadTrigger`: every value of the rest of the section. A `Cnd_*` key
/// (the factory's chain, `is_value`, first match) replaces the condition
/// and appends its listen ids; an `Act_*` key appends its action;
/// `InitState` is `ACTIVE`, `INACTIVE` or `COMPLETE` (`_stricmp`, anything
/// else leaves the state); `repeatable` true sets the flag (false leaves
/// it); `system` and every other key are skipped.
pub fn load_trigger(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    trigger: &mut Trigger,
    script: &mut MissionScript,
) {
    while reader.read_value() {
        if let Some(index) = condition::FACTORY
            .iter()
            .position(|(name, _)| reader.is_value(name))
        {
            trigger.condition = Some(condition::parse(cx, reader, index));
            trigger
                .events
                .extend_from_slice(condition::FACTORY[index].1);
            continue;
        }
        if let Some(index) = action::FACTORY
            .iter()
            .position(|name| reader.is_value(name))
        {
            let parsed = action::parse(cx, reader, index, script);
            trigger.actions.push(parsed);
            continue;
        }
        if reader.is_value(b"InitState") {
            let name = reader.value_string_at(0);
            let name = c_str(&name);
            if eq_ignore_case(b"ACTIVE", name) {
                trigger.state = STATE_ACTIVE;
            } else if eq_ignore_case(b"INACTIVE", name) {
                trigger.state = STATE_INACTIVE;
            } else if eq_ignore_case(b"COMPLETE", name) {
                trigger.state = STATE_COMPLETE;
            }
            continue;
        }
        if reader.is_value(b"repeatable") && reader.value_bool(0) {
            trigger.repeatable = true;
        }
    }
}

/// The `[Trigger]` branch: `nickname` makes a trigger of `CreateID` of the
/// value and reads the rest of the section into it (a known nickname is
/// skipped, and the values after it are read only for `nickname` and
/// `State`); the trigger goes into the map, and when its state is `ACTIVE`
/// into the initially active ones. `State` is read and dropped.
pub fn load_section(
    cx: &mut Cx<'_>,
    reader: &mut IniReader,
    script: &mut MissionScript,
    triggers: &mut BTreeMap<u32, Trigger>,
    initially_active: &mut BTreeMap<u32, u32>,
) {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            let id = create_id(&reader.value_string_at(0));
            if triggers.contains_key(&id) {
                continue;
            }
            let mut trigger = Trigger::new(id);
            load_trigger(cx, reader, &mut trigger, script);
            if trigger.state == STATE_ACTIVE {
                initially_active.entry(id).or_insert(id);
            }
            triggers.insert(id, trigger);
            continue;
        }
        if reader.is_value(b"State") {
            let _ = action::mission_state_from_name(&reader.value_string_at(0));
        }
    }
}
