//! Turret joints and gun pointing (`CEGun::JointChannel`, `CanPointAt`,
//! `CanSeeTargetPoint`, `ComputeTurretFrame`, `DecomposeJointMotion`,
//! `GetTurretOrigin` of `CppSource/Common/common_cegun.cpp`).
//!
//! A turret gun's model hangs on its hardpoint through a revolute joint
//! (the yaw joint), and one of the model's children through another (the
//! pitch joint). The functions here are the reference's arithmetic over
//! plain values; the world reads the instance tree into them
//! ([`crate::object::World::update_turrets`] and the pointing queries).
//!
//! `FPATAN` and `FCOS` come from the Rust library (decision T11): an angle
//! rounded to `f32` matches the x87 result except at a rounding boundary.

use neuron_core::math::{Matrix, Vector, mul_matrix, transform_matrix};
use neuron_core::scene::InstanceId;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `pi` as the reference's `float` constant.
pub const GUN_PI: f32 = core::f32::consts::PI;
/// `2 pi` as the reference's `float` constant.
pub const GUN_TWO_PI: f32 = 6.283_185_5;
/// `SEE_TARGET_ANGLE`: the cone (radians) in which a lock grows.
pub const SEE_TARGET_ANGLE: f32 = 0.052_359_88;
/// `TARGET_POINT_VALID_TIME`: how long a target point steers the turret.
pub const TARGET_POINT_VALID_TIME: f32 = 2.0;
/// The lead speed of a seeker munition (`1e9`).
pub const SEEKER_LEAD_SPEED: f32 = 1.0e9;
/// `FLT_MAX`, the target point's age before the first `SetTargetPoint`.
pub const TARGET_POINT_AGE_NONE: f32 = f32::MAX;
/// A joint whose limits span at least this much turns all the way round
/// (`JointChannel::Bind`'s double constant).
const FULL_TURN: f64 = 6.283_185_482_025_146_5;

/// `CEGun::JointChannel`: one turret joint.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct JointChannel {
    /// +0x08 the joint's engine instance; `None` is -1 (unbound).
    pub instance: Option<InstanceId>,
    /// +0x0c the joint has a limited range; cleared for a full turn.
    pub limited: bool,
    /// +0x10 the aim angle.
    pub desired: f32,
    /// +0x14 the joint angle read back from the engine.
    pub current: f32,
    /// +0x18 the last tick moved the joint less than 1e-4.
    pub settled: bool,
}

impl Default for JointChannel {
    /// `JointChannel::JointChannel`: unbound, limited, at rest at 0.
    fn default() -> Self {
        Self {
            instance: None,
            limited: true,
            desired: 0.0,
            current: 0.0,
            settled: false,
        }
    }
}

impl JointChannel {
    /// Whether the joint is bound (`EngineIndex != -1`).
    #[must_use]
    pub fn is_bound(&self) -> bool {
        self.instance.is_some()
    }

    /// `JointChannel::Bind`'s state: the instance, the range test of its
    /// limits (`|min - max| >= 2 pi` makes a full-turn joint) and the angle
    /// the engine reports (`None` when `GetJointParams` fails, which the
    /// reference reports and leaves `current` alone).
    pub fn bind(&mut self, instance: InstanceId, limits: Option<(f32, f32)>, angle: Option<f32>) {
        self.instance = Some(instance);
        if let Some((min, max)) = limits
            && (e(min) - e(max)).abs() >= FULL_TURN
        {
            self.limited = false;
        }
        if let Some(a) = angle {
            self.current = a;
        }
    }

    /// `CEGun::LoseJoints` for one joint.
    pub fn lose(&mut self) {
        *self = Self::default();
    }

    /// `|current - desired|`, folded to `pi - delta` above `pi` for a
    /// full-turn joint (`JointAbsDelta`).
    #[must_use]
    pub fn abs_delta(&self) -> f64 {
        let delta = (e(self.current) - e(self.desired)).abs();
        if !self.limited && e(GUN_PI) < delta {
            return e(GUN_PI) - delta;
        }
        delta
    }

