//! The faction tables of TU48 (`Content/FactionProps.cpp`): the empathy
//! rates of `missions\empathy.ini`, the faction properties and name tables
//! of `missions\faction_prop.ini`, the faction list, and the goods of
//! `equipment\commodities_per_faction.ini`.

use std::collections::BTreeMap;

use game_sim::data::costume::Costume;
use game_sim::data::vc6::vc6_sort;
use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::{
    archetype_id, costume_id, good_id, reputation_group, short_group_name, space_costume, voice_id,
};
use crate::api;
use crate::server::Cx;

/// `EFactionEmpathyEvent`: the index of [`FactionEmpathy::event_rates`].
pub mod event {
    /// `object_destruction`.
    pub const OBJECT_DESTRUCTION: usize = 0;
    /// `random_mission_success`.
    pub const RANDOM_MISSION_SUCCESS: usize = 1;
    /// `random_mission_failure`.
    pub const RANDOM_MISSION_FAILURE: usize = 2;
    /// `random_mission_abortion`.
    pub const RANDOM_MISSION_ABORTION: usize = 3;
}

/// One `empathy_rate` (`FFactionEmpathyRate`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EmpathyRate {
    /// The group, `GetReputationGroup` (`0xffffffff` for an unknown one).
    pub group: u32,
    /// The rate.
    pub rate: f32,
}

/// One `[RepChangeEffects]` (`FFactionEmpathy`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct FactionEmpathy {
    /// The empathy rates, sorted by rate with the VC6 `std::sort` (the
    /// order of equal rates is the C++'s; `GetFactionHostileGroups` shows it).
    pub empathy_rates: Vec<EmpathyRate>,
    /// The four `event` rates by [`event`] index.
    pub event_rates: [f32; 4],
}

/// `LoadFactionEmpathy`: every `[RepChangeEffects]` under its `group`; an
/// existing group keeps its first entry. The C++ opens the file itself and
/// returns at once when it does not open; the reservation it makes from the
/// group count is not observable.
pub fn load_empathy(
    cx: &Cx<'_>,
    table: &mut BTreeMap<u32, FactionEmpathy>,
    reader: &mut IniReader,
) {
    // The rate's frame locals outlive the sections.
    let mut rate = EmpathyRate {
        group: 0,
        rate: 0.0,
    };
    while reader.read_header() {
        if !reader.is_header(b"RepChangeEffects") {
            continue;
        }
        let mut empathy = FactionEmpathy::default();
        let mut group = u32::MAX;
        while reader.read_value() {
            if reader.is_value(b"empathy_rate") {
                rate.group = reputation_group(cx, &reader.value_string_at(0));
                rate.rate = reader.value_float(1) as f32;
                empathy.empathy_rates.push(rate);
            } else if reader.is_value(b"group") {
                group = reputation_group(cx, &reader.value_string());
            } else if reader.is_value(b"event") {
                let name = reader.value_string_at(0);
                let index = [
                    &b"object_destruction"[..],
                    b"random_mission_success",
                    b"random_mission_failure",
                    b"random_mission_abortion",
                ]
                .iter()
                .position(|n| eq_ignore_case(&name, n));
                if let Some(index) = index {
                    empathy.event_rates[index] = reader.value_float(1) as f32;
                }
            }
        }
        vc6_sort(&mut empathy.empathy_rates, |a, b| a.rate < b.rate);
        table.entry(group).or_insert(empathy);
    }
}

/// `scan_for_cargo` (`FFactionScanCargo`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ScanCargo {
    /// The good's archetype (`GetArchetypeID(GetGoodID(value 0))`).
    pub archetype: u32,
    /// Value 1 when it is 0, 1 or 2, else 0.
    pub level: i32,
}

/// `EFactionJumpPreference`.
pub mod jump {
    /// Any.
    pub const ANY: i32 = 0;
    /// `jumpgate`.
    pub const GATE: i32 = 1;
    /// `jumphole`.
    pub const HOLE: i32 = 2;
}

/// `EFactionLegality`.
pub mod legality {
    /// Not set.
    pub const UNSET: i32 = 0;
    /// `lawful`.
    pub const LAWFUL: i32 = 1;
    /// `unlawful`.
    pub const UNLAWFUL: i32 = 2;
}

