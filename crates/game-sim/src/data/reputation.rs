//! Factions and their feelings (`Reputation`), ported from
//! `CppSource/Common/common_reputation.cpp`.
//!
//! Faction ids are `MakeId` of the nickname: 16-bit and not collision-free,
//! so two nicknames can name one record. `Reputation::Startup` reads the
//! `[Group]` blocks of `InitialWorld.ini` positionally (nickname, name,
//! info, short name); the `rep` lines of a block are read later through
//! [`Reputation::load_feelings`] from the initial world or a save.
//!
//! The save writer (`Reputation::Save`) belongs to the server's save files
//! (P3); `Vibe` and `SolarReps` are not part of this module.

use std::collections::BTreeMap;

use neuron_core::id::make_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

use super::{DataSource, Reports, lossy, not_below, open_ini, value_f32};

const SOURCE: &str = "Common\\Reputation.cpp";

/// The id `get_id` and the feeling queries use for "no group".
pub const MISSING_GROUP: u32 = 0xFFFF_FFFF;

/// The story factions of `IsStoryFaction`, in order.
pub const STORY_FACTIONS: [&[u8]; 9] = [
    b"fc_ln_grp",
    b"fc_kn_grp",
    b"fc_rn_grp",
    b"fc_n_grp",
    b"fc_or_grp",
    b"fc_f_grp",
    b"fc_q_grp",
    b"fc_ouk_grp",
    b"fc_uk_grp",
];

/// `TString<16>`: at most 15 bytes.
fn tstring16(text: &[u8]) -> Vec<u8> {
    super::goods::tstring(text, 16)
}

/// One faction (`FGroup`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Group {
    /// The nickname, at most 15 bytes.
    pub nickname: Vec<u8>,
    /// The name text id.
    pub ids_name: i32,
    /// The information card id.
    pub ids_info: i32,
    /// The short name text id.
    pub ids_short_name: i32,
    /// Feelings towards other groups, by id.
    pub feelings: BTreeMap<u32, f32>,
}

/// The group registry.
#[derive(Clone, Debug, Default)]
pub struct Reputation {
    groups: BTreeMap<u32, Group>,
}

/// One relation for [`Reputation::set`].
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Relation {
    /// The other group.
    pub group_id: u32,
    /// The feeling.
    pub feeling: f32,
}

/// `MakeId` of a nickname cut to `TString<16>`, zero-extended.
#[must_use]
pub fn group_id(nickname: &[u8]) -> u32 {
    u32::from(make_id(&tstring16(nickname)))
}

impl Reputation {
    /// The groups in id order.
    pub fn groups(&self) -> impl Iterator<Item = (u32, &Group)> {
        self.groups.iter().map(|(&id, g)| (id, g))
    }

    /// The group with id `id`.
    #[must_use]
    pub fn group(&self, id: u32) -> Option<&Group> {
        self.groups.get(&id)
    }

    /// `Reputation::Startup(path)`: `false` when the file cannot be opened.
    pub fn startup(&mut self, source: &dyn DataSource, path: &[u8]) -> bool {
        let Some(mut reader) = open_ini(source, path) else {
            return false;
        };
        while reader.read_header() {
            if reader.is_header(b"Group") {
                self.read_group_block(&mut reader);
            }
        }
        true
    }

    /// `ReadGroupBlock`: four values by position.
    fn read_group_block(&mut self, reader: &mut IniReader) {
        reader.read_value();
        let name = tstring16(&reader.value_string());
        let id = u32::from(make_id(&name));
        let group = self.groups.entry(id).or_default();
        group.nickname = name;
        reader.read_value();
        group.ids_name = trunc_to_i32(reader.value_float(0));
        reader.read_value();
        group.ids_info = if reader.is_value(b"ids_info") {
            trunc_to_i32(reader.value_float(0))
        } else {
            0
        };
        reader.read_value();
        group.ids_short_name = if reader.is_value(b"ids_short_name") {
            trunc_to_i32(reader.value_float(0))
        } else {
            0
        };
    }

    /// `Reputation::Shutdown`.
    pub fn shutdown(&mut self) {
        self.groups.clear();
    }

    /// `Reputation::LoadFeelings`: the `rep` lines of each `[Group]` block,
    /// `rep = feeling, other`; the first feeling for a group wins. A block
    /// for an unknown group is skipped, as the C++ reference does (the
    /// original wrote into the map's head node there).
    pub fn load_feelings(&mut self, reader: &mut IniReader, reports: &mut Reports) {
        while reader.read_header() {
            if !reader.is_header(b"Group") {
                continue;
            }
            reader.read_value();
            let name = tstring16(&reader.value_string());
            let id = u32::from(make_id(&name));
            let mut count = 0u32;
            while reader.read_value() {
                if !reader.is_value(b"rep") {
                    continue;
                }
                count += 1;
                let other = group_id(&reader.value_string_at(1));
                let feeling = value_f32(reader, 0);
                if let Some(group) = self.groups.get_mut(&id) {
                    group.feelings.entry(other).or_insert(feeling);
                }
            }
            if count == 0 {
                let shown = self.groups.get(&id).map_or(&name, |g| &g.nickname);
                reports.push(
                    SOURCE,
                    0x10e,
                    format!(
                        "*** ERROR: FACTION: save game has corrupted reputation data for group {}, \
                         please re-create the savegame",
                        lossy(shown)
                    ),
                );
            }
        }
    }

    /// `Reputation::FreeFeelings`.
    pub fn free_feelings(&mut self) {
        for group in self.groups.values_mut() {
            group.feelings.clear();
        }
    }

