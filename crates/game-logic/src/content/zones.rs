//! A system's zones as Content keeps them (`FSystemZone`, TU64
//! `Content/Zones.cpp`) with their population block (`FZonePopulation`,
//! TU30 `Content/Encounters.cpp:216-298`): work package P3.4c, decision
//! record 0008 point 10.
//!
//! [`enumerate_system_zones`] is the zone part of
//! `FSystemInfo::LoadFromReader` (`SystemInfo.cpp:264-286`): the system's
//! zones in file order through `pub::System::EnumerateZones`, each parsed
//! by [`parse_system_zone`], with the path labels the patrol paths are
//! built from ([`super::patrol_paths`]). The system record that holds them
//! and calls it is P3.4d's; the population's run time (`Validate`,
//! `Populate`, the spawn chance) is the populators' (P3.6d).

use game_sim::data::DataSource;
use game_sim::data::zone::ZoneShape;
use neuron_core::hash::{c_str, stricmp};
use neuron_core::id::create_id;
use neuron_core::ini::{IniReader, IniState};
use neuron_core::num::trunc_to_i32_via_i64;

use super::encounter_params::{EncounterParameters, SystemEncounterParameters};
use super::tables::reputation_group;
use crate::api;
use crate::server::Cx;

/// `"density_restriction = count, type"` (`FDensityRestriction`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DensityRestriction {
    /// Value 0.
    pub max_count: i32,
    /// Value 1, `CreateID`.
    pub kind: u32,
}

/// A zone's `encounter` (`FZoneEncounter`): the system's parameters of
/// that id, bound to the zone and its level, with the chance.
#[derive(Clone, Debug, PartialEq)]
pub struct ZoneEncounter {
    /// The copied parameters.
    pub parameters: EncounterParameters,
    /// Value 2.
    pub chance: f32,
}

/// `FZonePopulation` (0x44 bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct ZonePopulation {
    /// `density` (0).
    pub density: i32,
    /// `repop_time` (10; a value not above 0 is 1).
    pub repop_time: f32,
    /// `Validate`'s result, the run time's (false).
    pub valid: bool,
    /// Set by every key [`ZonePopulation::parse_value`] takes.
    pub has_population_keys: bool,
    /// `tradelane_attack` times 0.01.
    pub tradelane_attack: f32,
    /// `density_restriction`.
    pub density_restrictions: Vec<DensityRestriction>,
    /// `+0x1c`: 3; no reader is known.
    pub field_0x1c: f32,
    /// `encounter`.
    pub encounters: Vec<ZoneEncounter>,
    /// `+0x30`: -1; no reader is known.
    pub field_0x30: f32,
    /// `population_additive` (true).
    pub population_additive: bool,
    /// `max_battle_size` (0).
    pub max_battle_size: i32,
    /// `relief_time` (15).
    pub relief_time: f32,
}

impl Default for ZonePopulation {
    fn default() -> Self {
        Self {
            density: 0,
            repop_time: 10.0,
            valid: false,
            has_population_keys: false,
            tradelane_attack: 0.0,
            density_restrictions: Vec::new(),
            field_0x1c: 3.0,
            encounters: Vec::new(),
            field_0x30: -1.0,
            population_additive: true,
            max_battle_size: 0,
            relief_time: 15.0,
        }
    }
}

impl ZonePopulation {
    /// `FZonePopulation::ParseValue` (`Encounters.cpp:216-298`): one `[Zone]`
    /// value; false when the key is not a population key.
    ///
    /// `encounter = id, level, chance` copies the system's parameters of
    /// that id (`GetEncounterParameters`), binds them to the zone, applies
    /// the level and, as the loaded parameters have no faction weights,
    /// takes the `faction = name, weight` lines that follow; the first other
    /// line is put back. For an id the system does not have the C++ copies
    /// the map's head node; the port copies default parameters (D795).
    pub fn parse_value(
        &mut self,
        cx: &mut Cx<'_>,
        reader: &mut IniReader,
        parameters: &SystemEncounterParameters,
        zone: u32,
    ) -> bool {
        if reader.is_value(b"encounter") {
            let id = create_id(c_str(&reader.value_string_at(0)));
            let level = trunc_to_i32_via_i64(f64::from(reader.value_float(1) as f32));
            let chance = reader.value_float(2) as f32;
            let mut copy = match parameters.get(&id) {
                Some(found) => found.clone(),
                None => default_parameters(),
            };
            copy.zone = zone;
            copy.apply_difficulty(level);
            if copy.factions.is_empty() {
                let mut factions = Vec::new();
                loop {
                    let saved: IniState = reader.state();
                    if !reader.read_value() {
                        break;
                    }
                    if !reader.is_value(b"faction") {
                        reader.set_state(&saved);
                        break;
                    }
                    let faction = reputation_group(cx, &reader.value_string_at(0));
                    let weight = reader.value_float(1) as f32;
                    factions.push((faction, weight));
                }
                copy.factions = factions;
            }
            self.encounters.push(ZoneEncounter {
                parameters: copy,
                chance,
            });
        } else if reader.is_value(b"density_restriction") {
            self.density_restrictions.push(DensityRestriction {
                max_count: reader.value_int(0),
                kind: create_id(c_str(&reader.value_string_at(1))),
            });
        } else if reader.is_value(b"population_additive") {
            self.population_additive = reader.value_bool(0);
        } else if reader.is_value(b"tradelane_attack") {
            let value = reader.value_float(0) as f32;
            self.tradelane_attack = (f64::from(value) * 0.01) as f32;
        } else if reader.is_value(b"max_battle_size") {
            self.max_battle_size = reader.value_int(0);
        } else if reader.is_value(b"relief_time") {
            self.relief_time = reader.value_float(0) as f32;
            if self.repop_time < 0.0 {
                self.relief_time = 0.0;
            }
        } else {
            return false;
        }
        self.has_population_keys = true;
        true
    }
}

