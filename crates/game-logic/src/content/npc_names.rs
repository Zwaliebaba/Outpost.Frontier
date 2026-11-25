//! The faction name reservations and the part of the NPC name builder the
//! random-mission generator uses (TU48 `Content/FactionProps.cpp:40-700`,
//! TU42 `Content/NpcNames.cpp:142-264` and the name readers of
//! `Content/NpcShips.cpp:240-330`).
//!
//! Record 0008 gives the name builder to P3.6c; P3.7a takes only what an
//! assassination offer needs ahead of it (decision O48): the reservations
//! of the pilot names, formation designations and large ship names
//! ([`FactionNameUses`], in Content's state because the C++ keeps them in
//! the faction's `FFactionNames`), the picks that reserve them, the
//! immediate releases, `BuildNpcName` for a default request (a named
//! leader without a ship type: `0x3f24` and `0x3f23`), and the four name
//! readers. The delayed releases with their timers, the ship-type names and
//! the rest of the reputation handles stay with P3.6c's follow-up; P3.6c
//! brought the reputation handle's destructor as far as the ships need it
//! ([`release_name_reputation`]).

use std::collections::BTreeMap;

use game_sim::protocol::fmtstr::{FmtStr, FmtValue};

use crate::api;
use crate::content::tables::FactionNames;
use crate::content::tables::factions::NameList;
use crate::server::Cx;

/// A reserved pair (`FFactionNamePairUse`): a pilot's first and last
/// name, or a formation designation and its number.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PairUse {
    /// `First`.
    pub first: i32,
    /// `Second`.
    pub second: i32,
    /// `UseCount`.
    pub count: i32,
}

/// One faction's reservations (`FFactionNames` `+0xa4..+0xd4`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NameUses {
    /// `LargeShipNameUses`: a name and its use count.
    pub large_ship: Vec<(i32, i32)>,
    /// `PilotNameUses`.
    pub pilot: Vec<PairUse>,
    /// `FormationDesigUses`.
    pub formation_desig: Vec<PairUse>,
}

/// The reservations of every faction, by the `GFactionNames` key.
pub type FactionNameUses = BTreeMap<u32, NameUses>;

fn find_pair(uses: &[PairUse], first: i32, second: i32) -> Option<usize> {
    uses.iter()
        .position(|u| u.first == first && u.second == second)
}

/// `AddPairUse`: the pair's count up, or the pair appended with 1.
fn add_pair(uses: &mut Vec<PairUse>, first: i32, second: i32) {
    match find_pair(uses, first, second) {
        Some(i) => uses[i].count += 1,
        None => uses.push(PairUse {
            first,
            second,
            count: 1,
        }),
    }
}

/// `ReleasePairUse`: the pair's count down, the pair erased at 0.
fn release_pair(uses: &mut Vec<PairUse>, first: i32, second: i32) {
    if let Some(i) = find_pair(uses, first, second) {
        uses[i].count -= 1;
        if uses[i].count == 0 {
            uses.remove(i);
        }
    }
}

/// `FactionRandIndex`: `rand() * count / 32768`.
fn rand_index(cx: &mut Cx<'_>, count: i32) -> i32 {
    cx.world.rng().rand().wrapping_mul(count) / 32768
}

/// `FFactionNameList::GetName`; the C++ reads past the ranges without a
/// bound, the port answers 0 there (D874).
fn name_at(list: &NameList, index: i32) -> i32 {
    list.name(index as u32).unwrap_or(0)
}

/// `a % b` as the C++ computes it; a zero divisor, which the C++ never
/// reaches with a pair count above the uses, answers 0.
fn rem(a: i32, b: i32) -> i32 {
    if b == 0 { 0 } else { a.wrapping_rem(b) }
}