    /// `JointWithin`: an unbound joint is always within; a bound one when
    /// its delta is below `threshold` (unordered is not).
    #[must_use]
    pub fn within(&self, threshold: f32) -> bool {
        if !self.is_bound() {
            return true;
        }
        self.abs_delta() < e(threshold)
    }

    /// `JointChannel::Tick` up to the engine call: the angle the joint is
    /// asked to take after `dt` at `turn_rate` radians a second.
    #[must_use]
    pub fn tick_target(&self, turn_rate: f32, dt: f32) -> f32 {
        let mut delta = e(self.desired) - e(self.current);
        if !self.limited {
            if delta > e(GUN_PI) {
                delta -= e(GUN_TWO_PI);
            } else if delta < -e(GUN_PI) {
                delta += e(GUN_TWO_PI);
            }
        }
        let step = e(turn_rate) * e(dt);
        let step_f = r(step);
        let mut next = if step > delta.abs() {
            r(e(self.current) + delta)
        } else if delta > 0.0 {
            r(e(step_f) + e(self.current))
        } else {
            r(e(self.current) - e(step_f))
        };
        if next > GUN_PI {
            next = r(e(next) - e(GUN_TWO_PI));
        } else if next < -GUN_PI {
            next = r(e(next) + e(GUN_TWO_PI));
        }
        next
    }

    /// `JointChannel::Tick` after the engine call: the read-back becomes
    /// the current angle; the joint settled when it moved less than 1e-4.
    pub fn settle(&mut self, read_back: f32) {
        self.settled = (e(self.current) - e(read_back)).abs() < 1.0e-4;
        self.current = read_back;
    }

    /// `CEGun::Update`'s registration test: the joint needs a tick when it
    /// is bound and its desired angle moves more than a tenth of
    /// `SEE_TARGET_ANGLE` from the current one.
    #[must_use]
    pub fn needs_tick(&self, desired: f32) -> bool {
        self.is_bound() && e(SEE_TARGET_ANGLE) * e(0.1_f32) < (e(desired) - e(self.current)).abs()
    }
}

/// `DecomposeJointMotion`: a world direction in the turret's rest frame
/// (`transpose(connector * turret_frame) * dir`) as the yaw
/// `atan2(x, -z)` and the pitch `atan(y / hypot(z, x))`, each rounded
/// once.
#[must_use]
pub fn decompose_joint_motion(
    connector: &Matrix,
    turret_frame: &Matrix,
    dir: Vector,
) -> (f32, f32) {
    let combined = mul_matrix(connector, turret_frame);
    let local = transform_matrix(&combined.transpose(), dir);
    let yaw = r(e(local.x).atan2(-e(local.z)));
    let horiz = (e(local.z) * e(local.z) + e(local.x) * e(local.x)).sqrt();
    let pitch = r((e(local.y) / horiz).atan());
    (yaw, pitch)
}

/// `ComputeAimAngles`: the angles a turret aims at a direction, the yaw
/// negated.
#[must_use]
pub fn aim_angles(connector: &Matrix, turret_frame: &Matrix, dir: Vector) -> (f32, f32) {
    let (yaw, pitch) = decompose_joint_motion(connector, turret_frame, dir);
    (-yaw, pitch)
}

/// `JointWindowContains`: the joint's window `[-angle, angle]`, widened
/// by a bound joint's limits, the low end moved down and the high end up
/// by whole turns until they bracket 0, holds `value`.
#[must_use]
pub fn joint_window_contains(limits: Option<(f32, f32)>, angle: f32, value: f32) -> bool {
    let neg = -angle;
    let (lo, mut hi) = match limits {
        Some((min, max)) => (r(e(neg) + e(min)), e(angle) + e(max)),
        None => (neg, e(angle)),
    };
    let two_pi = e(GUN_TWO_PI);
    let mut lo = e(lo);
    while lo > 0.0 {
        lo -= two_pi;
    }
    while hi < 0.0 {
        hi += two_pi;
    }
    lo <= e(value) && e(value) <= hi
}

