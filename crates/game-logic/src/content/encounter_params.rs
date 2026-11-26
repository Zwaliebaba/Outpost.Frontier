//! The encounter parameters of TU29 (`Content/EncounterParams.cpp`): the
//! files a system's `[EncounterParameters]` sections name, parsed into
//! formations, ship lines and permutations (work package P3.4c, decision
//! record 0008 point 10).
//!
//! The system record's loader reads them while it reads the zones
//! (`SystemInfo.cpp:84-114`), and a zone's `encounter` key copies them
//! ([`super::zones`]); that loader is P3.4d's. The run time of the
//! parameters (`Refresh`, the faction picks, `Spawn` and the formation
//! spawn) is the populators' (P3.6d).

use std::collections::BTreeMap;

use neuron_core::hash::{c_str, eq_ignore_case};
use neuron_core::id::create_id;
use neuron_core::ini::{IniReader, parse_number};
use neuron_core::num::trunc_to_i32_via_i64;

/// One formation reference of a permutation (`FEncounterPermutationEntry`):
/// `"permutation = 1 + 2* 3"` gives `{1, false}`, `{2, true}`, `{3, false}`.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PermutationEntry {
    /// `ParseNumber`, truncated.
    pub formation_index: i32,
    /// A `*` followed the number.
    pub star: bool,
}

/// `FEncounterPermutation`.
#[derive(Clone, Debug, PartialEq)]
pub struct Permutation {
    /// The formation references.
    pub entries: Vec<PermutationEntry>,
    /// Value 1, an integer as a float.
    pub weight: f32,
}

/// One ship line of an `[EncounterFormation]` (`FEncounterShip`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct EncounterShip {
    /// `ship_by_class` value 2, then the `diff<n>` tags of
    /// [`EncounterParameters::apply_difficulty`]; each id once.
    pub ship_classes: Vec<u32>,
    /// `ship_by_npc_arch` value 2 (0 when cleared).
    pub npc_arch: u32,
    /// `pilot_job` (the last ship line).
    pub pilot_job: u32,
    /// Value 0 (1 by default).
    pub min_count: i32,
    /// Value 1 (1 by default).
    pub max_count: i32,
    /// `make_class` (the last ship line), each id once.
    pub make_classes: Vec<u32>,
    /// `ship_by_class` value 3 (0 by default).
    pub difficulty_offset: i32,
    /// `+0x14`: the line's archetype the spawn resolved (its npc
    /// archetype, else one picked for the formation's faction), by id; the
    /// run time's (P3.6d).
    pub arch: Option<u32>,
}

impl Default for EncounterShip {
    fn default() -> Self {
        Self {
            ship_classes: Vec::new(),
            npc_arch: 0,
            pilot_job: 0,
            min_count: 1,
            max_count: 1,
            make_classes: Vec::new(),
            difficulty_offset: 0,
            arch: None,
        }
    }
}

/// The arrival flags' indices (`FEncounterFormation::Arrival`); index 0 is
/// never written.
pub mod arrival {
    /// `tradelane`.
    pub const TRADELANE: usize = 1;
    /// Set by `all` only.
    pub const ALL_ONLY: usize = 2;
    /// `object_docking_ring`.
    pub const DOCKING_RING: usize = 3;
    /// `object_jump_gate`.
    pub const JUMP_GATE: usize = 4;
    /// `object_station`.
    pub const STATION: usize = 5;
    /// `object_capital`.
    pub const CAPITAL: usize = 6;
    /// `cruise`.
    pub const CRUISE: usize = 7;
    /// `buzz`.
    pub const BUZZ: usize = 8;
}

