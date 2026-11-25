//! The random-mission tables the generator's constructor loads
//! (`FRandomMissionGenerator::FRandomMissionGenerator`,
//! `Content/RandomMissionGen.cpp:313-374`): `NpcRankToDiff.ini` and
//! `Diff2Money.ini` (TU63), `RMLootInfo.ini` (TU49,
//! `RandomMissionDB.cpp:421-622`), `SolarFormations.ini` (TU51,
//! `RandomMissionDB.cpp:823-975`), `KillableSolars.ini` and
//! `VignetteCriticalLoot.ini` (TU62, `RandomEncounters.cpp:40-350`), with
//! the picks the offer generator draws from them (work package P3.7a,
//! record 0008 points 5 and 8).
//!
//! `std::map` is a [`BTreeMap`], `std::vector` a [`Vec`] in insertion
//! order; `GRmDiff2Money`, a `std::map<float, int>`, is a [`Vec`] kept in
//! key order with the map's unique insert.

// The C++'s negated float tests (`!(x > 0.0f)`) send NaN to the failing
// side; the port spells them as the negated comparison.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{Matrix, Quaternion, Vector, quaternion_to_matrix};
use neuron_core::num::trunc_to_i32;

use crate::api;
use crate::content::tables::reputation_group;
use crate::server::Cx;

/// `GRmSolarTypes` (`RandomMissionDB.cpp:630-635`): the solar type names.
pub const SOLAR_TYPES: [(&[u8], i32); 4] = [
    (b"big_solar", 0),
    (b"defensive_solar", 1),
    (b"killable_neutral", 2),
    (b"background_solar", 3),
];

/// `GRmSolarFormationTypes` (`RandomMissionDB.cpp:656-661`).
pub const SOLAR_FORMATION_TYPES: [(&[u8], i32); 4] = [
    (b"big_solar_formation", 0),
    (b"battle_background_formation", 1),
    (b"defensive_outpost_formation", 2),
    (b"loot_group_formation", 3),
];

/// `GRmBonusLootAllFactions`: the faction key of `faction = all`.
pub const ALL_FACTIONS: u32 = u32::MAX;

/// A `{name, value}` table walk without regard to case: the first match.
pub(crate) fn named_value(table: &[(&[u8], i32)], name: &[u8]) -> Option<i32> {
    table
        .iter()
        .find(|(n, _)| eq_ignore_case(c_str(name), n))
        .map(|&(_, v)| v)
}

/// `AppendUnique` / `RmInsertUnique`: a linear find, appended when absent.
pub(crate) fn add_unique<T: PartialEq + Copy>(list: &mut Vec<T>, value: T) {
    if !list.contains(&value) {
        list.push(value);
    }
}

/// One `[RMBonusLoot]` record (`FRmBonusLootEntry`, 0x18 bytes).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct BonusLootEntry {
    /// `archetype`, `pub::GetEquipmentID`.
    pub archetype: u32,
    /// `num_to_drop` value 0.
    pub num_to_drop_min: i32,
    /// `num_to_drop` value 1 (value 0 when empty).
    pub num_to_drop_max: i32,
    /// `difficulty_range` value 0.
    pub difficulty_min: f32,
    /// `difficulty_range` value 1.
    pub difficulty_max: f32,
    /// `weight`, the integer value converted.
    pub weight: f32,
}

/// One `pos` line of a `[SolarFormation]` (`FRmSolarFormationSlot`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SolarFormationSlot {
    /// `mandatory` 1, `optional` 0; any other word keeps the previous
    /// line's value.
    pub mandatory: bool,
    /// The identity, or `QuaternionToMatrix` of values 5 to 8.
    pub orientation: Matrix,
    /// Values 2 to 4.
    pub position: Vector,
    /// Value 1 in [`SOLAR_TYPES`]; an unknown name keeps the previous
    /// line's value.
    pub solar_type: i32,
}

/// One `[SolarFormation]` (`FRmSolarFormation`).
#[derive(Clone, Debug, PartialEq)]
pub struct SolarFormation {
    /// `nickname`, `CreateID`.
    pub nickname: u32,
    /// `type` in [`SOLAR_FORMATION_TYPES`], -1 when absent or unknown.
    pub kind: i32,
    /// The `pos` lines.
    pub slots: Vec<SolarFormationSlot>,
}