/// One `[FactionProps]` (`FFactionProps`).
#[derive(Clone, Debug, PartialEq)]
pub struct FactionProps {
    /// `msg_id_prefix` (`CommReferrable`, a `TString<64>`).
    pub msg_id_prefix: Vec<u8>,
    /// `npc_ship`, `CreateID` of the whole value.
    pub npc_ships: Vec<u32>,
    /// `jump_preference` ([`jump`]).
    pub jump_preference: i32,
    /// `scan_for_cargo`, each pair once.
    pub scan_for_cargo: Vec<ScanCargo>,
    /// `scan_chance`.
    pub scan_chance: f32,
    /// `scan_announce`.
    pub scan_announce: bool,
    /// The `MarketGood`s of `commodities_per_faction.ini`.
    pub market_goods: Vec<u32>,
    /// `assassinate_target`.
    pub assassinate_targets: Vec<u32>,
    /// `Legality` ([`legality`]).
    pub legality: i32,
    /// `nickname_plurality`: singular true, plural false.
    pub singular_nickname: bool,
    /// `formation`: `CreateID(v0)` to `CreateID(v1)`, the first kept.
    pub formations: BTreeMap<u32, u32>,
    /// `space_costume`.
    pub space_costumes: Vec<Costume>,
    /// `voice`, `pub::GetVoiceID`.
    pub voices: Vec<u32>,
    /// `mc_costume`, `pub::GetCostumeID`.
    pub mc_costume: i32,
}

impl Default for FactionProps {
    /// `FFactionProps::FFactionProps`.
    fn default() -> Self {
        Self {
            msg_id_prefix: Vec::new(),
            npc_ships: Vec::new(),
            jump_preference: jump::ANY,
            scan_for_cargo: Vec::new(),
            scan_chance: 1.0,
            scan_announce: true,
            market_goods: Vec::new(),
            assassinate_targets: Vec::new(),
            legality: legality::UNSET,
            singular_nickname: true,
            formations: BTreeMap::new(),
            space_costumes: Vec::new(),
            voices: Vec::new(),
            mc_costume: 0,
        }
    }
}

impl FactionProps {
    /// `GetFormation`.
    #[must_use]
    pub fn formation(&self, key: u32) -> Option<u32> {
        self.formations.get(&key).copied()
    }
}

/// A name range `first, last` (`FFactionNameRange`).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct NameRange {
    /// The first name id.
    pub first: i32,
    /// The last name id.
    pub last: i32,
}

/// One name key list (`FFactionNameList`).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct NameList {
    /// The total name count, `last - first + 1` summed with wrapping.
    pub count: i32,
    /// The ranges in file order.
    pub ranges: Vec<NameRange>,
}

impl NameList {
    /// `AddRange`: value 0, then value 1 (value 0 again when it is empty).
    fn add_range(&mut self, reader: &IniReader) {
        let first = reader.value_int(0);
        let last = if reader.is_value_empty(1) {
            first
        } else {
            reader.value_int(1)
        };
        self.ranges.push(NameRange { first, last });
        self.count = self
            .count
            .wrapping_add(last.wrapping_sub(first).wrapping_add(1));
    }

    /// `GetName`: the `index`-th name over the ranges in order; `None` past
    /// the last range, which the C++ reads without a bound.
    #[must_use]
    pub fn name(&self, mut index: u32) -> Option<i32> {
        for range in &self.ranges {
            let size = range.last.wrapping_sub(range.first).wrapping_add(1) as u32;
            if index < size {
                return Some(range.first.wrapping_add(index as i32));
            }
            index -= size;
        }
        None
    }

    /// `IsEmpty`.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.count == 0
    }
}

/// The name tables of one `[FactionProps]` (`FFactionNames`); the run-time
/// reservations live in Content's state.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct FactionNames {
    /// The affiliation of the section's `affiliation` line. The C++
    /// constructor leaves it unset; the port starts it at 0 (D781).
    pub group: u32,
    /// `firstname_female`.
    pub female_first_names: NameList,
    /// `firstname_male`.
    pub male_first_names: NameList,
    /// `lastname`.
    pub last_names: NameList,
    /// `formation_desig`.
    pub formation_desigs: NameList,
    /// `large_ship_names`.
    pub large_ship_names: NameList,
    /// `large_ship_desig` value 0.
    pub large_ship_desig: i32,
    /// `pub::Reputation::GetShortGroupName` of the affiliation.
    pub short_group_name: u32,
    /// `rank_desig` values 0 to 2.
    pub rank_desig: [u32; 3],
    /// Eight name ids; the loader writes `[2]` = 0x4076 and `[3]`, `[4]`,
    /// `[7]` = 0x4074 (lawful) or 0x4075 (unlawful).
    pub name_ids: [u32; 8],
    /// `rank_desig` values 3 and 4.
    pub rank_threshold: [i32; 2],
}

