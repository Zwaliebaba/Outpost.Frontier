//! `Archetype::Root`: the fields every archetype shares
//! (`common_archetype.cpp`).

use neuron_core::id::create_id;
use neuron_core::ini::IniReader;

use super::object_type::decode_type;
use crate::data::string_cache::{CacheString, StringCache};
use crate::data::{Reports, value_f32};

/// `HpAttachmentType` names and values (`HpTypeTable`, 63 entries in the
/// C++ order; the first match wins).
pub const HP_TYPES: [(&[u8], u32); 63] = [
    (b"hp_gun", 4),
    (b"hp_shield_generator", 5),
    (b"hp_fighter_shield_generator", 6),
    (b"hp_elite_shield_generator", 7),
    (b"hp_freighter_shield_generator", 8),
    (b"hp_thruster", 9),
    (b"hp_torpedo", 0xa),
    (b"hp_mine_dropper", 0xb),
    (b"hp_turret", 0xd),
    (b"hp_countermeasure_dropper", 0xc),
    (b"hp_cargo_pod", 0xe),
    (b"hp_gun_special_1", 0xf),
    (b"hp_gun_special_2", 0x10),
    (b"hp_gun_special_3", 0x11),
    (b"hp_gun_special_4", 0x12),
    (b"hp_gun_special_5", 0x13),
    (b"hp_gun_special_6", 0x14),
    (b"hp_gun_special_7", 0x15),
    (b"hp_gun_special_8", 0x16),
    (b"hp_gun_special_9", 0x17),
    (b"hp_gun_special_10", 0x18),
    (b"hp_turret_special_1", 0x19),
    (b"hp_turret_special_2", 0x1a),
    (b"hp_turret_special_3", 0x1b),
    (b"hp_turret_special_4", 0x1c),
    (b"hp_turret_special_5", 0x1d),
    (b"hp_turret_special_6", 0x1e),
    (b"hp_turret_special_7", 0x1f),
    (b"hp_turret_special_8", 0x20),
    (b"hp_turret_special_9", 0x21),
    (b"hp_turret_special_10", 0x22),
    (b"hp_torpedo_special_1", 0x24),
    (b"hp_torpedo_special_2", 0x23),
    (b"hp_fighter_shield_special_1", 0x25),
    (b"hp_fighter_shield_special_2", 0x26),
    (b"hp_fighter_shield_special_3", 0x27),
    (b"hp_fighter_shield_special_4", 0x28),
    (b"hp_fighter_shield_special_5", 0x29),
    (b"hp_fighter_shield_special_6", 0x2a),
    (b"hp_fighter_shield_special_7", 0x2b),
    (b"hp_fighter_shield_special_8", 0x2c),
    (b"hp_fighter_shield_special_9", 0x2d),
    (b"hp_fighter_shield_special_10", 0x2e),
    (b"hp_elite_shield_special_1", 0x2f),
    (b"hp_elite_shield_special_2", 0x30),
    (b"hp_elite_shield_special_3", 0x31),
    (b"hp_elite_shield_special_4", 0x32),
    (b"hp_elite_shield_special_5", 0x33),
    (b"hp_elite_shield_special_6", 0x34),
    (b"hp_elite_shield_special_7", 0x35),
    (b"hp_elite_shield_special_8", 0x36),
    (b"hp_elite_shield_special_9", 0x37),
    (b"hp_elite_shield_special_10", 0x38),
    (b"hp_freighter_shield_special_1", 0x39),
    (b"hp_freighter_shield_special_2", 0x3a),
    (b"hp_freighter_shield_special_3", 0x3b),
    (b"hp_freighter_shield_special_4", 0x3c),
    (b"hp_freighter_shield_special_5", 0x3d),
    (b"hp_freighter_shield_special_6", 0x3e),
    (b"hp_freighter_shield_special_7", 0x3f),
    (b"hp_freighter_shield_special_8", 0x40),
    (b"hp_freighter_shield_special_9", 0x41),
    (b"hp_freighter_shield_special_10", 0x42),
];

/// `TryLookupHpType`: the value of the first name `stricmp` matches.
#[must_use]
pub fn lookup_hp_type(name: &[u8]) -> Option<u32> {
    HP_TYPES
        .iter()
        .find(|(n, _)| neuron_core::hash::eq_ignore_case(name, n))
        .map(|&(_, v)| v)
}