impl SolarFormation {
    /// `CollectMandatorySlotTypes`: each mandatory slot's type once.
    #[must_use]
    pub fn mandatory_slot_types(&self) -> Vec<i32> {
        let mut out = Vec::new();
        for slot in self.slots.iter().filter(|s| s.mandatory) {
            add_unique(&mut out, slot.solar_type);
        }
        out
    }

    /// `CollectOptionalSlotTypes`: each optional slot's type once.
    #[must_use]
    pub fn optional_slot_types(&self) -> Vec<i32> {
        let mut out = Vec::new();
        for slot in self.slots.iter().filter(|s| !s.mandatory) {
            add_unique(&mut out, slot.solar_type);
        }
        out
    }
}

/// One `[KillableSolar]` (`FRmKillableSolar`, 0x40 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct KillableSolar {
    /// `nickname`, `CreateID`.
    pub nickname: u32,
    /// `solar_type` in [`SOLAR_TYPES`], -1 when absent.
    pub solar_type: i32,
    /// `string_id`.
    pub string_id: i32,
    /// `faction = all`.
    pub any_faction: bool,
    /// The `faction` groups, each once.
    pub factions: Vec<u32>,
    /// `archetype`, `pub::GetNicknameId`.
    pub archetype: u32,
    /// `loadout`, `pub::GetLoadoutID`.
    pub loadout: u32,
    /// `hitpoints` value 0, clamped to the archetype's maximum.
    pub hitpoints_min: f32,
    /// `hitpoints` value 1 (value 0 when empty), clamped likewise.
    pub hitpoints_max: f32,
    /// `difficulty` value 0.
    pub difficulty_min: f32,
    /// `difficulty` value 1.
    pub difficulty_max: f32,
    /// `only_allowed_in_zone_type`: rock 0x40, debris 0x80, ice 0x100,
    /// lava 0x200, crystal 0x800.
    pub zone_type_mask: u32,
    /// `pilot_choices` (1).
    pub pilot_choices: i32,
}

/// One `[critical_loot]` (`FRmCriticalLoot`, 0x34 bytes).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CriticalLoot {
    /// `why_desired` values 0 and 1.
    pub why_desired: [i32; 2],
    /// `name`.
    pub name: i32,
    /// `archetype`, `pub::GetEquipmentID`.
    pub archetype: u32,
    /// `type`: destroy 1, tractor 2, return 3, each once.
    pub types: Vec<u32>,
    /// `faction = all`.
    pub any_faction: bool,
    /// The `faction` groups, each once.
    pub factions: Vec<u32>,
}

/// The generator constructor's tables, in its load order.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct RmTables {
    /// `GRmNpcRankToDifficulty`: an NPC rank to the difficulty of each
    /// formation size from 1.
    pub npc_rank_to_difficulty: BTreeMap<i32, Vec<f32>>,
    /// `GRmDiff2Money`, in key order.
    pub diff2money: Vec<(f32, i32)>,
    /// `GRmBonusLootDropChance`, in `[0, 1]`.
    pub bonus_loot_drop_chance: f32,
    /// `GRmBonusLoot`: a faction ([`ALL_FACTIONS`] for `all`) to its
    /// records.
    pub bonus_loot: BTreeMap<u32, Vec<BonusLootEntry>>,
    /// `GRmSolarFormations`, by nickname id.
    pub solar_formations: BTreeMap<u32, SolarFormation>,
    /// `GRmKillableSolars`, by nickname id.
    pub killable_solars: BTreeMap<u32, KillableSolar>,
    /// `GRmCriticalLoot`, in file order.
    pub critical_loot: Vec<CriticalLoot>,
}

/// `LoadNpcRankToDifficulty` (`RandomMissionGen.cpp:171-189`): each
/// `NPCRank = rank, d1, d2, ...` appends its values from index 1 to the
/// rank's list (a repeated rank appends to the first).
pub fn load_npc_rank_to_difficulty(table: &mut BTreeMap<i32, Vec<f32>>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"RankAndFormationSizeToDifficulty") || !reader.read_value() {
            continue;
        }
        loop {
            if reader.is_value(b"NPCRank") {
                let rank = reader.value_int(0);
                let list = table.entry(rank).or_default();
                let mut index = 1;
                while !reader.is_value_empty(index) {
                    list.push(reader.value_float(index) as f32);
                    index += 1;
                }
            }
            if !reader.read_value() {
                break;
            }
        }
    }
}