/// `FFactionNames::PickPilotName` (`FactionProps.cpp:457-566`): a free
/// first and last name pair from random starts, reserved when asked. With
/// only `want_last`, a free last name under the key `{1, last}`.
pub fn pick_pilot_name(
    cx: &mut Cx<'_>,
    names: &FactionNames,
    uses: &mut NameUses,
    want_first: bool,
    want_last: bool,
    gender: i32,
    reserve: bool,
) -> (bool, i32, i32) {
    let first_names = if gender == 1 {
        &names.male_first_names
    } else {
        &names.female_first_names
    };
    let first_count = first_names.count;
    let last_count = names.last_names.count;
    let mut first = 0;
    let mut last = 0;
    let result;
    if (uses.pilot.len() as u32) < (last_count.wrapping_mul(first_count) as u32) {
        if want_first && want_last {
            let mut outer = 0;
            let first_start = rand_index(cx, first_count);
            if first_count > 0 {
                loop {
                    first = name_at(first_names, rem(first_start + outer, first_count));
                    let mut inner = 0;
                    let last_start = rand_index(cx, last_count);
                    loop {
                        last = name_at(&names.last_names, rem(last_start + inner, last_count));
                        if find_pair(&uses.pilot, first, last).is_none() {
                            break;
                        }
                        let previous = inner;
                        inner += 1;
                        if !(previous < last_count) {
                            break;
                        }
                    }
                    if inner != last_count {
                        break;
                    }
                    outer += 1;
                    if !(outer < first_count) {
                        break;
                    }
                }
            }
            result = outer < first_count;
        } else if !want_first && want_last {
            first = 1;
            let mut inner = 0;
            let start = rand_index(cx, last_count);
            loop {
                last = name_at(&names.last_names, rem(start + inner, last_count));
                if find_pair(&uses.pilot, 1, last).is_none() {
                    break;
                }
                let previous = inner;
                inner += 1;
                if !(previous < last_count) {
                    break;
                }
            }
            result = inner != last_count;
        } else {
            last = 1;
            let mut inner = 0;
            let start = rand_index(cx, last_count);
            loop {
                first = name_at(first_names, rem(start + inner, last_count));
                if find_pair(&uses.pilot, first, 1).is_none() {
                    break;
                }
                let previous = inner;
                inner += 1;
                if !(previous < last_count) {
                    break;
                }
            }
            result = inner != last_count;
        }
    } else {
        first = if want_first {
            let i = rand_index(cx, first_count);
            name_at(first_names, i)
        } else {
            1
        };
        if want_last {
            let i = rand_index(cx, last_count);
            last = name_at(&names.last_names, i);
        } else {
            first = 1;
        }
        result = false;
    }
    if reserve {
        add_pair(&mut uses.pilot, first, last);
    }
    (result, first, last)
}

/// `FFactionNames::PickFormationDesig` (`FactionProps.cpp:569-616`): a
/// free designation and number (20 per designation) from random starts.
pub fn pick_formation_desig(
    cx: &mut Cx<'_>,
    names: &FactionNames,
    uses: &mut NameUses,
    reserve: bool,
) -> (bool, i32, i32) {
    let count = names.formation_desigs.count;
    let mut desig = 0;
    let mut number = 0;
    if (uses.formation_desig.len() as u32) < (count.wrapping_mul(20) as u32) {
        let mut outer = 0;
        let start = rand_index(cx, count);
        if count > 0 {
            loop {
                desig = name_at(&names.formation_desigs, rem(start + outer, count));
                let mut inner = 0;
                let number_start = cx.world.rng().rand().wrapping_mul(20) / 32768;
                loop {
                    number = (number_start + inner) % 20;
                    if find_pair(&uses.formation_desig, desig, number).is_none() {
                        break;
                    }
                    let previous = inner;
                    inner += 1;
                    if !(previous < 20) {
                        break;
                    }
                }
                if inner != 20 {
                    break;
                }
                outer += 1;
                if !(outer < count) {
                    break;
                }
            }
        }
        if reserve {
            add_pair(&mut uses.formation_desig, desig, number);
        }
        return (true, desig, number);
    }
    let i = rand_index(cx, count);
    desig = name_at(&names.formation_desigs, i);
    number = cx.world.rng().rand().wrapping_mul(20) / 32768;
    if reserve {
        add_pair(&mut uses.formation_desig, desig, number);
    }
    (false, desig, number)
}

/// The name a builder makes (`FNpcNameParams`).
#[derive(Clone, Debug, PartialEq)]
pub struct NpcNameParams {
    /// `+0x00`: the reputation's name.
    pub first: FmtStr,
    /// `+0x18`: the reputation's nickname.
    pub second: FmtStr,
    /// `+0x30`: 1 male, 2 female.
    pub gender: i32,
    /// `+0x34`: written by the builder (-1).
    pub affiliation: u32,
}

impl Default for NpcNameParams {
    fn default() -> Self {
        Self {
            first: FmtStr::new(0),
            second: FmtStr::new(0),
            gender: 0,
            affiliation: u32::MAX,
        }
    }
}