impl FactionNames {
    /// `ReadIniValue`: true only for the five range keys.
    fn read_ini_value(&mut self, cx: &Cx<'_>, reader: &IniReader) -> bool {
        if reader.is_value(b"affiliation") {
            self.group = reputation_group(cx, &reader.value_string());
            self.short_group_name = short_group_name(cx, self.group);
            return false;
        }
        if reader.is_value(b"firstname") {
            return false;
        }
        let list = if reader.is_value(b"firstname_male") {
            &mut self.male_first_names
        } else if reader.is_value(b"firstname_female") {
            &mut self.female_first_names
        } else if reader.is_value(b"lastname") {
            &mut self.last_names
        } else if reader.is_value(b"formation_desig") {
            &mut self.formation_desigs
        } else if reader.is_value(b"large_ship_names") {
            &mut self.large_ship_names
        } else {
            if reader.is_value(b"large_ship_desig") {
                self.large_ship_desig = reader.value_int(0);
            } else if reader.is_value(b"rank_desig") {
                for (index, desig) in self.rank_desig.iter_mut().enumerate() {
                    *desig = reader.value_int(index as u32) as u32;
                }
                self.rank_threshold = [reader.value_int(3), reader.value_int(4)];
            }
            return false;
        };
        list.add_range(reader);
        true
    }

    /// `GetRankDesig`.
    #[must_use]
    pub fn rank_desig_for(&self, level: i32) -> u32 {
        if level < self.rank_threshold[0] {
            self.rank_desig[0]
        } else if level < self.rank_threshold[1] {
            self.rank_desig[1]
        } else {
            self.rank_desig[2]
        }
    }
}

/// `TString<64>` assignment: cut at the first NUL and to 63 bytes.
fn tstring64(text: &[u8]) -> Vec<u8> {
    let mut text = neuron_core::hash::c_str(text).to_vec();
    text.truncate(63);
    text
}

/// The tables `LoadFactionProps` fills.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Factions {
    /// `GFactionProps`, by affiliation.
    pub props: BTreeMap<u32, FactionProps>,
    /// `GFactionNames`, by affiliation.
    pub names: BTreeMap<u32, FactionNames>,
    /// `GFactionGroups`: every reputation group in id order, once each.
    pub groups: Vec<u32>,
}

