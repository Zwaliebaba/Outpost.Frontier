//! `FaceIdea` (`Common/common_faceidea.cpp`, class in `include/common_idea.h`):
//! the idea that turns a ship to face a watched object, a point, a
//! direction or a whole frame (or applies a fixed axis throttle), banking
//! into the turn and limiting the axis throttle so it does not overshoot.
//!
//! The state is the C++ object's own fields; the `BaseIdea` plumbing
//! becomes arguments: the world and the ship handle, the
//! [`ThrustControl`] of the ship's `ThrustManager`, the [`ShipUpControl`]
//! of its behaviour manager and the [`PathfindFields`] of its
//! `PathfindManager`. The steering is Tier B (decision T11): it goes
//! through `TurnHelper::get_angular_throttle` and the bank helpers.

use neuron_core::math::{Matrix, Vector, transform_matrix};

use super::bank::{compute_banked_steering_frame, limit_axis_throttle_for_turn};
use super::fp::{add, column1, e, normalize3, r, sub};
use super::intercept::{iface_position_or_zero, iface_vector_magnitude};
use super::nav::{PathfindFields, ShipUpControl, ThrustControl, projected_axis_throttle_xy};
use super::raycast::clamped_dot3;
use super::steer::{
    build_orientation_basis, get_angular_throttle, steering_frame_from_relative_dir,
};
use super::world::{AiWorld, MotionModel};

/// `FaceIdea::GetKind`.
pub const FACE_IDEA_KIND: i16 = 0x10;

/// What [`FaceIdea::update`] returns.
pub mod status {
    /// Still turning.
    pub const TURNING: i32 = 0;
    /// Facing within the tolerance.
    pub const FACING: i32 = 1;
    /// The watched object is gone.
    pub const NO_TARGET: i32 = 2;
    /// No object, or no target kind.
    pub const NO_OBJECT: i32 = 3;
}

/// `FaceIdea` (`common_idea.h`), its fields from `+0x14`.
#[derive(Clone, Debug, PartialEq)]
pub struct FaceIdea<H> {
    /// +0x14 the watched object (kind 1).
    pub watcher: Option<H>,
    /// +0x1c the point to face (kind 2).
    pub position_1c: Vector,
    /// +0x28 the direction to face (kind 3).
    pub direction_28: Vector,
    /// +0x34 the frame to take (kind 4).
    pub basis_34: Matrix,
    /// +0x58 the axis throttle of kind 5, ship-local.
    pub vector_58: Vector,
    /// +0x64 use the precise aim (kind 1) or the raw up hint (kind 2);
    /// nothing in the reference sets it.
    pub field_64: u8,
    /// +0x6c, +0x78, +0x84 the raw up hint of kind 2 (three floats 12
    /// bytes apart in the unconfirmed block; zero).
    pub up_hint_6c: Vector,
    /// +0x8c the remaining angle below which the idea reports facing
    /// (π/32).
    pub facing_tolerance: f32,
    /// +0x90 the raw throttle scale (`SetThrottleScale`).
    pub field_90: f32,
    /// +0x94 the cap on the axis throttle's length (1).
    pub turn_throttle: f32,
    /// +0x98 the reference angle of the turn limiter (0.61, then the
    /// reachable angular distance at bind time).
    pub angle_weight_98: f32,
    /// +0x9c π/16.
    pub angle_weight_9c: f32,
    /// +0xa0 0 none, 1 watched, 2 point, 3 direction, 4 frame, 5 axis.
    pub target_kind: i32,
    /// +0xa8 the angle left after the last update (π).
    pub remaining_angle: f32,
    /// +0xad an up vector to add this tick (`SetUpVector`).
    pub up_valid: u8,
    /// +0xb0 that up vector, ship-local.
    pub up_b0: Vector,
    /// +0xbc 0.98.
    pub threshold_bc: f32,
}

impl<H> Default for FaceIdea<H> {
    fn default() -> Self {
        Self::new()
    }
}

impl<H> FaceIdea<H> {
    /// `FaceIdea::FaceIdea` (`common_faceidea.cpp:34`). `Field90` is
    /// `get_projected_axis_throttle_XY(1)` of the still-null object: 0.
    #[must_use]
    pub fn new() -> Self {
        Self {
            watcher: None,
            position_1c: Vector::ZERO,
            direction_28: Vector::ZERO,
            basis_34: Matrix::default(),
            vector_58: Vector::ZERO,
            field_64: 0,
            up_hint_6c: Vector::ZERO,
            facing_tolerance: f32::from_bits(0x3dc9_0fdb),
            field_90: 0.0,
            turn_throttle: 1.0,
            angle_weight_98: f32::from_bits(0x3f1c_28f6),
            angle_weight_9c: f32::from_bits(0x3e49_0fdb),
            target_kind: 0,
            remaining_angle: core::f32::consts::PI,
            up_valid: 0,
            up_b0: Vector::ZERO,
            threshold_bc: f32::from_bits(0x3f7a_e148),
        }
    }

