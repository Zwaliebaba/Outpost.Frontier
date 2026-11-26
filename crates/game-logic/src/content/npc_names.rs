//! The NPC name builder (TU42 `Content/NpcNames.cpp`) over the faction
//! name tables and their reservations (TU48 `Content/FactionProps.cpp:40-700`),
//! with the name readers of `Content/NpcShips.cpp:240-340`.
//!
//! P3.7a brought what an assassination offer needs (decision O48): the
//! reservations of the pilot names, formation designations and large ship
//! names ([`FactionNameUses`], in Content's state because the C++ keeps
//! them in the faction's `FFactionNames`), the picks that reserve them, the
//! immediate releases and the four name readers. P3.6c's follow-up brings
//! the rest: the full [`build_npc_name`] (the ship-type names of a
//! fighter, a freighter and a large ship, the escorts, the rank
//! designation), [`pick_large_ship_name`], the delayed releases with their
//! 30 s timers on SpaceSupport ([`release_pilot_name`] and its siblings,
//! [`handle_faction_name_timer`]), the named reputation handles
//! ([`create_npc_name_reputation`], [`create_msn_npc_reputation`]) whose
//! destructor gives the name back ([`release_name_reputation`]), their
//! three readers for the comm system, and the gender pick
//! ([`pick_npc_name_gender`]).

use std::collections::{BTreeMap, VecDeque};

use game_sim::protocol::fmtstr::{FmtStr, FmtValue};

use crate::api;
use crate::content::rep_handles::{RepHandle, RepKind};
use crate::content::tables::FactionNames;
use crate::content::tables::factions::{FactionProps, NameList};
use crate::content::tables::voices::find_voice_properties;
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

/// One faction's reservations (`FFactionNames` `+0xa4..+0xd4`) and the
/// releases waiting for their timers (`PendingPilotNames`,
/// `PendingLargeShipNames`, `PendingFormationDesigs`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NameUses {
    /// `LargeShipNameUses`: a name and its use count.
    pub large_ship: Vec<(i32, i32)>,
    /// `PilotNameUses`.
    pub pilot: Vec<PairUse>,
    /// `FormationDesigUses`.
    pub formation_desig: Vec<PairUse>,
    /// `PendingPilotNames`: first and last names to release, oldest first.
    pub pending_pilot: VecDeque<(i32, i32)>,
    /// `PendingLargeShipNames`.
    pub pending_large_ship: VecDeque<i32>,
    /// `PendingFormationDesigs`: designations and numbers.
    pub pending_formation_desig: VecDeque<(i32, i32)>,
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

/// `AddNameUse`: the name's count up, or the name appended with 1.
fn add_name(uses: &mut Vec<(i32, i32)>, name: i32) {
    match uses.iter().position(|&(n, _)| n == name) {
        Some(i) => uses[i].1 += 1,
        None => uses.push((name, 1)),
    }
}