/// One `[EncounterFormation]` (`FEncounterFormation`, 0x80 bytes): the
/// parsed fields. The faction choices, the picked faction and the created
/// objects are the run time's (P3.6d) and start empty.
#[derive(Clone, Debug, PartialEq)]
pub struct EncounterFormation {
    /// 1 once `feeling_to_formation` was read.
    pub feeling_to_formation: i32,
    /// once 0, synch 1, every 2 (2).
    pub reroll_frequency: i32,
    /// friendly 1, hostile 4, same 8, any 7 (7).
    pub feeling_mask: u32,
    /// `+0x0c`: -1; no reader is known.
    pub field_0x0c: i32,
    /// `feeling_to_formation` value 1 (-1).
    pub feeling_value: i32,
    /// `+0x14`: the faction choices (`weighted_vector`), the run time's.
    pub factions: Vec<(u32, f32)>,
    /// `+0x24`: the picked faction (`0xffffffff`), the run time's.
    pub faction: u32,
    /// `ship_by_class` and `ship_by_npc_arch`.
    pub ships: Vec<EncounterShip>,
    /// `formation` (set only while both formation ids are 0).
    pub formation: u32,
    /// `formation_by_class` (the same rule).
    pub formation_by_class: u32,
    /// `behavior`, [`parse_guide_behavior`] (0).
    pub behavior: i32,
    /// The nine arrival flags ([`arrival`]; all 1 by default).
    pub arrival: [u8; 9],
    /// `zone_creation_distance` value 0 as a double (1500).
    pub zone_creation_distance: f64,
    /// `+0x60`: the created objects, the run time's.
    pub created_objects: Vec<u32>,
    /// `times_to_create`, -1 for `infinite` (-1).
    pub times_to_create: i32,
    /// `allow_simultaneous_creation` (true).
    pub allow_simultaneous_creation: bool,
    /// `+0x78`: 0; the run time zeroes it on a reroll.
    pub field_0x78: i32,
    /// `longevity` (24).
    pub longevity: i32,
}

impl EncounterFormation {
    /// `FEncounterFormation(Reader)`: the defaults, then
    /// [`EncounterFormation::parse_section`], whose result is dropped.
    pub fn new(reader: &mut IniReader) -> Self {
        let mut formation = Self {
            feeling_to_formation: 0,
            reroll_frequency: 2,
            feeling_mask: 7,
            field_0x0c: -1,
            feeling_value: -1,
            factions: Vec::new(),
            faction: u32::MAX,
            ships: Vec::new(),
            formation: 0,
            formation_by_class: 0,
            behavior: 0,
            arrival: [1; 9],
            zone_creation_distance: 1500.0,
            created_objects: Vec::new(),
            times_to_create: -1,
            allow_simultaneous_creation: true,
            field_0x78: 0,
            longevity: 0x18,
        };
        formation.parse_section(reader);
        formation
    }