    /// `FaceIdea::SetContext` (`common_faceidea.cpp:71`): the turn
    /// limiter's reference angle from the ship's motion model (the angular
    /// distance it covers at `get_angular_speed_XY(1, 0)`, capped at 1000);
    /// false when the ship is gone.
    pub fn set_context<W: AiWorld<Handle = H> + ?Sized>(&mut self, world: &W, ship: H) -> bool
    where
        H: Copy,
    {
        if world.inspect(ship).is_none() {
            return false;
        }
        let motion = world.motion(ship);
        let rate = motion.map_or(0.0, |m| m.angular_speed_xy(1.0, 0.0));
        self.angle_weight_98 = angular_distance_travelled(motion, 0.0, 0.0, rate, 1000.0);
        // `Behavior_InvokeManagerSlotF4(Object, 1)`: queried and dropped.
        self.angle_weight_9c = f32::from_bits(0x3e49_0fdb);
        self.up_valid = 0;
        true
    }

    /// `FaceIdea::Commit` (`common_faceidea.cpp:104`): a zero axis throttle.
    pub fn commit(&self, thrust: &mut dyn ThrustControl) {
        thrust.set_axis_throttle(Vector::ZERO);
    }

    /// `FaceIdea::SetWatchObject` (`common_faceidea.cpp:119`).
    pub fn set_watch_object(&mut self, object: Option<H>) {
        self.target_kind = i32::from(object.is_some());
        self.watcher = object;
    }

    /// `FaceIdea::SetPosition`.
    pub fn set_position(&mut self, point: Vector) {
        self.target_kind = 2;
        self.position_1c = point;
    }

    /// `FaceIdea::SetDirection` (not normalised).
    pub fn set_direction(&mut self, dir: Vector) {
        self.target_kind = 3;
        self.direction_28 = dir;
    }

    /// `FaceIdea::SetBasis`.
    pub fn set_basis(&mut self, basis: &Matrix) {
        self.target_kind = 4;
        self.basis_34 = *basis;
    }

    /// `FaceIdea::SetAxis`.
    pub fn set_axis(&mut self, axis: Vector) {
        self.target_kind = 5;
        self.vector_58 = axis;
    }

    /// `FaceIdea::SetThrottleScale` (`common_faceidea.cpp:173`): the raw
    /// value to `Field90`, `get_projected_axis_throttle_XY` of it to the
    /// cap.
    pub fn set_throttle_scale(&mut self, motion: Option<&dyn MotionModel>, value: f32) {
        self.field_90 = value;
        self.turn_throttle = projected_axis_throttle_xy(motion, value);
    }

    /// `FaceIdea::SetUpVector` (`common_faceidea.cpp:185`): a zero vector
    /// clears the flag and writes nothing else.
    pub fn set_up_vector(&mut self, up: Vector) {
        if up.x == 0.0 && up.y == 0.0 && up.z == 0.0 {
            self.up_valid = 0;
            return;
        }
        self.up_valid = 1;
        self.up_b0 = up;
    }

    /// `FaceIdea::SetAngleWeight8c`.
    pub fn set_angle_weight_8c(&mut self, value: f32) {
        self.facing_tolerance = value;
    }

    /// `FaceIdea::SetScale94`.
    pub fn set_scale_94(&mut self, value: f32) {
        self.turn_throttle = value;
    }