/// `ReleaseNameUse`: the name's count down, the name erased at 0.
fn release_name_use(uses: &mut Vec<(i32, i32)>, name: i32) {
    if let Some(i) = uses.iter().position(|&(n, _)| n == name) {
        uses[i].1 -= 1;
        if uses[i].1 == 0 {
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

/// `FFactionNames::PickLargeShipName` (`FactionProps.cpp:619-646`): the
/// first free large ship name from a random start, reserved when asked;
/// with every name in use, a random one (false).
pub fn pick_large_ship_name(
    cx: &mut Cx<'_>,
    names: &FactionNames,
    uses: &mut NameUses,
    reserve: bool,
) -> (bool, i32) {
    let count = names.large_ship_names.count;
    if (uses.large_ship.len() as u32) < (count as u32) {
        let mut draw = rand_index(cx, count);
        let name = loop {
            let name = name_at(&names.large_ship_names, rem(draw, count));
            draw = draw.wrapping_add(1);
            if !uses.large_ship.iter().any(|&(n, _)| n == name) {
                break name;
            }
        };
        if reserve {
            add_name(&mut uses.large_ship, name);
        }
        return (true, name);
    }
    let i = rand_index(cx, count);
    let name = name_at(&names.large_ship_names, i);
    if reserve {
        add_name(&mut uses.large_ship, name);
    }
    (false, name)
}

/// The name request `BuildNpcName` reads (`FNpcNameRequest`, 0x20 bytes,
/// `NpcNames.h:19-55`), with the default constructor's values.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NpcNameRequest {
    /// `+0x00`: an index into the faction's `NameIds`; non-zero builds a
    /// ship-type name, 0 a pilot's.
    pub name_index: i32,
    /// `+0x04`: the formation designation, 0 to pick one.
    pub desig: i32,
    /// `+0x08`: its number.
    pub desig_number: i32,
    /// `+0x0c` (1): the ships in the formation.
    pub formation_size: i32,
    /// `+0x10`: the ship's index in its formation, 0 the leader.
    pub escort_number: i32,
    /// `+0x14`, never read.
    pub field_0x14: u8,
    /// `+0x18`: the rank level of the rank designation.
    pub rank_level: i32,
    /// `+0x1c`: the ship archetype.
    pub ship_arch: u32,
}

impl Default for NpcNameRequest {
    /// `FNpcNameRequest::FNpcNameRequest()` (`NpcNames.cpp:20-30`).
    fn default() -> Self {
        Self {
            name_index: 0,
            desig: 0,
            desig_number: 0,
            formation_size: 1,
            escort_number: 0,
            field_0x14: 0,
            rank_level: 0,
            ship_arch: 0,
        }
    }
}

/// `FFactionNames::GetNameId(Index)`: `NameIds[Index]`; an index past the
/// eight, which the C++ reads without a bound, is 0 (D865).
fn name_id(names: &FactionNames, index: i32) -> u32 {
    usize::try_from(index)
        .ok()
        .and_then(|i| names.name_ids.get(i))
        .copied()
        .unwrap_or(0)
}

/// The designation of a name: the request's own, counted as used
/// (`AddFormationDesigUse`), or a new one reserved.
fn take_desig(
    cx: &mut Cx<'_>,
    names: &FactionNames,
    uses: &mut NameUses,
    request: &NpcNameRequest,
) -> (i32, i32) {
    if request.desig != 0 {
        add_pair(
            &mut uses.formation_desig,
            request.desig,
            request.desig_number,
        );
        return (request.desig, request.desig_number);
    }
    let (_, desig, number) = pick_formation_desig(cx, names, uses, true);
    (desig, number)
}

/// `BuildNpcName(Affiliation, Request, Out, bNamedLeader)`
/// (`NpcNames.cpp:142-264`): the name's two parts from the faction's
/// tables by the request, the picked names reserved. Always true.
///
/// - A ship-type name (`name_index` non-zero): the first part names the
///   formation (`0x3f17` or `0x3f19` alone, `0x3f20` or `0x3f1d` for a
///   fighter's or freighter's leader or escort, `0x3f1b` for another ship
///   in a formation) with the short group name, the type name and a
///   designation. A fighter or freighter (type bits `0x30000`) gets a
///   pilot in the second part (`0x3f18` alone, `0x3f21` leading: the rank
///   designation and a first and last name; `0x3f1e` an escort: "Escort",
///   its number, the rank designation and a last name); any other ship its
///   archetype's name, the faction's large ship designation and a large
///   ship name (`0x3f1a` alone, `0x3f1c` in a formation).
/// - A pilot's name: a named leader's first part is `0x3f24` with the
///   short group name and a designation; the second part is `0x3f22` with
///   the rank designation for a rank level, else `0x3f23`, then a first
///   and last name.
///
/// The C++ reads the faction's names without a test; the port builds
/// nothing for a faction without names (D874).
pub fn build_npc_name(
    cx: &mut Cx<'_>,
    affiliation: u32,
    request: &NpcNameRequest,
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
    build_into(cx, names, &mut uses, request, out, named_leader);
    cx.logic
        .content
        .state
        .faction_name_uses
        .insert(affiliation, uses);
    true
}

/// The body of [`build_npc_name`] over the faction's tables.
fn build_into(
    cx: &mut Cx<'_>,
    names: &FactionNames,
    uses: &mut NameUses,
    request: &NpcNameRequest,
    out: &mut NpcNameParams,
    named_leader: bool,
) {
    if request.name_index != 0 {
        // `pub::GetType`'s failure leaves the type unset in the C++; 0 here.
        let ship_type = api::misc::ship_type(cx, request.ship_arch).unwrap_or(0);
        let small = ship_type & 0x30000 != 0;
        out.first.mad_lib_id = match (request.formation_size == 1, small) {
            (true, true) => 0x3f17,
            (true, false) => 0x3f19,
            (false, true) if request.escort_number == 0 => 0x3f20,
            (false, true) => 0x3f1d,
            (false, false) => 0x3f1b,
        };
        out.first.append_string(names.short_group_name);
        out.first.append_string(name_id(names, request.name_index));
        let (desig, number) = take_desig(cx, names, uses, request);
        out.first.append_string(desig as u32);
        out.first.append_int(number as u32);
        if small {
            out.second.mad_lib_id = if request.formation_size == 1 {
                0x3f18
            } else if request.escort_number == 0 {
                0x3f21
            } else {
                0x3f1e
            };
            if out.second.mad_lib_id != 0x3f1e {
                out.second
                    .append_string(names.rank_desig_for(request.rank_level));
                let (_, first, last) =
                    pick_pilot_name(cx, names, uses, true, true, out.gender, true);
                out.second.append_string(first as u32);
                out.second.append_string(last as u32);
                return;
            }
            out.second.append_string(0x3f1f);
            out.second.append_int(request.escort_number as u32);
            out.second
                .append_string(names.rank_desig_for(request.rank_level));
            let (_, _, last) = pick_pilot_name(cx, names, uses, false, true, out.gender, true);
            out.second.append_string(last as u32);
            return;
        }
        out.second.mad_lib_id = if request.formation_size == 1 {
            0x3f1a
        } else {
            0x3f1c
        };
        // The C++ reads the name of an unknown archetype into an unset
        // local; 0 here.
        let ship_name = api::misc::ship_arch_strid(cx, request.ship_arch).unwrap_or(0);
        out.second.append_string(ship_name);
        out.second.append_string(names.large_ship_desig as u32);
        let (_, large) = pick_large_ship_name(cx, names, uses, true);
        out.second.append_string(large as u32);
        return;
    }
    if named_leader {
        out.first.mad_lib_id = 0x3f24;
        out.first.append_string(names.short_group_name);
        let (desig, number) = take_desig(cx, names, uses, request);
        out.first.append_string(desig as u32);
        out.first.append_int(number as u32);
    }
    if request.rank_level != 0 {
        out.second.mad_lib_id = 0x3f22;
        out.second
            .append_string(names.rank_desig_for(request.rank_level));
    } else {
        out.second.mad_lib_id = 0x3f23;
    }
    let (_, first, last) = pick_pilot_name(cx, names, uses, true, true, out.gender, true);
    out.second.append_string(first as u32);
    out.second.append_string(last as u32);
}

/// `BuildNpcName(affiliation, FNpcNameRequest(), out, named_leader)`: the
/// default request (no ship type, no designation, rank 0), for a named
/// leader `0x3f24` with the short group name and a reserved designation,
/// then `0x3f23` with a reserved pilot name ([`build_npc_name`]).
pub fn build_default_npc_name(
    cx: &mut Cx<'_>,
    affiliation: u32,
    out: &mut NpcNameParams,
    named_leader: bool,
) -> bool {
    build_npc_name(
        cx,
        affiliation,
        &NpcNameRequest::default(),
        out,
        named_leader,
    )
}

/// `PickNpcNameGender(Affiliation, Params)` (`NpcNames.cpp:136-140`): the
/// faction's `PickGender` into the name's gender ([`pick_gender`]). The
/// C++ reads the faction's properties without a test; false without them
/// (D865).
pub fn pick_npc_name_gender(cx: &mut Cx<'_>, affiliation: u32, out: &mut NpcNameParams) -> bool {
    let tables = cx.logic.content.tables.clone();
    let Some(props) = tables.faction_props(affiliation) else {
        return false;
    };
    match pick_gender(cx, props) {
        Some(gender) => {
            out.gender = gender;
            true
        }
        None => false,
    }
}

/// `FFactionProps::PickGender` (`FactionProps.cpp:275-293`): the male
/// voices counted (those that support role 1), then one draw over all the
/// voices: below the male count male (1), else female (2). `None` without
/// voices (no draw).
pub fn pick_gender(cx: &mut Cx<'_>, props: &FactionProps) -> Option<i32> {
    let tables = cx.logic.content.tables.clone();
    let male = props
        .voices
        .iter()
        .filter(|&&v| {
            find_voice_properties(&tables.voice_properties, v).is_some_and(|p| p.supports_roles(1))
        })
        .count() as u32;
    if props.voices.is_empty() {
        return None;
    }
    let draw = rand_index(cx, props.voices.len() as i32) as u32;
    Some(if draw < male { 1 } else { 2 })
}

/// `FFactionProps::PickVoice(Roles, &Out)` (`FactionProps.cpp:250-272`):
/// from a copy of the faction's voices, a random one (no draw while one is
/// left) that supports every role of `roles`; the others leave the copy.
/// `None` when none does.
pub fn pick_voice(cx: &mut Cx<'_>, props: &FactionProps, roles: u32) -> Option<u32> {
    let tables = cx.logic.content.tables.clone();
    let mut candidates = props.voices.clone();
    while !candidates.is_empty() {
        let count = candidates.len() as i32;
        let pick = if count == 1 { 0 } else { rand_index(cx, count) };
        let at = usize::try_from(pick)
            .ok()
            .filter(|&i| i < candidates.len())?;
        let voice = candidates[at];
        if find_voice_properties(&tables.voice_properties, voice)
            .is_some_and(|p| p.supports_roles(roles))
        {
            return Some(voice);
        }
        candidates.remove(at);
    }
    None
}

/// `FFactionProps::PickSpaceCostume(Gender, &Out)` (`FactionProps.cpp:241-272`):
/// of the faction's space costumes those whose skeleton is of the gender
/// (`pub::GF::GetCostumeSkeletonGender`), one drawn (`rand() * n /
/// 32768`); `None`, without a draw, when none is.
pub fn pick_space_costume(
    cx: &mut Cx<'_>,
    props: &FactionProps,
    gender: i32,
) -> Option<game_sim::data::costume::Costume> {
    let mut matches = Vec::with_capacity(props.space_costumes.len());
    for (index, costume) in props.space_costumes.iter().enumerate() {
        if api::gf::costume_skeleton_gender(cx, costume) == gender {
            matches.push(index);
        }
    }
    if matches.is_empty() {
        return None;
    }
    let pick = rand_index(cx, matches.len() as i32);
    let at = usize::try_from(pick)
        .ok()
        .and_then(|i| matches.get(i).copied())?;
    props.space_costumes.get(at).cloned()
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

/// The kinds of the three release timers (`GFactionNameMsgPilot`,
/// `GFactionNameMsgLargeShip`, `GFactionNameMsgFormation`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum NameTimer {
    Pilot,
    LargeShip,
    Formation,
}

/// `SetFactionNameTimer(MessageId, Group)`: `TimerExpired {MessageId,
/// Group}` to SpaceSupport in 30 s.
fn set_faction_name_timer(cx: &mut Cx<'_>, timer: NameTimer, group: u32) {
    let ids = cx.logic.content.state.load_ids;
    let message = match timer {
        NameTimer::Pilot => ids.faction_name_pilot,
        NameTimer::LargeShip => ids.faction_name_large_ship,
        NameTimer::Formation => ids.faction_name_formation,
    };
    let to = cx.logic.content.state.space_support;
    let _ = api::controller::set_timer(cx, to, message, group, 30.0);
}

/// The faction's `Group` (`FFactionNames +0x00`), the timer's second word.
fn names_group(cx: &Cx<'_>, faction: u32) -> u32 {
    cx.logic
        .content
        .tables
        .faction_names(faction)
        .map_or(0, |n| n.group)
}

/// `FFactionNames::ReleasePilotName(First, Last, bImmediate)`
/// (`FactionProps.cpp:655-668`): at once one use of the pair less; else
/// the pair waits in the faction's queue and a 30 s timer releases the
/// oldest one ([`handle_faction_name_timer`]).
pub fn release_pilot_name(cx: &mut Cx<'_>, faction: u32, first: i32, last: i32, immediate: bool) {
    let uses = cx
        .logic
        .content
        .state
        .faction_name_uses
        .entry(faction)
        .or_default();
    if immediate {
        release_pair(&mut uses.pilot, first, last);
        return;
    }
    uses.pending_pilot.push_back((first, last));
    let group = names_group(cx, faction);
    set_faction_name_timer(cx, NameTimer::Pilot, group);
}

/// `FFactionNames::ReleaseLargeShipName(Name, bImmediate)` (`:671-680`).
pub fn release_large_ship_name(cx: &mut Cx<'_>, faction: u32, name: i32, immediate: bool) {
    let uses = cx
        .logic
        .content
        .state
        .faction_name_uses
        .entry(faction)
        .or_default();
    if immediate {
        release_name_use(&mut uses.large_ship, name);
        return;
    }
    uses.pending_large_ship.push_back(name);
    let group = names_group(cx, faction);
    set_faction_name_timer(cx, NameTimer::LargeShip, group);
}

/// `FFactionNames::ReleaseFormationDesig(Desig, Number, bImmediate)`
/// (`:683-697`).
pub fn release_formation_desig(
    cx: &mut Cx<'_>,
    faction: u32,
    desig: i32,
    number: i32,
    immediate: bool,
) {
    let uses = cx
        .logic
        .content
        .state
        .faction_name_uses
        .entry(faction)
        .or_default();
    if immediate {
        release_pair(&mut uses.formation_desig, desig, number);
        return;
    }
    uses.pending_formation_desig.push_back((desig, number));
    let group = names_group(cx, faction);
    set_faction_name_timer(cx, NameTimer::Formation, group);
}

/// `HandleFactionNameTimer(Timer)` (`FactionProps.cpp:700-724`), from
/// SpaceSupport's `TimerExpired`: one of the three timers releases the
/// oldest queued entry of the group's faction at once. The C++ takes the
/// front of the queue without a test, and of the faction's names; an empty
/// queue or an unknown faction releases nothing (D865).
pub fn handle_faction_name_timer(cx: &mut Cx<'_>, param1: u32, group: u32) {
    let ids = cx.logic.content.state.load_ids;
    let timer = if param1 == ids.faction_name_pilot {
        NameTimer::Pilot
    } else if param1 == ids.faction_name_large_ship {
        NameTimer::LargeShip
    } else if param1 == ids.faction_name_formation {
        NameTimer::Formation
    } else {
        return;
    };
    if cx.logic.content.tables.faction_names(group).is_none() {
        return;
    }
    let Some(uses) = cx.logic.content.state.faction_name_uses.get_mut(&group) else {
        return;
    };
    match timer {
        NameTimer::Pilot => {
            if let Some((first, last)) = uses.pending_pilot.pop_front() {
                release_pair(&mut uses.pilot, first, last);
            }
        }
        NameTimer::LargeShip => {
            if let Some(name) = uses.pending_large_ship.pop_front() {
                release_name_use(&mut uses.large_ship, name);
            }
        }
        NameTimer::Formation => {
            if let Some((desig, number)) = uses.pending_formation_desig.pop_front() {
                release_pair(&mut uses.formation_desig, desig, number);
            }
        }
    }
}

/// `CreateNpcNameReputation(Params, &Out)` (`NpcNames.cpp:267-275`): a
/// named handle (`FNpcNameReputation`) whose vibe is allocated from the
/// name's two parts (`pub::Reputation::Alloc`, -1 when it fails) with the
/// name's affiliation; one reference, the creator's.
pub fn create_npc_name_reputation(cx: &mut Cx<'_>, name: &NpcNameParams) -> RepHandle {
    let vibe = api::reputation::alloc(cx, &name.first, &name.second).unwrap_or(-1);
    let _ = api::reputation::set_affiliation(cx, vibe, name.affiliation);
    cx.logic
        .content
        .state
        .rep_handles
        .create(vibe, RepKind::Named)
}

/// `CreateMsnNpcReputation(Npc, &Out)` (`NpcNames.cpp:278-293`): the same
/// from a mission's `[NPC]`: an empty first part, the second the mad lib
/// of the NPC's `individual_name` without values, its affiliation.
pub fn create_msn_npc_reputation(
    cx: &mut Cx<'_>,
    individual_name: u32,
    affiliation: u32,
) -> RepHandle {
    let name = NpcNameParams {
        second: FmtStr::new(individual_name),
        affiliation,
        ..NpcNameParams::default()
    };
    create_npc_name_reputation(cx, &name)
}

/// `RollNpcNameValue`: `rand() * 5 / 32768 + 1`; false.
fn roll_npc_name_value(cx: &mut Cx<'_>) -> (bool, i32) {
    let value = cx.world.rng().rand().wrapping_mul(5) / 32768 + 1;
    (false, value)
}

/// The vibe's two names (`pub::Reputation::GetName` into two empty
/// `FmtStr`s, which a failure leaves empty).
fn vibe_name(cx: &mut Cx<'_>, vibe: i32) -> (FmtStr, FmtStr) {
    api::reputation::name(cx, vibe).unwrap_or_else(|_| (FmtStr::new(0), FmtStr::new(0)))
}

/// `FNpcNameReputation::GetFormationDesig` (`NpcNames.cpp:108-117`): the
/// designation of the vibe's name ([`first_name_value`]).
pub fn reputation_formation_desig(cx: &mut Cx<'_>, vibe: i32) -> (bool, i32) {
    let (first, _) = vibe_name(cx, vibe);
    first_name_value(&first)
}

/// `FNpcNameReputation::GetFormationDesigNumber` (`:120-129`): its number
/// ([`first_name_last_value`]).
pub fn reputation_formation_desig_number(cx: &mut Cx<'_>, vibe: i32) -> (bool, i32) {
    let (first, _) = vibe_name(cx, vibe);
    first_name_last_value(&first)
}

/// `FNpcNameReputation::RollNameValue` (`:132-139`): the name fetched and
/// not read, then `rand() * 5 / 32768 + 1`; false.
pub fn reputation_roll_name_value(cx: &mut Cx<'_>, vibe: i32) -> (bool, i32) {
    let _ = vibe_name(cx, vibe);
    roll_npc_name_value(cx)
}

/// `~FNpcNameReputation` (`NpcNames.cpp:52-105`): the vibe's name read
/// back (the pilot's first and last name, a large ship name, the
/// designation and its number; the name value it rolls is drawn and not
/// read), each part that is not 1 queued for release in the faction of
/// the vibe's affiliation (not at once: 30 s timers), then the vibe freed
/// when there is one. The C++ releases through the faction's names
/// without a test; a faction without names releases nothing (D865).
pub fn release_name_reputation(cx: &mut Cx<'_>, vibe: i32) {
    let (first, second) = vibe_name(cx, vibe);
    let (_, pilot_first, pilot_last) = second_name_values(&second);
    let (_, large_ship) = second_name_third_value(&second);
    let (_, desig) = first_name_value(&first);
    let (_, number) = first_name_last_value(&first);
    let _ = roll_npc_name_value(cx);
    let faction = api::reputation::affiliation(cx, vibe).unwrap_or(u32::MAX);
    if cx.logic.content.tables.faction_names(faction).is_some() {
        if pilot_first != 1 || pilot_last != 1 {
            release_pilot_name(cx, faction, pilot_first, pilot_last, false);
        }
        if large_ship != 1 {
            release_large_ship_name(cx, faction, large_ship, false);
        }
        if desig != 1 {
            release_formation_desig(cx, faction, desig, number, false);
        }
    }
    if vibe != -1 {
        let _ = api::reputation::free(cx, vibe);
    }
}
