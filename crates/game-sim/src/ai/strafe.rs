//! `StrafeBehavior` (behaviour 13, `Common/common_strafebehavior.cpp`,
//! class in `include/common_strafebehavior.h`): the attack run of a ship
//! against a slow or stationary non-ship target. Mode 2 picks a point on
//! the target, mode 0 flies at it until something (the target or a body
//! along the ship's heading) is within the trigger range, mode 1 runs away
//! to a strafe point off to the side until the ship is far enough to turn
//! back.

use neuron_core::math::{Matrix, Quaternion, Vector, quaternion_to_matrix, transform_matrix};
use neuron_core::math::{transform_xform, transpose_transform};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp};
use super::behaviors::op_dword;
use super::face_idea::{FaceIdea, angular_distance_travelled};
use super::fp::{add, column2, e, inline_fast_length, neg, r, scale, sub};
use super::idea::{AfterburnerIdea, Idea, publish_target_id};
use super::move_idea::MoveIdea;
use super::nav::{max_scalar_attribute, speed, with_own_motion};
use super::raycast::bearing_angle_to_object;
use super::world::{AiRand, ObjectView, ThrustEquipType};

/// `GRandTwoPi`: 2π through a float.
const RAND_TWO_PI: f64 = 6.283_185_482_025_146_5;

/// `RandUnit`: `rand() / 32768`, unrounded.
fn rand_unit(rand: &mut (impl AiRand + ?Sized)) -> f64 {
    f64::from(rand.rand()) * (1.0 / 32768.0)
}

/// `random_sphere_constrained_2(dir, min, max)` (`common_misc.cpp`): a
/// random direction at an angle in `[min, max)` from +z, spun about +z,
/// then turned so that +z maps onto `dir` (unless `dir` is within about
/// 3.6° of +z). Two draws.
#[must_use]
pub fn random_sphere_constrained_2(
    rand: &mut (impl AiRand + ?Sized),
    dir: Vector,
    min_angle: f32,
    max_angle: f32,
) -> Vector {
    let angle = rand_unit(rand) * (e(max_angle) - e(min_angle)) + e(min_angle);
    let cone = Vector::new(r(angle.sin()), 0.0, r(angle.cos()));
    let phi = rand_unit(rand) * RAND_TWO_PI;
    let phi_stored = r(phi);
    let cos_phi = r(phi.cos());
    let sin_phi = r(e(phi_stored).sin());
    let spin = Matrix::from_rows([
        [cos_phi, -sin_phi, 0.0],
        [sin_phi, cos_phi, 0.0],
        [0.0, 0.0, 1.0],
    ]);
    let spun = transform_matrix(&spin, cone);
    let zero_y = e(dir.y) * 0.0;
    let zero_x = e(dir.x) * 0.0;
    let dot = zero_x + zero_y + e(dir.z);
    let dot_stored = r(dot);
    if !(dot < 0.998) {
        return spun;
    }
    let zero_z = e(dir.z) * 0.0;
    let axis_x = r(e(dir.y) - zero_z);
    let axis_y = r(zero_z - e(dir.x));
    let axis_z = r(zero_x - zero_y);
    let half = e(dot_stored).acos() * 0.5;
    let axis_scale = half.sin()
        / ((e(axis_z) * e(axis_z) + e(axis_y) * e(axis_y)) + e(axis_x) * e(axis_x)).sqrt();
    let rotation = Quaternion::new(
        r(half.cos()),
        r(e(axis_x) * axis_scale),
        r(e(axis_y) * axis_scale),
        r(e(axis_z) * axis_scale),
    );
    transpose_transform(&quaternion_to_matrix(&rotation), spun)
}

/// `NormalizeInlineX87`: `1 / sqrt((z*z + y*y) + x*x)` on the stack.
fn normalize_zyx(v: Vector) -> Vector {
    let inv = 1.0 / ((e(v.z) * e(v.z) + e(v.y) * e(v.y)) + e(v.x) * e(v.x)).sqrt();
    Vector::new(r(e(v.x) * inv), r(e(v.y) * inv), r(e(v.z) * inv))
}

