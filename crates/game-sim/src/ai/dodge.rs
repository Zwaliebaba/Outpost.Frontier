//! The dodge ideas the combat behaviours tick (`include/common_idea.h`):
//! `DodgeIdea` (`Common/common_dodgeidea.cpp`: waggle, waggle-random,
//! slide and corkscrew dodges), `BreakAwayIdea`
//! (`Common/common_breakawayidea.cpp`: outrun, sideways and reverse
//! breaks) and `DodgeMissileIdea` (`Common/common_dodgemissileidea.cpp`).
//!
//! The reference's ideas reach the ship through `Object` (the owner),
//! `Handle` (the system manager: its `SelfTarget` record over the same
//! ship, the thrust manager, the timer table) and `Context` (the
//! behaviour manager: the ship-up direction, the pathfinder). The port
//! runs them on the behaviour tick context ([`Cx`]), which holds all of
//! them. The record's cached position is the view's `center`, as in
//! [`super::intercept`].

use neuron_core::math::{Matrix, Vector};

use super::bank::{compute_banked_steering_frame, limit_axis_throttle_for_turn};
use super::behavior::{AiEnv, Cx};
use super::fp::{column1, column2, cross, dot_zyx, e, neg, normalize3, r, sub};
use super::idea::{DirectionPicker, Idea, IdeaBase};
use super::intercept::{
    get_intercept_clearance_of_iface, is_aligned_and_outside_heading_cone,
    is_point_outside_heading_cone, is_within_intercept_range_of_iface, negated_iface_direction,
};
use super::nav::{projected_throttle, speed, with_motion_of, with_own_motion};
use super::steer::{
    build_orientation_basis, build_perpendicular_frame, build_random_spread_frame,
    get_angular_throttle, random_vector, steering_frame_from_relative_dir,
    transform_vector_by_matrix,
};
use super::timer_table::slot;
use super::world::{ObjectView, ThrustEquipType};

/// `1 / 32768`, the `rand()` scale.
const RAND_SCALE: f64 = 1.0 / 32768.0;
/// π/16 as the float the reference pushes (`0x3e490fdb`).
const SIXTEENTH_PI: f32 = 0.196_349_55;
/// π/8 (`0x3ec90fdb`).
const EIGHTH_PI: f32 = core::f32::consts::FRAC_PI_8;

/// `X' = X*C - S*Z, Z' = C*Z + X*S` on each row (the dodge idea's XZ
/// rotation; Z is re-read after X is stored, which changes nothing).
fn rotate_xz(m: &mut Matrix, c: f64, s: f64) {
    for row in &mut m.m {
        let x = e(row[0]);
        row[0] = r(x * c - s * e(row[2]));
        row[2] = r(c * e(row[2]) + x * s);
    }
}

/// `Y' = S*Z + Y*C, Z' = C*Z - Y*S` on each row.
fn rotate_yz(m: &mut Matrix, c: f64, s: f64) {
    for row in &mut m.m {
        let y = e(row[1]);
        row[1] = r(s * e(row[2]) + y * c);
        row[2] = r(c * e(row[2]) - y * s);
    }
}

/// The break-away idea's XZ rotation: `x' = x*Cos - Sin*z, z' = Cos*z +
/// x*Sin`, both from the row's old values.
fn break_rotate_xz(m: &mut Matrix, angle: f64) {
    let (c, s) = (angle.cos(), angle.sin());
    for row in &mut m.m {
        let (x, z) = (e(row[0]), e(row[2]));
        row[0] = r(x * c - s * z);
        row[2] = r(c * z + x * s);
    }
}

/// `y' = y*Cos + Sin*z, z' = Cos*z - y*Sin`.
fn break_rotate_yz(m: &mut Matrix, angle: f64) {
    let (c, s) = (angle.cos(), angle.sin());
    for row in &mut m.m {
        let (y, z) = (e(row[1]), e(row[2]));
        row[1] = r(y * c + s * z);
        row[2] = r(c * z - y * s);
    }
}

