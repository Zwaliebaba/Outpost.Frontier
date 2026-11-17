//! Equipment archetypes: `Equipment` and its 25 section kinds
//! (`common_archetype_equipment.cpp`, `common_archetype_weapons.cpp`).
//!
//! The C++ class tree (`Equipment` → `AttachedEquipment` → `Launcher` →
//! `Gun`, and so on) is a struct with the shared fields and an enum of
//! kinds whose data embeds the intermediate levels. `read` tests the keys
//! level by level from the root down, as the virtual `read` chain does.

use std::collections::BTreeMap;

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

use super::SOURCE;
use super::eq_obj::degrees;
use super::root::{Root, lookup_hp_type};
use crate::data::string_cache::{CacheString, StringCache};
use crate::data::{Reports, not_above, not_below, value_f32};

/// `AttachedEquipment`'s fields.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Attached {
    /// `separation_explosion`.
    pub separation_explosion: u32,
    /// `debris_type`.
    pub debris_type: u32,
    /// `child_impulse`.
    pub child_impulse: f32,
    /// `parent_impulse`.
    pub parent_impulse: f32,
    /// `HP_child`.
    pub hp_child: CacheString,
    /// `toughness`.
    pub toughness: i32,
}

impl Attached {
    fn read(&mut self, reader: &mut IniReader, strings: &mut StringCache) -> bool {
        if reader.is_value(b"separation_explosion") {
            self.separation_explosion = create_id(&reader.value_string_at(0));
        } else if reader.is_value(b"debris_type") {
            self.debris_type = create_id(&reader.value_string_at(0));
        } else if reader.is_value(b"child_impulse") {
            self.child_impulse = value_f32(reader, 0);
        } else if reader.is_value(b"parent_impulse") {
            self.parent_impulse = value_f32(reader, 0);
        } else if reader.is_value(b"HP_child") {
            self.hp_child = strings.alloc(&reader.value_string_at(0), false);
        } else if reader.is_value(b"toughness") {
            self.toughness = reader.value_int(0);
        } else {
            return false;
        }
        true
    }
}

/// `Launcher`'s fields (with `AttachedEquipment`'s).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Launcher {
    /// The attached-equipment fields.
    pub attached: Attached,
    /// `damage_per_fire`.
    pub damage_per_fire: f32,
    /// `power_usage`.
    pub power_usage: f32,
    /// `refire_delay`.
    pub refire_delay: f32,
    /// `muzzle_velocity`.
    pub muzzle_velocity: f32,
    /// `projectile_archetype`.
    pub projectile_archetype: u32,
    /// `use_animation`.
    pub use_animation: CacheString,
}

impl Launcher {
    fn read(&mut self, reader: &mut IniReader, strings: &mut StringCache) -> bool {
        if self.attached.read(reader, strings) {
            return true;
        }
        if reader.is_value(b"damage_per_fire") {
            self.damage_per_fire = value_f32(reader, 0);
        } else if reader.is_value(b"power_usage") {
            self.power_usage = value_f32(reader, 0);
        } else if reader.is_value(b"refire_delay") {
            self.refire_delay = value_f32(reader, 0);
        } else if reader.is_value(b"muzzle_velocity") {
            self.muzzle_velocity = value_f32(reader, 0);
        } else if reader.is_value(b"projectile_archetype") {
            self.projectile_archetype = create_id(&reader.value_string());
        } else if reader.is_value(b"use_animation") {
            self.use_animation = strings.alloc(&reader.value_string(), false);
        } else {
            return false;
        }
        true
    }
}

/// `Projectile`'s fields.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Projectile {
    /// `lifetime`.
    pub lifetime: f32,
    /// `owner_safe_time` (-1).
    pub owner_safe_time: f32,
    /// `requires_ammo`.
    pub requires_ammo: bool,
    /// `force_gun_ori`.
    pub force_gun_ori: bool,
    /// `loot_appearance`: the equipment it looks like when dropped.
    pub loot_appearance: Option<u32>,
}

impl Projectile {
    fn new(requires_ammo: bool) -> Self {
        Self {
            lifetime: 0.0,
            owner_safe_time: -1.0,
            requires_ammo,
            force_gun_ori: false,
            loot_appearance: None,
        }
    }

    fn read(&mut self, reader: &mut IniReader, cx: &mut ReadCx<'_>) -> bool {
        if reader.is_value(b"lifetime") {
            self.lifetime = value_f32(reader, 0);
        } else if reader.is_value(b"owner_safe_time") {
            self.owner_safe_time = value_f32(reader, 0);
        } else if reader.is_value(b"requires_ammo") {
            self.requires_ammo = reader.value_bool(0);
        } else if reader.is_value(b"force_gun_ori") {
            self.force_gun_ori = reader.value_bool(0);
        } else {
            return read_loot_appearance(reader, cx, &mut self.loot_appearance);
        }
        true
    }
}

