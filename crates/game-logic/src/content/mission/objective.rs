//! The `[NNObjective]` sections (TU38, `Content/MissionObject.cpp`
//! `LoadNNObjective`, `:841-924`, and the objectives' `Parse` slots,
//! `:390-760`): the objectives of the navigation map, types 1 to 8, as one
//! [`Objective`] whose [`kind`](Objective::kind) says which fields its C++
//! class has.

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::condition::is_numeric_token;
use super::{base_id_of, f32_at, system_id_into, tstring, value_tstring};
use crate::server::Cx;

/// One objective (`FMissionObjective` and its eight classes,
/// `MissionObject.h:314-480`). The fields a type does not have stay at
/// their defaults and are not dumped.
#[derive(Clone, Debug, PartialEq)]
pub struct Objective {
    /// `GetType`: 1 base, 2 navmarker, 3 ids, 4 rep_inst, 5 to 8.
    pub kind: i32,
    /// `bHidden`: `hidden`, or `state = HIDDEN`.
    pub hidden: bool,
    /// `Nickname`: `atoi` of the `nickname` value, or `CreateID` of it when
    /// that is 0.
    pub nickname: u32,
    /// `Ids`: the mad lib.
    pub ids: i32,
    /// `Index` (-1).
    pub index: i32,
    /// `Objective.Type`: `state` (a number, or 10 for a name).
    pub state: u32,
    /// Type 1: the base.
    pub base_id: u32,
    /// Types 2, 4, 5 and 6: the nav marker (system, an int, x, y, z bits).
    pub marker: [u32; 5],
    /// Type 4: value 7.
    pub label: Vec<u8>,
    /// Types 5 and 6: value 3 (-1).
    pub rep_instance: i32,
    /// Types 5 and 6: value 2.
    pub system_id: u32,
}

impl Objective {
    /// `CreateMissionObjective(kind)` with the base part's values: hidden
    /// 0, no nickname, ids 0, index -1, state 4.
    #[must_use]
    pub fn new(kind: i32) -> Self {
        Self {
            kind,
            hidden: false,
            nickname: 0,
            ids: 0,
            index: -1,
            state: 4,
            base_id: 0,
            marker: [0; 5],
            label: Vec::new(),
            rep_instance: -1,
            system_id: 0,
        }
    }
}

/// `ParseSystemValue`: value 1 a system id number, or a system nickname.
fn system_value(cx: &mut Cx<'_>, reader: &IniReader, slot: &mut u32) {
    let text = value_tstring(reader, 1, 0x40);
    if is_numeric_token(&text) {
        *slot = reader.value_int(1) as u32;
    } else {
        system_id_into(cx, slot, &text);
    }
}

/// `ParsePosition`: values 5, 4, 6 read, stored x = 4, y = 5, z = 6.
fn position(reader: &mut IniReader, marker: &mut [u32; 5]) {
    let y = f32_at(reader, 5);
    let x = f32_at(reader, 4);
    let z = f32_at(reader, 6);
    marker[2] = x.to_bits();
    marker[3] = y.to_bits();
    marker[4] = z.to_bits();
}

/// Slot 1 (`Parse`) of the objective's class: the rest of the `type` line.
fn parse(cx: &mut Cx<'_>, reader: &mut IniReader, objective: &mut Objective) {
    match objective.kind {
        1 => {
            let text = value_tstring(reader, 1, 0x40);
            objective.base_id = if is_numeric_token(&text) {
                reader.value_int(1) as u32
            } else {
                base_id_of(cx, &text)
            };
            objective.ids = reader.value_int(2);
        }
        2 | 4 => {
            system_value(cx, reader, &mut objective.marker[0]);
            objective.ids = reader.value_int(2);
            objective.marker[1] = reader.value_int(3) as u32;
            position(reader, &mut objective.marker);
            if objective.kind == 4 {
                objective.label = tstring(&reader.value_string_at(7), 0x30);
            }
        }
        3 => objective.ids = reader.value_int(1),
        5 | 6 => {
            // `ParseType56`: the position read 7, 8, 6 and stored in
            // reverse (value 8 first).
            objective.ids = reader.value_int(1);
            objective.system_id = reader.value_int(2) as u32;
            objective.rep_instance = reader.value_int(3);
            objective.marker[1] = reader.value_int(4) as u32;
            objective.marker[0] = reader.value_int(5) as u32;
            let seven = f32_at(reader, 7);
            let eight = f32_at(reader, 8);
            let six = f32_at(reader, 6);
            objective.marker[2] = eight.to_bits();
            objective.marker[3] = seven.to_bits();
            objective.marker[4] = six.to_bits();
        }
        _ => {}
    }
}

/// `LoadNNObjective`: one `[NNObjective]` section. A `type` makes a new
/// objective (a second one replaces the first, which the C++ leaks); a
/// number outside 1 to 8 before any other type calls through null in the
/// C++, and the port makes none (D813). The section's `nickname`, `state`
/// and `hidden` go to the objective last; `None` without one.
pub fn load(cx: &mut Cx<'_>, reader: &mut IniReader) -> Option<Objective> {
    let mut nickname = 0u32;
    let mut hidden = false;
    let mut objective: Option<Objective> = None;
    // The C++ leaves this local unset; every shipped section has a state.
    let mut state = 0u32;
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            nickname = neuron_core::msvcrt::atoi(&reader.value_string_at(0)) as u32;
            if nickname == 0 {
                nickname = create_id(&reader.value_string_at(0));
            }
        } else if reader.is_value(b"type") {
            let text = value_tstring(reader, 0, 0x40);
            let kind = if is_numeric_token(&text) {
                match reader.value_int(0) {
                    0 => continue,
                    kind => kind,
                }
            } else {
                let name = reader.value_string_at(0);
                let name = c_str(&name);
                if eq_ignore_case(name, b"navmarker") {
                    2
                } else if eq_ignore_case(name, b"base") {
                    1
                } else if eq_ignore_case(name, b"ids") {
                    3
                } else if eq_ignore_case(name, b"rep_inst") {
                    4
                } else {
                    continue;
                }
            };
            if (1..=8).contains(&kind) {
                objective = Some(Objective::new(kind));
            }
            if let Some(o) = objective.as_mut() {
                parse(cx, reader, o);
            }
        } else if reader.is_value(b"state") {
            let text = value_tstring(reader, 0, 0x40);
            if is_numeric_token(&text) {
                state = reader.value_int(0) as u32;
            } else {
                state = 10;
                let name = reader.value_string_at(0);
                if eq_ignore_case(b"ACTIVE", c_str(&name)) {
                    hidden = false;
                } else if eq_ignore_case(b"HIDDEN", c_str(&name)) {
                    hidden = true;
                }
            }
        } else if reader.is_value(b"hidden") {
            hidden = reader.value_bool(0);
        }
    }
    let mut objective = objective?;
    objective.nickname = nickname;
    objective.state = state;
    objective.hidden = hidden;
    Some(objective)
}