/// Each component times `k`, rounded.
fn scaled(v: Vector, k: f32) -> Vector {
    Vector::new(r(e(v.x) * e(k)), r(e(v.y) * e(k)), r(e(v.z) * e(k)))
}

/// The ship's view and the system manager's watched object (the ship).
fn own<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> Option<ObjectView> {
    cx.world.inspect(cx.ship)
}

/// `ComputeBankedSteeringFrame(&Sys->SelfTarget, get_ship_up_direction(),
/// frame, steer, mode)`.
fn bank<W: AiEnv + ?Sized>(
    cx: &Cx<'_, W>,
    view: &ObjectView,
    frame: &mut Matrix,
    steer: &mut Vector,
    mode: u8,
) {
    let up = cx.state.ship_up;
    with_own_motion(cx, |motion| {
        compute_banked_steering_frame(view, motion, up, frame, steer, mode);
    });
}

/// `LimitAxisThrottleForTurn(ship, steer, frame, turn, reference, true)`.
fn limit<W: AiEnv + ?Sized>(
    cx: &Cx<'_, W>,
    view: &ObjectView,
    steer: Vector,
    frame: &Matrix,
    turn_angle: f32,
    reference_angle: f32,
) -> Vector {
    with_own_motion(cx, |motion| {
        limit_axis_throttle_for_turn(
            Some(view),
            motion,
            steer,
            frame,
            turn_angle,
            reference_angle,
            true,
        )
    })
}

/// The avoid arm the break-away and missile ideas share: a spread frame
/// down the pathfinder's user-zone exit, banked when the turn is wider
/// than π/16, at full throttle. Whether the zone answered.
fn steer_out_of_user_zone<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>, view: &ObjectView) -> bool {
    let Some(probe) = cx
        .state
        .pathfind
        .user_zone_approach_direction(&*cx.world, cx.ship)
    else {
        return false;
    };
    let frame = build_orientation_basis(&view.orientation, probe, None);
    let mut spread = build_random_spread_frame(&mut cx.world, &frame, SIXTEENTH_PI, 0.0);
    let turn = get_angular_throttle(&view.orientation, &spread);
    let mut steer = turn.throttle;
    let mut command = Vector::ZERO;
    if turn.angle > SIXTEENTH_PI {
        bank(cx, view, &mut spread, &mut steer, 1);
        command = steer;
    }
    cx.set_axis_throttle(command);
    cx.set_throttle(1.0, 0);
    true
}

/// `DodgeIdea` (0xe0 bytes, kind 0x12): every interval it rerolls a
/// dodge of its style (0 waggle, 1 waggle random, 2 slide, 3 corkscrew)
/// and steers it.
#[derive(Clone, Debug, PartialEq)]
pub struct DodgeIdea {
    base: IdeaBase,
    /// +0x14 the direction picker.
    pub picker: DirectionPicker,
    /// +0x34 `IntervalTime` (5).
    pub interval_time: f32,
    /// +0x38 `IntervalTimeVariancePercent`.
    pub interval_time_variance_percent: f32,
    /// +0x3c `ConeAngle` (`0x3f860a92`).
    pub cone_angle: f32,
    /// +0x40 `ConeAngleVariancePercent`.
    pub cone_angle_variance_percent: f32,
    /// +0x44 `WaggleAxisConeAngle`.
    pub waggle_axis_cone_angle: f32,
    /// +0x48 `RollAngle`.
    pub roll_angle: f32,
    /// +0x4c `DodgeDistance` (150).
    pub dodge_distance: f32,
    /// +0x50 the distance scale of the interval.
    pub field_50: f32,
    /// +0x54 its spread (0.2).
    pub field_54: f32,
    /// +0x58 the forward a behaviour hands over (`SetField58`).
    pub field_58: Vector,
    /// +0x64 `field_58` is set.
    pub field_64: u8,
    /// +0x68 the frame the dodge steers to.
    pub working_frame: Matrix,
    /// +0x8c `IntervalTimeLeft`.
    pub interval_time_left: f32,
    /// +0x90 `TurnThrottle` (1).
    pub turn_throttle: f32,
    /// +0x94 `Throttle`.
    pub throttle: f32,
    /// +0xb4 `DodgeStyle` (1).
    pub dodge_style: i32,
    /// +0xb8 the slide direction.
    pub vector_b8: Vector,
    /// +0xc8 `SlideThrottle` (1).
    pub slide_throttle: f32,
    /// +0xcc the corkscrew axis throttle (0.1, 0, 1).
    pub vector_cc: Vector,
    /// +0xd8 `bRollFlipDirection`.
    pub roll_flip_direction: u8,
    /// +0xda the afterburner timer switched the thrusters on.
    pub flag_da: u8,
    /// +0xdc the far throttle scale (1).
    pub field_dc: f32,
}