/// `FEncounterParameters()`: the zone key's local, every field 0.
fn default_parameters() -> EncounterParameters {
    EncounterParameters {
        nickname: 0,
        system: 0,
        zone: 0,
        reroll_interval: 0.0,
        last_reroll_time: 0.0,
        difficulty: 0,
        permutations: Vec::new(),
        current_permutation: None,
        formations: Vec::new(),
        factions: Vec::new(),
    }
}

/// `FSystemZone` (0x5c bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct SystemZone {
    /// The enumerated zone.
    pub zone_id: u32,
    /// `CreateID` of `nickname`.
    pub nickname: u32,
    /// The patrol path of the zone's `path_label`, an index into the
    /// system's paths ([`super::patrol_paths::build_patrol_paths`]).
    pub path: Option<usize>,
    /// True by default; on an encounter load, true only when the zone has
    /// neither population keys nor a `path_label`.
    pub unpopulated: bool,
    /// A `Tradelane` box zone ten times longer than wide.
    pub tradelane: bool,
    /// `mission_type` lawful, or a `vignette` nickname.
    pub lawful: bool,
    /// `mission_type` unlawful.
    pub unlawful: bool,
    /// `vignette_type`: 1 by default, 2 `field`, 4 `exclusion`.
    pub vignette_type: u32,
    /// The population block.
    pub population: ZonePopulation,
}

impl SystemZone {
    /// The enumerator's local: the zone id, the rest the defaults.
    #[must_use]
    pub fn new(zone_id: u32) -> Self {
        Self {
            zone_id,
            nickname: 0,
            path: None,
            unpopulated: true,
            tradelane: false,
            lawful: false,
            unlawful: false,
            vignette_type: 1,
            population: ZonePopulation::default(),
        }
    }

    /// `FSystemZone::GetTradelaneAttack`: 0 for an unpopulated zone.
    #[must_use]
    pub fn tradelane_attack(&self) -> f32 {
        if self.unpopulated {
            0.0
        } else {
            self.population.tradelane_attack
        }
    }
}

/// The `path_label` record of a zone (`FZonePathLabel`).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ZonePathLabel {
    /// 0 when the zone had no `path_label`.
    pub zone_id: u32,
    /// `CreateID` of value 0.
    pub label: u32,
    /// Values 1 on, each once, in first-seen order.
    pub indices: Vec<i32>,
    /// The zone's first path end, written by the patrol path.
    pub start: neuron_core::math::Vector,
    /// The zone's second path end.
    pub end: neuron_core::math::Vector,
    /// `usage`: trade `0x10`, patrol `0x01`.
    pub usage: u32,
    /// `mission_eligible`.
    pub mission_eligible: bool,
}

