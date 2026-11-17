//! Asteroids, motors and explosions (`common_archetype_misc.cpp`).

use neuron_core::hash::eq_ignore_case;
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::num::trunc_to_i32;

use super::root::Root;
use crate::data::string_cache::StringCache;
use crate::data::{Reports, value_f32};

/// `Archetype::Asteroid` (`[Asteroid]` and `[AsteroidMine]`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Asteroid {
    /// The common fields; `type` defaults to `ASTEROID`.
    pub root: Root,
    /// `detect_radius` (25).
    pub detect_radius: f32,
    /// `explosion_offset`.
    pub explosion_offset: f32,
    /// `recharge_time` (1000).
    pub recharge_time: f32,
}

impl Default for Asteroid {
    fn default() -> Self {
        Self {
            root: Root {
                object_type: 0x2000_0000,
                ..Root::default()
            },
            detect_radius: 25.0,
            explosion_offset: 0.0,
            recharge_time: 1000.0,
        }
    }
}

impl Asteroid {
    /// `Asteroid::read`.
    pub fn read(
        &mut self,
        reader: &mut IniReader,
        strings: &mut StringCache,
        reports: &mut Reports,
    ) -> bool {
        if self.root.read(reader, strings, reports) {
            return true;
        }
        if reader.is_value(b"detect_radius") {
            self.detect_radius = value_f32(reader, 0);
        } else if reader.is_value(b"explosion_offset") {
            self.explosion_offset = value_f32(reader, 0);
        } else if reader.is_value(b"recharge_time") {
            self.recharge_time = value_f32(reader, 0);
        } else {
            return false;
        }
        true
    }

    /// The `inherit` copy of `LoadAsteroids`.
    pub fn inherit(&mut self, donor: &Self) {
        self.root.inherit(&donor.root);
        self.detect_radius = donor.detect_radius;
        self.explosion_offset = donor.explosion_offset;
        self.recharge_time = donor.recharge_time;
    }
}

/// `Archetype::DynamicAsteroid`: Root only, `type` 0x4000_0000.
#[must_use]
pub fn dynamic_asteroid() -> Root {
    Root {
        object_type: 0x4000_0000,
        ..Root::default()
    }
}

/// `MotorData` (`[Motor]` in an equipment file).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Motor {
    /// `nickname`.
    pub nickname: u32,
    /// `delay`.
    pub delay: f32,
    /// `accel` (15).
    pub accel: f32,
    /// `lifetime` (1.5).
    pub lifetime: f32,
}

impl Default for Motor {
    fn default() -> Self {
        Self {
            nickname: 0,
            delay: 0.0,
            accel: 15.0,
            lifetime: 1.5,
        }
    }
}

impl Motor {
    /// `MotorData::read`.
    pub fn read(&mut self, reader: &mut IniReader) -> bool {
        if reader.is_value(b"nickname") {
            self.nickname = create_id(&reader.value_string());
        } else if reader.is_value(b"accel") {
            self.accel = value_f32(reader, 0);
        } else if reader.is_value(b"lifetime") {
            self.lifetime = value_f32(reader, 0);
        } else if reader.is_value(b"delay") {
            self.delay = value_f32(reader, 0);
        } else {
            return false;
        }
        true
    }

    /// `MotorData::redefine`: everything but the nickname.
    pub fn redefine(&mut self, other: &Self) {
        super::root::take_f32(&mut self.accel, other.accel);
        super::root::take_f32(&mut self.lifetime, other.lifetime);
        super::root::take_f32(&mut self.delay, other.delay);
    }
}

/// `FExplosionEffect`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ExplosionEffect {
    /// The effect.
    pub fx: u32,
    /// The strength, -1 when not given.
    pub strength: f32,
}

/// `FExplosionDebrisType`.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ExplosionDebris {
    /// The debris archetype.
    pub debris: u32,
    /// The count, as a float.
    pub count: f32,
}