impl Default for DodgeIdea {
    fn default() -> Self {
        Self::new()
    }
}

impl DodgeIdea {
    /// `DodgeIdea::DodgeIdea`.
    #[must_use]
    pub fn new() -> Self {
        Self {
            base: IdeaBase::default(),
            picker: DirectionPicker::default(),
            interval_time: 5.0,
            interval_time_variance_percent: 0.0,
            cone_angle: f32::from_bits(0x3f86_0a92),
            cone_angle_variance_percent: 0.0,
            waggle_axis_cone_angle: 0.0,
            roll_angle: 0.0,
            dodge_distance: 150.0,
            field_50: 0.0,
            field_54: 0.2,
            field_58: Vector::ZERO,
            field_64: 0,
            working_frame: Matrix::default(),
            interval_time_left: 0.0,
            turn_throttle: 1.0,
            throttle: 0.0,
            dodge_style: 1,
            vector_b8: Vector::ZERO,
            slide_throttle: 1.0,
            vector_cc: Vector::new(0.1, 0.0, 1.0),
            roll_flip_direction: 0,
            flag_da: 0,
            field_dc: 1.0,
        }
    }

    /// `SetField58`.
    pub fn set_field_58(&mut self, value: Vector) {
        self.field_58 = value;
        self.field_64 = 1;
    }

    /// The cone angle of a reroll, one draw, rounded once.
    fn cone_draw<W: AiEnv + ?Sized>(&self, cx: &mut Cx<'_, W>) -> f32 {
        let draw = f64::from(cx.rand());
        r((1.0 - (draw * e(self.cone_angle_variance_percent)) * RAND_SCALE) * e(self.cone_angle))
    }