/// `CanPointAt` past its destroyed test: the direction from the turret
/// origin to the point in the turret's frame lies in both joints' windows
/// (the yaw tested negated). `yaw_limits` and `pitch_limits` are the bound
/// joints' limits (`None` for an unbound joint).
#[must_use]
pub fn can_point_at(
    connector: &Matrix,
    turret_frame: &Matrix,
    origin: Vector,
    point: Vector,
    angle: f32,
    yaw_limits: Option<(f32, f32)>,
    pitch_limits: Option<(f32, f32)>,
) -> bool {
    let dir = Vector::new(
        r(e(point.x) - e(origin.x)),
        r(e(point.y) - e(origin.y)),
        r(e(point.z) - e(origin.z)),
    );
    let (yaw, pitch) = decompose_joint_motion(connector, turret_frame, dir);
    joint_window_contains(yaw_limits, angle, -yaw)
        && joint_window_contains(pitch_limits, angle, pitch)
}

/// `CanSeeTargetPoint`: the point lies in the cone of half-angle `angle`
/// about the first barrel's direction from its muzzle (unordered is not).
#[must_use]
pub fn can_see_target_point(muzzle: Vector, barrel_dir: Vector, point: Vector, angle: f32) -> bool {
    let dx = e(point.x) - e(muzzle.x);
    let dy = e(point.y) - e(muzzle.y);
    let dz_retained = e(point.z) - e(muzzle.z);
    let dz = r(dz_retained);
    let len = ((dz_retained * e(dz) + dx * dx) + dy * dy).sqrt();
    let inv = r(1.0 / len);
    let nx = r(dx * e(inv));
    let ny = r(dy * e(inv));
    let nz = r(e(dz) * e(inv));
    let dot = e(barrel_dir.z) * e(nz) + e(barrel_dir.y) * e(ny) + e(nx) * e(barrel_dir.x);
    e(angle).cos() < dot
}

/// `ComputeTurretFrame`: the turret's rest basis (pitch axis, yaw axis and
/// their cross product as columns) in the connector's frame,
/// `transpose(connector) * basis`. `yaw_local` is the yaw axis in the
/// connector's frame (the yaw joint's axis, or the root's rest
/// orientation times up); `pitch_world` the pitch axis in world space.
#[must_use]
pub fn compute_turret_frame(connector: &Matrix, yaw_local: Vector, pitch_world: Vector) -> Matrix {
    let yaw = transform_matrix(connector, yaw_local);
    let p = pitch_world;
    let cross = Vector::new(
        r(e(yaw.z) * e(p.y) - e(p.z) * e(yaw.y)),
        r(e(p.z) * e(yaw.x) - e(yaw.z) * e(p.x)),
        r(e(yaw.y) * e(p.x) - e(p.y) * e(yaw.x)),
    );
    let basis = Matrix::from_rows([
        [p.x, yaw.x, cross.x],
        [p.y, yaw.y, cross.y],
        [p.z, yaw.z, cross.z],
    ]);
    mul_matrix(&connector.transpose(), &basis)
}

/// `GetTurretOrigin` with a bound pitch joint: the pitch joint's position
/// less its orientation times the joint's child position, rounded per
/// component.
#[must_use]
pub fn pitch_turret_origin(pitch_pos: Vector, pitch_ori: &Matrix, child_pos: Vector) -> Vector {
    let offset = transform_matrix(pitch_ori, child_pos);
    Vector::new(
        r(e(pitch_pos.x) - e(offset.x)),
        r(e(pitch_pos.y) - e(offset.y)),
        r(e(pitch_pos.z) - e(offset.z)),
    )
}