/// `Archetype::Explosion`.
#[derive(Clone, Debug, PartialEq)]
pub struct Explosion {
    /// The nickname.
    pub nickname: u32,
    /// `radius`.
    pub radius: f32,
    /// `impulse`.
    pub impulse: f32,
    /// `hull_damage`.
    pub hull_damage: f32,
    /// `energy_damage`.
    pub energy_damage: f32,
    /// `process`: 0 shatter, 1 disappear (default), 2 none.
    pub process: u32,
    /// `lifetime`: minimum.
    pub lifetime: f32,
    /// `lifetime`: maximum (10000).
    pub lifetime_max: f32,
    /// `effect` lines.
    pub effects: Vec<ExplosionEffect>,
    /// `num_child_pieces`, truncated.
    pub num_child_pieces: i32,
    /// `debris_impulse` (5).
    pub debris_impulse: f32,
    /// `innards_debris_start_time` (-1).
    pub innards_debris_start_time: f32,
    /// `innards_debris_num`.
    pub innards_debris_num: f32,
    /// `innards_debris_radius`.
    pub innards_debris_radius: f32,
    /// `innards_debris_object` lines.
    pub innards_debris_objects: Vec<u32>,
    /// `debris_type` lines.
    pub debris_types: Vec<ExplosionDebris>,
}

impl Explosion {
    /// `Explosion(nickname)`.
    #[must_use]
    pub fn new(nickname: u32) -> Self {
        Self {
            nickname,
            radius: 0.0,
            impulse: 0.0,
            hull_damage: 0.0,
            energy_damage: 0.0,
            process: 1,
            lifetime: 0.0,
            lifetime_max: 10000.0,
            effects: Vec::new(),
            num_child_pieces: 0,
            debris_impulse: 5.0,
            innards_debris_start_time: -1.0,
            innards_debris_num: 0.0,
            innards_debris_radius: 0.0,
            innards_debris_objects: Vec::new(),
            debris_types: Vec::new(),
        }
    }

    /// `Explosion::read`.
    pub fn read(&mut self, reader: &mut IniReader) {
        if reader.is_value(b"radius") {
            self.radius = value_f32(reader, 0);
        } else if reader.is_value(b"impulse") {
            self.impulse = value_f32(reader, 0);
        } else if reader.is_value(b"hull_damage") {
            self.hull_damage = value_f32(reader, 0);
        } else if reader.is_value(b"energy_damage") {
            self.energy_damage = value_f32(reader, 0);
        } else if reader.is_value(b"lifetime") {
            self.lifetime = value_f32(reader, 0);
            self.lifetime_max = if reader.is_value_empty(1) {
                self.lifetime
            } else {
                value_f32(reader, 1)
            };
        } else if reader.is_value(b"effect") {
            let fx = create_id(&reader.value_string_at(0));
            let strength = if reader.is_value_empty(1) {
                -1.0
            } else {
                value_f32(reader, 1)
            };
            self.effects.push(ExplosionEffect { fx, strength });
        } else if reader.is_value(b"process") {
            let text = reader.value_string();
            if eq_ignore_case(&text, b"none") {
                self.process = 2;
            } else if eq_ignore_case(&text, b"shatter") {
                self.process = 0;
            } else if eq_ignore_case(&text, b"disappear") {
                self.process = 1;
            }
        } else if reader.is_value(b"debris_type") {
            let debris = create_id(&reader.value_string_at(0));
            let count = if reader.is_value_empty(1) {
                0.0
            } else {
                reader.value_int(1) as f32
            };
            self.debris_types.push(ExplosionDebris { debris, count });
        } else if reader.is_value(b"num_child_pieces") {
            self.num_child_pieces = trunc_to_i32(reader.value_float(0));
        } else if reader.is_value(b"debris_impulse") {
            self.debris_impulse = value_f32(reader, 0);
        } else if reader.is_value(b"innards_debris_object") {
            self.innards_debris_objects
                .push(create_id(&reader.value_string()));
        } else if reader.is_value(b"innards_debris_radius") {
            self.innards_debris_radius = value_f32(reader, 0);
        } else if reader.is_value(b"innards_debris_start_time") {
            self.innards_debris_start_time = value_f32(reader, 0);
        } else if reader.is_value(b"innards_debris_num") {
            self.innards_debris_num = value_f32(reader, 0);
        }
    }
}