    /// The reroll of the interval (the first switch of `Update`).
    fn reroll<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, view: &ObjectView) {
        let draw = f64::from(cx.rand()) * RAND_SCALE;
        self.interval_time_left = r(e(self.interval_time)
            - (e(self.interval_time_variance_percent) * e(self.interval_time)) * draw);
        self.field_50 = r((f64::from(cx.rand()) * e(self.field_54)) * RAND_SCALE + 1.0);

        if let Some(probe) = cx
            .state
            .pathfind
            .user_zone_approach_direction(&*cx.world, cx.ship)
        {
            let basis = build_orientation_basis(&view.orientation, probe, None);
            self.working_frame =
                build_random_spread_frame(&mut cx.world, &basis, SIXTEENTH_PI, 0.0);
            return;
        }
        let orient = view.orientation;
        match self.dodge_style {
            0 => {
                let angle1 = self.cone_draw(cx);
                let spread2 = f64::from(cx.rand()) * RAND_SCALE * e(self.waggle_axis_cone_angle);
                let angle2 = r((spread2 + spread2) - e(self.waggle_axis_cone_angle));
                let up = Vector::new(0.0, 1.0, 0.0);
                let forward = if self.field_64 != 0 {
                    self.field_58
                } else {
                    neg(column2(&orient))
                };
                self.working_frame = build_orientation_basis(&orient, forward, Some(up));
                let (a1, a2) = (e(angle1), e(angle2));
                match self.picker.pick(&mut cx.world) {
                    1 => {
                        rotate_xz(&mut self.working_frame, a1.cos(), a1.sin());
                        rotate_yz(&mut self.working_frame, a2.cos(), a2.sin());
                    }
                    2 => {
                        let n = -a1;
                        rotate_xz(&mut self.working_frame, e(r(n.cos())), n.sin());
                        rotate_yz(&mut self.working_frame, a2.cos(), a2.sin());
                    }
                    3 => {
                        rotate_yz(&mut self.working_frame, a1.cos(), a1.sin());
                        rotate_xz(&mut self.working_frame, a2.cos(), a2.sin());
                    }
                    4 => {
                        let n = -a1;
                        rotate_yz(&mut self.working_frame, e(r(n.cos())), n.sin());
                        rotate_xz(&mut self.working_frame, a2.cos(), a2.sin());
                    }
                    _ => {}
                }
            }
            1 => {
                let angle1 = self.cone_draw(cx);
                let basis = if self.field_64 != 0 {
                    build_orientation_basis(&orient, self.field_58, None)
                } else {
                    let mut forward = neg(column2(&orient));
                    if let Some(w) = cx.view_of(self.picker.watched) {
                        let direction = negated_iface_direction(Some(&w));
                        forward = if self.picker.flag_1c != 0 {
                            direction
                        } else {
                            neg(direction)
                        };
                    }
                    build_orientation_basis(&orient, forward, None)
                };
                if !view.is_player {
                    self.working_frame =
                        build_random_spread_frame(&mut cx.world, &basis, angle1, self.roll_angle);
                    return;
                }
                if cx.check_steering_gate()
                    && let Some(p) = build_perpendicular_frame(self.field_58, 0.999, None)
                {
                    self.working_frame = build_random_spread_frame(&mut cx.world, &p, angle1, 0.0);
                    return;
                }
                self.working_frame =
                    build_random_spread_frame(&mut cx.world, &orient, angle1, self.roll_angle);
            }
            2 => {
                let draw = random_vector(&mut cx.world, 1.0);
                let b = self.vector_b8;
                let dot = (e(draw.z) * e(b.z) + e(draw.y) * e(b.y)) + e(draw.x) * e(b.x);
                self.vector_b8 = if dot > e(0.7) { neg(draw) } else { draw };
            }
            3 if self.roll_flip_direction != 0 => {
                self.vector_cc.z = -self.vector_cc.z;
            }
            _ => {}
        }
    }

    /// `DodgeIdea::Update(DeltaTime)`: 3 without an owner, else 0.
    #[allow(clippy::too_many_lines)]
    pub fn update<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        if self.base.owner.is_none() {
            return 3;
        }
        let Some(view) = own(cx) else {
            return 3;
        };
        let left = e(self.interval_time_left) - e(dt);
        self.interval_time_left = r(left);
        if left <= 0.0 {
            self.reroll(cx, &view);
        }

        if cx.state.pathfind.field_214 == 2 {
            let dir = sub(cx.state.pathfind.field_22c, view.center);
            self.working_frame = build_orientation_basis(&view.orientation, dir, None);
        }

        let mut throttle = self.throttle;
        if let Some(target) = cx.view_of(self.picker.watched)
            && !view.is_player
        {
            let clearance = r(get_intercept_clearance_of_iface(&view, &target));
            if self.picker.flag_1c != 0 {
                let mut value = with_motion_of(&*cx, Some(target.id), |m| {
                    speed(m, 1.0, ThrustEquipType::Engine)
                });
                let limit = e(self.field_50) * e(self.dodge_distance);
                if limit + 5.0 < e(clearance) {
                    value = r(e(value) * e(self.field_dc));
                } else if limit - 5.0 > e(clearance) {
                    value = with_own_motion(&*cx, |m| speed(m, 1.0, ThrustEquipType::Engine));
                }
                if e(value) < 50.0 {
                    value = 50.0;
                }
                throttle = with_own_motion(&*cx, |m| {
                    projected_throttle(m, value, ThrustEquipType::Engine)
                });
            }
        }

        if cx.state.sys.timers.active(slot::AFTERBURNER) {
            self.flag_da = 1;
            cx.set_thrusters_active(true);
            throttle = 1.0;
        } else {
            if self.flag_da != 0 {
                cx.set_thrusters_active(false);
            }
            self.flag_da = 0;
        }
        cx.set_throttle(throttle, 0);

        // The steering reads the ship again: the bank and the turn limit
        // read the throttle just commanded.
        let Some(view) = own(cx) else {
            return 0;
        };
        let roll_up = cx.state.sys.timers.active(slot::ROLL);
        match self.dodge_style {
            0 | 1 => {
                let turn = get_angular_throttle(&view.orientation, &self.working_frame);
                let mut steer = scaled(turn.throttle, self.turn_throttle);
                let mut frame = self.working_frame;
                bank(cx, &view, &mut frame, &mut steer, 1);
                self.working_frame = frame;
                steer = limit(cx, &view, steer, &self.working_frame, 0.0, EIGHTH_PI);
                if roll_up {
                    steer =
                        transform_vector_by_matrix(&view.orientation, Vector::new(0.0, 0.0, 1.0));
                }
                cx.set_axis_throttle(steer);
                0
            }
            2 => {
                if cx.state.pathfind.field_214 != 0 {
                    return 0;
                }
                let c = column2(&view.orientation);
                let b = self.vector_b8;
                let dot = (e(c.z) * e(b.z) + e(c.y) * e(b.y)) + e(c.x) * e(b.x);
                if dot.abs() < e(0.9999) {
                    let cross_v = Vector::new(
                        r(e(c.z) * e(b.y) - e(c.y) * e(b.z)),
                        r(e(c.x) * e(b.z) - e(c.z) * e(b.x)),
                        r(e(c.y) * e(b.x) - e(c.x) * e(b.y)),
                    );
                    let n = normalize3(cross_v);
                    let nudge = scaled(n, self.slide_throttle);
                    cx.state
                        .thrust
                        .blend_nudge_direction(&mut *cx.world, cx.ship, nudge, dt);
                } else {
                    let m = &view.orientation.m;
                    let right = Vector::new(m[0][0], m[1][0], m[2][0]);
                    cx.set_nudge_direction(right);
                }
                let up = Vector::new(0.0, 1.0, 0.0);
                let back = neg(column2(&view.orientation));
                let mut frame = steering_frame_from_relative_dir(&view.orientation, back, up);
                let turn = get_angular_throttle(&view.orientation, &frame);
                let mut steer = turn.throttle;
                bank(cx, &view, &mut frame, &mut steer, 1);
                steer = limit(cx, &view, steer, &frame, 0.0, EIGHTH_PI);
                if roll_up {
                    steer =
                        transform_vector_by_matrix(&view.orientation, Vector::new(0.0, 0.0, 1.0));
                }
                cx.set_axis_throttle(steer);
                0
            }
            3 => {
                let steer = transform_vector_by_matrix(&view.orientation, self.vector_cc);
                cx.set_axis_throttle(steer);
                0
            }
            _ => 0,
        }
    }

    /// `DodgeIdea::Commit`.
    pub fn commit<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.interval_time_left = 0.0;
        cx.set_nudge_direction(Vector::ZERO);
        cx.set_axis_throttle(Vector::ZERO);
        cx.set_thrusters_active(false);
        self.field_50 = 0.0;
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for DodgeIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        DodgeIdea::update(self, cx, dt)
    }

    fn kind(&self) -> i16 {
        0x12
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        DodgeIdea::commit(self, cx);
    }
}