    /// `ParseSection` (`:115-226`): the section's values; true when it has a
    /// ship line.
    pub fn parse_section(&mut self, reader: &mut IniReader) -> bool {
        while reader.read_value() {
            if reader.is_value(b"ship_by_class") {
                let mut ship = EncounterShip {
                    min_count: reader.value_int(0),
                    max_count: reader.value_int(1),
                    ..EncounterShip::default()
                };
                add_unique(&mut ship.ship_classes, id_of(&reader.value_string_at(2)));
                if !reader.is_value_empty(3) {
                    ship.difficulty_offset = reader.value_int(3);
                }
                self.ships.push(ship);
            } else if reader.is_value(b"ship_by_npc_arch") {
                self.ships.push(EncounterShip {
                    min_count: reader.value_int(0),
                    max_count: reader.value_int(1),
                    npc_arch: id_of(&reader.value_string_at(2)),
                    ..EncounterShip::default()
                });
            } else if reader.is_value(b"make_class") {
                let Some(ship) = self.ships.last_mut() else {
                    continue;
                };
                let mut index = 0;
                while !reader.is_value_empty(index) {
                    add_unique(
                        &mut ship.make_classes,
                        id_of(&reader.value_string_at(index)),
                    );
                    index += 1;
                }
            } else if reader.is_value(b"pilot") {
                // Recognised and ignored.
            } else if reader.is_value(b"pilot_job") {
                if let Some(ship) = self.ships.last_mut() {
                    ship.pilot_job = id_of(&reader.value_string());
                }
            } else if reader.is_value(b"formation") {
                if self.formation == 0 && self.formation_by_class == 0 {
                    self.formation = id_of(&reader.value_string());
                }
            } else if reader.is_value(b"formation_by_class") {
                if self.formation_by_class == 0 && self.formation == 0 {
                    self.formation_by_class = id_of(&reader.value_string());
                }
            } else if reader.is_value(b"behavior") {
                self.behavior = parse_guide_behavior(&reader.value_string());
            } else if reader.is_value(b"arrival") {
                self.parse_arrival(reader);
            } else if reader.is_value(b"longevity") {
                self.longevity = reader.value_int(0);
            } else if reader.is_value(b"allow_simultaneous_creation") {
                // Exact compares; any other value leaves the flag.
                let value = reader.value_string();
                let value = c_str(&value);
                if value == b"yes" || value == b"true" {
                    self.allow_simultaneous_creation = true;
                } else if value == b"no" || value == b"false" {
                    self.allow_simultaneous_creation = false;
                }
            } else if reader.is_value(b"zone_creation_distance") {
                self.zone_creation_distance = f64::from(reader.value_int(0));
            } else if reader.is_value(b"times_to_create") {
                if c_str(&reader.value_string()) == b"infinite" {
                    self.times_to_create = -1;
                } else {
                    self.times_to_create = reader.value_int(0);
                }
            } else if (reader.is_value(b"feeling_to_local")
                || reader.is_value(b"feeling_to_group")
                || reader.is_value(b"feeling_to_formation")
                || reader.is_value(b"explicit_group"))
                && self.feeling_to_formation == 0
            {
                self.parse_feeling(reader);
            }
        }
        !self.ships.is_empty()
    }

    /// `ParseFeeling` (`:231-268`). Every call resets the feeling value; its
    /// only caller passes the four feeling keys, so the
    /// `group_reroll_frequency` branch is unreachable.
    pub fn parse_feeling(&mut self, reader: &IniReader) {
        self.feeling_value = -1;
        if reader.is_value(b"group_reroll_frequency") {
            match c_str(&reader.value_string_at(0)) {
                b"synch" => self.reroll_frequency = 1,
                b"every" => self.reroll_frequency = 2,
                b"once" => self.reroll_frequency = 0,
                _ => {}
            }
            return;
        }
        if !reader.is_value(b"feeling_to_formation") {
            return;
        }
        self.feeling_to_formation = 1;
        match c_str(&reader.value_string_at(0)) {
            b"friendly" => self.feeling_mask = 1,
            b"hostile" => self.feeling_mask = 4,
            b"same" => self.feeling_mask = 8,
            b"any" => self.feeling_mask = 7,
            _ => {}
        }
        self.reroll_frequency = 2;
        self.feeling_value = reader.value_int(1);
    }

    /// `ParseArrival` (`:271-349`): every flag cleared, then each value
    /// (`_stricmp`, at most 49 bytes) sets its flags, or clears them after a
    /// leading `-`.
    pub fn parse_arrival(&mut self, reader: &IniReader) {
        use arrival::*;
        self.arrival = [0; 9];
        let mut index = 0;
        while !reader.is_value_empty(index) {
            let value = reader.value_string_at(index);
            index += 1;
            let value = c_str(&value);
            let (set, name) = match value.split_first() {
                Some((b'-', rest)) => (0, rest),
                _ => (1, value),
            };
            let name = &name[..name.len().min(0x31)];
            let flags: &[usize] = if eq_ignore_case(name, b"all") {
                &[
                    TRADELANE,
                    ALL_ONLY,
                    STATION,
                    DOCKING_RING,
                    JUMP_GATE,
                    CAPITAL,
                    CRUISE,
                    BUZZ,
                ]
            } else if eq_ignore_case(name, b"tradelane") {
                &[TRADELANE]
            } else if eq_ignore_case(name, b"object_all") {
                &[STATION, DOCKING_RING, JUMP_GATE, CAPITAL]
            } else if eq_ignore_case(name, b"object_docking_ring") {
                &[DOCKING_RING]
            } else if eq_ignore_case(name, b"object_capital") {
                &[CAPITAL]
            } else if eq_ignore_case(name, b"object_jump_gate") {
                &[JUMP_GATE]
            } else if eq_ignore_case(name, b"object_station") {
                &[STATION]
            } else if eq_ignore_case(name, b"cruise") {
                &[CRUISE]
            } else if eq_ignore_case(name, b"buzz") {
                &[BUZZ]
            } else {
                &[]
            };
            for &flag in flags {
                self.arrival[flag] = set;
            }
        }
    }
}