/// `NormalizeByVectorLengthX87`: `1 / sqrt((x*x + y*y) + z*z)`.
fn normalize_xyz(v: Vector) -> Vector {
    let inv = 1.0 / ((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt();
    Vector::new(r(e(v.x) * inv), r(e(v.y) * inv), r(e(v.z) * inv))
}

/// `StrafeBehavior` (0x1bc bytes).
#[derive(Clone, Debug, PartialEq)]
pub struct StrafeBehavior {
    base: BehaviorBase,
    /// +0x2c the trigger range of the run.
    pub trigger_range: f32,
    /// +0x34 the run-away distance.
    pub strafe_distance: f32,
    /// +0x38 the strafe direction.
    pub strafe_dir: Vector,
    /// +0x44 the ship's archetype minimum z.
    pub forward_extent: f32,
    /// +0x48 the mode (0 attack, 1 run away, 2 pick a point).
    pub mode: i32,
    /// +0x4c the strafe point.
    pub point: Vector,
    /// +0x58 the attack throttle.
    pub scale_near: f32,
    /// +0x5c the turn throttle.
    pub scale_far: f32,
    /// +0x60 (2; the poll writes 1).
    pub field_60: i32,
    /// +0x64.
    pub field_64: u8,
    /// +0x68 (999 from `SetContext`).
    pub field_68: i32,
    /// +0x6c.
    pub field_6c: i32,
    /// +0x70 the move idea.
    pub move_idea: MoveIdea,
    /// +0xe8 the face idea.
    pub face: FaceIdea,
    /// +0x1a8 the afterburner idea.
    pub afterburner: AfterburnerIdea,
}

impl StrafeBehavior {
    /// `StrafeBehavior::StrafeBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            trigger_range: 0.0,
            strafe_distance: 0.0,
            strafe_dir: Vector::ZERO,
            forward_extent: 0.0,
            mode: 2,
            point: Vector::ZERO,
            scale_near: 0.0,
            scale_far: 0.0,
            field_60: 2,
            field_64: 0,
            field_68: 0,
            field_6c: 0,
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            afterburner: AfterburnerIdea::default(),
        }
    }

    /// Whether the target is a non-ship moving at most 20 m/s and the ship
    /// a ship (the poll's and the tick's common test).
    fn applies(own: &ObjectView, target: &ObjectView) -> bool {
        if target.type_flags & 0x7_0000 != 0 || own.type_flags & 0x7_0000 == 0 {
            return false;
        }
        let v = target.velocity;
        let speed_sq = (e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z);
        !(speed_sq > 400.0)
    }

    /// `SelectStrafePoint(mode, target)`.
    fn select_strafe_point<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        mode: i32,
        own: &ObjectView,
        target: &ObjectView,
    ) {
        if mode == 1 {
            let self_pos = own.position;
            let target_pos = target.position;
            let extent = max_scalar_attribute(target);
            let delta = sub(self_pos, target_pos);
            let delta_len_sq =
                r((e(delta.z) * e(delta.z) + e(delta.y) * e(delta.y)) + e(delta.x) * e(delta.x));
            let to_target = normalize_zyx(sub(target_pos, self_pos));
            let distance = inline_fast_length(e(delta_len_sq));
            let mut dir = if distance < e(extent) {
                random_sphere_constrained_2(&mut *cx.world, to_target, 0.0, 0.157_079_64)
            } else {
                let t = to_target;
                let side = Vector::new(
                    r(e(t.y) * 0.0 - e(t.z)),
                    r(e(t.z) * 0.0 - e(t.x) * 0.0),
                    r(e(t.x) - e(t.y) * 0.0),
                );
                random_sphere_constrained_2(&mut *cx.world, normalize_xyz(side), 0.0, 0.349_065_87)
            };
            dir.y = 0.0;
            self.strafe_dir = normalize_xyz(dir);
            let d = self.strafe_dir;
            let k = e(self.strafe_distance);
            self.point = Vector::new(
                r(e(self_pos.x) + e(d.x) * k),
                r(e(self_pos.y) + e(d.y) * k),
                r(e(self_pos.z) + e(d.z) * k),
            );
            let point = self.point;
            self.move_idea.set_position(cx, point);
            self.face.set_position(point);
            self.move_idea.set_range_3c(r(e(own.radius) * 5.0));
            self.mode = mode;
            return;
        }
        if mode == 0 {
            self.point = target.center;
            if let Some(sub_id) = own.subtarget.filter(|&s| s != 0)
                && let Some(sub_center) = cx.world.sub_object_center(cx.ship, sub_id)
            {
                let toward = normalize_zyx(sub(target.center_of_mass, sub_center));
                let back = neg(column2(&own.orientation));
                let facing =
                    (e(back.z) * e(toward.z) + e(back.y) * e(toward.y)) + e(back.x) * e(toward.x);
                if facing > 0.2 {
                    self.point = sub_center;
                }
            }
            self.move_idea.set_field_44(2);
            self.move_idea.set_range_3c(1.0);
            let point = self.point;
            self.move_idea.set_position(cx, point);
            self.face.set_position(point);
        }
        self.mode = mode;
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for StrafeBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        if !self.base.bind(owner_id) {
            return false;
        }
        Idea::<W>::bind(&mut self.afterburner, owner_id);
        self.field_60 = 2;
        self.mode = 2;
        self.field_64 = 0;
        self.field_6c = 0;
        self.field_68 = 999;
        true
    }

    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if self.base.bound {
            self.face.set_context(&*cx);
            self.move_idea.set_context(&*cx);
        }
    }

    /// Slot 3: the trigger range from the ship's turn (the time to turn
    /// through π/2 in steps of 0.1 s, plus 1 s, at top speed), the run
    /// parameters, mode 2.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        cx.set_cruise_lock(1, 3);
        let (step, top) = with_own_motion(&*cx, |motion| {
            let mut step = 0.1_f32;
            loop {
                let angular = angular_distance_travelled(motion, 1.0, 0.0, 0.0, step);
                let next = e(step) + e(0.1);
                step = r(next);
                if next > 10.0 {
                    break;
                }
                let half_pi = 1.570_796_370_506_286_6;
                if !(e(angular) < half_pi || e(angular) == half_pi) {
                    break;
                }
            }
            (step, speed(motion, 1.0, ThrustEquipType::Engine))
        });
        self.trigger_range = r(e(top) * (e(step) + 1.0));
        let own = cx.own();
        self.forward_extent = own
            .as_ref()
            .and_then(|v| v.extents)
            .map_or(0.0, |(min, _)| min.z);
        self.field_64 = 0;
        if let Some(v) = own {
            self.strafe_dir = neg(column2(&v.orientation));
        }
        let p = cx.state.personality.strafe;
        self.strafe_distance = p.run_away_distance;
        self.scale_near = p.attack_throttle;
        self.scale_far = p.turn_throttle;
        self.mode = 2;
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        publish_target_id(cx, 0);
        self.move_idea.commit(cx);
        self.face.commit(cx);
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.base.disengage(cx.state);
    }

    /// Slot 5: 1 for a slow non-ship target of a ship, unless the
    /// formation block holds the ship.
    fn poll(&mut self, cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut result = 2;
        let own = cx.own();
        let is_player = own.as_ref().is_some_and(|v| v.is_player);
        let behavior = cx.state.behavior;
        let personality = cx.state.personality;
        let gate = cx
            .state
            .sys
            .formation
            .gate_a(&*cx.world, is_player, behavior, &personality);
        if !gate
            && let (Some(target), Some(own)) = (cx.target_view(), own.as_ref())
            && Self::applies(own, &target)
        {
            result = 1;
            self.field_60 = 1;
        }
        self.base.mode = if self.base.flag29 != 0 { 3 } else { result };
        self.base.mode
    }

    #[allow(clippy::too_many_lines)]
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        let Some(target) = cx.target_view() else {
            self.base.result = 3;
            return 3;
        };
        publish_target_id(cx, target.id);
        let Some(own) = cx.own() else {
            self.base.result = 3;
            return 3;
        };
        if !Self::applies(&own, &target) {
            self.base.result = 3;
            return 3;
        }
        let dt = self.base.tick_delta;
        let mut result = 1;
        let self_pos = own.center;
        match self.mode {
            0 => {
                let bearing = bearing_angle_to_object(own.center, &own.orientation, target.center);
                let scale_40 = if bearing < 0.392_699_092_626_571_66 {
                    self.scale_near
                } else {
                    self.scale_far
                };
                self.move_idea.set_scale_40(scale_40);
                let local = Vector::new(0.0, 0.0, r(e(self.forward_extent) * 1.2));
                let ahead = transform_xform(&own.transform(), local);
                let back = neg(column2(&own.orientation));
                let ray_end = add(ahead, scale(back, 10000.0));
                let system = cx.world.system_of(cx.ship).unwrap_or(0);
                let hits = cx.world.find_ray_collisions(system, ahead, ray_end, 1);
                let gap = match hits.first() {
                    Some(hit) => sub(ahead, hit.point),
                    None => sub(self_pos, target.position),
                };
                let gap_sq = (e(gap.z) * e(gap.z) + e(gap.y) * e(gap.y)) + e(gap.x) * e(gap.x);
                if inline_fast_length(gap_sq) < e(self.trigger_range) {
                    self.select_strafe_point(cx, 1, &own, &target);
                }
                result = 0;
            }
            1 => {
                let t = target.position;
                let toward = sub(t, self_pos);
                let toward_sq = (e(toward.z) * e(toward.z) + e(toward.y) * e(toward.y))
                    + e(toward.x) * e(toward.x);
                let length = r(inline_fast_length(toward_sq));
                let inv = 1.0 / e(length);
                let toward = Vector::new(
                    r(e(toward.x) * inv),
                    r(e(toward.y) * inv),
                    r(e(toward.z) * inv),
                );
                let back = neg(column2(&own.orientation));
                let facing =
                    (e(back.z) * e(toward.z) + e(back.y) * e(toward.y)) + e(back.x) * e(toward.x);
                if facing < 0.0 {
                    let reach = e(max_scalar_attribute(&target)) + e(self.strafe_distance);
                    if reach < e(length) {
                        self.select_strafe_point(cx, 0, &own, &target);
                    }
                }
            }
            2 => self.select_strafe_point(cx, 0, &own, &target),
            _ => {}
        }
        self.move_idea.update(cx, dt);
        self.face.update(cx);
        self.base.result = result;
        result
    }

    /// Slot 10: accepted when the op's object exists.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let id = op_dword(op, 0x0c);
        if cx.view_of((id != 0).then_some(id)).is_some() {
            self.base.flags |= 0x2;
            return true;
        }
        self.base.flags &= 0xfd;
        false
    }
}