/// `BreakAwayIdea` (0x9c bytes, kind 0x52): a break of its style (0
/// outrun, 1 sideways, 2 reverse) away from the watched object.
#[derive(Clone, Debug, PartialEq)]
pub struct BreakAwayIdea {
    base: IdeaBase,
    /// +0x14 the direction picker (its flag 1).
    pub picker: DirectionPicker,
    /// +0x34 `BreakStyle`.
    pub break_style: i32,
    /// +0x38 the range the break ends beyond (350).
    pub field_38: f32,
    /// +0x40 `Throttle` (1).
    pub throttle: f32,
    /// +0x48 `AttemptReverseElapsed`.
    pub attempt_reverse_elapsed: f32,
    /// +0x4c `AttemptReverseTime` (4).
    pub attempt_reverse_time: f32,
    /// +0x50 the reverse wait.
    pub field_50: f32,
    /// +0x54 `ReverseDistance` (100).
    pub reverse_distance: f32,
    /// +0x58 the reverse wait's limit (6).
    pub field_58: f32,
    /// +0x5c reversing.
    pub flag_5c: u8,
    /// +0x60 the break frame.
    pub frame_60: Matrix,
    /// +0x84 `IntervalElapsed` (100000).
    pub interval_elapsed: f32,
    /// +0x88 `IntervalTime` (100000).
    pub interval_time: f32,
    /// +0x8c `AfterburnerDelay`.
    pub afterburner_delay: f32,
    /// +0x90 `AfterburnerElapsed`.
    pub afterburner_elapsed: f32,
    /// +0x94 `TurnThrottle` (1).
    pub turn_throttle: f32,
    /// +0x98 `RollThrottle` (1).
    pub roll_throttle: f32,
}

