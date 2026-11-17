//! Game constants (`ReadConstants`, `Collision::Load`), ported from
//! `CppSource/Common/common_constants.cpp` and the collision table of
//! `common_physys.cpp`. The defaults are the C++ globals' initial values
//! (`common_misc.cpp`, `common_physys.cpp`, `common_constants.cpp`).
//!
//! Every block of `constants.ini` overrides the keys it names; a key the
//! block does not know is reported and skipped. A float key is stored as
//! `float` from the parser's double; an integer key is the truncation of
//! that unrounded double (`ftol`); a double key keeps it.

use std::collections::BTreeMap;

use neuron_core::id::{IdString, create_id};
use neuron_core::ini::IniReader;
use neuron_core::math::Vector;
use neuron_core::num::trunc_to_i32;

use super::archetype::object_type::decode_type;
use super::{DataSource, Reports, lossy, open_ini, value_f32};

const SOURCE: &str = "Common\\Constants.cpp";

/// `[Constants]`, the globals of `common_misc.cpp`, plus the `TestValue`
/// and `TestString` keys nothing reads.
#[derive(Clone, Debug, PartialEq)]
pub struct GeneralConsts {
    /// `TestValue` (3.1415).
    pub test_value: f32,
    /// `TestString`: at most 0x7F bytes ("sample").
    pub test_string: Vec<u8>,
    /// `COLLISION_DAMAGE_FACTOR`.
    pub collision_damage_factor: f32,
    /// `LOOT_OWNER_SAFE_TIME`.
    pub loot_owner_safe_time: f32,
    /// `LOOT_UNSEEN_LIFE_TIME`.
    pub loot_unseen_life_time: f32,
    /// `LOOT_UNSEEN_RADIUS`.
    pub loot_unseen_radius: f32,
    /// `MUSIC_CROSS_FADE_DELAY`.
    pub music_cross_fade_delay: f32,
    /// `JETTISONED_CARGO_VELOCITY`.
    pub jettisoned_cargo_velocity: f32,
    /// `SND_CARGO_JETTISONED`.
    pub snd_cargo_jettisoned: IdString,
    /// `MUZZLE_CONE_ANGLE`.
    pub muzzle_cone_angle: f32,
    /// `FIRE_FAILED_SOUND`.
    pub fire_failed_sound: IdString,
    /// `FIRE_FAILED_DELAY`.
    pub fire_failed_delay: f32,
    /// `PLAYER_COLLISION_GROUP_HIT_PTS_SCALE`.
    pub player_collision_group_hit_pts_scale: f32,
    /// `PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE`.
    pub player_attached_equip_hit_pts_scale: f32,
    /// `MAX_PLAYER_AMMO`.
    pub max_player_ammo: i32,
}

impl Default for GeneralConsts {
    fn default() -> Self {
        Self {
            // The compiled-in 0x40490e56, not pi.
            test_value: f32::from_bits(0x4049_0e56),
            test_string: b"sample".to_vec(),
            collision_damage_factor: 0.25,
            loot_owner_safe_time: 1.0,
            loot_unseen_life_time: 60.0,
            loot_unseen_radius: 2500.0,
            music_cross_fade_delay: 30.0,
            jettisoned_cargo_velocity: 10.0,
            snd_cargo_jettisoned: IdString(create_id(b"cargo_jettison")),
            muzzle_cone_angle: 90.0,
            fire_failed_sound: IdString(create_id(b"fire_dry")),
            fire_failed_delay: 1.0,
            player_collision_group_hit_pts_scale: 1.0,
            player_attached_equip_hit_pts_scale: 1.0,
            max_player_ammo: 50,
        }
    }
}