/// `ShieldGenerator`.
#[derive(Clone, Debug, PartialEq)]
pub struct ShieldGenerator {
    /// The attached-equipment fields.
    pub attached: Attached,
    /// `hp_type` (5; an unknown name gives 7).
    pub hp_type: u32,
    /// `shield_type`.
    pub shield_type: u32,
    /// `regeneration_rate`.
    pub regeneration_rate: f32,
    /// `max_capacity`.
    pub max_capacity: f32,
    /// `constant_power_draw`.
    pub constant_power_draw: f32,
    /// `rebuild_power_draw`.
    pub rebuild_power_draw: f32,
    /// `offline_threshold`.
    pub offline_threshold: f32,
    /// `offline_rebuild_time`.
    pub offline_rebuild_time: f32,
    /// `hp_shield_type`: the known names, in order.
    pub hp_shield_types: Vec<u32>,
}

/// `Gun`.
#[derive(Clone, Debug, PartialEq)]
pub struct Gun {
    /// The launcher fields.
    pub launcher: Launcher,
    /// `dispersion_angle`, in radians.
    pub dispersion_angle: f32,
    /// `turn_rate`, in radians (15 degrees).
    pub turn_rate: f32,
    /// `gun_azimuth`: minimum.
    pub azimuth_min: f32,
    /// `gun_azimuth`: maximum.
    pub azimuth_max: f32,
    /// `gun_elevation`: minimum.
    pub elevation_min: f32,
    /// `gun_elevation`: maximum.
    pub elevation_max: f32,
    /// The azimuth range is 1 to 360 degrees wide.
    pub azimuth_valid: bool,
    /// The elevation range is 1 to 360 degrees wide.
    pub elevation_valid: bool,
    /// `auto_turret`.
    pub auto_turret: bool,
    /// `hp_gun_type`: the known names, in order.
    pub hp_gun_types: Vec<u32>,
}

/// `Munition`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Munition {
    /// The projectile fields.
    pub projectile: Projectile,
    /// `damage` / `hull_damage`.
    pub hull_damage: f32,
    /// `energy_damage`.
    pub energy_damage: f32,
    /// `weapon_type`.
    pub weapon_type: u32,
    /// `motor`.
    pub motor: u32,
    /// `seeker`: 0 none, 1 dumb, 2 lock.
    pub seeker: u32,
    /// `time_to_lock`.
    pub time_to_lock: f32,
    /// `seeker_range`.
    pub seeker_range: f32,
    /// `seeker_fov_deg` as a cosine (1). Through `FCOS` in the reference, so
    /// Tier B.
    pub seeker_fov_cos: f32,
    /// `max_angular_velocity` (100).
    pub max_angular_velocity: f32,
    /// `detonation_dist` squared.
    pub detonation_dist_sq: f32,
    /// `hp_type` (4; an unknown name gives 4).
    pub hp_type: u32,
    /// `cruise_disruptor`.
    pub cruise_disruptor: bool,
}