/// The `[Diff2Money]` loop of the generator's constructor
/// (`RandomMissionGen.cpp:326-345`): `Diff2Money = difficulty, money`, a
/// unique insert into the map.
pub fn load_diff2money(table: &mut Vec<(f32, i32)>, reader: &mut IniReader) {
    while reader.read_header() {
        if !reader.is_header(b"Diff2Money") || !reader.read_value() {
            continue;
        }
        loop {
            if reader.is_value(b"Diff2Money") {
                let difficulty = reader.value_float(0) as f32;
                let money = reader.value_int(1);
                // `std::map<float, int>::insert` with `operator<`.
                let at = table.iter().position(|&(k, _)| !(k < difficulty));
                match at {
                    Some(i) if !(difficulty < table[i].0) => {}
                    Some(i) => table.insert(i, (difficulty, money)),
                    None => table.push((difficulty, money)),
                }
            }
            if !reader.read_value() {
                break;
            }
        }
    }
}

/// `pub::GetEquipmentID` into a slot: the id of a non-empty name (written
/// whether the id is valid or not), else the slot is left.
fn equipment_id_into(slot: &mut u32, name: &[u8]) {
    if !c_str(name).is_empty() {
        *slot = create_id(c_str(name));
    }
}

/// `LoadRmBonusLootSection` (`RandomMissionDB.cpp:478-544`): one
/// `[RMBonusLoot]`'s values, the record appended to each of its factions.
fn load_bonus_loot_section(
    cx: &Cx<'_>,
    table: &mut BTreeMap<u32, Vec<BonusLootEntry>>,
    reader: &mut IniReader,
) {
    let mut record = BonusLootEntry::default();
    let mut factions = Vec::new();
    while reader.read_value() {
        if reader.is_value(b"archetype") {
            equipment_id_into(&mut record.archetype, &reader.value_string_at(0));
        } else if reader.is_value(b"num_to_drop") {
            record.num_to_drop_min = reader.value_int(0);
            record.num_to_drop_max = if reader.is_value_empty(1) {
                record.num_to_drop_min
            } else {
                reader.value_int(1)
            };
        } else if reader.is_value(b"faction") {
            if eq_ignore_case(&reader.value_string_at(0), b"all") {
                add_unique(&mut factions, ALL_FACTIONS);
            } else {
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    add_unique(
                        &mut factions,
                        reputation_group(cx, &reader.value_string_at(index)),
                    );
                    index += 1;
                }
            }
        } else if reader.is_value(b"difficulty_range") {
            record.difficulty_min = reader.value_float(0) as f32;
            record.difficulty_max = reader.value_float(1) as f32;
        } else if reader.is_value(b"weight") {
            record.weight = reader.value_int(0) as f32;
        }
    }
    for faction in factions {
        table.entry(faction).or_default().push(record);
    }
}

/// `LoadRmLootInfo` (`RandomMissionDB.cpp:547-571`): `[Global]`
/// `BonusLootDropChance` (clamped to `[0, 1]`, a NaN to 0) and the
/// `[RMBonusLoot]` sections.
pub fn load_rm_loot_info(cx: &Cx<'_>, tables: &mut RmTables, reader: &mut IniReader) {
    while reader.read_header() {
        if reader.is_header(b"Global") {
            while reader.read_value() {
                if !reader.is_value(b"BonusLootDropChance") {
                    continue;
                }
                let chance = reader.value_float(0) as f32;
                tables.bonus_loot_drop_chance = if !(chance > 0.0) {
                    0.0
                } else if !(chance < 1.0) {
                    1.0
                } else {
                    chance
                };
            }
        } else if reader.is_header(b"RMBonusLoot") {
            load_bonus_loot_section(cx, &mut tables.bonus_loot, reader);
        }
    }
}