/// `EngineEquipConsts`.
#[derive(Clone, Debug, PartialEq)]
pub struct EngineEquipConsts {
    /// `CRUISING_SPEED`.
    pub cruising_speed: f32,
    /// `CRUISE_ACCEL_TIME`.
    pub cruise_accel_time: f32,
    /// `CRUISE_DRAG`.
    pub cruise_drag: f32,
    /// `CRUISE_DISRUPT_TIME`.
    pub cruise_disrupt_time: f32,
    /// `MAX_DELTA_FX_THROTTLE`.
    pub max_delta_fx_throttle: f32,
    /// `MAX_ENGINE_FX_THROTTLE`.
    pub max_engine_fx_throttle: f32,
    /// `THROTTLE_STEADY_TIME`.
    pub throttle_steady_time: f32,
    /// `THROTTLE_ATTEN_MOD_RANGE`.
    pub throttle_atten_mod_range: f32,
    /// `DELTA_THROTTLE_ATTEN_MOD_CHANGING`.
    pub delta_throttle_atten_mod_changing: f32,
    /// `DELTA_THROTTLE_ATTEN_MOD_STEADY`.
    pub delta_throttle_atten_mod_steady: f32,
    /// `CRUISE_STEADY_TIME`.
    pub cruise_steady_time: f32,
    /// `DELTA_CRUISE_ATTEN_MOD_STEADY`.
    pub delta_cruise_atten_mod_steady: f32,
    /// `CRUISE_ATTEN_MOD_RANGE`.
    pub cruise_atten_mod_range: f32,
}

impl Default for EngineEquipConsts {
    fn default() -> Self {
        Self {
            cruising_speed: 300.0,
            cruise_accel_time: 5.0,
            cruise_drag: 3.0,
            cruise_disrupt_time: 0.0,
            max_delta_fx_throttle: 0.1,
            max_engine_fx_throttle: 0.9,
            throttle_steady_time: 0.0,
            throttle_atten_mod_range: 0.0,
            delta_throttle_atten_mod_changing: 0.0,
            delta_throttle_atten_mod_steady: 0.0,
            cruise_steady_time: 0.0,
            delta_cruise_atten_mod_steady: 0.0,
            cruise_atten_mod_range: 0.0,
        }
    }
}

/// `ShieldEquipConsts`.
#[derive(Clone, Debug, PartialEq)]
pub struct ShieldEquipConsts {
    /// `HULL_DAMAGE_FACTOR`.
    pub hull_damage_factor: f32,
}

impl Default for ShieldEquipConsts {
    fn default() -> Self {
        Self {
            hull_damage_factor: 1.0,
        }
    }
}

/// `ThrusterEquipConsts`.
#[derive(Clone, Debug, PartialEq)]
pub struct ThrusterEquipConsts {
    /// `INTERIOR_SOUND_NAME`.
    pub interior_sound_name: IdString,
    /// `EXTERIOR_SOUND_NAME`.
    pub exterior_sound_name: IdString,
    /// `MAX_VOLUME_FORCE`.
    pub max_volume_force: f32,
    /// `MIN_VOLUME_FORCE`.
    pub min_volume_force: f32,
    /// `INSIDE_CONE_ANGLE`.
    pub inside_cone_angle: f32,
    /// `OUTSIDE_CONE_ANGLE`.
    pub outside_cone_angle: f32,
    /// `OUTSIDE_CONE_ATTENUATION`.
    pub outside_cone_attenuation: f32,
}

impl Default for ThrusterEquipConsts {
    fn default() -> Self {
        Self {
            interior_sound_name: IdString(create_id(b"interior_thruster_sound")),
            exterior_sound_name: IdString(create_id(b"exterior_thruster_sound")),
            max_volume_force: 20000.0,
            min_volume_force: 5000.0,
            inside_cone_angle: 60.0,
            outside_cone_angle: 220.0,
            outside_cone_attenuation: -10.0,
        }
    }
}

