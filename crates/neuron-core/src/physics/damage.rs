//! The `Collision` namespace of Common: the impact damage table of
//! `[CollisionConsts]` and both `CalcCollisionDamage` overloads
//! (`CppSource/Common/common_physys.cpp:325-416`), exact on the 53-bit
//! model.

use std::collections::BTreeMap;

use crate::ini::IniReader;
use crate::math::Vector;

/// The table `Collision::Load` fills: damage per object type bit
/// (`DecodeType` of the name) and the impact speed the damage is scaled
/// against.
#[derive(Clone, Debug, PartialEq)]
pub struct CollisionDamage {
    damage: BTreeMap<u32, f32>,
    max_impact_speed: f32,
}

impl Default for CollisionDamage {
    fn default() -> Self {
        Self {
            damage: BTreeMap::new(),
            max_impact_speed: 250.0,
        }
    }
}

impl CollisionDamage {
    /// `Collision::Load`: false unless the reader is on a
    /// `[CollisionConsts]` header. `damage = type, amount` inserts
    /// `decode_type(type)` when it is not zero, keeping the first value of a
    /// repeated type (`std::map::insert`); `max_impact_speed` sets the
    /// speed. The amount is read before the type test, as the reference
    /// does.
    pub fn load(&mut self, reader: &mut IniReader, decode_type: impl Fn(&[u8]) -> u32) -> bool {
        if !reader.is_header(b"CollisionConsts") {
            return false;
        }
        while reader.read_value() {
            if reader.is_value(b"damage") {
                let type_id = decode_type(&reader.value_string_at(0));
                let amount = reader.value_float(1) as f32;
                if type_id != 0 {
                    self.damage.entry(type_id).or_insert(amount);
                }
            } else if reader.is_value(b"max_impact_speed") {
                self.max_impact_speed = reader.value_float(0) as f32;
            }
        }
        true
    }

    /// `Collision::Free`: the table is emptied; the speed stays.
    pub fn free(&mut self) {
        self.damage.clear();
    }

    /// The impact speed the damage scales against (250 by default).
    #[must_use]
    pub fn max_impact_speed(&self) -> f32 {
        self.max_impact_speed
    }

    /// `Collision::GetImpactDamageFromType`: 0 for a type not in the table.
    #[must_use]
    pub fn impact_damage_from_type(&self, type_id: u32) -> f32 {
        self.damage.get(&type_id).copied().unwrap_or(0.0)
    }

    /// `Collision::CalcCollisionDamage(type, relative velocity, normal)`:
    /// the table's damage times the normal's length over the impact speed
    /// times the absolute normal component of the relative velocity. A
    /// normal of zero length (or NaN) gives 0.
    #[must_use]
    pub fn calc_collision_damage(&self, type_id: u32, rel_vel: Vector, normal: Vector) -> f32 {
        let nx = f64::from(normal.x);
        let ny = f64::from(normal.y);
        let nz = f64::from(normal.z);
        let len = ((nx * nx + ny * ny) + nz * nz).sqrt();
        if !(len > 0.0) {
            return 0.0;
        }
        let inv = 1.0 / len;
        let dot = ((nz * inv) * f64::from(rel_vel.z) + (ny * inv) * f64::from(rel_vel.y))
            + (nx * inv) * f64::from(rel_vel.x);
        let abs_dot = dot.abs() as f32;
        let ratio = (len / f64::from(self.max_impact_speed)) as f32;
        let damage = f64::from(self.impact_damage_from_type(type_id));
        (damage * (f64::from(ratio) * f64::from(abs_dot))) as f32
    }
}