/// The data of each equipment section kind.
#[derive(Clone, Debug, PartialEq)]
pub enum EquipmentKind {
    /// `[LootCrate]`.
    LootCrate,
    /// `[Commodity]`.
    Commodity {
        /// `decay_per_second`.
        decay_per_second: f32,
        /// `loot_appearance`.
        loot_appearance: Option<u32>,
        /// `pod_appearance`: a cargo pod.
        pod_appearance: Option<u32>,
    },
    /// `[Power]`.
    Power {
        /// `capacity`.
        capacity: f32,
        /// `charge_rate`.
        charge_rate: f32,
        /// `thrust_capacity`.
        thrust_capacity: f32,
        /// `thrust_charge_rate`.
        thrust_charge_rate: f32,
    },
    /// `[Engine]`.
    Engine {
        /// `max_force`.
        max_force: f32,
        /// `linear_drag`.
        linear_drag: f32,
        /// `power_usage`.
        power_usage: f32,
        /// `cruise_power_usage`.
        cruise_power_usage: f32,
        /// `cruise_charge_time`.
        cruise_charge_time: f32,
        /// `indestructible`.
        indestructible: bool,
        /// `reverse_fraction`.
        reverse_fraction: f32,
    },
    /// `[Armor]`.
    Armor {
        /// `hit_pts_scale` (1).
        hit_pts_scale: f32,
    },
    /// `[Shield]`.
    Shield(Attached),
    /// `[ShieldGenerator]`.
    ShieldGenerator(ShieldGenerator),
    /// `[Thruster]`.
    Thruster {
        /// The attached-equipment fields.
        attached: Attached,
        /// `power_usage`.
        power_usage: f32,
        /// `max_force`.
        max_force: f32,
    },
    /// `[Gun]`.
    Gun(Gun),
    /// `[MineDropper]`.
    MineDropper(Launcher),
    /// `[CounterMeasureDropper]`.
    CounterMeasureDropper {
        /// The launcher fields.
        launcher: Launcher,
        /// `ai_range` squared.
        ai_range_sq: f32,
    },
    /// `[Scanner]`.
    Scanner {
        /// `power_usage`.
        power_usage: f32,
        /// `range` (1000).
        range: f32,
        /// `cargo_scan_range`.
        cargo_scan_range: f32,
    },
    /// `[Light]`.
    Light {
        /// `always_on` (true).
        always_on: bool,
        /// `docking_light`.
        docking_light: bool,
    },
    /// `[Tractor]`.
    Tractor {
        /// `max_length`.
        max_length: f32,
        /// `reach_speed` (100).
        reach_speed: f32,
    },
    /// `[CargoPod]`.
    CargoPod(Attached),
    /// `[AttachedFX]`.
    AttachedFx,
    /// `[InternalFX]`.
    InternalFx {
        /// `use_animation`.
        use_animation: CacheString,
    },
    /// `[TradeLane]`.
    TradeLane,
    /// `[RepairDroid]`.
    RepairDroid {
        /// `repair_rate`.
        repair_rate: f32,
    },
    /// `[RepairKit]`.
    RepairKit {
        /// `loot_appearance`.
        loot_appearance: Option<u32>,
    },
    /// `[ShieldBattery]`.
    ShieldBattery {
        /// `loot_appearance`.
        loot_appearance: Option<u32>,
    },
    /// `[CloakingDevice]`.
    CloakingDevice {
        /// The attached-equipment fields.
        attached: Attached,
        /// `power_usage`.
        power_usage: f32,
        /// `cloakin_time`.
        cloakin_time: f32,
        /// `cloakout_time`.
        cloakout_time: f32,
        /// `cloakin_fx`.
        cloakin_fx: u32,
        /// `cloakout_fx`.
        cloakout_fx: u32,
    },
    /// `[Munition]`.
    Munition(Munition),
    /// `[Mine]`.
    Mine {
        /// The projectile fields.
        projectile: Projectile,
        /// `linear_drag`.
        linear_drag: f32,
        /// `detonation_dist`.
        detonation_dist: f32,
        /// `seek_dist`.
        seek_dist: f32,
        /// `acceleration` (-1).
        acceleration: f32,
        /// `top_speed` (40).
        top_speed: f32,
    },
    /// `[CounterMeasure]`.
    CounterMeasure {
        /// The projectile fields.
        projectile: Projectile,
        /// `linear_drag`.
        linear_drag: f32,
        /// `range`.
        range: f32,
        /// `diversion_pctg`, truncated.
        diversion_pctg: i32,
    },
}

/// The section names of `LoadEquipment`, in its test order, with the
/// `AClassType` of each.
pub const SECTIONS: [(&[u8], u32); 25] = [
    (b"LootCrate", 8),
    (b"Commodity", 0xA),
    (b"Power", 0xB),
    (b"Engine", 0xC),
    (b"Armor", 0x22),
    (b"Shield", 0xD),
    (b"ShieldGenerator", 0xE),
    (b"Thruster", 0xF),
    (b"Gun", 0x11),
    (b"MineDropper", 0x12),
    (b"CounterMeasureDropper", 0x13),
    (b"Scanner", 0x14),
    (b"Light", 0x15),
    (b"Tractor", 0x16),
    (b"CargoPod", 9),
    (b"AttachedFX", 0x17),
    (b"InternalFX", 0x18),
    (b"TradeLane", 0x1D),
    (b"RepairDroid", 0x19),
    (b"RepairKit", 0x1A),
    (b"ShieldBattery", 0x1B),
    (b"CloakingDevice", 0x1C),
    (b"Munition", 0x1F),
    (b"Mine", 0x20),
    (b"CounterMeasure", 0x21),
];

/// What `read` needs besides the reader.
pub struct ReadCx<'a> {
    /// The string cache.
    pub strings: &'a mut StringCache,
    /// The equipment loaded so far (`inherit`, `loot_appearance`).
    pub equipment: &'a BTreeMap<u32, Equipment>,
    /// Where messages go.
    pub reports: &'a mut Reports,
}

impl ReadCx<'_> {
    /// `ArchDB::Get(id)`: the miss is reported (line 0x3c).
    fn get(&mut self, id: u32) -> Option<&Equipment> {
        let found = self.equipment.get(&id);
        if found.is_none() {
            self.reports
                .push(SOURCE, 0x3c, format!("*** ERROR: ArchDB::Get({id}) failed"));
        }
        found
    }

    /// `ArchDB::Get(ID_String)` (line 0x49).
    fn get_by_name(&mut self, id: u32) -> Option<&Equipment> {
        let found = self.equipment.get(&id);
        if found.is_none() {
            self.reports.push(
                SOURCE,
                0x49,
                format!(
                    "*** ERROR: ArchDB::Get(0x{id:x}) failed. ID={}",
                    i32::from_ne_bytes(id.to_ne_bytes())
                ),
            );
        }
        found
    }
}