/// `[PhySysConsts]`: the `PhySys` globals.
#[derive(Clone, Debug, PartialEq)]
pub struct PhySysConsts {
    /// `PHYSICAL_SIM_RATE`.
    pub physical_sim_rate: f32,
    /// `MAX_SPAWNED_MINDIST_COUNT`.
    pub max_spawned_mindist_count: i32,
    /// `ANOM_LIMITS_MAX_VELOCITY`.
    pub anom_limits_max_velocity: f32,
    /// `ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI`.
    pub anom_limits_max_angular_velocity_per_psi: f32,
    /// `MATERIAL_FRICTION`.
    pub material_friction: f64,
    /// `MATERIAL_ELASTICITY`.
    pub material_elasticity: f64,
    /// `MIN_TIME_BETWEEN_COLLISIONS`.
    pub min_time_between_collisions: f64,
    /// `RMGR_LOOK_AHEAD_TIME_WORLD`.
    pub rmgr_look_ahead_time_world: f32,
    /// `RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD`.
    pub rmgr_look_ahead_max_radius_world: f32,
    /// `RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD`.
    pub rmgr_look_ahead_min_distance_world: f32,
    /// `RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD`.
    pub rmgr_look_ahead_max_distance_world: f32,
    /// `RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD`.
    pub rmgr_look_ahead_min_seconds_world: f32,
    /// `RMGR_LOOK_AHEAD_TIME_INTRA`.
    pub rmgr_look_ahead_time_intra: f32,
    /// `RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA`.
    pub rmgr_look_ahead_max_radius_intra: f32,
    /// `RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA`.
    pub rmgr_look_ahead_min_distance_intra: f32,
    /// `RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA`.
    pub rmgr_look_ahead_max_distance_intra: f32,
    /// `RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA`.
    pub rmgr_look_ahead_min_seconds_intra: f32,
    /// `DEFAULT_LINEAR_DAMPING`.
    pub default_linear_damping: f32,
    /// `DEFAULT_ANGULAR_DAMPING`.
    pub default_angular_damping: Vector,
    /// `GOLEM_CHILD_MASS`.
    pub golem_child_mass: f32,
    /// `GOLEM_CHILD_LINEAR_DAMP`.
    pub golem_child_linear_damp: f32,
    /// `GOLEM_CHILD_ANGULAR_DAMP`.
    pub golem_child_angular_damp: Vector,
    /// `GOLEM_MAX_DELTA_POSITION`.
    pub golem_max_delta_position: f32,
    /// `GOLEM_DELTA_ORIENTATION`.
    pub golem_delta_orientation: f32,
    /// `GOLEM_MAX_TRANSLATION_FORCE`.
    pub golem_max_translation_force: Vector,
    /// `GOLEM_MAX_TORQUE`.
    pub golem_max_torque: f32,
    /// `GOLEM_FORCE_FACTOR`.
    pub golem_force_factor: f32,
    /// `GOLEM_DAMP_FACTOR`.
    pub golem_damp_factor: f32,
    /// `GOLEM_TORQUE_FACTOR`.
    pub golem_torque_factor: f32,
    /// `GOLEM_ANGULAR_DAMP_FACTOR`.
    pub golem_angular_damp_factor: f32,
    /// `RC_MAX_DELTA_POSITION`.
    pub rc_max_delta_position: f32,
    /// `RC_MAX_DELTA_ORIENTATION`.
    pub rc_max_delta_orientation: f32,
}

fn splat(value: f32) -> Vector {
    Vector {
        x: value,
        y: value,
        z: value,
    }
}

impl Default for PhySysConsts {
    fn default() -> Self {
        // The CRT initialiser: (float)((double)2.1f / PHYSICAL_SIM_RATE).
        let min_seconds = (f64::from(2.1_f32) / 16.0) as f32;
        Self {
            physical_sim_rate: 16.0,
            max_spawned_mindist_count: 100,
            anom_limits_max_velocity: 1000.0,
            anom_limits_max_angular_velocity_per_psi: 0.942_477_9,
            material_friction: 0.1,
            material_elasticity: 0.9,
            min_time_between_collisions: 0.5,
            rmgr_look_ahead_time_world: 1.0,
            rmgr_look_ahead_max_radius_world: 5.0,
            rmgr_look_ahead_min_distance_world: 100.0,
            rmgr_look_ahead_max_distance_world: 1000.0,
            rmgr_look_ahead_min_seconds_world: min_seconds,
            rmgr_look_ahead_time_intra: 0.5,
            rmgr_look_ahead_max_radius_intra: 0.9,
            rmgr_look_ahead_min_distance_intra: 5.0,
            rmgr_look_ahead_max_distance_intra: 50.0,
            rmgr_look_ahead_min_seconds_intra: min_seconds,
            default_linear_damping: 1.0,
            default_angular_damping: splat(2.0),
            golem_child_mass: 100_000.0,
            golem_child_linear_damp: 1.0,
            golem_child_angular_damp: splat(1.0),
            golem_max_delta_position: 10.0,
            golem_delta_orientation: 1.570_796_4,
            golem_max_translation_force: splat(1.0e12),
            golem_max_torque: 1.0e12,
            golem_force_factor: 0.8,
            golem_damp_factor: 1.0,
            golem_torque_factor: 0.8,
            golem_angular_damp_factor: 1.0,
            rc_max_delta_position: 50.0,
            rc_max_delta_orientation: 5.0,
        }
    }
}