impl Default for BreakAwayIdea {
    fn default() -> Self {
        Self::new()
    }
}

/// 2π/3, the break-away turn (`0x4000C15240000000`).
const BREAK_ANGLE: f64 = 2.094_395_102_393_195_3;

impl BreakAwayIdea {
    /// `BreakAwayIdea::BreakAwayIdea`.
    #[must_use]
    pub fn new() -> Self {
        Self {
            base: IdeaBase::default(),
            picker: DirectionPicker {
                flag_1c: 1,
                ..DirectionPicker::default()
            },
            break_style: 0,
            field_38: 350.0,
            throttle: 1.0,
            attempt_reverse_elapsed: 0.0,
            attempt_reverse_time: 4.0,
            field_50: 0.0,
            reverse_distance: 100.0,
            field_58: 6.0,
            flag_5c: 0,
            frame_60: Matrix::default(),
            interval_elapsed: 100_000.0,
            interval_time: 100_000.0,
            afterburner_delay: 0.0,
            afterburner_elapsed: 0.0,
            turn_throttle: 1.0,
            roll_throttle: 1.0,
        }
    }

    /// `BreakAwayIdea::SetWatchObject`: the picker watches the object and
    /// its flag is cleared.
    pub fn set_watch_object(&mut self, object: Option<u32>) {
        self.picker.watched = object;
        self.picker.flag_1c = 0;
    }

    /// `EvaluateBreakAwayClearance`: 2 nothing to break from, 1 clear, 0
    /// still reversing.
    fn evaluate_clearance<W: AiEnv + ?Sized>(
        &mut self,
        cx: &mut Cx<'_, W>,
        view: &ObjectView,
    ) -> i32 {
        let Some(watched) = cx.view_of(self.picker.watched) else {
            return 2;
        };
        let mut nearly_astern = false;
        if self.flag_5c == 0 {
            nearly_astern = is_point_outside_heading_cone(
                watched.position,
                Some(view),
                f32::from_bits(0xbf7f_f972),
            );
            let sync = Vector::new(if nearly_astern { 1.0 } else { 0.0 }, 0.0, 0.0);
            cx.set_nudge_direction(sync);
        }
        cx.set_axis_throttle(Vector::ZERO);
        let clearance = get_intercept_clearance_of_iface(view, &watched);
        if self.flag_5c == 0 {
            if !(clearance < e(self.reverse_distance)) {
                self.flag_5c = 0;
                return 2;
            }
            let astern = is_point_outside_heading_cone(
                watched.position,
                Some(view),
                f32::from_bits(0xbf34_fdf4),
            );
            if !astern || nearly_astern {
                self.flag_5c = 0;
                return 2;
            }
        }
        self.flag_5c = 1;
        cx.set_throttle(-1.0, 0);
        let Some(target) = cx.view_of(self.picker.watched) else {
            return 0;
        };
        if !is_aligned_and_outside_heading_cone(view, Some(&target), 0.0) {
            return 0;
        }
        self.flag_5c = 0;
        1
    }