/// `ParseSystemZone` (`Zones.cpp:139-266`): one `[Zone]` section into
/// `zone` and `label`; false when the zone is dropped (unpopulated, without
/// a `path_label` and without a `mission_type`).
pub fn parse_system_zone(
    cx: &mut Cx<'_>,
    zone: &mut SystemZone,
    reader: &mut IniReader,
    label: &mut ZonePathLabel,
    load_encounters: bool,
    parameters: &SystemEncounterParameters,
) -> bool {
    while reader.read_value() {
        if reader.is_value(b"nickname") {
            let value = reader.value_string();
            let value = c_str(&value);
            zone.nickname = create_id(value);
            if contains(value, b"Tradelane") {
                if api::zone::shape(cx, zone.zone_id) == Ok(ZoneShape::Box)
                    && let Ok(size) = api::zone::size(cx, zone.zone_id)
                {
                    let z = f64::from(size.z);
                    if f64::from(size.x) * 10.0 < z && f64::from(size.y) * 10.0 < z {
                        zone.tradelane = true;
                    }
                }
            } else if contains(value, b"vignette") {
                zone.lawful = true;
            }
        } else if reader.is_value(b"path_label") {
            label.zone_id = zone.zone_id;
            label.label = create_id(c_str(&reader.value_string_at(0)));
            add_unique_index(&mut label.indices, reader.value_int(1));
            let mut index = 2;
            while !reader.is_value_empty(index) {
                add_unique_index(&mut label.indices, reader.value_int(index));
                index += 1;
            }
        } else if reader.is_value(b"usage") {
            let mut index = 0;
            while !reader.is_value_empty(index) {
                let usage = reader.value_string_at(index);
                if stricmp(&usage, b"trade").is_eq() {
                    label.usage |= 0x10;
                } else if stricmp(&usage, b"patrol").is_eq() {
                    label.usage |= 0x1;
                }
                index += 1;
            }
        } else if reader.is_value(b"mission_eligible") {
            label.mission_eligible = reader.value_bool(0);
        } else if reader.is_value(b"faction") {
            // Recognised and ignored.
        } else if reader.is_value(b"density") {
            zone.population.density = reader.value_int(0);
        } else if reader.is_value(b"repop_time") {
            zone.population.repop_time = reader.value_float(0) as f32;
            if zone.population.repop_time <= 0.0 {
                zone.population.repop_time = 1.0;
            }
        } else if reader.is_value(b"mission_type") {
            // The unlawful flag is not reset first.
            zone.lawful = false;
            let mut index = 0;
            while !reader.is_value_empty(index) {
                let kind = reader.value_string_at(index);
                if stricmp(&kind, b"unlawful").is_eq() {
                    zone.unlawful = true;
                } else if stricmp(&kind, b"lawful").is_eq() {
                    zone.lawful = true;
                }
                index += 1;
            }
            if !zone.unlawful && !zone.lawful {
                zone.lawful = true;
            }
        } else if reader.is_value(b"vignette_type") {
            let kind = reader.value_string_at(0);
            zone.vignette_type = if stricmp(&kind, b"field").is_eq() {
                2
            } else if stricmp(&kind, b"exclusion").is_eq() {
                4
            } else {
                1
            };
        } else if load_encounters {
            let id = zone.zone_id;
            zone.population.parse_value(cx, reader, parameters, id);
        }
    }
    if load_encounters {
        zone.unpopulated = !zone.population.has_population_keys && label.zone_id == 0;
    }
    !(zone.unpopulated && label.zone_id == 0 && !zone.lawful && !zone.unlawful)
}

/// `strstr` (case-sensitive).
fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    haystack.windows(needle.len()).any(|w| w == needle)
}

/// `AddUniqueIndex`.
fn add_unique_index(indices: &mut Vec<i32>, value: i32) {
    if !indices.contains(&value) {
        indices.push(value);
    }
}

/// The zones of one system and their path labels.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SystemZones {
    /// The kept zones, in file order.
    pub zones: Vec<SystemZone>,
    /// The `path_label` records of the kept zones that have one.
    pub path_labels: Vec<ZonePathLabel>,
}

/// The zone enumeration of `FSystemInfo::LoadFromReader`
/// (`FSystemZoneEnumerator::Slot00`, `Zones.cpp:116-131`): every zone of
/// `system`'s file under `source` that `pub::System::EnumerateZones` hands
/// out, parsed with [`parse_system_zone`] and kept when it says so, with
/// its path label when it has one. An unknown system or a file that does
/// not open gives no zones.
pub fn enumerate_system_zones(
    cx: &mut Cx<'_>,
    source: &dyn DataSource,
    system: u32,
    load_encounters: bool,
    parameters: &SystemEncounterParameters,
) -> SystemZones {
    let mut found: Vec<(u32, IniState)> = Vec::new();
    if api::system::enumerate_zones(cx, source, system, |id, reader| {
        found.push((id, reader.state()));
    })
    .is_err()
    {
        return SystemZones::default();
    }
    let mut out = SystemZones::default();
    let Ok(mut reader) = api::misc::build_system_reader(cx, source, system) else {
        return out;
    };
    for (id, state) in found {
        reader.set_state(&state);
        let mut zone = SystemZone::new(id);
        let mut label = ZonePathLabel::default();
        if parse_system_zone(
            cx,
            &mut zone,
            &mut reader,
            &mut label,
            load_encounters,
            parameters,
        ) {
            out.zones.push(zone);
            if label.zone_id != 0 {
                out.path_labels.push(label);
            }
        }
    }
    out
}
