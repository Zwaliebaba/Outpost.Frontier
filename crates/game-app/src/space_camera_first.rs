//! The cockpit camera's motion (`space_camera_first.cpp`: the constructor,
//! the attach, slot 0x58, and the update, slot 0x6c; work package P5.4b).
//!
//! [`FirstPersonCamera`] is `FFirstPersonCamera`'s own state (+0x100 to
//! +0x1a0) and its module globals (the head's lag and the last forward
//! speed); the projection and the watched object are the [`GameCamera`] of
//! `first_person`. The camera's eye is the ship's `HpCockpit` hardpoint
//! plus the cockpit mesh's `cam` part, which the head lags behind under
//! acceleration and rolls with the turn, unless the `cockpit` performance
//! option keeps it rigid.
//!
//! Not here: the cockpit file's loader (`FirstPersonCameraLoad`: the
//! mesh, its material library, `head_turn`, and the `cam` part's
//! transform out of the engine, which [`FirstPersonCamera::set_head`]
//! takes) and the scene draw (slot 0x78). The cockpit mesh's pose
//! ([`FirstPersonCamera::mesh_position`], `mesh_orientation`) is kept
//! here for the renderer to place the instance; the engine hands back
//! what it was given, so the update reads it back unchanged.

use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::math::{Matrix, Vector, mul_matrix, transform_matrix, transpose_transform};

use crate::space_camera::GameCamera;
use crate::space_camera_third::ChaseCx;

/// `G_CockpitLagScale`.
pub const LAG_SCALE: f32 = 0.3;
/// `G_CockpitHeadPivot`.
pub const HEAD_PIVOT: f32 = 1.5;
/// `G_CockpitRollPerSpin` (degrees per rad/s).
pub const ROLL_PER_SPIN: f32 = -4.0;
/// `G_CockpitRollLimit` (degrees).
pub const ROLL_LIMIT: f32 = 4.0;
/// The `cockpit` option's applied value at or above which the head lags
/// and rolls.
pub const COCKPIT_OPTION_THRESHOLD: f64 = 0.999;

fn e(v: f32) -> f64 {
    f64::from(v)
}

#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

fn add(a: Vector, b: Vector) -> Vector {
    Vector::new(r(e(a.x) + e(b.x)), r(e(a.y) + e(b.y)), r(e(a.z) + e(b.z)))
}

/// `RotateMatrix({0, 0, degrees})`: the turn about z (none at or below
/// 0.01 degrees), its cosine and sine stored as floats.
#[must_use]
#[allow(clippy::neg_cmp_op_on_partial_ord)]
pub fn rotate_matrix_z(degrees: f32) -> Matrix {
    if !(e(degrees).abs() > 0.01) {
        return Matrix::IDENTITY;
    }
    let angle = r(e(degrees) * e(0.017_453_292));
    let c = r(e(angle).cos());
    let s = r(e(angle).sin());
    let rot = Matrix {
        m: [[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]],
    };
    mul_matrix(&Matrix::IDENTITY, &rot)
}

/// `FFirstPersonCamera` +0x100 to +0x1a0, and the update's module state.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct FirstPersonCamera {
    /// +0x100 the `HpCockpit` hardpoint's position (fallback `(0, 0, -4)`).
    pub hardpoint_pos: Vector,
    /// +0x10c its orientation.
    pub hardpoint_orient: Matrix,
    /// +0x130 the cockpit mesh's `cam` part position.
    pub head_position: Vector,
    /// +0x13c its orientation.
    pub head_orientation: Matrix,
    /// +0x160 the eye.
    pub eye: Vector,
    /// +0x16c the eye's extra orientation (identity; never written).
    pub orientation_c: Matrix,
    /// The cockpit mesh instance's position (+0x190's instance).
    pub mesh_position: Vector,
    /// Its orientation.
    pub mesh_orientation: Matrix,
    /// +0x1a0 set by the attach; the next update puts the manager back in
    /// the normal camera mode.
    pub attached: bool,
    /// `G_CockpitLastForwardSpeed`.
    pub last_forward_speed: f32,
    /// `G_CockpitHeadLag`.
    pub head_lag: f32,
}

impl Default for FirstPersonCamera {
    /// `FirstPersonCameraConstruct`: the three pairs zero and identity, the
    /// flag set.
    fn default() -> Self {
        Self {
            hardpoint_pos: Vector::ZERO,
            hardpoint_orient: Matrix::IDENTITY,
            head_position: Vector::ZERO,
            head_orientation: Matrix::IDENTITY,
            eye: Vector::ZERO,
            orientation_c: Matrix::IDENTITY,
            mesh_position: Vector::ZERO,
            mesh_orientation: Matrix::IDENTITY,
            attached: true,
            last_forward_speed: 0.0,
            head_lag: 0.0,
        }
    }
}

impl FirstPersonCamera {
    /// The cockpit mesh's `cam` part (`FirstPersonCameraLoad`, IEngine slots
    /// 0x90 and 0x98).
    pub fn set_head(&mut self, position: Vector, orientation: Matrix) {
        self.head_position = position;
        self.head_orientation = orientation;
    }