    /// `BreakAwayIdea::Update(DeltaTime)`: 3 without an owner; 1 when the
    /// break is done, else 0.
    #[allow(clippy::too_many_lines)]
    pub fn update<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        if self.base.owner.is_none() {
            return 3;
        }
        let Some(view) = own(cx) else {
            return 3;
        };
        if steer_out_of_user_zone(cx, &view) {
            return 0;
        }
        let clock = e(dt) + e(self.interval_elapsed);
        self.interval_elapsed = r(clock);
        if clock >= e(self.interval_time) {
            let up = Vector::new(0.0, 1.0, 0.0);
            let back = neg(column2(&view.orientation));
            self.frame_60 = build_orientation_basis(&view.orientation, back, Some(up));
            match self.picker.pick(&mut cx.world) {
                1 => break_rotate_xz(&mut self.frame_60, BREAK_ANGLE),
                2 => break_rotate_xz(&mut self.frame_60, -BREAK_ANGLE),
                3 => break_rotate_yz(&mut self.frame_60, BREAK_ANGLE),
                4 => break_rotate_yz(&mut self.frame_60, -BREAK_ANGLE),
                _ => {}
            }
            self.interval_elapsed = 0.0;
        }

        match self.break_style {
            2 => {
                if !(self.attempt_reverse_elapsed < self.attempt_reverse_time) {
                    self.attempt_reverse_elapsed = 0.0;
                    self.field_50 = 0.0;
                    self.flag_5c = 0;
                    return 0;
                }
                match self.evaluate_clearance(cx, &view) {
                    0 => {
                        let waited = e(dt) + e(self.field_50);
                        self.field_50 = r(waited);
                        if !(waited > e(self.field_58)) {
                            return 0;
                        }
                        self.field_50 = 0.0;
                        1
                    }
                    1 => {
                        self.attempt_reverse_elapsed = 0.0;
                        self.field_50 = 0.0;
                        1
                    }
                    2 => {
                        self.flag_5c = 0;
                        self.attempt_reverse_elapsed = r(e(self.attempt_reverse_elapsed) + e(dt));
                        0
                    }
                    _ => 0,
                }
            }
            1 => {
                cx.set_throttle(self.throttle, 0);
                // The turn limit and the bank read the throttle just set.
                let Some(view) = own(cx) else {
                    return 0;
                };
                let mut aim = neg(column2(&view.orientation));
                if let Some(w) = cx.view_of(self.picker.watched) {
                    let c = column2(&w.orientation);
                    aim = if self.picker.flag_1c != 0 { neg(c) } else { c };
                }
                let back = neg(column2(&view.orientation));
                let facing = (e(back.x) * e(aim.x) + e(back.z) * e(aim.z)) + e(back.y) * e(aim.y);
                if facing > 0.0 {
                    let turn = get_angular_throttle(&view.orientation, &self.frame_60);
                    let mut command = scaled(turn.throttle, self.turn_throttle);
                    command = limit(cx, &view, command, &self.frame_60, 0.0, 1.0);
                    let mut frame = self.frame_60;
                    bank(cx, &view, &mut frame, &mut command, 0);
                    self.frame_60 = frame;
                    cx.set_axis_throttle(command);
                    return 0;
                }
                let drive = transform_vector_by_matrix(
                    &view.orientation,
                    Vector::new(0.0, 0.0, self.roll_throttle),
                );
                cx.set_axis_throttle(drive);
                let elapsed = e(dt) + e(self.afterburner_elapsed);
                self.afterburner_elapsed = r(elapsed);
                if elapsed >= e(self.afterburner_delay) {
                    cx.set_thrusters_active(true);
                }
                let target = cx.view_of(self.picker.watched);
                let within = target
                    .as_ref()
                    .is_some_and(|t| is_within_intercept_range_of_iface(&view, t, self.field_38));
                if within { 0 } else { 1 }
            }
            0 => {
                let drive = transform_vector_by_matrix(
                    &view.orientation,
                    Vector::new(0.0, 0.0, self.roll_throttle),
                );
                cx.set_axis_throttle(drive);
                cx.set_throttle(self.throttle, 0);
                let elapsed = e(dt) + e(self.afterburner_elapsed);
                self.afterburner_elapsed = r(elapsed);
                if elapsed >= e(self.afterburner_delay) {
                    cx.set_thrusters_active(true);
                }
                let Some(target) = cx.view_of(self.picker.watched) else {
                    return 0;
                };
                if is_within_intercept_range_of_iface(&view, &target, self.field_38) {
                    0
                } else {
                    1
                }
            }
            _ => 0,
        }
    }

    /// `BreakAwayIdea::Commit`.
    pub fn commit<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.interval_elapsed = 100_000.0;
        self.afterburner_elapsed = 0.0;
        cx.set_axis_throttle(Vector::ZERO);
        let is_player = own(cx).is_some_and(|v| v.is_player);
        if !is_player {
            cx.set_thrusters_active(false);
        }
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for BreakAwayIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        BreakAwayIdea::update(self, cx, dt)
    }

    fn kind(&self) -> i16 {
        0x52
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        BreakAwayIdea::commit(self, cx);
    }
}