/// `if (Other.X != X) X = Other.X;` for a float: an equal value (0 and -0
/// included) keeps its own bits.
pub(crate) fn take_f32(dst: &mut f32, src: f32) {
    if src != *dst {
        *dst = src;
    }
}

/// What every archetype reads (`Archetype::Root`). The engine side of the
/// C++ record (mesh and script handles, load count, collision surfaces,
/// the client companion) is not data and is not kept here.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Root {
    /// `nickname`: its `CreateID`.
    pub nickname: u32,
    /// `DA_archetype` / `attachment_archetype` (a temporary copy).
    pub da_archetype: CacheString,
    /// `type`: the `OBJECT_TYPE` bits.
    pub object_type: u32,
    /// `ids_name`.
    pub ids_name: i32,
    /// `ids_info`.
    pub ids_info: i32,
    /// `hit_pts` (100).
    pub hit_pts: f32,
    /// `mass` (1).
    pub mass: f32,
    /// `explosion_arch`.
    pub explosion_arch: u32,
    /// `explosion_resistance` (1).
    pub explosion_resistance: f32,
    /// `rotation_inertia`.
    pub rotation_inertia: [f32; 3],
    /// Whether `rotation_inertia` was given.
    pub has_rotation_inertia: bool,
    /// `phantom_physics`.
    pub phantom_physics: bool,
}

impl Default for Root {
    fn default() -> Self {
        Self {
            nickname: 0,
            da_archetype: CacheString::EMPTY,
            object_type: 0,
            ids_name: 0,
            ids_info: 0,
            hit_pts: 100.0,
            mass: 1.0,
            explosion_arch: 0,
            explosion_resistance: 1.0,
            rotation_inertia: [0.0; 3],
            has_rotation_inertia: false,
            phantom_physics: false,
        }
    }
}

impl Root {
    /// `Root::read`: `true` when the key is one of Root's. A key no level
    /// takes would go to the client companion (`ReadUnknown`), which the
    /// data does not have here.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> bool {
        if reader.is_value(b"nickname") {
            self.nickname = create_id(&reader.value_string_at(0));
        } else if reader.is_value(b"DA_archetype") || reader.is_value(b"attachment_archetype") {
            self.da_archetype = strings.alloc(&reader.value_string_at(0), true);
        } else if reader.is_value(b"type") {
            self.object_type = decode_type(&reader.value_string_at(0), reports);
        } else if reader.is_value(b"ids_name") {
            self.ids_name = reader.value_int(0);
        } else if reader.is_value(b"ids_info") {
            self.ids_info = reader.value_int(0);
        } else if reader.is_value(b"explosion_arch") {
            self.explosion_arch = create_id(&reader.value_string_at(0));
        } else if reader.is_value(b"explosion_resistance") {
            self.explosion_resistance = value_f32(reader, 0);
        } else if reader.is_value(b"hit_pts") {
            self.hit_pts = value_f32(reader, 0);
        } else if reader.is_value(b"mass") {
            self.mass = value_f32(reader, 0);
        } else if reader.is_value(b"rotation_inertia") {
            self.rotation_inertia = [
                value_f32(reader, 0),
                value_f32(reader, 1),
                value_f32(reader, 2),
            ];
            self.has_rotation_inertia = true;
        } else if reader.is_value(b"phantom_physics") {
            self.phantom_physics = reader.value_bool(0);
        } else {
            return reader.is_value(b"mission_property");
        }
        true
    }

    /// `Root::redefine`: names, hit points, explosion, mass and rotation
    /// inertia from a redefinition.
    pub fn redefine(&mut self, other: &Self) {
        self.ids_name = other.ids_name;
        self.ids_info = other.ids_info;
        take_f32(&mut self.hit_pts, other.hit_pts);
        self.explosion_arch = other.explosion_arch;
        take_f32(&mut self.explosion_resistance, other.explosion_resistance);
        take_f32(&mut self.mass, other.mass);
        if other.rotation_inertia != self.rotation_inertia {
            self.rotation_inertia = other.rotation_inertia;
        }
    }

    /// `Root::operator=` without the nickname, as the `inherit` copies use
    /// it (they put the nickname back).
    pub fn inherit(&mut self, donor: &Self) {
        let nickname = self.nickname;
        *self = *donor;
        self.nickname = nickname;
    }
}