/// `LoadSolarFormations` (`RandomMissionDB.cpp:829-937`). The mandatory
/// flag and the solar type live for the whole call, so a `pos` line with
/// an unknown word keeps the previous line's value; the C++ starts both
/// uninitialised, the port at 0 (D870).
pub fn load_solar_formations(table: &mut BTreeMap<u32, SolarFormation>, reader: &mut IniReader) {
    let mut mandatory = false;
    let mut solar_type = 0;
    while reader.read_header() {
        if !reader.is_header(b"SolarFormation") {
            continue;
        }
        let mut nickname = 0;
        let mut formation = SolarFormation {
            nickname: 0,
            kind: -1,
            slots: Vec::new(),
        };
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                nickname = create_id(c_str(&reader.value_string()));
            } else if reader.is_value(b"type") {
                if let Some(kind) = named_value(&SOLAR_FORMATION_TYPES, &reader.value_string_at(0))
                {
                    formation.kind = kind;
                }
            } else if reader.is_value(b"pos") {
                let word = reader.value_string_at(0);
                if eq_ignore_case(&word, b"mandatory") {
                    mandatory = true;
                } else if eq_ignore_case(&word, b"optional") {
                    mandatory = false;
                }
                if let Some(kind) = named_value(&SOLAR_TYPES, &reader.value_string_at(1)) {
                    solar_type = kind;
                }
                let x = reader.value_float(2) as f32;
                let y = reader.value_float(3) as f32;
                let z = reader.value_float(4) as f32;
                let mut orientation = Matrix::IDENTITY;
                if !reader.is_value_empty(5) {
                    let w = reader.value_float(5) as f32;
                    let qx = reader.value_float(6) as f32;
                    let qy = reader.value_float(7) as f32;
                    let qz = reader.value_float(8) as f32;
                    // The sum of squares from the last part to the first, the
                    // inverse length kept unrounded.
                    let (fw, fx, fy, fz) =
                        (f64::from(w), f64::from(qx), f64::from(qy), f64::from(qz));
                    let sum = fz * fz + fy * fy + fx * fx + fw * fw;
                    let inv = 1.0 / sum.sqrt();
                    let quat = Quaternion {
                        w: (fw * inv) as f32,
                        x: (fx * inv) as f32,
                        y: (fy * inv) as f32,
                        z: (inv * fz) as f32,
                    };
                    orientation = quaternion_to_matrix(&quat);
                }
                formation.slots.push(SolarFormationSlot {
                    mandatory,
                    orientation,
                    position: Vector::new(x, y, z),
                    solar_type,
                });
            }
        }
        formation.nickname = nickname;
        table.entry(nickname).or_insert(formation);
    }
}