/// `DodgeMissileIdea` (kind 0x56): away from the missile the timer table
/// tracks, by mode (0 afterburn ahead, 1 turn across, 2 nudge aside).
#[derive(Clone, Debug, Default, PartialEq)]
pub struct DodgeMissileIdea {
    base: IdeaBase,
    /// +0x14 the missile.
    pub missile: Option<u32>,
    /// +0x1c the mode.
    pub mode: i32,
}

impl DodgeMissileIdea {
    /// `DodgeMissileIdea::SetWatchObject`.
    pub fn set_watch_object(&mut self, object: Option<u32>) {
        self.missile = object;
    }

    /// The unit direction from the ship's centre to the missile and its
    /// cosine with the ship's column 2.
    fn toward(view: &ObjectView, missile: &ObjectView) -> (Vector, f64) {
        let dir = normalize3(sub(missile.position, view.center));
        let dot = dot_zyx(dir, column2(&view.orientation));
        (dir, dot)
    }

    /// `DodgeMissileIdea::Update(DeltaTime)`: 3 without an owner, 1
    /// without a missile, else 0.
    pub fn update<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, _dt: f32) -> i32 {
        if self.base.owner.is_none() {
            return 3;
        }
        let Some(missile) = cx.view_of(self.missile) else {
            return 1;
        };
        let Some(view) = own(cx) else {
            return 3;
        };
        if steer_out_of_user_zone(cx, &view) {
            return 0;
        }
        let head_on = e(0.7);
        let mut command = Vector::ZERO;
        match self.mode {
            2 => {
                let (dir, dot) = Self::toward(&view, &missile);
                if !(dot.abs() > head_on) {
                    return 0;
                }
                let sideways = normalize3(cross(dir, column1(&view.orientation)));
                cx.set_nudge_direction(sideways);
                return 0;
            }
            1 => {
                let (dir, dot) = Self::toward(&view, &missile);
                let axis = if !(dot.abs() < head_on) {
                    column1(&view.orientation)
                } else {
                    column2(&view.orientation)
                };
                let escape = normalize3(cross(dir, axis));
                let up = column1(&view.orientation);
                let mut basis = build_orientation_basis(&view.orientation, escape, Some(up));
                let turn = get_angular_throttle(&view.orientation, &basis);
                let mut steer = turn.throttle;
                bank(cx, &view, &mut basis, &mut steer, 1);
                command = steer;
            }
            0 => {}
            _ => return 0,
        }
        cx.set_axis_throttle(command);
        cx.set_throttle(1.0, 0);
        cx.set_thrusters_active(true);
        0
    }

    /// `DodgeMissileIdea::Commit`.
    pub fn commit<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        cx.set_nudge_direction(Vector::ZERO);
        cx.set_axis_throttle(Vector::ZERO);
        cx.set_thrusters_active(false);
    }
}

impl<W: AiEnv + ?Sized> Idea<W> for DodgeMissileIdea {
    fn base(&self) -> &IdeaBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut IdeaBase {
        &mut self.base
    }

    fn update(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        DodgeMissileIdea::update(self, cx, dt)
    }

    fn kind(&self) -> i16 {
        0x56
    }

    fn commit(&mut self, cx: &mut Cx<'_, W>) {
        DodgeMissileIdea::commit(self, cx);
    }
}
