//! Projectiles (`CppSource/Common/common_cprojectile.cpp`,
//! `common_cmine.cpp`): `CProjectile` and the four concrete kinds,
//! `CBeam` (unguided munitions, moved as rays), `CGuided` (missiles with a
//! body and a motor), `CMine` and `CCounterMeasure`.

use neuron_core::math::{Matrix, Vector};

use crate::data::archetype::misc::Motor;

use super::ObjHandle;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `CProjectile` (+0xe4..+0xf0).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Projectile {
    /// +0xe4 the owner's id.
    pub owner_id: u32,
    /// +0xe8 the time left (negative: dead).
    pub lifetime: f32,
    /// +0xec the time the owner cannot be hit; 0 once expired.
    pub owner_safe_time: f32,
}

impl Projectile {
    /// `CProjectile::init`'s archetype part.
    #[must_use]
    pub fn new(owner_id: u32, lifetime: f32, owner_safe_time: f32) -> Self {
        Self {
            owner_id,
            lifetime,
            owner_safe_time,
        }
    }

    /// `CProjectile::update`'s own part: the lifetime runs down; true when
    /// the owner-safe time ran out in this update (`expire_safe_time`).
    pub fn update(&mut self, dt: f32) -> bool {
        self.lifetime -= dt;
        if self.owner_safe_time > 0.0 {
            self.owner_safe_time -= dt;
            if self.owner_safe_time <= 0.0 {
                self.owner_safe_time = 0.0;
                return true;
            }
        }
        false
    }

    /// `is_alive`.
    #[must_use]
    pub fn is_alive(&self) -> bool {
        self.lifetime >= 0.0
    }

    /// `set_dead`.
    pub fn set_dead(&mut self) {
        self.lifetime = -1.0;
    }

    /// `is_owner_safe`: an exact 0 (or NaN) is not.
    #[must_use]
    pub fn is_owner_safe(&self) -> bool {
        self.owner_safe_time < 0.0 || self.owner_safe_time > 0.0
    }
}

/// `_itoa(value, buf, 24)` as `CBeam` and the bodies name their collision
/// group: lower-case base-24 digits of the signed value.
#[must_use]
pub fn itoa_radix24(value: u32) -> Vec<u8> {
    // `_itoa` takes an int; radix other than 10 prints the unsigned bits.
    let mut v = value;
    if v == 0 {
        return b"0".to_vec();
    }
    let mut out = Vec::new();
    while v != 0 {
        let d = (v % 24) as u8;
        out.push(if d < 10 { b'0' + d } else { b'a' + d - 10 });
        v /= 24;
    }
    out.reverse();
    out
}

/// `CBeam` (+0xf0..+0x120).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct BeamData {
    /// The projectile part.
    pub projectile: Projectile,
    /// The munition archetype's id.
    pub munition: u32,
    /// +0xf0 the collision group (`_itoa(owner, 24)`), cleared when the
    /// owner-safe time runs out.
    pub collision_group: Vec<u8>,
    /// +0xf8 the velocity.
    pub velocity: Vector,
    /// +0x104 the first position it moved from.
    pub origin_pos: Vector,
    /// +0x110 the position before the last move.
    pub last_pos: Vector,
    /// +0x11c it has moved.
    pub moved: bool,
}

impl BeamData {
    /// `CBeam::move(dt)` on `position` (the object's world position).
    pub fn advance(&mut self, dt: f32, position: &mut Vector) {
        if !(self.projectile.lifetime >= 0.0) {
            return;
        }
        if !self.moved {
            self.origin_pos = *position;
            self.last_pos = *position;
            self.moved = true;
        }
        self.last_pos = *position;
        let step_z = r(e(dt) * e(self.velocity.z));
        position.x = r(e(dt) * e(self.velocity.x) + e(position.x));
        position.y = r(e(dt) * e(self.velocity.y) + e(position.y));
        position.z = r(e(step_z) + e(position.z));
    }

    /// `get_last_pos`.
    #[must_use]
    pub fn last_pos(&self, position: Vector) -> Vector {
        if self.moved { self.last_pos } else { position }
    }
}

/// `CGuided` (+0xf0..+0x104).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct GuidedData {
    /// The projectile part.
    pub projectile: Projectile,
    /// The munition archetype's id.
    pub munition: u32,
    /// +0xf0 the target (a watcher).
    pub target: Option<ObjHandle>,
    /// +0xf8 its sub-object.
    pub sub_target: u16,
    /// +0xfc the motor archetype.
    pub motor: Option<Motor>,
    /// +0x100 the time since launch.
    pub seeker_elapsed: f32,
    /// The munition's seeker range.
    pub seeker_range: f32,
    /// The munition's seeker field-of-view cosine.
    pub seeker_fov_cos: f32,
}