    /// `FaceIdea::Update` (`common_faceidea.cpp:302`): builds the frame to
    /// face, turns toward it with `get_angular_throttle`, banks, limits and
    /// caps the axis throttle and hands it to the thrust manager. Returns
    /// a [`status`] value; kind 5 applies its axis throttle and returns 0.
    pub fn update<W: AiWorld<Handle = H> + ?Sized>(
        &mut self,
        world: &W,
        ship: H,
        thrust: &mut dyn ThrustControl,
        manager: &mut dyn ShipUpControl,
        pathfind: &PathfindFields,
    ) -> i32
    where
        H: Copy,
    {
        let Some(view) = world.inspect(ship) else {
            return status::NO_OBJECT;
        };
        let motion = world.motion(ship);
        let reference = view.orientation;
        let center = view.center;
        let watched = self.watcher.and_then(|h| world.inspect(h));

        // A frame toward `rel`: with the ship's up when the steering gate
        // is open, else from the reference frame alone.
        let toward = |rel: Vector, gate: bool, manager: &dyn ShipUpControl| -> Matrix {
            if gate {
                steering_frame_from_relative_dir(&reference, rel, manager.ship_up_direction())
            } else {
                build_orientation_basis(&reference, rel, None)
            }
        };
        let fallback = |thrust: &dyn ThrustControl, manager: &dyn ShipUpControl| -> Matrix {
            let gate = thrust.check_steering_gate();
            let rel = sub(iface_position_or_zero(watched.as_ref()), center);
            toward(rel, gate, manager)
        };

        // Kind > 5 (or negative, `JA`) keeps a zero frame.
        let mut working = Matrix::default();
        match self.target_kind {
            0 => return status::NO_OBJECT,
            1 => {
                let Some(target) = watched.as_ref() else {
                    return status::NO_TARGET;
                };
                let w = target.angular_velocity;
                let angular_sq = (e(w.y) * e(w.y) + e(w.z) * e(w.z)) + e(w.x) * e(w.x);
                if self.field_64 == 0 || !(angular_sq < 0.01) {
                    working = fallback(thrust, manager);
                } else {
                    let node_up = column1(&target.orientation);
                    let rel = sub(target.center, center);
                    working = build_orientation_basis(&reference, rel, Some(node_up));
                }
            }
            2 => {
                let raw_up = self.field_64 != 0;
                let gate = !raw_up && thrust.check_steering_gate();
                let rel = sub(self.position_1c, center);
                working = if raw_up {
                    build_orientation_basis(&reference, rel, Some(self.up_hint_6c))
                } else {
                    toward(rel, gate, manager)
                };
            }
            3 => {
                let gate = thrust.check_steering_gate();
                working = toward(self.direction_28, gate, manager);
            }
            4 => working = self.basis_34,
            5 => {
                working = reference;
                let mut banked = !view.is_player;
                if !banked {
                    banked = manager.camera_level_status() && thrust.check_steering_gate();
                }
                let steer = if banked {
                    let mut steer = self.vector_58;
                    let up = manager.ship_up_direction();
                    compute_banked_steering_frame(&view, motion, up, &mut working, &mut steer, 0);
                    steer
                } else {
                    transform_matrix(&reference, self.vector_58)
                };
                thrust.set_axis_throttle(steer);
                return status::TURNING;
            }
            _ => {}
        }

        if pathfind.field_214 & 0x2 != 0 {
            let gate = thrust.check_steering_gate();
            working = toward(sub(pathfind.field_22c, center), gate, manager);
        }

        if view.is_player {
            manager.relevel_ship_up_direction(true);
        }

        let turn = get_angular_throttle(&reference, &working);
        let mut steer = turn.throttle;
        self.remaining_angle = turn.angle;

        let mut refresh =
            view.is_player && manager.camera_level_status() && thrust.check_steering_gate();
        if !refresh {
            refresh = !view.is_player;
        }
        if refresh {
            let up = manager.ship_up_direction();
            compute_banked_steering_frame(&view, motion, up, &mut working, &mut steer, 1);
        }

        let lead_node = watched
            .as_ref()
            .filter(|node| iface_vector_magnitude(Some(node)) > 1.0);
        steer = if let Some(node) = lead_node {
            let lead_angle = lead_angle(node.center, node.velocity, center);
            // `Behavior_InvokeManagerSlotF8(Object, LeadAngle)`: queried
            // and dropped.
            let _ = projected_axis_throttle_xy(motion, lead_angle);
            limit_axis_throttle_for_turn(
                Some(&view),
                motion,
                steer,
                &working,
                lead_angle,
                0.35,
                true,
            )
        } else {
            limit_axis_throttle_for_turn(
                Some(&view),
                motion,
                steer,
                &working,
                0.0,
                self.angle_weight_98,
                true,
            )
        };

        steer = cap_length(steer, self.turn_throttle);
        if self.up_valid != 0 {
            steer = add(steer, transform_matrix(&reference, self.up_b0));
        }
        self.up_valid = 0;
        thrust.set_axis_throttle(steer);

        if self.remaining_angle < self.facing_tolerance {
            status::FACING
        } else {
            status::TURNING
        }
    }
}

/// `Behavior_GetAngularDistanceTravelled` (`common_behavior.cpp:57`): 0
/// for a non-positive maximum or without a motion model; a `dt` below
/// 0.001 counts as 0.
#[must_use]
pub fn angular_distance_travelled(
    motion: Option<&dyn MotionModel>,
    from: f32,
    to: f32,
    dt: f32,
    max_speed: f32,
) -> f32 {
    let Some(m) = motion else {
        return 0.0;
    };
    if max_speed <= 0.0 {
        return 0.0;
    }
    let dt = if e(dt) < 0.001 { 0.0 } else { dt };
    m.angular_distance_travelled_max(from, to, dt, max_speed)
}