/// `ReadLootAppearance`: `GetEquipmentByName(CreateID(value))`; a miss is 0.
fn read_loot_appearance(
    reader: &mut IniReader,
    cx: &mut ReadCx<'_>,
    out: &mut Option<u32>,
) -> bool {
    if !reader.is_value(b"loot_appearance") {
        return false;
    }
    let id = create_id(&reader.value_string());
    *out = cx.get_by_name(id).map(|e| e.root.nickname);
    true
}

/// An equipment archetype (`Archetype::Equipment`).
#[derive(Clone, Debug, PartialEq)]
pub struct Equipment {
    /// The common fields.
    pub root: Root,
    /// `volume`.
    pub volume: f32,
    /// `use_count` (true).
    pub use_count: bool,
    /// `units_per_container` (1).
    pub units_per_container: i32,
    /// `tractored_explosion`.
    pub tractored_explosion: u32,
    /// `lootable` (true).
    pub lootable: bool,
    /// The section kind and its data.
    pub kind: EquipmentKind,
}

fn new_kind(class: u32) -> (EquipmentKind, Option<u32>, Option<f32>) {
    // The kind, an OBJECT_TYPE the constructor sets, a hit_pts it sets.
    let attached = Attached::default();
    let launcher = Launcher::default();
    match class {
        8 => (EquipmentKind::LootCrate, Some(0x1000_0000), None),
        0xA => (
            EquipmentKind::Commodity {
                decay_per_second: 0.0,
                loot_appearance: None,
                pod_appearance: None,
            },
            Some(0x1000_0000),
            None,
        ),
        0xB => (
            EquipmentKind::Power {
                capacity: 0.0,
                charge_rate: 0.0,
                thrust_capacity: 0.0,
                thrust_charge_rate: 0.0,
            },
            None,
            None,
        ),
        0xC => (
            EquipmentKind::Engine {
                max_force: 0.0,
                linear_drag: 0.0,
                power_usage: 0.0,
                cruise_power_usage: 0.0,
                cruise_charge_time: 0.0,
                indestructible: false,
                reverse_fraction: 0.0,
            },
            None,
            None,
        ),
        0x22 => (EquipmentKind::Armor { hit_pts_scale: 1.0 }, None, None),
        0xD => (EquipmentKind::Shield(attached), None, Some(0.0)),
        0xE => (
            EquipmentKind::ShieldGenerator(ShieldGenerator {
                attached,
                hp_type: 5,
                shield_type: 0,
                regeneration_rate: 0.0,
                max_capacity: 0.0,
                constant_power_draw: 0.0,
                rebuild_power_draw: 0.0,
                offline_threshold: 0.0,
                offline_rebuild_time: 0.0,
                hp_shield_types: Vec::new(),
            }),
            None,
            None,
        ),
        0xF => (
            EquipmentKind::Thruster {
                attached,
                power_usage: 0.0,
                max_force: 0.0,
            },
            None,
            None,
        ),
        0x11 => (
            EquipmentKind::Gun(Gun {
                launcher,
                dispersion_angle: 0.0,
                turn_rate: 0.261_799_4,
                azimuth_min: 0.0,
                azimuth_max: 0.0,
                elevation_min: 0.0,
                elevation_max: 0.0,
                azimuth_valid: false,
                elevation_valid: false,
                auto_turret: false,
                hp_gun_types: Vec::new(),
            }),
            None,
            None,
        ),
        0x12 => (EquipmentKind::MineDropper(launcher), None, None),
        0x13 => (
            EquipmentKind::CounterMeasureDropper {
                launcher,
                ai_range_sq: 0.0,
            },
            None,
            None,
        ),
        0x14 => (
            EquipmentKind::Scanner {
                power_usage: 0.0,
                range: 1000.0,
                cargo_scan_range: 0.0,
            },
            None,
            None,
        ),
        0x15 => (
            EquipmentKind::Light {
                always_on: true,
                docking_light: false,
            },
            None,
            None,
        ),
        0x16 => (
            EquipmentKind::Tractor {
                max_length: 0.0,
                reach_speed: 100.0,
            },
            None,
            None,
        ),
        9 => (EquipmentKind::CargoPod(attached), None, None),
        0x17 => (EquipmentKind::AttachedFx, None, None),
        0x18 => (
            EquipmentKind::InternalFx {
                use_animation: CacheString::EMPTY,
            },
            None,
            None,
        ),
        0x1D => (EquipmentKind::TradeLane, None, None),
        0x19 => (EquipmentKind::RepairDroid { repair_rate: 0.0 }, None, None),
        0x1A => (
            EquipmentKind::RepairKit {
                loot_appearance: None,
            },
            None,
            None,
        ),
        0x1B => (
            EquipmentKind::ShieldBattery {
                loot_appearance: None,
            },
            None,
            None,
        ),
        0x1C => (
            EquipmentKind::CloakingDevice {
                attached,
                power_usage: 0.0,
                cloakin_time: 0.0,
                cloakout_time: 0.0,
                cloakin_fx: 0,
                cloakout_fx: 0,
            },
            None,
            None,
        ),
        0x1F => (
            EquipmentKind::Munition(Munition {
                projectile: Projectile::new(false),
                hull_damage: 0.0,
                energy_damage: 0.0,
                weapon_type: 0,
                motor: 0,
                seeker: 0,
                time_to_lock: 0.0,
                seeker_range: 0.0,
                seeker_fov_cos: 1.0,
                max_angular_velocity: 100.0,
                detonation_dist_sq: 0.0,
                hp_type: 4,
                cruise_disruptor: false,
            }),
            Some(0x200_0000),
            None,
        ),
        0x20 => (
            EquipmentKind::Mine {
                projectile: Projectile::new(true),
                linear_drag: 0.0,
                detonation_dist: 0.0,
                seek_dist: 0.0,
                acceleration: -1.0,
                top_speed: 40.0,
            },
            Some(0x400_0000),
            None,
        ),
        _ => (
            EquipmentKind::CounterMeasure {
                projectile: Projectile::new(true),
                linear_drag: 0.0,
                range: 0.0,
                diversion_pctg: 0,
            },
            None,
            None,
        ),
    }
}