impl GuidedData {
    /// `motor_on`: between the motor's delay and its delay plus lifetime.
    #[must_use]
    pub fn motor_on(&self) -> bool {
        let Some(m) = &self.motor else {
            return false;
        };
        if !(self.seeker_elapsed >= m.delay) {
            return false;
        }
        e(m.lifetime) + e(m.delay) >= e(self.seeker_elapsed)
    }

    /// `seeker_can_see(pos)`: in range and inside the field of view of the
    /// −Z axis of `orientation` from `from`.
    #[must_use]
    pub fn seeker_can_see(&self, from: Vector, orientation: &Matrix, pos: Vector) -> bool {
        let fx = -orientation.m[0][2];
        let fy = -orientation.m[1][2];
        let fz = -orientation.m[2][2];
        let dx = pos.x - from.x;
        let dy = pos.y - from.y;
        let dz_wide = e(pos.z) - e(from.z);
        let dz = r(dz_wide);
        let dist_sq = (dz_wide * e(dz) + e(dy) * e(dy)) + e(dx) * e(dx);
        let range = e(self.seeker_range);
        if !(dist_sq < range * range) {
            return false;
        }
        let inv = 1.0 / dist_sq.sqrt();
        let dot = (e(dz) * inv * e(fz) + e(dy) * inv * e(fy)) + e(dx) * inv * e(fx);
        dot >= e(self.seeker_fov_cos)
    }
}

/// `CMine` with its archetype values.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct MineData {
    /// The projectile part.
    pub projectile: Projectile,
    /// `linear_drag`.
    pub linear_drag: f32,
    /// `detonation_dist`.
    pub detonation_dist: f32,
    /// `seek_dist`.
    pub seek_dist: f32,
    /// `acceleration`.
    pub acceleration: f32,
    /// `top_speed`.
    pub top_speed: f32,
}

/// `CCounterMeasure` with its archetype values.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct CounterMeasureData {
    /// The projectile part.
    pub projectile: Projectile,
    /// `linear_drag`.
    pub linear_drag: f32,
    /// `range`.
    pub range: f32,
    /// `diversion_pctg`.
    pub diversion_pctg: i32,
}

/// The linear damping of a mine or counter-measure body
/// (`LinearDrag / Mass`, one store).
#[must_use]
pub fn drag_damping(linear_drag: f32, mass: f32) -> f32 {
    r(e(linear_drag) / e(mass))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn safe_time_expires_once() {
        let mut p = Projectile::new(5, 1.0, 0.25);
        assert!(p.is_owner_safe());
        assert!(!p.update(0.2));
        assert!(p.update(0.2));
        assert!(!p.is_owner_safe());
        assert!(!p.update(0.2));
        assert!(p.is_alive());
        assert!(!{
            p.update(0.5);
            p.is_alive()
        });
    }

    #[test]
    fn beam_moves_along_its_velocity() {
        let mut b = BeamData {
            projectile: Projectile::new(1, 1.0, 0.0),
            velocity: Vector::new(10.0, 0.0, -750.0),
            ..BeamData::default()
        };
        let mut pos = Vector::new(1.0, 2.0, 3.0);
        b.advance(0.1, &mut pos);
        assert_eq!(pos, Vector::new(2.0, 2.0, -72.0));
        assert_eq!(b.origin_pos, Vector::new(1.0, 2.0, 3.0));
        assert_eq!(b.last_pos(pos), Vector::new(1.0, 2.0, 3.0));
    }

    #[test]
    fn radix_24_names() {
        assert_eq!(itoa_radix24(0), b"0");
        assert_eq!(itoa_radix24(23), b"n");
        assert_eq!(itoa_radix24(24), b"10");
        assert_eq!(itoa_radix24(0x8000_0001), itoa_radix24(2_147_483_649));
    }

    #[test]
    fn motor_window() {
        let g = GuidedData {
            motor: Some(Motor {
                nickname: 1,
                delay: 0.5,
                accel: 10.0,
                lifetime: 2.0,
            }),
            seeker_elapsed: 1.0,
            ..GuidedData::default()
        };
        assert!(g.motor_on());
        let late = GuidedData {
            seeker_elapsed: 3.0,
            ..g.clone()
        };
        assert!(!late.motor_on());
    }
}