/// `LoadFactionProps`: every `[FactionProps]` into both tables under its
/// affiliation (an existing key keeps its entry), then every reputation
/// group appended to the group list once.
///
/// The affiliation, good and voice ids outlive the sections; the C++ leaves
/// them unset before their first line and the port starts them at 0 (D781).
pub fn load_faction_props(cx: &mut Cx<'_>, factions: &mut Factions, reader: &mut IniReader) {
    let mut group = 0;
    let mut good = 0;
    let mut voice = 0;
    while reader.read_header() {
        if !reader.is_header(b"FactionProps") {
            continue;
        }
        let mut props = FactionProps::default();
        let mut names = FactionNames::default();
        while reader.read_value() {
            if reader.is_value(b"affiliation") {
                group = reputation_group(cx, &reader.value_string());
                names.read_ini_value(cx, reader);
            } else if reader.is_value(b"scan_announce") {
                props.scan_announce = reader.value_bool(0);
            } else if reader.is_value(b"formation") {
                let key = create_id(&reader.value_string_at(0));
                let value = create_id(&reader.value_string_at(1));
                props.formations.entry(key).or_insert(value);
            } else if reader.is_value(b"scan_chance") {
                props.scan_chance = reader.value_float(0) as f32;
            } else if reader.is_value(b"npc_ship") {
                props.npc_ships.push(create_id(&reader.value_string()));
            } else if reader.is_value(b"scan_for_cargo") {
                good_id(cx, &mut good, &reader.value_string_at(0));
                let archetype = archetype_id(cx, good);
                let level = match reader.value_int(1) {
                    level @ 0..=2 => level,
                    _ => 0,
                };
                let cargo = ScanCargo { archetype, level };
                if !props.scan_for_cargo.contains(&cargo) {
                    props.scan_for_cargo.push(cargo);
                }
            } else if reader.is_value(b"msg_id_prefix") {
                // `CommReferrable::read_comm_ini_data`.
                props.msg_id_prefix = tstring64(&reader.value_string());
            } else if reader.is_value(b"mc_costume") {
                costume_id(cx, &mut props.mc_costume, &reader.value_string_at(0));
            } else if reader.is_value(b"space_costume") {
                let mut appearance = Costume::default();
                space_costume(cx, &mut appearance, reader);
                props.space_costumes.push(appearance);
            } else if reader.is_value(b"voice") {
                voice_id(&mut voice, &reader.value_string());
                props.voices.push(voice);
            } else if names.read_ini_value(cx, reader) {
            } else if reader.is_value(b"jump_preference") {
                // Compared byte by byte with each word.
                let value = reader.value_string_at(0);
                props.jump_preference = match value.as_slice() {
                    b"jumpgate" => jump::GATE,
                    b"jumphole" => jump::HOLE,
                    _ => jump::ANY,
                };
            } else if reader.is_value(b"assassinate_target") {
                props
                    .assassinate_targets
                    .push(create_id(&reader.value_string_at(0)));
            } else if reader.is_value(b"Legality") {
                let value = reader.value_string_at(0);
                if eq_ignore_case(&value, b"lawful") {
                    props.legality = legality::LAWFUL;
                } else if eq_ignore_case(&value, b"unlawful") {
                    props.legality = legality::UNLAWFUL;
                }
            } else if reader.is_value(b"nickname_plurality") {
                let value = reader.value_string_at(0);
                if eq_ignore_case(&value, b"singular") {
                    props.singular_nickname = true;
                } else if eq_ignore_case(&value, b"plural") {
                    props.singular_nickname = false;
                }
            }
        }
        if props.legality == legality::LAWFUL || props.legality == legality::UNLAWFUL {
            let id = if props.legality == legality::LAWFUL {
                0x4074
            } else {
                0x4075
            };
            names.name_ids[7] = id;
            names.name_ids[3] = id;
            names.name_ids[2] = 0x4076;
            names.name_ids[4] = id;
        }
        factions.names.entry(group).or_insert(names);
        factions.props.entry(group).or_insert(props);
    }
    // `FFactionGroupCollector` over `pub::Reputation::EnumerateGroups`.
    let groups = &mut factions.groups;
    api::reputation::enumerate_groups(cx, |id| {
        if !groups.contains(&id) {
            groups.push(id);
        }
        true
    });
}

/// `LoadFactionGoods`: each `[FactionGood]`'s `MarketGood`s appended to the
/// named faction's goods.
///
/// The good id outlives the sections (the C++ leaves it unset; the port
/// starts it at 0, D781). A `MarketGood` without a known faction writes
/// through a null pointer in the C++; the port ignores it (D780).
pub fn load_faction_goods(cx: &Cx<'_>, factions: &mut Factions, reader: &mut IniReader) {
    let mut good = 0;
    while reader.read_header() {
        if !reader.is_header(b"FactionGood") {
            continue;
        }
        let mut faction: Option<u32> = None;
        while reader.read_value() {
            if reader.is_value(b"faction") {
                let group = reputation_group(cx, &reader.value_string_at(0));
                faction = factions.props.contains_key(&group).then_some(group);
            } else if reader.is_value(b"MarketGood") {
                good_id(cx, &mut good, &reader.value_string_at(0));
                if let Some(props) = faction.and_then(|f| factions.props.get_mut(&f)) {
                    props.market_goods.push(good);
                }
            }
        }
    }
}

/// `GetFactionHostileGroups(Group, Out, Count)` (`FactionProps.cpp:1077-1097`):
/// the groups of `group`'s empathy rates from the lowest while their rate is
/// below zero, `0xffffffff` after them, into `out`. Without an empathy entry
/// for the group `out` is left as it is.
pub fn faction_hostile_groups(
    empathy: &BTreeMap<u32, FactionEmpathy>,
    group: u32,
    out: &mut [u32],
) {
    let Some(entry) = empathy.get(&group) else {
        return;
    };
    let mut rates = entry.empathy_rates.iter().peekable();
    for slot in out.iter_mut() {
        match rates.peek() {
            Some(rate) if rate.rate < 0.0 => {
                *slot = rate.group;
                rates.next();
            }
            _ => *slot = u32::MAX,
        }
    }
}