/// `[CommConsts]`.
#[derive(Clone, Debug, PartialEq)]
pub struct CommConsts {
    /// `COMM_PLAYER_FAR_DIST`.
    pub comm_player_far_dist: f32,
    /// `COMM_PLAYER_FAR_DIST_ATTEN`.
    pub comm_player_far_dist_atten: f32,
    /// `CHATTER_MAX_DIST`.
    pub chatter_max_dist: f32,
    /// `CHATTER_MAX_DIST_ATTEN`.
    pub chatter_max_dist_atten: f32,
    /// `CHATTER_START_ATTEN`.
    pub chatter_start_atten: f32,
    /// `COMM_CONFLICT_PRIORITY_CUTOFF`.
    pub comm_conflict_priority_cutoff: i32,
    /// `WALLA_MAX_DIST`.
    pub walla_max_dist: f32,
    /// `WALLA_MAX_DIST_ATTEN`.
    pub walla_max_dist_atten: f32,
    /// `WALLA_START_ATTEN`.
    pub walla_start_atten: f32,
    /// `WALLA_PRIORITY_CUTOFF`.
    pub walla_priority_cutoff: i32,
}

impl Default for CommConsts {
    fn default() -> Self {
        Self {
            comm_player_far_dist: 2000.0,
            comm_player_far_dist_atten: -15.0,
            chatter_max_dist: 2000.0,
            chatter_max_dist_atten: -20.0,
            chatter_start_atten: -5.0,
            comm_conflict_priority_cutoff: -2,
            walla_max_dist: 2500.0,
            walla_max_dist_atten: -26.0,
            walla_start_atten: -8.0,
            walla_priority_cutoff: -4,
        }
    }
}

/// `[CollisionConsts]` (`Collision::Load`): damage by object type and the
/// impact speed that deals it in full.
#[derive(Clone, Debug, PartialEq)]
pub struct CollisionConsts {
    /// `damage = type, amount`: the first entry for a type wins.
    pub damage: BTreeMap<u32, f32>,
    /// `max_impact_speed` (250).
    pub max_impact_speed: f32,
}

impl Default for CollisionConsts {
    fn default() -> Self {
        Self {
            damage: BTreeMap::new(),
            max_impact_speed: 250.0,
        }
    }
}

impl CollisionConsts {
    /// `Collision::GetImpactDamageFromType`: 0 for a type without an entry.
    #[must_use]
    pub fn impact_damage(&self, type_bits: u32) -> f32 {
        self.damage.get(&type_bits).copied().unwrap_or(0.0)
    }

    /// `Collision::CalcCollisionDamage`, in the reference's operation order.
    #[must_use]
    pub fn collision_damage(&self, type_bits: u32, rel_vel: Vector, normal: Vector) -> f32 {
        let (nx, ny, nz) = (
            f64::from(normal.x),
            f64::from(normal.y),
            f64::from(normal.z),
        );
        let len = ((nx * nx + ny * ny) + nz * nz).sqrt();
        if len.partial_cmp(&0.0) != Some(core::cmp::Ordering::Greater) {
            return 0.0;
        }
        let inv = 1.0 / len;
        let dot = ((nz * inv) * f64::from(rel_vel.z) + (ny * inv) * f64::from(rel_vel.y))
            + (nx * inv) * f64::from(rel_vel.x);
        let abs_dot = dot.abs() as f32;
        let ratio = (len / f64::from(self.max_impact_speed)) as f32;
        let damage = f64::from(self.impact_damage(type_bits));
        (damage * (f64::from(ratio) * f64::from(abs_dot))) as f32
    }

    /// `Collision::Load` after the caller found `[CollisionConsts]`.
    pub fn load(&mut self, reader: &mut IniReader, reports: &mut Reports) {
        while reader.read_value() {
            if reader.is_value(b"damage") {
                let type_bits = decode_type(&reader.value_string_at(0), reports);
                let amount = value_f32(reader, 1);
                if type_bits != 0 {
                    self.damage.entry(type_bits).or_insert(amount);
                }
            } else if reader.is_value(b"max_impact_speed") {
                self.max_impact_speed = value_f32(reader, 0);
            }
        }
    }
}