/// `LoadKillableSolars` (`RandomEncounters.cpp:60-209`): the complete
/// entries in range, the hit points clamped to the archetype's maximum, a
/// unique insert by nickname. The maximum's slot lives for the whole call
/// (a failed lookup compares against the previous section's value); the
/// C++ starts it uninitialised, the port at 0 (D870).
pub fn load_killable_solars(
    cx: &Cx<'_>,
    table: &mut BTreeMap<u32, KillableSolar>,
    reader: &mut IniReader,
) {
    let mut max_hit_points = 0;
    while reader.read_header() {
        if !reader.is_header(b"KillableSolar") {
            continue;
        }
        let mut e = KillableSolar {
            nickname: 0,
            solar_type: -1,
            string_id: 0,
            any_faction: false,
            factions: Vec::new(),
            archetype: 0,
            loadout: 0,
            hitpoints_min: 0.0,
            hitpoints_max: 0.0,
            difficulty_min: 0.0,
            difficulty_max: 0.0,
            zone_type_mask: 0,
            pilot_choices: 1,
        };
        while reader.read_value() {
            if reader.is_value(b"nickname") {
                e.nickname = create_id(c_str(&reader.value_string()));
            } else if reader.is_value(b"solar_type") {
                if let Some(kind) = named_value(&SOLAR_TYPES, &reader.value_string()) {
                    e.solar_type = kind;
                }
            } else if reader.is_value(b"string_id") {
                e.string_id = reader.value_int(0);
            } else if reader.is_value(b"faction") {
                if eq_ignore_case(c_str(&reader.value_string()), b"all") {
                    e.any_faction = true;
                } else {
                    e.any_faction = false;
                    let mut index = 0;
                    while !reader.is_value_empty(index) {
                        add_unique(
                            &mut e.factions,
                            reputation_group(cx, &reader.value_string_at(index)),
                        );
                        index += 1;
                    }
                }
            } else if reader.is_value(b"archetype") {
                e.archetype = api::misc::nickname_id(c_str(&reader.value_string()));
            } else if reader.is_value(b"loadout") {
                if let Ok(id) = api::misc::loadout_id(&reader.value_string()) {
                    e.loadout = id;
                }
            } else if reader.is_value(b"hitpoints") {
                e.hitpoints_min = reader.value_int(0) as f32;
                e.hitpoints_max = if reader.is_value_empty(1) {
                    e.hitpoints_min
                } else {
                    reader.value_int(1) as f32
                };
            } else if reader.is_value(b"difficulty") {
                e.difficulty_min = reader.value_float(0) as f32;
                e.difficulty_max = reader.value_float(1) as f32;
            } else if reader.is_value(b"only_allowed_in_zone_type") {
                e.zone_type_mask = 0;
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    let word = reader.value_string_at(index);
                    for (name, bit) in [
                        (&b"rock"[..], 0x40),
                        (b"debris", 0x80),
                        (b"ice", 0x100),
                        (b"lava", 0x200),
                        (b"crystal", 0x800),
                    ] {
                        if eq_ignore_case(&word, name) {
                            e.zone_type_mask |= bit;
                            break;
                        }
                    }
                    index += 1;
                }
            } else if reader.is_value(b"pilot_choices") {
                e.pilot_choices = reader.value_int(0);
            }
        }
        if e.nickname == 0
            || e.solar_type == -1
            || (!e.any_faction && e.factions.is_empty())
            || e.archetype == 0
            || e.loadout == 0
            || !(e.hitpoints_min > 0.0)
            || !(e.hitpoints_max >= e.hitpoints_min)
            || !(e.difficulty_min >= 0.0)
            || !(e.difficulty_min <= e.difficulty_max)
            || !(e.difficulty_max <= 100.0)
        {
            continue;
        }
        if let Ok(hp) = api::misc::max_hit_points(cx, e.archetype) {
            max_hit_points = hp;
        }
        if f64::from(e.hitpoints_max) > f64::from(max_hit_points) {
            e.hitpoints_min = max_hit_points as f32;
            e.hitpoints_max = max_hit_points as f32;
        }
        table.entry(e.nickname).or_insert(e);
    }
}

/// `LoadVignetteCriticalLoot` (`RandomEncounters.cpp:258-327`): each
/// `[critical_loot]` appends a fresh record and fills it. The C++ leaves
/// `why_desired` and the archetype of the fresh record unset; the
/// reconstruction and the port start them at 0.
pub fn load_vignette_critical_loot(
    cx: &Cx<'_>,
    table: &mut Vec<CriticalLoot>,
    reader: &mut IniReader,
) {
    while reader.read_header() {
        if !reader.is_header(b"critical_loot") {
            continue;
        }
        let mut record = CriticalLoot::default();
        while reader.read_value() {
            if reader.is_value(b"faction") {
                if eq_ignore_case(c_str(&reader.value_string()), b"all") {
                    record.any_faction = true;
                } else {
                    record.any_faction = false;
                    let mut index = 0;
                    while !reader.is_value_empty(index) {
                        add_unique(
                            &mut record.factions,
                            reputation_group(cx, &reader.value_string_at(index)),
                        );
                        index += 1;
                    }
                }
            } else if reader.is_value(b"archetype") {
                equipment_id_into(&mut record.archetype, &reader.value_string_at(0));
            } else if reader.is_value(b"name") {
                record.name = reader.value_int(0);
            } else if reader.is_value(b"why_desired") {
                record.why_desired = [reader.value_int(0), reader.value_int(1)];
            } else if reader.is_value(b"type") {
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    let word = reader.value_string_at(index);
                    if eq_ignore_case(&word, b"destroy") {
                        add_unique(&mut record.types, 1);
                    } else if eq_ignore_case(&word, b"tractor") {
                        add_unique(&mut record.types, 2);
                    } else if eq_ignore_case(&word, b"return") {
                        add_unique(&mut record.types, 3);
                    }
                    index += 1;
                }
            }
        }
        table.push(record);
    }
}