    /// `Reputation::get_id`: [`MISSING_GROUP`] for an empty or unknown name.
    #[must_use]
    pub fn get_id(&self, nickname: &[u8]) -> u32 {
        let name = tstring16(nickname);
        if name.is_empty() {
            return MISSING_GROUP;
        }
        let id = u32::from(make_id(&name));
        if self.groups.contains_key(&id) {
            id
        } else {
            MISSING_GROUP
        }
    }

    /// `Reputation::get_nickname`.
    #[must_use]
    pub fn get_nickname(&self, id: u32) -> Option<&[u8]> {
        self.groups.get(&id).map(|g| &g.nickname[..])
    }

    /// `Reputation::get_name`: 0 for an unknown group.
    #[must_use]
    pub fn get_name(&self, id: u32) -> u32 {
        self.groups
            .get(&id)
            .map_or(0, |g| u32::from_ne_bytes(g.ids_name.to_ne_bytes()))
    }

    /// `Reputation::get_short_name`.
    #[must_use]
    pub fn get_short_name(&self, id: u32) -> u32 {
        self.groups
            .get(&id)
            .map_or(0, |g| u32::from_ne_bytes(g.ids_short_name.to_ne_bytes()))
    }

    /// `Reputation::get_info_card`.
    #[must_use]
    pub fn get_info_card(&self, id: u32) -> u32 {
        self.groups
            .get(&id)
            .map_or(0, |g| u32::from_ne_bytes(g.ids_info.to_ne_bytes()))
    }

    /// `Reputation::group_count`.
    #[must_use]
    pub fn group_count(&self) -> usize {
        self.groups.len()
    }

    /// `Reputation::get_feelings_towards`: the result code (0, -1 for an
    /// unknown `from`, -2 for an unknown `to`) and the feeling written (the
    /// out value is left alone on an error).
    #[must_use]
    pub fn feelings_towards(&self, from: u32, to: u32) -> (i32, Option<f32>) {
        if from == MISSING_GROUP || to == MISSING_GROUP {
            return (0, Some(0.0));
        }
        let Some(from_group) = self.groups.get(&from) else {
            return (-1, None);
        };
        if !self.groups.contains_key(&to) {
            return (-2, None);
        }
        if from == to {
            return (0, Some(1.0));
        }
        (
            0,
            Some(from_group.feelings.get(&to).copied().unwrap_or(0.0)),
        )
    }

    /// `Reputation::affect_relation`: adds `delta`, clamped to -1..=1 (NaN
    /// gives 1). 0, or -1 / -2 for an unknown group.
    pub fn affect_relation(&mut self, from: u32, to: u32, delta: f32) -> i32 {
        let to_known = self.groups.contains_key(&to);
        let Some(group) = self.groups.get_mut(&from) else {
            return -1;
        };
        if let Some(value) = group.feelings.get_mut(&to) {
            let mut sum = f64::from(delta) + f64::from(*value);
            if not_below(sum, 1.0) {
                sum = 1.0;
            }
            if -1.0 > sum {
                sum = -1.0;
            }
            *value = sum as f32;
            return 0;
        }
        if !to_known {
            return -2;
        }
        let mut value = delta;
        if not_below(f64::from(value), 1.0) {
            value = 1.0;
        }
        if -1.0 > value {
            value = -1.0;
        }
        group.feelings.insert(to, value);
        0
    }

    /// `Reputation::IsStoryFaction`.
    #[must_use]
    pub fn is_story_faction(id: u32) -> bool {
        STORY_FACTIONS
            .iter()
            .any(|name| u32::from(make_id(name)) == id)
    }

    /// `Reputation::set`: replaces the feelings of `nickname`'s group (a new
    /// group gets an empty nickname and zero names, as in the C++
    /// reference); the first value for a repeated group in `relations` wins.
    pub fn set(&mut self, nickname: &[u8], relations: &[Relation]) {
        let id = u32::from(make_id(&tstring16(nickname)));
        let group = self.groups.entry(id).or_default();
        group.feelings.clear();
        for relation in relations {
            group
                .feelings
                .entry(relation.group_id)
                .or_insert(relation.feeling);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct One(&'static [u8]);

    impl DataSource for One {
        fn read(&self, _path: &[u8]) -> Option<Vec<u8>> {
            Some(self.0.to_vec())
        }
    }

    #[test]
    fn groups_and_feelings() {
        let text = b"[Group]\nnickname = li_n_grp\nids_name = 196846.7\nids_info = 66\n\
ids_short_name = 9\nrep = 0.5, li_p_grp\n[Group]\nnickname = li_p_grp\nids_name = 1\n\
rep = -0.5, li_n_grp\n";
        let mut rep = Reputation::default();
        assert!(rep.startup(&One(text), b"InitialWorld.ini"));
        let n = rep.get_id(b"li_n_grp");
        assert_eq!(n, 0x224A);
        assert_eq!(rep.get_name(n), 196_846);
        let p = rep.get_id(b"li_p_grp");
        assert_eq!(rep.get_info_card(p), 0);
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text));
        let mut reports = Reports::default();
        rep.load_feelings(&mut reader, &mut reports);
        assert_eq!(rep.feelings_towards(n, p), (0, Some(0.5)));
        assert_eq!(rep.affect_relation(n, p, 0.75), 0);
        assert_eq!(rep.feelings_towards(n, p), (0, Some(1.0)));
        assert_eq!(rep.feelings_towards(n, n), (0, Some(1.0)));
        assert_eq!(rep.get_id(b"nobody"), MISSING_GROUP);
        assert!(Reputation::is_story_faction(u32::from(make_id(
            b"fc_q_grp"
        ))));
    }
}