/// `CEGun::ComputeTgtLeadPosition` past its target lookup: where to aim
/// at a target at `target_pos` moving at `target_vel` from a shooter at
/// `shooter_pos` moving at `shooter_vel`, with shots of `speed`. The
/// target's position when no intercept exists.
#[must_use]
pub fn lead_position(
    target_pos: Vector,
    target_vel: Vector,
    shooter_pos: Vector,
    shooter_vel: Vector,
    speed: f32,
) -> Vector {
    let rel_pos = Vector::new(
        r(e(target_pos.x) - e(shooter_pos.x)),
        r(e(target_pos.y) - e(shooter_pos.y)),
        r(e(target_pos.z) - e(shooter_pos.z)),
    );
    let rel_vel = Vector::new(
        r(e(target_vel.x) - e(shooter_vel.x)),
        r(e(target_vel.y) - e(shooter_vel.y)),
        r(e(target_vel.z) - e(shooter_vel.z)),
    );
    match super::launcher::compute_time_to_target(rel_pos, rel_vel, speed) {
        Some(t) => Vector::new(
            r(e(rel_vel.x) * e(t) + e(target_pos.x)),
            r(e(rel_vel.y) * e(t) + e(target_pos.y)),
            r(e(rel_vel.z) * e(t) + e(target_pos.z)),
        ),
        None => target_pos,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn tick_slews_at_the_turn_rate_and_lands_on_the_target() {
        let mut j = JointChannel {
            desired: 1.0,
            ..JointChannel::default()
        };
        let next = j.tick_target(2.0, 0.1);
        assert!((next - 0.2).abs() < 1e-6);
        j.settle(next);
        assert!(!j.settled);
        j.current = 0.95;
        assert_eq!(j.tick_target(2.0, 0.1), 1.0);
        j.desired = -1.0;
        j.current = 0.0;
        assert!((j.tick_target(2.0, 0.1) + 0.2).abs() < 1e-6);
    }

    #[test]
    fn a_full_turn_joint_takes_the_short_way_round() {
        let j = JointChannel {
            instance: None,
            limited: false,
            desired: 3.0,
            current: -3.0,
            settled: false,
        };
        // The short way from -3 to 3 goes down through -pi.
        let next = j.tick_target(1.0, 0.1);
        assert!((next + 3.1).abs() < 1e-6);
    }

    #[test]
    fn window_widens_by_limits() {
        assert!(joint_window_contains(None, 0.1, 0.05));
        assert!(!joint_window_contains(None, 0.1, 0.2));
        assert!(joint_window_contains(Some((-1.0, 1.0)), 0.1, 1.05));
        assert!(!joint_window_contains(Some((-1.0, 1.0)), 0.1, 1.2));
        // A window above 0 has its low end moved down a turn.
        assert!(joint_window_contains(Some((0.5, 1.0)), 0.0, -1.0));
        assert!(!joint_window_contains(Some((0.5, 1.0)), 0.0, 1.5));
    }

    #[test]
    fn identity_frames_decompose_forward_as_zero() {
        let (yaw, pitch) = decompose_joint_motion(
            &Matrix::IDENTITY,
            &Matrix::IDENTITY,
            Vector::new(0.0, 0.0, -5.0),
        );
        assert_eq!((yaw, pitch), (0.0, 0.0));
        let (yaw, _) = decompose_joint_motion(
            &Matrix::IDENTITY,
            &Matrix::IDENTITY,
            Vector::new(1.0, 0.0, -1.0),
        );
        assert!((yaw - core::f32::consts::FRAC_PI_4).abs() < 1e-6);
        let (_, pitch) = decompose_joint_motion(
            &Matrix::IDENTITY,
            &Matrix::IDENTITY,
            Vector::new(0.0, 1.0, -1.0),
        );
        assert!((pitch - core::f32::consts::FRAC_PI_4).abs() < 1e-6);
    }

    #[test]
    fn turret_frame_of_standard_axes_is_identity() {
        let f = compute_turret_frame(
            &Matrix::IDENTITY,
            Vector::new(0.0, 1.0, 0.0),
            Vector::new(1.0, 0.0, 0.0),
        );
        assert_eq!(f, Matrix::IDENTITY);
    }

    #[test]
    fn see_cone() {
        let fwd = Vector::new(0.0, 0.0, -1.0);
        assert!(can_see_target_point(
            Vector::ZERO,
            fwd,
            Vector::new(0.0, 1.0, -100.0),
            0.05
        ));
        assert!(!can_see_target_point(
            Vector::ZERO,
            fwd,
            Vector::new(0.0, 10.0, -100.0),
            0.05
        ));
        assert!(!can_see_target_point(Vector::ZERO, fwd, Vector::ZERO, 0.05));
    }
}