/// `(rand() * count) / 0x8000` for more than one candidate, index 0 for
/// one, without a draw.
pub(crate) fn random_index(cx: &mut Cx<'_>, count: usize) -> usize {
    let count = count as i32;
    if count - 1 == 0 {
        return 0;
    }
    (cx.world.rng().rand().wrapping_mul(count) / 0x8000) as usize
}

/// `weighted_vector::Pick` in a server process (the 53-bit x87 mode): the
/// weights summed in `f64`, the total stored as a float, the target
/// `rand() * 2^-15 * total` stored as a float, and the first entry whose
/// running `f64` sum reaches it. `None` for no entries, a zero sum or a
/// run past the end; no draw for the first two.
pub(crate) fn weighted_pick(cx: &mut Cx<'_>, weights: &[f32]) -> Option<usize> {
    if weights.is_empty() {
        return None;
    }
    let sum: f64 = weights.iter().fold(0.0, |s, &w| s + f64::from(w));
    let total = sum as f32;
    if sum == 0.0 {
        return None;
    }
    let target =
        (f64::from(cx.world.rng().rand()) * f64::from(1.0f32 / 32768.0) * f64::from(total)) as f32;
    let mut acc = 0.0;
    for (index, &w) in weights.iter().enumerate() {
        acc += f64::from(w);
        if f64::from(target) <= acc {
            return Some(index);
        }
    }
    None
}

impl RmTables {
    /// `RmDifficultyToMoney` (`RandomMissionGen.cpp:196-229`): the money of
    /// the first key above `difficulty`, interpolated from the key before
    /// it and truncated, then [`round_money`]. Past the last key the C++
    /// reads the map's head node, which holds no value; at the first key
    /// it steps onto the head node too. The reconstruction answers 0 and
    /// interpolates from `{0, 0}`, and so does the port (D871).
    #[must_use]
    pub fn difficulty_to_money(&self, difficulty: f32) -> u32 {
        let Some(hi) = self.diff2money.iter().position(|&(k, _)| k > difficulty) else {
            return 0;
        };
        let (lo_key, lo_money) = if hi == 0 {
            (0.0f32, 0)
        } else {
            self.diff2money[hi - 1]
        };
        let (hi_key, hi_money) = self.diff2money[hi];
        let fraction =
            (f64::from(difficulty) - f64::from(lo_key)) / (f64::from(hi_key) - f64::from(lo_key));
        let money = trunc_to_i32(
            fraction * f64::from(hi_money.wrapping_sub(lo_money)) + f64::from(lo_money),
        );
        round_money(money as u32)
    }

    /// `PickRmBonusLoot` (`RandomMissionDB.cpp:576-622`): a weighted pick
    /// among the `all` records and then the faction's whose difficulty range
    /// holds `difficulty`. A zero total draws nothing and gives `None`.
    pub fn pick_bonus_loot(
        &self,
        cx: &mut Cx<'_>,
        faction: u32,
        difficulty: f32,
    ) -> Option<BonusLootEntry> {
        let mut candidates = Vec::new();
        for key in [ALL_FACTIONS, faction] {
            if let Some(records) = self.bonus_loot.get(&key) {
                for r in records {
                    if r.difficulty_min <= difficulty && difficulty <= r.difficulty_max {
                        candidates.push(*r);
                    }
                }
            }
        }
        if candidates.is_empty() {
            return None;
        }
        let sum: f64 = candidates.iter().fold(0.0, |s, c| s + f64::from(c.weight));
        let total = sum as f32;
        if sum == 0.0 {
            return None;
        }
        let target =
            (f64::from(cx.world.rng().rand()) * 3.051_757_812_5e-5 * f64::from(total)) as f32;
        let mut acc = 0.0;
        for c in &candidates {
            acc += f64::from(c.weight);
            if f64::from(target) <= acc {
                return Some(*c);
            }
        }
        None
    }