/// `(azimuth|elevation)`: min, the unrounded max, and whether the width is
/// 1 to 360.
fn arc(reader: &mut IniReader) -> (f32, f32, bool) {
    let min = value_f32(reader, 0);
    let max_raw = reader.value_float(1);
    let delta = max_raw - f64::from(min);
    let valid = not_below(delta, 0.0) && not_above(delta, 360.0) && not_below(delta, 1.0);
    (min, max_raw as f32, valid)
}

fn push_hp_types(reader: &IniReader, out: &mut Vec<u32>, from: u32) {
    let mut index = from;
    while !reader.is_value_empty(index) {
        if let Some(value) = lookup_hp_type(&reader.value_string_at(index)) {
            out.push(value);
        }
        index += 1;
    }
}

impl Equipment {
    /// The record a `[<section>]` of class `class` (see [`SECTIONS`]) starts
    /// with.
    #[must_use]
    pub fn new(class: u32) -> Self {
        let (kind, object_type, hit_pts) = new_kind(class);
        let mut root = Root {
            hit_pts: 1.0,
            ..Root::default()
        };
        if let Some(object_type) = object_type {
            root.object_type = object_type;
        }
        if let Some(hit_pts) = hit_pts {
            root.hit_pts = hit_pts;
        }
        Self {
            root,
            volume: 0.0,
            use_count: true,
            units_per_container: 1,
            tractored_explosion: 0,
            lootable: true,
            kind,
        }
    }

    /// `get_class_type`.
    #[must_use]
    pub fn class(&self) -> u32 {
        match &self.kind {
            EquipmentKind::LootCrate => 8,
            EquipmentKind::CargoPod(_) => 9,
            EquipmentKind::Commodity { .. } => 0xA,
            EquipmentKind::Power { .. } => 0xB,
            EquipmentKind::Engine { .. } => 0xC,
            EquipmentKind::Shield(_) => 0xD,
            EquipmentKind::ShieldGenerator(_) => 0xE,
            EquipmentKind::Thruster { .. } => 0xF,
            EquipmentKind::Gun(_) => 0x11,
            EquipmentKind::MineDropper(_) => 0x12,
            EquipmentKind::CounterMeasureDropper { .. } => 0x13,
            EquipmentKind::Scanner { .. } => 0x14,
            EquipmentKind::Light { .. } => 0x15,
            EquipmentKind::Tractor { .. } => 0x16,
            EquipmentKind::AttachedFx => 0x17,
            EquipmentKind::InternalFx { .. } => 0x18,
            EquipmentKind::RepairDroid { .. } => 0x19,
            EquipmentKind::RepairKit { .. } => 0x1A,
            EquipmentKind::ShieldBattery { .. } => 0x1B,
            EquipmentKind::CloakingDevice { .. } => 0x1C,
            EquipmentKind::TradeLane => 0x1D,
            EquipmentKind::Munition(_) => 0x1F,
            EquipmentKind::Mine { .. } => 0x20,
            EquipmentKind::CounterMeasure { .. } => 0x21,
            EquipmentKind::Armor { .. } => 0x22,
        }
    }

    /// The attached-equipment fields, for the kinds that have them
    /// (`is_attached`).
    #[must_use]
    pub fn attached(&self) -> Option<&Attached> {
        match &self.kind {
            EquipmentKind::Shield(a)
            | EquipmentKind::CargoPod(a)
            | EquipmentKind::Thruster { attached: a, .. }
            | EquipmentKind::CloakingDevice { attached: a, .. } => Some(a),
            EquipmentKind::ShieldGenerator(g) => Some(&g.attached),
            EquipmentKind::Gun(g) => Some(&g.launcher.attached),
            EquipmentKind::MineDropper(l)
            | EquipmentKind::CounterMeasureDropper { launcher: l, .. } => Some(&l.attached),
            _ => None,
        }
    }