/// `ParseGuideBehavior` (`Guides.cpp:85-102`): the guide kind of a
/// `behavior` name (exact compares); 1 for another non-empty name, 0 for an
/// empty one.
#[must_use]
pub fn parse_guide_behavior(name: &[u8]) -> i32 {
    match c_str(name) {
        b"trade" => 2,
        b"patrol_path" => 3,
        b"nothing" => 1,
        b"wander" => 4,
        b"goto_guide" => 5,
        b"dock_guide" => 6,
        b"patrol_guide" => 7,
        b"" => 0,
        _ => 1,
    }
}

/// `FEncounterParameters` (0x4c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct EncounterParameters {
    /// The `[EncounterParameters]` nickname id.
    pub nickname: u32,
    /// The system.
    pub system: u32,
    /// 0; a zone's `encounter` key writes it.
    pub zone: u32,
    /// Value 2 of a `permutation` line (0).
    pub reroll_interval: f32,
    /// -1 until the run time refreshes the parameters.
    pub last_reroll_time: f32,
    /// -1 until [`EncounterParameters::apply_difficulty`].
    pub difficulty: i32,
    /// `[Creation] permutation`.
    pub permutations: Vec<Permutation>,
    /// `+0x28`: the permutation the run time picked, by index; `None`, the
    /// end of the empty list the constructor sees.
    pub current_permutation: Option<usize>,
    /// `[EncounterFormation]`.
    pub formations: Vec<EncounterFormation>,
    /// `+0x3c`: the faction weights a zone's `faction` lines give.
    pub factions: Vec<(u32, f32)>,
}

impl EncounterParameters {
    /// `FEncounterParameters(Reader, &Id, &SystemId)` (`:356-368`): the
    /// defaults, then [`EncounterParameters::load`], whose result is
    /// dropped.
    pub fn new(reader: &mut IniReader, id: u32, system: u32) -> Self {
        let mut parameters = Self {
            nickname: id,
            system,
            zone: 0,
            reroll_interval: 0.0,
            last_reroll_time: -1.0,
            difficulty: -1,
            permutations: Vec::new(),
            current_permutation: None,
            formations: Vec::new(),
            factions: Vec::new(),
        };
        parameters.load(reader);
        parameters
    }

    /// `Load` (`:370-410`): the `[EncounterFormation]` and `[Creation]`
    /// sections; without a permutation, one of every formation (weight 1).
    /// True when the first formation has a ship line.
    ///
    /// The C++ reads the first formation without testing that there is one,
    /// so a file without an `[EncounterFormation]` faults; the port answers
    /// false (D794).
    pub fn load(&mut self, reader: &mut IniReader) -> bool {
        while reader.read_header() {
            if reader.is_header(b"EncounterFormation") {
                self.formations.push(EncounterFormation::new(reader));
            } else if reader.is_header(b"Creation") {
                self.parse_creation(reader);
            }
        }
        let has_ships = self.formations.first().is_some_and(|f| !f.ships.is_empty());
        if self.permutations.is_empty() {
            let entries = (0..self.formations.len())
                .map(|index| PermutationEntry {
                    formation_index: index as i32,
                    star: false,
                })
                .collect();
            self.permutations.push(Permutation {
                entries,
                weight: 1.0,
            });
        }
        has_ships
    }