    /// `PickKillableSolar` (`RandomEncounters.cpp:213-254`): a random entry
    /// of the faction (or any), of the type, whose difficulty range holds
    /// `difficulty` and whose zone mask is within `zone_mask`.
    pub fn pick_killable_solar(
        &self,
        cx: &mut Cx<'_>,
        faction: u32,
        solar_type: i32,
        zone_mask: u32,
        difficulty: f32,
    ) -> Option<&KillableSolar> {
        let candidates: Vec<&KillableSolar> = self
            .killable_solars
            .values()
            .filter(|e| {
                (e.any_faction || e.factions.contains(&faction))
                    && e.solar_type == solar_type
                    && e.difficulty_min <= difficulty
                    && difficulty <= e.difficulty_max
                    && (e.zone_type_mask | zone_mask) == zone_mask
            })
            .collect();
        if candidates.is_empty() {
            return None;
        }
        let index = random_index(cx, candidates.len());
        Some(candidates[index])
    }

    /// `PickCriticalLoot` (`RandomEncounters.cpp:331-350`): a random record
    /// of the faction (or any) with the type, by index.
    pub fn pick_critical_loot(&self, cx: &mut Cx<'_>, faction: u32, kind: u32) -> Option<usize> {
        let candidates: Vec<usize> = self
            .critical_loot
            .iter()
            .enumerate()
            .filter(|(_, r)| {
                (r.any_faction || r.factions.contains(&faction)) && r.types.contains(&kind)
            })
            .map(|(i, _)| i)
            .collect();
        if candidates.is_empty() {
            return None;
        }
        let index = random_index(cx, candidates.len());
        Some(candidates[index])
    }

    /// `PickSolarFormation` (`RandomMissionGen.cpp:45-102`): a random
    /// formation, in key order, whose mandatory slot types are all in
    /// `types` and whose mandatory and optional types cover `types`; at
    /// most 0x40 candidates. `rand() * count / 0x8000` even for one.
    pub fn pick_solar_formation(&self, cx: &mut Cx<'_>, types: &[i32]) -> Option<u32> {
        let mut candidates = Vec::new();
        for (&key, formation) in &self.solar_formations {
            let mandatory = formation.mandatory_slot_types();
            let optional = formation.optional_slot_types();
            let fits = mandatory.iter().all(|t| types.contains(t))
                && types
                    .iter()
                    .all(|t| mandatory.contains(t) || optional.contains(t));
            if !fits {
                continue;
            }
            candidates.push(key);
            if candidates.len() == 0x40 {
                break;
            }
        }
        if candidates.is_empty() {
            return None;
        }
        let product = cx.world.rng().rand().wrapping_mul(candidates.len() as i32);
        let index = (product + ((product >> 31) & 0x7fff)) >> 15;
        candidates.get(index as usize).copied()
    }
}

/// `RoundRmMoney` (`RandomMissionGen.cpp:234-256`): above 100 the two
/// leading decimal digits are kept and, when the first is below 5, the
/// third rounded to 0 or 5 in the unit below the second.
#[must_use]
pub fn round_money(mut amount: u32) -> u32 {
    if amount > 100 {
        let mut digits = Vec::new();
        let mut power: u32 = 1;
        while amount != 0 {
            power = power.wrapping_mul(10);
            digits.push(amount % 10);
            amount /= 10;
        }
        let count = digits.len();
        let second = power / 10 / 10;
        amount = digits[count - 1]
            .wrapping_mul(power / 10)
            .wrapping_add(digits[count - 2].wrapping_mul(second));
        if digits[count - 1] < 5 {
            let third = if digits[count - 3] < 5 { 0 } else { 5 };
            amount = amount.wrapping_add(third * (second / 10));
        }
    }
    amount
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn money_keeps_two_digits_and_a_half_step() {
        assert_eq!(round_money(99), 99);
        assert_eq!(round_money(1234), 1200);
        assert_eq!(round_money(1264), 1250);
        assert_eq!(round_money(7264), 7200);
        assert_eq!(round_money(13994), 13500);
        assert_eq!(round_money(247_065), 245_000);
    }

    #[test]
    fn diff2money_is_a_unique_sorted_insert() {
        let mut reader = IniReader::new();
        reader.open_memory(
            b"[Diff2Money]\nDiff2Money = 1, 10\nDiff2Money = 0, 5\nDiff2Money = 1, 99\n",
        );
        let mut table = Vec::new();
        load_diff2money(&mut table, &mut reader);
        assert_eq!(table, vec![(0.0, 5), (1.0, 10)]);
    }
}