    /// `FirstPersonCameraAttach` (slot 0x58): HUDCamera's attach (the HUD's
    /// retarget is its owner's), then with an object watched its frame, the
    /// `HpCockpit` hardpoint, and the eye `p + R hp + (R hpR) head` with the
    /// head's orientation into the camera.
    pub fn attach(&mut self, cam: &mut GameCamera, world: &World, object: Option<ObjHandle>) {
        cam.attach(object);
        if let Some(o) = cam.watched.and_then(|h| world.get(h)) {
            let (rotation, position) = (o.engine.orientation, o.engine.position);
            cam.camera.set_world(rotation, position);
            match world.find_hardpoint_ps(o.engine.instance, b"HpCockpit") {
                Some((_, frame)) => {
                    self.hardpoint_pos = frame.translation;
                    self.hardpoint_orient = frame.rotation;
                }
                None => {
                    self.hardpoint_pos = Vector::new(0.0, 0.0, -4.0);
                    self.hardpoint_orient = Matrix::IDENTITY;
                }
            }
            let head = mul_matrix(&cam.camera.world, &self.hardpoint_orient);
            let mut eye = add(
                transform_matrix(&cam.camera.world, self.hardpoint_pos),
                position,
            );
            eye = add(transform_matrix(&head, self.head_position), eye);
            self.eye = eye;
            cam.camera.set_world(head, eye);
        }
        self.attached = true;
    }

    /// `FirstPersonCameraUpdate` (slot 0x6c): with a watched ship, the
    /// cockpit mesh at the hardpoint, the head's lag from the forward
    /// acceleration and its roll from the turn, the eye and orientation
    /// (rigid while the `cockpit` option's applied value is under 0.999),
    /// the shake; then the base's velocity and the attach flag down.
    pub fn update(
        &mut self,
        cam: &mut GameCamera,
        cx: &mut ChaseCx<'_>,
        dt: f32,
        cockpit_option: f32,
    ) {
        let ship = cam
            .watched
            .filter(|h| cx.world.get(*h).is_some_and(|o| o.kind == ObjectKind::Ship));
        if let Some(h) = ship {
            self.update_ship(cam, cx, h, dt, cockpit_option);
        }
        cam.update(dt);
        self.attached = false;
    }

    fn update_ship(
        &mut self,
        cam: &mut GameCamera,
        cx: &mut ChaseCx<'_>,
        h: ObjHandle,
        dt: f32,
        cockpit_option: f32,
    ) {
        let Some(view) = cx.world.inspect(h) else {
            return;
        };
        let ship_rot = view.orientation;
        let cockpit_rot = mul_matrix(&ship_rot, &self.hardpoint_orient);
        if self.attached {
            let _ = cx
                .world
                .with_behavior_manager(h, |m, _| m.state.camera_level_status = 0);
        }
        let eye = add(
            transform_matrix(&ship_rot, self.hardpoint_pos),
            view.position,
        );
        self.eye = eye;
        self.mesh_position = eye;
        self.mesh_orientation = cockpit_rot;

        // The head's lag from the forward acceleration.
        let local_velocity = transpose_transform(&ship_rot, view.velocity);
        let accel = if dt > 0.0 {
            r((e(local_velocity.z) - e(self.last_forward_speed)) / e(dt))
        } else {
            0.0
        };
        let mut step = (1.0 - 1.0 / (e(accel).abs() + 1.0)) * e(LAG_SCALE);
        if accel < 0.0 {
            step = -step;
        }
        step -= e(self.head_lag);
        self.last_forward_speed = local_velocity.z;
        step *= 0.01;
        let limit = r(e(dt) * 3.0);
        let neg = -limit;
        if step < e(neg) {
            step = e(neg);
        } else if step > e(limit) {
            step = e(limit);
        }
        self.head_lag = r(step + e(self.head_lag));
        let lag = self.head_lag;

        let mesh_rot = self.mesh_orientation;
        let rigid_eye = add(
            self.mesh_position,
            transform_matrix(&mesh_rot, self.head_position),
        );

        // The roll from the turn about the ship's y.
        let spin = transpose_transform(&ship_rot, view.angular_velocity);
        let mut roll = e(spin.y) * e(ROLL_PER_SPIN);
        let neg_roll = -ROLL_LIMIT;
        if roll < e(neg_roll) {
            roll = e(neg_roll);
        } else if roll > e(ROLL_LIMIT) {
            roll = e(ROLL_LIMIT);
        }
        let roll_matrix = rotate_matrix_z(r(roll));

        // The head pivot pushed back by the lag and rolled.
        let pivot = Vector::new(0.0, HEAD_PIVOT, 0.0);
        let pivot_offset = transform_matrix(&self.head_orientation, pivot);
        let base = Vector::new(
            r(e(self.head_position.x) - e(pivot_offset.x)),
            r(e(self.head_position.y) - e(pivot_offset.y)),
            r(e(self.head_position.z) - e(pivot_offset.z)),
        );
        let swing = Vector::new(pivot.x, pivot.y, r(e(pivot.z) + -e(lag)));
        let local = add(transform_matrix(&roll_matrix, swing), base);
        self.eye = add(transform_matrix(&mesh_rot, local), self.mesh_position);

        let head = mul_matrix(&mesh_rot, &self.head_orientation);
        let mut orientation = mul_matrix(&head, &self.orientation_c);
        if e(cockpit_option) < COCKPIT_OPTION_THRESHOLD {
            self.eye = rigid_eye;
        } else {
            orientation = mul_matrix(&orientation, &roll_matrix);
        }
        cx.shake.apply(&mut orientation, true, cx.hits, cx.rand);
        cam.camera.set_world(orientation, self.eye);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_roll_matrix_skips_small_angles() {
        assert_eq!(rotate_matrix_z(0.005), Matrix::IDENTITY);
        let m = rotate_matrix_z(90.0);
        assert!((m.m[0][1] + 1.0).abs() < 1e-6 && (m.m[1][0] - 1.0).abs() < 1e-6);
    }

    #[test]
    fn the_constructor_plants_identities() {
        let c = FirstPersonCamera::default();
        assert!(c.attached);
        assert_eq!(c.head_orientation, Matrix::IDENTITY);
        assert_eq!(c.orientation_c, Matrix::IDENTITY);
    }
}