    /// `ParseCreation` (`:413-467`): each `permutation` line's text, value 0,
    /// into formation references: leading spaces skipped, the first number
    /// always taken (`ParseNumber`, truncated), then `' '` and `'+'`
    /// skipped, a `'*'` marking the last reference, and any other byte
    /// starting the next number. Value 1 is the weight, value 2 (when
    /// given) the reroll interval.
    pub fn parse_creation(&mut self, reader: &mut IniReader) {
        while reader.read_value() {
            if !reader.is_value(b"permutation") {
                continue;
            }
            let text = reader.value_string_at(0);
            let text = c_str(&text);
            let length = text.len();
            let mut position = 0;
            while position < length && text[position] == b' ' {
                position += 1;
            }
            let number = |at: usize| trunc_to_i32_via_i64(parse_number(&text[at..]).value);
            let mut entries = vec![PermutationEntry {
                formation_index: number(position),
                star: false,
            }];
            while position < length && text[position].is_ascii_digit() {
                position += 1;
            }
            while position < length {
                while position < length && (text[position] == b' ' || text[position] == b'+') {
                    position += 1;
                }
                if position == length {
                    break;
                }
                if text[position] == b'*' {
                    if let Some(last) = entries.last_mut() {
                        last.star = true;
                    }
                    position += 1;
                    continue;
                }
                entries.push(PermutationEntry {
                    formation_index: number(position),
                    star: false,
                });
                while position < length && text[position].is_ascii_digit() {
                    position += 1;
                }
            }
            self.permutations.push(Permutation {
                entries,
                weight: reader.value_int(1) as f32,
            });
            if !reader.is_value_empty(2) {
                self.reroll_interval = reader.value_int(2) as f32;
            }
        }
    }

    /// `ApplyDifficulty` (`:470-495`): the first call sets the difficulty
    /// and adds `diff<n>` to every ship line with ship classes, `n` the
    /// difficulty plus the line's offset, at least 1 (a `TString<10>`, at
    /// most 9 bytes).
    pub fn apply_difficulty(&mut self, level: i32) {
        if self.difficulty != -1 {
            return;
        }
        self.difficulty = level;
        for formation in &mut self.formations {
            for ship in &mut formation.ships {
                if ship.ship_classes.is_empty() {
                    continue;
                }
                let tag = ship.difficulty_offset.wrapping_add(level).max(1);
                let mut name = format!("diff{tag}").into_bytes();
                name.truncate(9);
                add_unique(&mut ship.ship_classes, create_id(&name));
            }
        }
    }
}

/// The parameters of one system file, by nickname id (the loader's
/// `EncounterParameters` map; a repeated id keeps its node and takes the
/// later parameters, where the C++ leaks the earlier object).
pub type SystemEncounterParameters = BTreeMap<u32, EncounterParameters>;

/// The `[EncounterParameters]` part of `FSystemInfo::LoadFromReader`
/// (`SystemInfo.cpp:84-114`): for each such section, its `nickname` and
/// `filename` lines (the first two values, in that order), and the file
/// opened through `open`; the parameters of a file that does not open are
/// left out. The rest of the system file is the system record's (P3.4d).
pub fn load_system_encounter_parameters(
    reader: &mut IniReader,
    system: u32,
    open: &mut dyn FnMut(&[u8]) -> Option<IniReader>,
) -> SystemEncounterParameters {
    let mut parameters = SystemEncounterParameters::new();
    while reader.read_header() {
        if !reader.is_header(b"EncounterParameters") {
            continue;
        }
        reader.read_value();
        let id = id_of(&reader.value_string());
        reader.read_value();
        let mut file = c_str(&reader.value_string()).to_vec();
        file.truncate(0x103);
        if let Some(mut file_reader) = open(&file) {
            parameters.insert(id, EncounterParameters::new(&mut file_reader, id, system));
        }
    }
    parameters
}