    fn attached_mut(&mut self) -> Option<&mut Attached> {
        match &mut self.kind {
            EquipmentKind::Shield(a)
            | EquipmentKind::CargoPod(a)
            | EquipmentKind::Thruster { attached: a, .. }
            | EquipmentKind::CloakingDevice { attached: a, .. } => Some(a),
            EquipmentKind::ShieldGenerator(g) => Some(&mut g.attached),
            EquipmentKind::Gun(g) => Some(&mut g.launcher.attached),
            EquipmentKind::MineDropper(l)
            | EquipmentKind::CounterMeasureDropper { launcher: l, .. } => Some(&mut l.attached),
            _ => None,
        }
    }

    /// The projectile fields of munitions, mines and countermeasures.
    #[must_use]
    pub fn projectile(&self) -> Option<&Projectile> {
        match &self.kind {
            EquipmentKind::Munition(m) => Some(&m.projectile),
            EquipmentKind::Mine { projectile, .. }
            | EquipmentKind::CounterMeasure { projectile, .. } => Some(projectile),
            _ => None,
        }
    }

    /// `read`: every level's keys from Root down. `inherit` copies a loaded
    /// record of the same class (all of it but the nickname; nothing for
    /// `Armor`, whose class the C++ copy does not list).
    pub fn read(&mut self, reader: &mut IniReader, cx: &mut ReadCx<'_>) -> bool {
        if self.root.read(reader, cx.strings, cx.reports) {
            return true;
        }
        if self.read_base(reader, cx) {
            return true;
        }
        let launcher = matches!(
            self.kind,
            EquipmentKind::Gun(_)
                | EquipmentKind::MineDropper(_)
                | EquipmentKind::CounterMeasureDropper { .. }
        );
        if !launcher
            && let Some(attached) = self.attached_mut()
            && attached.read(reader, cx.strings)
        {
            return true;
        }
        self.read_kind(reader, cx)
    }

    fn read_base(&mut self, reader: &mut IniReader, cx: &mut ReadCx<'_>) -> bool {
        if reader.is_value(b"inherit") {
            let id = create_id(&reader.value_string());
            match cx.get(id) {
                Some(donor) => {
                    if donor.class() == self.class() && self.class() != 0x22 {
                        let nickname = self.root.nickname;
                        *self = donor.clone();
                        self.root.nickname = nickname;
                    }
                }
                None => cx.reports.push(
                    SOURCE,
                    0x828,
                    format!(
                        "*** WARNING: 'inherit' failed. couldn't find {}",
                        crate::data::lossy(&reader.value_string())
                    ),
                ),
            }
        } else if reader.is_value(b"use_count") {
            self.use_count = reader.value_bool(0);
        } else if reader.is_value(b"units_per_container") {
            self.units_per_container = reader.value_int(0);
        } else if reader.is_value(b"volume") {
            self.volume = value_f32(reader, 0);
        } else if reader.is_value(b"tractored_explosion") {
            self.tractored_explosion = create_id(&reader.value_string());
        } else if reader.is_value(b"lootable") {
            self.lootable = reader.value_bool(0);
        } else {
            return false;
        }
        true
    }