/// The lead angle of `FaceIdea::Update`'s tail (`common_faceidea.cpp:690`):
/// the angle between the direction to the node and the direction to where
/// it is one second later.
#[must_use]
pub fn lead_angle(node_pos: Vector, node_velocity: Vector, center: Vector) -> f32 {
    let direct = sub(node_pos, center);
    let lead_x = r(e(node_pos.x) + e(node_velocity.x));
    let lead_y = r(e(node_pos.y) + e(node_velocity.y));
    let lead = Vector::new(
        r(e(lead_x) - e(center.x)),
        r(e(lead_y) - e(center.y)),
        r((e(node_pos.z) + e(node_velocity.z)) - e(center.z)),
    );
    r(clamped_dot3(normalize3(direct), normalize3(lead)).acos())
}

/// The magnitude cap of `FaceIdea::Update`'s tail (`common_faceidea.cpp:735`):
/// a vector longer than `cap` rescaled to exactly `cap`.
#[must_use]
pub fn cap_length(v: Vector, cap: f32) -> Vector {
    let sq = (e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x);
    let cap = e(cap);
    if !(sq > cap * cap) {
        return v;
    }
    let inv = 1.0 / sq.sqrt();
    let unit_y = r(e(v.y) * inv);
    let unit_z = r(e(v.z) * inv);
    Vector::new(
        r(cap * (e(v.x) * inv)),
        r(e(unit_y) * cap),
        r(e(unit_z) * cap),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ai::move_idea::tests::{Motion, ThrustLog, Two};

    #[derive(Default)]
    struct Up {
        relevels: usize,
    }

    impl ShipUpControl for Up {
        fn ship_up_direction(&self) -> Vector {
            Vector::new(0.0, 1.0, 0.0)
        }
        fn camera_level_status(&self) -> bool {
            false
        }
        fn relevel_ship_up_direction(&mut self, _: bool) {
            self.relevels += 1;
        }
    }

    #[test]
    fn faces_a_point_ahead_and_turns_toward_one_aside() {
        let w = Two::new(Vector::ZERO, Vector::ZERO);
        let mut idea = FaceIdea::<u32>::new();
        assert!(idea.set_context(&w, 1));
        let mut thrust = ThrustLog::default();
        let mut up = Up::default();
        let pf = PathfindFields::new();
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::NO_OBJECT
        );
        idea.set_position(Vector::new(0.0, 0.0, -100.0));
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::FACING
        );
        assert_eq!(idea.remaining_angle, 0.0);
        idea.set_position(Vector::new(100.0, 0.0, 0.0));
        thrust.calls.clear();
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::TURNING
        );
        assert!((idea.remaining_angle - core::f32::consts::FRAC_PI_2).abs() < 1e-5);
        assert_eq!(thrust.calls.len(), 1);
        assert!(thrust.calls[0].starts_with("axis"), "{:?}", thrust.calls);
        assert_eq!(up.relevels, 0);
    }

    #[test]
    fn watched_target_gone_and_axis_mode() {
        let w = Two::new(Vector::ZERO, Vector::new(0.0, 0.0, -100.0));
        let mut idea = FaceIdea::<u32>::new();
        let mut thrust = ThrustLog::default();
        let mut up = Up::default();
        let pf = PathfindFields::new();
        idea.set_watch_object(Some(7));
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::NO_TARGET
        );
        idea.set_watch_object(Some(2));
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::FACING
        );
        idea.set_axis(Vector::new(0.0, 1.0, 0.0));
        thrust.calls.clear();
        // Not a ship-like type, so the bank helper declines and the local
        // axis passes unrotated.
        assert_eq!(
            idea.update(&w, 1, &mut thrust, &mut up, &pf),
            status::TURNING
        );
        assert_eq!(thrust.calls, ["axis 0 1 0"]);
        idea.commit(&mut thrust);
        assert_eq!(thrust.calls.last().map(String::as_str), Some("axis 0 0 0"));
    }

    #[test]
    fn helpers() {
        assert_eq!(
            cap_length(Vector::new(3.0, 0.0, 4.0), 1.0),
            Vector::new(0.6, 0.0, 0.8)
        );
        assert_eq!(
            cap_length(Vector::new(0.3, 0.0, 0.4), 1.0),
            Vector::new(0.3, 0.0, 0.4)
        );
        let a = lead_angle(
            Vector::new(0.0, 0.0, -100.0),
            Vector::new(100.0, 0.0, 0.0),
            Vector::ZERO,
        );
        assert!((a - core::f32::consts::FRAC_PI_4).abs() < 1e-6, "{a}");
        let mut idea = FaceIdea::<u32>::new();
        idea.set_up_vector(Vector::ZERO);
        assert_eq!(idea.up_valid, 0);
        idea.set_up_vector(Vector::new(0.0, 0.5, 0.0));
        assert_eq!(idea.up_valid, 1);
        idea.set_throttle_scale(Some(&Motion), 0.5);
        assert_eq!((idea.field_90, idea.turn_throttle), (0.5, 0.5));
    }
}