/// `CreateID` of a value.
fn id_of(text: &[u8]) -> u32 {
    create_id(c_str(text))
}

/// `AddUniqueId`: a linear find, then an append.
pub(crate) fn add_unique(ids: &mut Vec<u32>, id: u32) {
    if !ids.contains(&id) {
        ids.push(id);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn reader(text: &str) -> IniReader {
        let mut reader = IniReader::default();
        reader.open(b"test.ini", text.as_bytes().to_vec());
        reader
    }

    fn entries(p: &Permutation) -> Vec<(i32, bool)> {
        p.entries
            .iter()
            .map(|e| (e.formation_index, e.star))
            .collect()
    }

    #[test]
    fn permutations_parse_as_the_creation_section_reads_them() {
        let mut r = reader(
            "[EncounterFormation]\nship_by_class = 1, 2, sc_a, 1\n[EncounterFormation]\n\
             ship_by_npc_arch = 1, 1, arch\n[Creation]\npermutation = 0 + 1* 2, 5, 30\n\
             permutation =   3*, 1\n",
        );
        let p = EncounterParameters::new(&mut r, 7, 9);
        assert_eq!(p.permutations.len(), 2);
        assert_eq!(
            entries(&p.permutations[0]),
            [(0, false), (1, true), (2, false)]
        );
        assert_eq!((p.permutations[0].weight, p.reroll_interval), (5.0, 30.0));
        assert_eq!(entries(&p.permutations[1]), [(3, true)]);
        assert_eq!(p.formations[1].ships[0].npc_arch, create_id(b"arch"));
        assert_eq!(p.formations[0].ships[0].difficulty_offset, 1);
    }

    #[test]
    fn a_missing_permutation_takes_every_formation() {
        let mut r =
            reader("[EncounterFormation]\nship_by_class = 1, 1, sc_a\n[EncounterFormation]\n");
        let mut p = EncounterParameters::new(&mut r, 1, 2);
        assert_eq!(p.permutations.len(), 1);
        assert_eq!(p.permutations[0].weight, 1.0);
        assert_eq!(entries(&p.permutations[0]), [(0, false), (1, false)]);
        p.apply_difficulty(0);
        assert_eq!(
            p.formations[0].ships[0].ship_classes,
            [create_id(b"sc_a"), create_id(b"diff1")]
        );
        p.apply_difficulty(5);
        assert_eq!(p.difficulty, 0);
        // A file without a formation answers false (D794).
        let mut empty = reader("[Creation]\n");
        let q = EncounterParameters::new(&mut empty, 1, 2);
        assert!(q.formations.is_empty());
        assert_eq!(q.permutations[0].entries.len(), 0);
    }

    #[test]
    fn arrival_feeling_and_behaviour() {
        let mut r = reader(
            "[EncounterFormation]\nship_by_class = 1, 1, sc_a\narrival = all, -tradelane, -BUZZ\n\
             behavior = patrol_path\nfeeling_to_formation = hostile, 2\ntimes_to_create = 3\n\
             allow_simultaneous_creation = No\n",
        );
        r.read_header();
        let f = EncounterFormation::new(&mut r);
        assert_eq!(f.arrival, [0, 0, 1, 1, 1, 1, 1, 1, 0]);
        assert_eq!(f.behavior, 3);
        assert_eq!(
            (f.feeling_mask, f.feeling_value, f.feeling_to_formation),
            (4, 2, 1)
        );
        assert_eq!(f.times_to_create, 3);
        assert!(f.allow_simultaneous_creation);
        assert_eq!(parse_guide_behavior(b"Trade"), 1);
        assert_eq!(parse_guide_behavior(b""), 0);
    }
}