/// `BuildNpcName(affiliation, FNpcNameRequest(), out, named_leader)` for
/// the default request (no ship type, no designation, rank 0)
/// (`NpcNames.cpp:142-264`): for a named leader `0x3f24` with the short
/// group name and a reserved designation, then `0x3f23` with a reserved
/// pilot name. The C++ reads the faction's names without a test; the port
/// builds nothing for a faction without names (D874).
pub fn build_default_npc_name(
    cx: &mut Cx<'_>,
    affiliation: u32,
    out: &mut NpcNameParams,
    named_leader: bool,
) -> bool {
    out.affiliation = affiliation;
    let tables = cx.logic.content.tables.clone();
    let Some(names) = tables.faction_names(affiliation) else {
        return true;
    };
    let mut uses = cx
        .logic
        .content
        .state
        .faction_name_uses
        .remove(&affiliation)
        .unwrap_or_default();
    if named_leader {
        out.first.mad_lib_id = 0x3f24;
        out.first.append_string(names.short_group_name);
        let (_, desig, number) = pick_formation_desig(cx, names, &mut uses, true);
        out.first.append_string(desig as u32);
        out.first.append_int(number as u32);
    }
    out.second.mad_lib_id = 0x3f23;
    let (_, first, last) = pick_pilot_name(cx, names, &mut uses, true, true, out.gender, true);
    out.second.append_string(first as u32);
    out.second.append_string(last as u32);
    cx.logic
        .content
        .state
        .faction_name_uses
        .insert(affiliation, uses);
    true
}

/// `NameValue` of the `index`-th value of `s`; the C++ follows the chain
/// without a test, the port answers 0 past its end (D874).
fn value_at(s: &FmtStr, index: usize) -> i32 {
    match s.values.get(index) {
        Some(FmtValue::Value { value, .. }) => *value as i32,
        _ => 0,
    }
}

/// `GetSecondNameValues`: the pilot's first and last name of the second
/// part, `(1, 1)` and false for a pattern without them.
#[must_use]
pub fn second_name_values(second: &FmtStr) -> (bool, i32, i32) {
    match second.mad_lib_id {
        0x3f18 | 0x3f21 | 0x3f22 => (true, value_at(second, 1), value_at(second, 2)),
        0x3f23 => (true, value_at(second, 0), value_at(second, 1)),
        0x3f1e => (true, 1, value_at(second, 3)),
        _ => (false, 1, 1),
    }
}

/// `GetSecondNameThirdValue`: the large ship name of `0x3f1a` and
/// `0x3f1c`, else 1 and false.
#[must_use]
pub fn second_name_third_value(second: &FmtStr) -> (bool, i32) {
    match second.mad_lib_id {
        0x3f1a | 0x3f1c => (true, value_at(second, 2)),
        _ => (false, 1),
    }
}

/// `GetFirstNameValue`: the formation designation, else 1 and false.
#[must_use]
pub fn first_name_value(first: &FmtStr) -> (bool, i32) {
    match first.mad_lib_id {
        0x3f17 | 0x3f19 | 0x3f1b | 0x3f1d | 0x3f20 => (true, value_at(first, 2)),
        0x3f24 => (true, value_at(first, 1)),
        _ => (false, 1),
    }
}

/// `GetFirstNameLastValue`: the designation's number, else -1 and false.
#[must_use]
pub fn first_name_last_value(first: &FmtStr) -> (bool, i32) {
    match first.mad_lib_id {
        0x3f17 | 0x3f19 | 0x3f1b | 0x3f1d | 0x3f20 => (true, value_at(first, 3)),
        0x3f24 => (true, value_at(first, 2)),
        _ => (false, -1),
    }
}

/// The immediate releases of `FFactionNames::ReleasePilotName`,
/// `ReleaseLargeShipName` and `ReleaseFormationDesig` (`bImmediate`) for
/// a name `name` made: the pilot pair when both values are non-zero, else
/// a large ship name, then always the designation.
pub fn release_name_now(cx: &mut Cx<'_>, name: &NpcNameParams) {
    let uses = cx
        .logic
        .content
        .state
        .faction_name_uses
        .entry(name.affiliation)
        .or_default();
    let (_, pilot_first, pilot_last) = second_name_values(&name.second);
    if pilot_first != 0 && pilot_last != 0 {
        release_pair(&mut uses.pilot, pilot_first, pilot_last);
    } else {
        let (_, large) = second_name_third_value(&name.second);
        if large != 0
            && let Some(i) = uses.large_ship.iter().position(|&(n, _)| n == large)
        {
            uses.large_ship[i].1 -= 1;
            if uses.large_ship[i].1 == 0 {
                uses.large_ship.remove(i);
            }
        }
    }
    let (_, desig) = first_name_value(&name.first);
    let (_, number) = first_name_last_value(&name.first);
    release_pair(&mut uses.formation_desig, desig, number);
}

/// `~FNpcNameReputation` (`NpcNames.cpp:52-120`): the name's pool entries
/// go back to the faction's tables and the vibe is freed.
pub fn release_name_reputation(cx: &mut Cx<'_>, vibe: i32) {
    let _ = api::reputation::free(cx, vibe);
}