/// Every global `ReadConstants` fills.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Constants {
    /// `[Constants]`.
    pub general: GeneralConsts,
    /// `[EngineEquipConsts]`.
    pub engine: EngineEquipConsts,
    /// `[ShieldEquipConsts]`.
    pub shield: ShieldEquipConsts,
    /// `[ThrusterEquipConsts]`.
    pub thruster: ThrusterEquipConsts,
    /// `[PhySysConsts]`.
    pub phy_sys: PhySysConsts,
    /// `[CommConsts]`.
    pub comm: CommConsts,
    /// `[CollisionConsts]`.
    pub collision: CollisionConsts,
}

/// Where a key of a block goes.
enum Slot<'a> {
    /// A `float`.
    Float(&'a mut f32),
    /// `ftol` of the unrounded value.
    Int(&'a mut i32),
    /// The unrounded value.
    Double(&'a mut f64),
    /// `CreateID` of the whole value text.
    Id(&'a mut IdString),
    /// Three floats.
    Vector(&'a mut Vector),
}

/// Applies the first key of `slots` the reader's value names; `false` when
/// none does.
fn apply(reader: &mut IniReader, slots: Vec<(&[u8], Slot<'_>)>) -> bool {
    let Some((_, slot)) = slots.into_iter().find(|(name, _)| reader.is_value(name)) else {
        return false;
    };
    match slot {
        Slot::Float(dest) => *dest = value_f32(reader, 0),
        Slot::Int(dest) => *dest = trunc_to_i32(reader.value_float(0)),
        Slot::Double(dest) => *dest = reader.value_float(0),
        Slot::Id(dest) => *dest = IdString(create_id(&reader.value_string())),
        Slot::Vector(dest) => {
            dest.x = value_f32(reader, 0);
            dest.y = value_f32(reader, 1);
            dest.z = value_f32(reader, 2);
        }
    }
    true
}

fn unknown(reader: &IniReader, line: u32, reports: &mut Reports) {
    reports.push(
        SOURCE,
        line,
        format!(
            "*** WARNING: Unknown Constant definition: {}",
            lossy(&reader.line())
        ),
    );
}

impl Constants {
    /// `ReadConstants(path)`: `false` when the file cannot be opened, which
    /// leaves every value as it was.
    pub fn read(&mut self, source: &dyn DataSource, path: &[u8], reports: &mut Reports) -> bool {
        let Some(mut reader) = open_ini(source, path) else {
            return false;
        };
        while reader.read_header() {
            if reader.is_header(b"Constants") {
                while reader.read_value() {
                    if !self.general_value(&mut reader) {
                        unknown(&reader, 0x86, reports);
                    }
                }
            } else if reader.is_header(b"EngineEquipConsts") {
                while reader.read_value() {
                    if !self.engine_value(&mut reader) {
                        unknown(&reader, 0x9f, reports);
                    }
                }
            } else if reader.is_header(b"ShieldEquipConsts") {
                while reader.read_value() {
                    let s = &mut self.shield;
                    let slots = vec![(
                        &b"HULL_DAMAGE_FACTOR"[..],
                        Slot::Float(&mut s.hull_damage_factor),
                    )];
                    if !apply(&mut reader, slots) {
                        unknown(&reader, 0xa8, reports);
                    }
                }
            } else if reader.is_header(b"ThrusterEquipConsts") {
                while reader.read_value() {
                    if !self.thruster_value(&mut reader) {
                        unknown(&reader, 0xb8, reports);
                    }
                }
            } else if reader.is_header(b"PhySysConsts") {
                while reader.read_value() {
                    if !self.phy_sys_value(&mut reader) {
                        unknown(&reader, 0xe8, reports);
                    }
                }
            } else if reader.is_header(b"CommConsts") {
                while reader.read_value() {
                    if !self.comm_value(&mut reader) {
                        unknown(&reader, 0xfa, reports);
                    }
                }
            } else if reader.is_header(b"CollisionConsts") {
                self.collision.load(&mut reader, reports);
            }
        }
        true
    }

    fn general_value(&mut self, reader: &mut IniReader) -> bool {
        if reader.is_value(b"TestString") {
            let text = reader.value_string();
            let mut text = neuron_core::hash::c_str(&text).to_vec();
            text.truncate(0x7F);
            self.general.test_string = text;
            return true;
        }
        let g = &mut self.general;
        let slots = vec![
            (&b"TestValue"[..], Slot::Float(&mut g.test_value)),
            (
                b"COLLISION_DAMAGE_FACTOR",
                Slot::Float(&mut g.collision_damage_factor),
            ),
            (
                b"LOOT_OWNER_SAFE_TIME",
                Slot::Float(&mut g.loot_owner_safe_time),
            ),
            (
                b"LOOT_UNSEEN_LIFE_TIME",
                Slot::Float(&mut g.loot_unseen_life_time),
            ),
            (
                b"LOOT_UNSEEN_RADIUS",
                Slot::Float(&mut g.loot_unseen_radius),
            ),
            (
                b"MUSIC_CROSS_FADE_DELAY",
                Slot::Float(&mut g.music_cross_fade_delay),
            ),
            (
                b"JETTISONED_CARGO_VELOCITY",
                Slot::Float(&mut g.jettisoned_cargo_velocity),
            ),
            (b"MUZZLE_CONE_ANGLE", Slot::Float(&mut g.muzzle_cone_angle)),
            (b"FIRE_FAILED_DELAY", Slot::Float(&mut g.fire_failed_delay)),
            (
                b"PLAYER_COLLISION_GROUP_HIT_PTS_SCALE",
                Slot::Float(&mut g.player_collision_group_hit_pts_scale),
            ),
            (
                b"PLAYER_ATTACHED_EQUIP_HIT_PTS_SCALE",
                Slot::Float(&mut g.player_attached_equip_hit_pts_scale),
            ),
            (
                b"SND_CARGO_JETTISONED",
                Slot::Id(&mut g.snd_cargo_jettisoned),
            ),
            (b"FIRE_FAILED_SOUND", Slot::Id(&mut g.fire_failed_sound)),
            (b"MAX_PLAYER_AMMO", Slot::Int(&mut g.max_player_ammo)),
        ];
        apply(reader, slots)
    }

    fn engine_value(&mut self, reader: &mut IniReader) -> bool {
        let e = &mut self.engine;
        let slots = vec![
            (&b"CRUISING_SPEED"[..], Slot::Float(&mut e.cruising_speed)),
            (b"CRUISE_ACCEL_TIME", Slot::Float(&mut e.cruise_accel_time)),
            (b"CRUISE_DRAG", Slot::Float(&mut e.cruise_drag)),
            (
                b"CRUISE_DISRUPT_TIME",
                Slot::Float(&mut e.cruise_disrupt_time),
            ),
            (
                b"MAX_DELTA_FX_THROTTLE",
                Slot::Float(&mut e.max_delta_fx_throttle),
            ),
            (
                b"MAX_ENGINE_FX_THROTTLE",
                Slot::Float(&mut e.max_engine_fx_throttle),
            ),
            (
                b"THROTTLE_STEADY_TIME",
                Slot::Float(&mut e.throttle_steady_time),
            ),
            (
                b"THROTTLE_ATTEN_MOD_RANGE",
                Slot::Float(&mut e.throttle_atten_mod_range),
            ),
            (
                b"DELTA_THROTTLE_ATTEN_MOD_CHANGING",
                Slot::Float(&mut e.delta_throttle_atten_mod_changing),
            ),
            (
                b"DELTA_THROTTLE_ATTEN_MOD_STEADY",
                Slot::Float(&mut e.delta_throttle_atten_mod_steady),
            ),
            (
                b"CRUISE_STEADY_TIME",
                Slot::Float(&mut e.cruise_steady_time),
            ),
            (
                b"DELTA_CRUISE_ATTEN_MOD_STEADY",
                Slot::Float(&mut e.delta_cruise_atten_mod_steady),
            ),
            (
                b"CRUISE_ATTEN_MOD_RANGE",
                Slot::Float(&mut e.cruise_atten_mod_range),
            ),
        ];
        apply(reader, slots)
    }

    fn thruster_value(&mut self, reader: &mut IniReader) -> bool {
        let t = &mut self.thruster;
        let slots = vec![
            (
                &b"INTERIOR_SOUND_NAME"[..],
                Slot::Id(&mut t.interior_sound_name),
            ),
            (b"EXTERIOR_SOUND_NAME", Slot::Id(&mut t.exterior_sound_name)),
            (b"MAX_VOLUME_FORCE", Slot::Float(&mut t.max_volume_force)),
            (b"MIN_VOLUME_FORCE", Slot::Float(&mut t.min_volume_force)),
            (b"INSIDE_CONE_ANGLE", Slot::Float(&mut t.inside_cone_angle)),
            (
                b"OUTSIDE_CONE_ANGLE",
                Slot::Float(&mut t.outside_cone_angle),
            ),
            (
                b"OUTSIDE_CONE_ATTENUATION",
                Slot::Float(&mut t.outside_cone_attenuation),
            ),
        ];
        apply(reader, slots)
    }

    fn phy_sys_value(&mut self, reader: &mut IniReader) -> bool {
        let p = &mut self.phy_sys;
        let slots = vec![
            (
                &b"DEFAULT_ANGULAR_DAMPING"[..],
                Slot::Vector(&mut p.default_angular_damping),
            ),
            (
                b"GOLEM_CHILD_ANGULAR_DAMP",
                Slot::Vector(&mut p.golem_child_angular_damp),
            ),
            (
                b"GOLEM_MAX_TRANSLATION_FORCE",
                Slot::Vector(&mut p.golem_max_translation_force),
            ),
            (b"PHYSICAL_SIM_RATE", Slot::Float(&mut p.physical_sim_rate)),
            (
                b"ANOM_LIMITS_MAX_VELOCITY",
                Slot::Float(&mut p.anom_limits_max_velocity),
            ),
            (
                b"ANOM_LIMITS_MAX_ANGULAR_VELOCITY_PER_PSI",
                Slot::Float(&mut p.anom_limits_max_angular_velocity_per_psi),
            ),
            (
                b"RMGR_LOOK_AHEAD_TIME_WORLD",
                Slot::Float(&mut p.rmgr_look_ahead_time_world),
            ),
            (
                b"RMGR_LOOK_AHEAD_MAX_RADIUS_WORLD",
                Slot::Float(&mut p.rmgr_look_ahead_max_radius_world),
            ),
            (
                b"RMGR_LOOK_AHEAD_MIN_DISTANCE_WORLD",
                Slot::Float(&mut p.rmgr_look_ahead_min_distance_world),
            ),
            (
                b"RMGR_LOOK_AHEAD_MAX_DISTANCE_WORLD",
                Slot::Float(&mut p.rmgr_look_ahead_max_distance_world),
            ),
            (
                b"RMGR_LOOK_AHEAD_MIN_SECONDS_WORLD",
                Slot::Float(&mut p.rmgr_look_ahead_min_seconds_world),
            ),
            (
                b"RMGR_LOOK_AHEAD_TIME_INTRA",
                Slot::Float(&mut p.rmgr_look_ahead_time_intra),
            ),
            (
                b"RMGR_LOOK_AHEAD_MAX_RADIUS_INTRA",
                Slot::Float(&mut p.rmgr_look_ahead_max_radius_intra),
            ),
            (
                b"RMGR_LOOK_AHEAD_MIN_DISTANCE_INTRA",
                Slot::Float(&mut p.rmgr_look_ahead_min_distance_intra),
            ),
            (
                b"RMGR_LOOK_AHEAD_MAX_DISTANCE_INTRA",
                Slot::Float(&mut p.rmgr_look_ahead_max_distance_intra),
            ),
            (
                b"RMGR_LOOK_AHEAD_MIN_SECONDS_INTRA",
                Slot::Float(&mut p.rmgr_look_ahead_min_seconds_intra),
            ),
            (
                b"DEFAULT_LINEAR_DAMPING",
                Slot::Float(&mut p.default_linear_damping),
            ),
            (b"GOLEM_CHILD_MASS", Slot::Float(&mut p.golem_child_mass)),
            (
                b"GOLEM_CHILD_LINEAR_DAMP",
                Slot::Float(&mut p.golem_child_linear_damp),
            ),
            (
                b"GOLEM_MAX_DELTA_POSITION",
                Slot::Float(&mut p.golem_max_delta_position),
            ),
            (
                b"GOLEM_DELTA_ORIENTATION",
                Slot::Float(&mut p.golem_delta_orientation),
            ),
            (b"GOLEM_MAX_TORQUE", Slot::Float(&mut p.golem_max_torque)),
            (
                b"GOLEM_FORCE_FACTOR",
                Slot::Float(&mut p.golem_force_factor),
            ),
            (b"GOLEM_DAMP_FACTOR", Slot::Float(&mut p.golem_damp_factor)),
            (
                b"GOLEM_TORQUE_FACTOR",
                Slot::Float(&mut p.golem_torque_factor),
            ),
            (
                b"GOLEM_ANGULAR_DAMP_FACTOR",
                Slot::Float(&mut p.golem_angular_damp_factor),
            ),
            (
                b"RC_MAX_DELTA_POSITION",
                Slot::Float(&mut p.rc_max_delta_position),
            ),
            (
                b"RC_MAX_DELTA_ORIENTATION",
                Slot::Float(&mut p.rc_max_delta_orientation),
            ),
            (b"MATERIAL_FRICTION", Slot::Double(&mut p.material_friction)),
            (
                b"MATERIAL_ELASTICITY",
                Slot::Double(&mut p.material_elasticity),
            ),
            (
                b"MIN_TIME_BETWEEN_COLLISIONS",
                Slot::Double(&mut p.min_time_between_collisions),
            ),
            (
                b"MAX_SPAWNED_MINDIST_COUNT",
                Slot::Int(&mut p.max_spawned_mindist_count),
            ),
        ];
        apply(reader, slots)
    }

    fn comm_value(&mut self, reader: &mut IniReader) -> bool {
        let c = &mut self.comm;
        let slots = vec![
            (
                &b"COMM_PLAYER_FAR_DIST"[..],
                Slot::Float(&mut c.comm_player_far_dist),
            ),
            (
                b"COMM_PLAYER_FAR_DIST_ATTEN",
                Slot::Float(&mut c.comm_player_far_dist_atten),
            ),
            (b"CHATTER_MAX_DIST", Slot::Float(&mut c.chatter_max_dist)),
            (
                b"CHATTER_MAX_DIST_ATTEN",
                Slot::Float(&mut c.chatter_max_dist_atten),
            ),
            (
                b"CHATTER_START_ATTEN",
                Slot::Float(&mut c.chatter_start_atten),
            ),
            (b"WALLA_MAX_DIST", Slot::Float(&mut c.walla_max_dist)),
            (
                b"WALLA_MAX_DIST_ATTEN",
                Slot::Float(&mut c.walla_max_dist_atten),
            ),
            (b"WALLA_START_ATTEN", Slot::Float(&mut c.walla_start_atten)),
            (
                b"COMM_CONFLICT_PRIORITY_CUTOFF",
                Slot::Int(&mut c.comm_conflict_priority_cutoff),
            ),
            (
                b"WALLA_PRIORITY_CUTOFF",
                Slot::Int(&mut c.walla_priority_cutoff),
            ),
        ];
        apply(reader, slots)
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
    fn blocks_override_their_keys() {
        let text = b"[Constants]\nMAX_PLAYER_AMMO = 12.9\nTestString = abc\nbogus = 1\n\
[PhySysConsts]\nMATERIAL_FRICTION = 0.100000001\nDEFAULT_ANGULAR_DAMPING = 1, 2, 3\n\
[CollisionConsts]\ndamage = FIGHTER, 5\ndamage = fighter, 9\nmax_impact_speed = 100\n";
        let mut constants = Constants::default();
        let mut reports = Reports::default();
        assert!(constants.read(&One(text), b"constants.ini", &mut reports));
        assert_eq!(constants.general.max_player_ammo, 12);
        assert_eq!(constants.general.test_string, b"abc");
        assert_eq!(constants.phy_sys.material_friction, 0.100000001);
        assert_eq!(constants.phy_sys.default_angular_damping.z, 3.0);
        assert_eq!(constants.collision.impact_damage(0x10000), 5.0);
        assert_eq!(reports.0.len(), 1);
        assert_eq!(reports.0[0].line, 0x86);
        let normal = Vector {
            x: 50.0,
            y: 0.0,
            z: 0.0,
        };
        let rel = Vector {
            x: 2.0,
            y: 0.0,
            z: 0.0,
        };
        assert_eq!(
            constants.collision.collision_damage(0x10000, rel, normal),
            5.0
        );
    }

    #[test]
    fn derived_defaults() {
        let p = PhySysConsts::default();
        assert_eq!(p.rmgr_look_ahead_min_seconds_world.to_bits(), 0x3e06_6666);
        assert_eq!(
            p.anom_limits_max_angular_velocity_per_psi.to_bits(),
            0x3f71_463b
        );
    }
}