    #[allow(clippy::too_many_lines)]
    fn read_kind(&mut self, reader: &mut IniReader, cx: &mut ReadCx<'_>) -> bool {
        match &mut self.kind {
            EquipmentKind::LootCrate
            | EquipmentKind::AttachedFx
            | EquipmentKind::TradeLane
            | EquipmentKind::Shield(_)
            | EquipmentKind::CargoPod(_) => return false,
            EquipmentKind::Commodity {
                decay_per_second,
                loot_appearance,
                pod_appearance,
            } => {
                if reader.is_value(b"decay_per_second") {
                    *decay_per_second = value_f32(reader, 0);
                } else if read_loot_appearance(reader, cx, loot_appearance) {
                    return true;
                } else if reader.is_value(b"pod_appearance") {
                    let id = create_id(&reader.value_string());
                    if let Some(found) = cx.get_by_name(id)
                        && found.class() == 9
                    {
                        *pod_appearance = Some(found.root.nickname);
                    }
                } else {
                    return false;
                }
            }
            EquipmentKind::Power {
                capacity,
                charge_rate,
                thrust_capacity,
                thrust_charge_rate,
            } => {
                if reader.is_value(b"capacity") {
                    *capacity = value_f32(reader, 0);
                } else if reader.is_value(b"charge_rate") {
                    *charge_rate = value_f32(reader, 0);
                } else if reader.is_value(b"thrust_capacity") {
                    *thrust_capacity = value_f32(reader, 0);
                } else if reader.is_value(b"thrust_charge_rate") {
                    *thrust_charge_rate = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Engine {
                max_force,
                linear_drag,
                power_usage,
                cruise_power_usage,
                cruise_charge_time,
                indestructible,
                reverse_fraction,
            } => {
                if reader.is_value(b"power_usage") {
                    *power_usage = value_f32(reader, 0);
                } else if reader.is_value(b"cruise_power_usage") {
                    *cruise_power_usage = value_f32(reader, 0);
                } else if reader.is_value(b"cruise_charge_time") {
                    *cruise_charge_time = value_f32(reader, 0);
                } else if reader.is_value(b"max_force") {
                    *max_force = value_f32(reader, 0);
                } else if reader.is_value(b"linear_drag") {
                    *linear_drag = value_f32(reader, 0);
                } else if reader.is_value(b"indestructible") {
                    *indestructible = reader.value_bool(0);
                } else if reader.is_value(b"reverse_fraction") {
                    *reverse_fraction = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Armor { hit_pts_scale } => {
                if !reader.is_value(b"hit_pts_scale") {
                    return false;
                }
                *hit_pts_scale = value_f32(reader, 0);
            }
            EquipmentKind::ShieldGenerator(g) => {
                if reader.is_value(b"hp_type") {
                    g.hp_type = lookup_hp_type(&reader.value_string_at(0)).unwrap_or(7);
                } else if reader.is_value(b"shield_type") {
                    g.shield_type = create_id(&reader.value_string_at(0));
                } else if reader.is_value(b"regeneration_rate") {
                    g.regeneration_rate = value_f32(reader, 0);
                } else if reader.is_value(b"max_capacity") {
                    g.max_capacity = value_f32(reader, 0);
                } else if reader.is_value(b"constant_power_draw") {
                    g.constant_power_draw = value_f32(reader, 0);
                } else if reader.is_value(b"rebuild_power_draw") {
                    g.rebuild_power_draw = value_f32(reader, 0);
                } else if reader.is_value(b"offline_rebuild_time") {
                    g.offline_rebuild_time = value_f32(reader, 0);
                } else if reader.is_value(b"offline_threshold") {
                    g.offline_threshold = value_f32(reader, 0);
                } else if reader.is_value(b"hp_shield_type") {
                    push_hp_types(reader, &mut g.hp_shield_types, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Thruster {
                power_usage,
                max_force,
                ..
            } => {
                if reader.is_value(b"power_usage") {
                    *power_usage = value_f32(reader, 0);
                } else if reader.is_value(b"max_force") {
                    *max_force = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Gun(g) => {
                if g.launcher.read(reader, cx.strings) {
                    return true;
                }
                if reader.is_value(b"dispersion_angle") {
                    g.dispersion_angle = (reader.value_float(0) * degrees()) as f32;
                } else if reader.is_value(b"auto_turret") {
                    g.auto_turret = reader.value_bool(0);
                } else if reader.is_value(b"turn_rate") {
                    g.turn_rate = (reader.value_float(0) * degrees()) as f32;
                } else if reader.is_value(b"gun_azimuth") {
                    let (min, max, valid) = arc(reader);
                    g.azimuth_min = min;
                    g.azimuth_max = max;
                    if valid {
                        g.azimuth_valid = true;
                    }
                } else if reader.is_value(b"gun_elevation") {
                    let (min, max, valid) = arc(reader);
                    g.elevation_min = min;
                    g.elevation_max = max;
                    if valid {
                        g.elevation_valid = true;
                    }
                } else if reader.is_value(b"hp_gun_type") {
                    push_hp_types(reader, &mut g.hp_gun_types, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::MineDropper(launcher) => return launcher.read(reader, cx.strings),
            EquipmentKind::CounterMeasureDropper {
                launcher,
                ai_range_sq,
            } => {
                if launcher.read(reader, cx.strings) {
                    return true;
                }
                if !reader.is_value(b"ai_range") {
                    return false;
                }
                let range = reader.value_float(0);
                *ai_range_sq = (range * range) as f32;
            }
            EquipmentKind::Scanner {
                power_usage,
                range,
                cargo_scan_range,
            } => {
                if reader.is_value(b"power_usage") {
                    *power_usage = value_f32(reader, 0);
                } else if reader.is_value(b"range") {
                    *range = value_f32(reader, 0);
                } else if reader.is_value(b"cargo_scan_range") {
                    *cargo_scan_range = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Light {
                always_on,
                docking_light,
            } => {
                if reader.is_value(b"always_on") {
                    *always_on = reader.value_bool(0);
                } else if reader.is_value(b"docking_light") {
                    *docking_light = reader.value_bool(0);
                } else {
                    return false;
                }
            }
            EquipmentKind::Tractor {
                max_length,
                reach_speed,
            } => {
                if reader.is_value(b"max_length") {
                    *max_length = value_f32(reader, 0);
                } else if reader.is_value(b"reach_speed") {
                    *reach_speed = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::InternalFx { use_animation } => {
                if !reader.is_value(b"use_animation") {
                    return false;
                }
                *use_animation = cx.strings.alloc(&reader.value_string(), false);
            }
            EquipmentKind::RepairDroid { repair_rate } => {
                if !reader.is_value(b"repair_rate") {
                    return false;
                }
                *repair_rate = value_f32(reader, 0);
            }
            EquipmentKind::RepairKit { loot_appearance }
            | EquipmentKind::ShieldBattery { loot_appearance } => {
                return read_loot_appearance(reader, cx, loot_appearance);
            }
            EquipmentKind::CloakingDevice {
                power_usage,
                cloakin_time,
                cloakout_time,
                cloakin_fx,
                cloakout_fx,
                ..
            } => {
                if reader.is_value(b"power_usage") {
                    *power_usage = value_f32(reader, 0);
                } else if reader.is_value(b"cloakin_time") {
                    *cloakin_time = value_f32(reader, 0);
                } else if reader.is_value(b"cloakout_time") {
                    *cloakout_time = value_f32(reader, 0);
                } else if reader.is_value(b"cloakin_fx") {
                    *cloakin_fx = create_id(&reader.value_string());
                } else if reader.is_value(b"cloakout_fx") {
                    *cloakout_fx = create_id(&reader.value_string());
                } else {
                    return false;
                }
            }
            EquipmentKind::Munition(m) => {
                if m.projectile.read(reader, cx) {
                    return true;
                }
                return read_munition(m, reader, &mut self.root);
            }
            EquipmentKind::Mine {
                projectile,
                linear_drag,
                detonation_dist,
                seek_dist,
                acceleration,
                top_speed,
            } => {
                if projectile.read(reader, cx) {
                    return true;
                }
                if reader.is_value(b"linear_drag") {
                    *linear_drag = value_f32(reader, 0);
                } else if reader.is_value(b"detonation_dist") {
                    *detonation_dist = value_f32(reader, 0);
                } else if reader.is_value(b"seek_dist") {
                    *seek_dist = value_f32(reader, 0);
                } else if reader.is_value(b"acceleration") {
                    *acceleration = value_f32(reader, 0);
                } else if reader.is_value(b"top_speed") {
                    *top_speed = value_f32(reader, 0);
                } else {
                    return false;
                }
            }
            EquipmentKind::CounterMeasure {
                projectile,
                linear_drag,
                range,
                diversion_pctg,
            } => {
                if projectile.read(reader, cx) {
                    return true;
                }
                if reader.is_value(b"linear_drag") {
                    *linear_drag = value_f32(reader, 0);
                } else if reader.is_value(b"range") {
                    *range = value_f32(reader, 0);
                } else if reader.is_value(b"diversion_pctg") {
                    *diversion_pctg = trunc_to_i32(reader.value_float(0));
                } else {
                    return false;
                }
            }
        }
        true
    }
}

fn read_munition(m: &mut Munition, reader: &mut IniReader, root: &mut Root) -> bool {
    if reader.is_value(b"damage") || reader.is_value(b"hull_damage") {
        m.hull_damage = value_f32(reader, 0);
    } else if reader.is_value(b"energy_damage") {
        m.energy_damage = value_f32(reader, 0);
    } else if reader.is_value(b"weapon_type") {
        m.weapon_type = create_id(&reader.value_string());
    } else if reader.is_value(b"motor") {
        m.motor = create_id(&reader.value_string());
    } else if reader.is_value(b"seeker") {
        let kind = reader.value_string();
        m.seeker = if eq_ignore_case(b"dumb", &kind) {
            1
        } else if eq_ignore_case(b"lock", &kind) {
            2
        } else {
            0
        };
        root.object_type = if m.seeker != 0 {
            0x100_0000
        } else {
            0x200_0000
        };
    } else if reader.is_value(b"time_to_lock") {
        m.time_to_lock = value_f32(reader, 0);
    } else if reader.is_value(b"seeker_range") {
        m.seeker_range = value_f32(reader, 0);
    } else if reader.is_value(b"seeker_fov_deg") {
        m.seeker_fov_cos = (reader.value_float(0) * degrees()).cos() as f32;
    } else if reader.is_value(b"max_angular_velocity") {
        m.max_angular_velocity = value_f32(reader, 0);
    } else if reader.is_value(b"detonation_dist") {
        let dist = reader.value_float(0);
        m.detonation_dist_sq = (dist * dist) as f32;
    } else if reader.is_value(b"hp_type") {
        m.hp_type = lookup_hp_type(&reader.value_string()).unwrap_or(4);
    } else if reader.is_value(b"cruise_disruptor") {
        m.cruise_disruptor = reader.value_bool(0);
    } else {
        return false;
    }
    true
}