/// `Collision::CalcCollisionDamage(damage factor, max speed, actual
/// speed)`: `COLLISION_DAMAGE_FACTOR * damage_factor`, scaled by
/// `actual / max` when the actual speed is below the maximum.
/// `collision_damage_factor` is the `[Constants]` value (0.25 by default,
/// `common_misc.cpp:383`), which `game-sim::data` owns.
#[must_use]
pub fn calc_collision_damage_scaled(
    collision_damage_factor: f32,
    damage_factor: f32,
    max_speed: f32,
    actual_speed: f32,
) -> f32 {
    let scaled = f64::from(collision_damage_factor) * f64::from(damage_factor);
    if max_speed > actual_speed {
        return (scaled * (f64::from(actual_speed) / f64::from(max_speed))) as f32;
    }
    (scaled * 1.0) as f32
}

/// `COLLISION_DAMAGE_FACTOR`'s compiled-in default.
pub const DEFAULT_COLLISION_DAMAGE_FACTOR: f32 = 0.25;

#[cfg(test)]
mod tests {
    use super::*;

    fn table() -> CollisionDamage {
        let text = b"[CollisionConsts]\ndamage = ship, 100\ndamage = asteroid, 50\n\
damage = ship, 999\ndamage = nothing, 7\nmax_impact_speed = 200\n";
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text));
        assert!(reader.read_header());
        let mut t = CollisionDamage::default();
        let decode = |name: &[u8]| match name {
            b"ship" => 0x400,
            b"asteroid" => 0x2,
            _ => 0,
        };
        assert!(t.load(&mut reader, decode));
        t
    }

    #[test]
    fn load_keeps_first_value_and_skips_unknown_types() {
        let t = table();
        assert_eq!(t.impact_damage_from_type(0x400), 100.0);
        assert_eq!(t.impact_damage_from_type(0x2), 50.0);
        assert_eq!(t.impact_damage_from_type(0), 0.0);
        assert_eq!(t.max_impact_speed(), 200.0);
        let mut other = IniReader::new();
        assert!(other.open_memory(b"[Other]\n"));
        assert!(other.read_header());
        let mut t2 = CollisionDamage::default();
        assert!(!t2.load(&mut other, |_| 1));
        let mut t3 = t;
        t3.free();
        assert_eq!(t3.impact_damage_from_type(0x400), 0.0);
        assert_eq!(t3.max_impact_speed(), 200.0);
    }

    #[test]
    fn damage_from_velocity_and_normal() {
        let t = table();
        // |n| = 2, n = (0, 2, 0): dot = 30, ratio = 2/200.
        let d = t.calc_collision_damage(
            0x400,
            Vector::new(5.0, -30.0, 1.0),
            Vector::new(0.0, 2.0, 0.0),
        );
        assert_eq!(d, (100.0f64 * (f64::from(0.01f32) * 30.0)) as f32);
        assert_eq!(
            t.calc_collision_damage(0x400, Vector::new(1.0, 1.0, 1.0), Vector::ZERO),
            0.0
        );
        assert_eq!(
            t.calc_collision_damage(
                0x400,
                Vector::new(1.0, 1.0, 1.0),
                Vector::new(f32::NAN, 0.0, 0.0)
            ),
            0.0
        );
        assert_eq!(
            t.calc_collision_damage(0x8, Vector::new(1.0, 1.0, 1.0), Vector::new(1.0, 0.0, 0.0)),
            0.0
        );
    }

    #[test]
    fn scaled_overload() {
        assert_eq!(calc_collision_damage_scaled(0.25, 400.0, 100.0, 50.0), 50.0);
        assert_eq!(
            calc_collision_damage_scaled(0.25, 400.0, 100.0, 150.0),
            100.0
        );
        assert_eq!(
            calc_collision_damage_scaled(0.25, 400.0, 100.0, 100.0),
            100.0
        );
        assert_eq!(
            calc_collision_damage_scaled(0.25, 400.0, f32::NAN, 1.0),
            100.0
        );
        let odd = calc_collision_damage_scaled(0.3, 0.7, 3.0, 1.0);
        assert_eq!(odd, ((0.3f32 as f64 * 0.7f32 as f64) * (1.0 / 3.0)) as f32);
    }
}
