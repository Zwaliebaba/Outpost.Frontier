//! `CSolar` (`CppSource/Common/common_csolar.cpp`): stations, planets,
//! gates, lane rings and other fixed objects of a system.

use neuron_core::math::Vector;

/// `CSolar` (+0x1a4..+0x1ec).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct SolarData {
    /// +0x1a4 the nickname id.
    pub nickname: u32,
    /// +0x1a8 destructible.
    pub destructible: bool,
    /// +0x1a9 dynamic (made at run time).
    pub dynamic: bool,
    /// +0x1ac the atmosphere range.
    pub atmosphere_range: f32,
    /// +0x1b0 the loadout id.
    pub loadout: u32,
    /// +0x1b4 the system a gate leads to.
    pub dest_system: u32,
    /// +0x1b8 the gate there.
    pub dest_gate: u32,
    /// +0x1c4 the gate animation state (4 closed).
    pub gate_anim_state: i32,
    /// +0x1c8 the time in that state.
    pub gate_anim_timer: f32,
    /// +0x1d8 the previous lane ring.
    pub prev_ring: u32,
    /// +0x1dc the next lane ring.
    pub next_ring: u32,
    /// +0x1e0 the lane's space name.
    pub tradelane_space_name: u32,
    /// +0x1e4 the visit value.
    pub visit_value: u8,
    /// +0x1e8 the parent's nickname.
    pub parent_nickname: u32,
}

impl SolarData {
    /// `CSolar::CSolar`: the gate closed.
    #[must_use]
    pub fn new(nickname: u32) -> Self {
        Self {
            nickname,
            gate_anim_state: 4,
            ..Self::default()
        }
    }
}

/// `is_planetary`: the type bits 7.
#[must_use]
pub fn is_planetary(type_bits: u32) -> bool {
    type_bits & 7 != 0
}

/// `is_system_gate`: only gate bits (0xc40).
#[must_use]
pub fn is_system_gate(type_bits: u32) -> bool {
    type_bits == type_bits & 0xc40
}

/// `is_lane_ring`.
#[must_use]
pub fn is_lane_ring(type_bits: u32) -> bool {
    (type_bits >> 7) & 1 != 0
}

/// `is_waypoint`.
#[must_use]
pub fn is_waypoint(type_bits: u32) -> bool {
    (type_bits >> 9) & 1 != 0
}

/// `get_atmosphere_immersion(pos)`: 1 inside the atmosphere range of
/// `position`, else 0.
#[must_use]
pub fn atmosphere_immersion(atmosphere_range: f32, position: Vector, pos: Vector) -> f32 {
    if !(atmosphere_range > 0.0) {
        return 0.0;
    }
    let dx = f64::from(pos.x) - f64::from(position.x);
    let dy = f64::from(pos.y) - f64::from(position.y);
    let dz = f64::from(pos.z) - f64::from(position.z);
    let dist_sq = (dx * dx + dz * dz) + dy * dy;
    let range_sq = f64::from(atmosphere_range) * f64::from(atmosphere_range);
    if range_sq > dist_sq { 1.0 } else { 0.0 }
}

/// The body parameters `CSolar::init_physics` picks: a still solar is
/// fixed (flag +0x1c) with 1e6 damping; a spinning one free with 1e12
/// linear and no angular damping. Returns (fixed, linear, angular).
#[must_use]
pub fn body_damping(angular_velocity: Vector) -> (bool, f32, f32) {
    let a = angular_velocity;
    let spin_sq = (f64::from(a.x) * f64::from(a.x) + f64::from(a.y) * f64::from(a.y))
        + f64::from(a.z) * f64::from(a.z);
    if !(spin_sq > 0.0) {
        (true, 1.0e6, 1.0e6)
    } else {
        (false, 1.0e12, 0.0)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn type_tests() {
        assert!(is_planetary(2));
        assert!(is_system_gate(0x40));
        assert!(!is_system_gate(0x100));
        assert!(is_lane_ring(0x80));
        assert!(is_waypoint(0x200));
    }

    #[test]
    fn immersion_is_a_sphere() {
        let p = Vector::ZERO;
        assert_eq!(
            atmosphere_immersion(100.0, p, Vector::new(50.0, 0.0, 0.0)),
            1.0
        );
        assert_eq!(
            atmosphere_immersion(100.0, p, Vector::new(100.0, 0.0, 0.0)),
            0.0
        );
        assert_eq!(atmosphere_immersion(0.0, p, p), 0.0);
        assert_eq!(body_damping(Vector::ZERO), (true, 1.0e6, 1.0e6));
        assert_eq!(
            body_damping(Vector::new(0.0, 0.1, 0.0)),
            (false, 1.0e12, 0.0)
        );
    }
}
