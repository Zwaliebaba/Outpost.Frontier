//! The dock behaviour (`DockBehavior`, behaviour 7,
//! `Common/common_dockbehavior.cpp`, `include/common_dockbehavior.h`): a
//! ship flies to a base, a docking ring, a jump gate, a jump hole or a
//! trade lane ring, asks the server for a dock port (or the lane) and
//! follows the docking points into it.
//!
//! The states of `ModeState320` (`EnterState`):
//!
//! * 1 the approach, through the manager's goto behaviour (the "pilot",
//!   `NavPilotPtr`) to within 1000 m;
//! * 2 the final approach to the docking sphere's outer point
//!   (`InterceptPoint39c`, the second dock point; for a lane ring or a
//!   gate a point before the ring, with a random spread);
//! * 3 the request (`IObjAffect::slot_5c`), whose answer comes back through
//!   [`Behavior::on_request_result`] (`RequestCB::OnResult`);
//! * 4 waiting for the answer;
//! * 6, 7, 8, 9 the docking points (the second, the first, a spline for a
//!   ring or gate, the mount), 10 in the lane, 11 the 3 s wait at the
//!   mount of a terminal that does not take the ship in, 12 and 13 done
//!   (directive result 0 and 2).
//!
//! The server's side (the dock queue and the bay handshake that answer
//! the request and finally dock the ship) is P3.3's.

use neuron_core::math::{Matrix, Transform, Vector, transform_matrix, transpose_transform};
use neuron_core::util::spline::CrSplineSegment;

use super::behavior::{
    AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, DockOp, EvaluateResult, GotoOp, ids,
};
use super::face_idea::FaceIdea;
use super::fp::{add, column0, column1, column2, e, len_sq_xyz, neg, normalize3, r, scale, sub};
use super::goto::{GotoBehavior, ProxWatcher, formation_follower_envelope};
use super::gun_convergence::LeaderNotification;
use super::gun_groups::GunGroups;
use super::idea::{AfterburnerIdea, Idea, publish_target_id};
use super::intercept::{
    find_object_docking_at_base, is_point_outside_heading_cone, is_within_intercept_range_of_iface,
    object_type_or_zero,
};
use super::move_idea::MoveIdea;
use super::nav::{max_scalar_attribute, query_fixed_falloff_effect, speed, with_own_motion};
use super::raycast::clamped_dot3;
use super::spline_move::SplineMoveIdea;
use super::world::{AiCommand, ObjectView, StrafeDir, ThrustEquipType};

/// `float` π (`0x40490fdb`).
const PI_F: f32 = core::f32::consts::PI;
/// The approach range of state 1 and the speed cap's "no limit"
/// (`.data` 1000.0f).
const NO_LIMIT: f32 = 1000.0;
/// `0x3eaaaaab`, `1/3` as a `float`, widened.
const ONE_THIRD: f64 = 0.333_333_343_267_440_8;
/// `.rdata` 0.6 (a double), the mount radius scale of state 9.
const MOUNT_RADIUS_SCALE: f64 = 0.6;

/// The terminal types state 9 takes the ship in at (`TERMINAL_TYPE` 3 to
/// 5); at any other the ship waits 3 s at the mount and asks again.
fn takes_ship_in(terminal: i32) -> bool {
    matches!(terminal, 3..=5)
}

/// `LookMatrixYup(direction)` (`common_csys.cpp:268`): an orientation
/// whose −Z column points along `direction` and whose up is as near +Y as
/// the direction allows.
#[must_use]
pub fn look_matrix_yup(direction: Vector) -> Matrix {
    let (x, y, z) = (e(direction.x), e(direction.y), e(direction.z));
    let len_sq = (x * x + y * y) + z * z;
    let (look_x, look_y, look_z) = if len_sq > 1e-4 {
        let inv = -1.0 / len_sq.sqrt();
        (r(inv * x), r(inv * y), r(inv * z))
    } else {
        (0.0f32, 0.0f32, -1.0f32)
    };
    let (lx, ly, lz) = (e(look_x), e(look_y), e(look_z));
    let (ref_x, ref_z) = if !(lx.abs() < 1e-4) || !(lz.abs() < 1e-4) {
        (-lx, lz)
    } else {
        (0.0, 1.0)
    };
    let up_x = e(r(ref_x * ly - lz * 0.0));
    let up_y = e(r(ref_z * lz - ref_x * lx));
    let up_z = e(r(lx * 0.0 - ly * ref_z));
    let up_inv = e(r(1.0 / ((up_z * up_z + up_y * up_y) + up_x * up_x).sqrt()));
    let up_xn = up_x * up_inv;
    let up_yn = up_y * up_inv;
    let up_zn = e(r(up_z * up_inv));
    let mut m = [[0.0f32; 3]; 3];
    m[2][2] = look_z;
    m[2][1] = r(up_zn);
    m[0][0] = r(lz * up_yn - up_zn * ly);
    m[1][0] = r(up_zn * lx - up_xn * lz);
    m[2][0] = r(ly * up_xn - up_yn * lx);
    m[0][1] = r(up_xn);
    m[1][1] = r(up_yn);
    m[0][2] = look_x;
    m[1][2] = look_y;
    Matrix::from_rows(m)
}

/// `DockBehavior` (0x470 bytes), field by field.
#[derive(Clone, Debug, PartialEq)]
pub struct DockBehavior {
    base: BehaviorBase,
    /// +0x30 the move idea (`SubObjectA`).
    pub move_idea: MoveIdea,
    /// +0xa8 the face idea (`SubObjectB`).
    pub face: FaceIdea,
    /// +0x168 the spline-move idea (`SubObjectC`).
    pub spline_idea: SplineMoveIdea,
    /// +0x2dc the afterburner idea of the pilot handle.
    pub afterburner: AfterburnerIdea,
    /// +0x2f0 the manager's goto behaviour drives the ship (`NavPilotPtr`).
    pub pilot: bool,
    /// +0x2f4 the directive.
    pub op: DockOp,
    /// +0x320 the state (`ModeState320`).
    pub mode: i32,
    /// +0x324 the object docked with (`Watcher324`).
    pub watched: Option<u32>,
    /// +0x32c its type flags (`StateFlags32c`): 0x80 a lane ring, 0x40 a
    /// jump gate, 0x800 a jump hole, 0x20 a docking ring.
    pub type_flags: u32,
    /// +0x330 the granted port, -1 for none.
    pub field_330: i32,
    /// +0x334 the port was granted with the directive.
    pub cleared_334: u8,
    /// +0x338 the ring's half or third size (state 2).
    pub field_338: f32,
    /// +0x33c 1.4 times the coast distance at full engine speed.
    pub field_33c: f32,
    /// +0x340 the wait at the mount (state 11).
    pub timer_340: f32,
    /// +0x344 the three docking points line up (state 6 goes to 9).
    pub combat_344: u8,
    /// +0x348 the final approach's segment (state 8).
    pub spline: CrSplineSegment,
    /// +0x388 the trade lane's other ring (`TargetShipPtr388`).
    pub target_ship: Option<u32>,
    /// +0x38c the direction out of the docking sphere, or along the lane.
    pub facing: Vector,
    /// +0x398 in the lane.
    pub abort_398: u8,
    /// +0x399 the lane brought the ship to its end.
    pub flag_399: u8,
    /// +0x39a a request is out (cancelled on disengage).
    pub flag_39a: u8,
    /// +0x39b the player's target died (reported once).
    pub flag_39b: u8,
    /// +0x39c the second dock point (`hpdockpoint<c>02`).
    pub intercept: Vector,
    /// +0x3a8 its orientation.
    pub orient_3a8: Matrix,
    /// +0x3cc the first dock point (`hpdockpoint<c>01`), or the lane start.
    pub dest: Vector,
    /// +0x3d8 its orientation.
    pub orient_3d8: Matrix,
    /// +0x3fc the mount (`hpdockmount<c>`).
    pub saved: Vector,
    /// +0x408 the docking sphere's radius (-1 before a port).
    pub field_408: f32,
    /// +0x40c the mount's orientation.
    pub orient_40c: Matrix,
    /// +0x430 the ship's `hpmount` in its own frame.
    pub offset_430: Vector,
    /// +0x43c the port asked about (`get_dock_hardpoints`' index).
    pub field_43c: i32,
    /// +0x440 the port's terminal type.
    pub sub_440: i32,
    /// +0x444 an offset added to the first dock point (the base follower).
    pub field_444: Vector,
    /// +0x450 the proximity watcher.
    pub prox: ProxWatcher,
    /// +0x46c this tick's speed cap.
    pub max_speed_46c: f32,
}

/// The zero matrix (`Matrix`'s constructor writes nothing).
fn zero_matrix() -> Matrix {
    Matrix::from_rows([[0.0; 3]; 3])
}

impl DockBehavior {
    /// `DockBehavior::DockBehavior(TypeId)`; the fields its constructor
    /// leaves unwritten are zero (D178).
    #[must_use]
    pub fn new(id: i32) -> Self {
        let mut move_idea = MoveIdea::new();
        // The ideas keep their own constructors' values.
        move_idea.set_field_44(0);
        Self {
            base: BehaviorBase::new(id),
            move_idea,
            face: FaceIdea::new(),
            spline_idea: SplineMoveIdea::new(),
            afterburner: AfterburnerIdea::default(),
            pilot: false,
            op: DockOp::default(),
            mode: 0,
            watched: None,
            type_flags: 0,
            field_330: 0,
            cleared_334: 0,
            field_338: 0.0,
            field_33c: 0.0,
            timer_340: 0.0,
            combat_344: 0,
            spline: CrSplineSegment::new([0.0; 3], [0.0; 3], [0.0; 3], [0.0; 3]),
            target_ship: None,
            facing: Vector::ZERO,
            abort_398: 0,
            flag_399: 0,
            flag_39a: 0,
            flag_39b: 0,
            intercept: Vector::ZERO,
            orient_3a8: zero_matrix(),
            dest: Vector::ZERO,
            orient_3d8: zero_matrix(),
            saved: Vector::ZERO,
            field_408: 0.0,
            orient_40c: zero_matrix(),
            offset_430: Vector::ZERO,
            field_43c: 0,
            sub_440: 0,
            field_444: Vector::ZERO,
            prox: ProxWatcher::default(),
            max_speed_46c: 0.0,
        }
    }

    /// `SetDestPointOffset`: the base follower's offset of the first dock
    /// point.
    pub fn set_dest_point_offset(&mut self, offset: Vector) {
        self.field_444 = offset;
    }

    /// The low byte of the type flags as a signed byte is negative: a lane
    /// ring (`0x80`).
    fn lane(&self) -> bool {
        self.type_flags & 0x80 != 0
    }

    /// The object docked with, while it lives.
    fn watched_view<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> Option<ObjectView> {
        cx.view_of(self.watched)
    }

    /// The watched object's id, 0 without one.
    fn watched_id<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> u32 {
        self.watched_view(cx).map_or(0, |v| v.id)
    }

    /// `GetWatchedRawBlock` (slots 24 and 26).
    fn watched_live(&self) -> Option<u32> {
        self.watched
    }

    /// The pilot's slot 4 and `Behavior_ReleaseDirectiveTarget` on it.
    fn release_pilot<W: AiEnv + ?Sized>(cx: &mut Cx<'_, W>) {
        cx.with_peer(ids::GOTO, |b, cx| {
            b.disengage(cx);
            let flags = &mut b.base_mut().flags;
            *flags = (*flags & 0xfd) | 0x01;
        });
    }

    /// The `IObjAffect::slot_60` sites of `Disengage`: the request of the
    /// type cancelled with the watched object's id.
    fn cancel_owner_request<W: AiEnv + ?Sized>(&self, cx: &mut Cx<'_, W>, kind: i32) {
        let id = self.watched_id(cx);
        cx.cancel_request(kind, id, 0);
    }

    /// The pilot's goto op (`EnterState(1)` and `SubmitGotoDirective`).
    fn pilot_op<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>, far: f32) -> GotoOp {
        let mut op = GotoOp {
            range: NO_LIMIT,
            field_58: 1,
            field_5c: cx.view_of(self.prox.watched).map_or(0, |v| v.id),
            field_60: self.prox.near,
            field_64: far,
            ..GotoOp::default()
        };
        if self.type_flags & 0xc0 != 0 {
            op.kind = 1;
            op.destination = self.dest;
        } else {
            op.kind = 0;
            op.field_1c = i32::from_ne_bytes(self.watched_id(cx).to_ne_bytes());
        }
        op
    }

    /// `RefreshApproachState`: the port's three hardpoints
    /// (`get_dock_hardpoints`), whether they line up, and the way out.
    fn refresh_approach_state<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        if self.field_43c < -1 {
            return;
        }
        let Some(h) = self.watched.and_then(|id| cx.world.handle_of(id)) else {
            return;
        };
        if cx.world.inspect(h).is_none() {
            return;
        }
        let answer = cx.world.dock_hardpoints(h, self.field_43c);
        if let Some(t) = answer.terminal {
            self.sub_440 = t;
        }
        if let Some(d) = answer.radius {
            self.field_408 = d;
        }
        // An output the call did not write is the caller's uninitialised
        // local in the reference (D178).
        let zero = Transform::new(zero_matrix(), Vector::ZERO);
        let a = answer.mount.unwrap_or(zero);
        let b = answer.point1.unwrap_or(zero);
        let c = answer.point2.unwrap_or(zero);
        self.saved = a.translation;
        self.orient_40c = a.rotation;
        self.dest = b.translation;
        self.orient_3d8 = b.rotation;
        self.intercept = c.translation;
        self.orient_3a8 = c.rotation;

        self.combat_344 = 0;
        if self.type_flags & 0x0020_0100 == 0 {
            let d1 = sub(self.dest, self.intercept);
            let d2 = sub(self.saved, self.dest);
            let n2 = normalize3(d2);
            let n1 = normalize3(d1);
            let angle = clamped_dot3(n1, n2).acos();
            if angle < e(f32::from_bits(0x3dc9_0fdb)) {
                let sum1 = add(column2(&self.orient_40c), column2(&self.orient_3d8));
                let sum2 = add(sum1, column2(&self.orient_3a8));
                if clamped_dot3(d2, neg(sum2)) > e(f32::from_bits(0x3f7f_f972)) {
                    self.combat_344 = 1;
                }
            }
        }
        if self.type_flags & 0x40 != 0 {
            self.facing = normalize3(sub(self.saved, self.dest));
        }
    }

    /// The own ship's cached centre (`NavHelperRefreshLeadPoint`, the
    /// system manager's self target).
    fn own_center<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> Vector {
        cx.own().map_or(Vector::ZERO, |v| v.center)
    }

    /// `EnterState(NewState)`.
    #[allow(clippy::too_many_lines)]
    fn enter_state<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, new_state: i32) {
        if new_state != 1 && self.mode == 1 {
            Self::release_pilot(cx);
            self.pilot = false;
            cx.engage_cruise(false);
            Idea::<W>::commit(&mut self.afterburner, cx);
        }
        self.max_speed_46c = NO_LIMIT;
        self.mode = new_state;
        match new_state {
            1 => {
                self.pilot = true;
                let op = self.pilot_op(cx, self.prox.far);
                cx.with_peer(ids::GOTO, |b, cx| {
                    b.accept_directive(cx, &DirectiveOp::Goto(op));
                    b.engage(cx);
                    if let Some(g) = b
                        .as_any_mut()
                        .and_then(|a| a.downcast_mut::<GotoBehavior>())
                    {
                        g.flag_36c = 0;
                    }
                });
            }
            2 => {
                cx.set_cruise_lock(1, 3);
                if self.type_flags & 0xc0 != 0 {
                    self.lane_approach_point(cx);
                }
                self.move_idea.set_mode(cx, 0);
                self.move_idea.set_range_3c(64.0);
                self.move_idea.set_position(cx, self.intercept);
                self.face.set_position(self.intercept);
                self.move_idea.set_field_44(0);
            }
            4 => {
                self.move_idea.set_field_44(0);
                self.move_idea.commit(cx);
                self.face.commit(cx);
            }
            6 => {
                if self.type_flags & 0xc0 != 0 {
                    self.face.set_angle_weight_8c(f32::from_bits(0x3ec9_0fdb));
                    self.face.set_position(self.dest);
                    self.move_idea.set_position(cx, self.dest);
                    self.move_idea.set_range_3c(10.0);
                    self.move_idea.set_field_44(2);
                    self.max_speed_46c = 10.0;
                    return;
                }
                if self.type_flags & 0x800 != 0 {
                    return;
                }
                self.face.set_basis(&self.orient_3a8);
                self.move_idea.set_position(cx, self.intercept);
                self.move_idea.set_range_3c(10.0);
                self.move_idea.set_mode(cx, 1);
                self.max_speed_46c = 18.0;
                self.move_idea.offset_4c = self.offset_430;
            }
            7 => {
                cx.set_cruise_lock(0, 3);
                self.move_idea.set_field_44(2);
                self.move_idea.offset_4c = self.offset_430;
                if self.combat_344 != 0 {
                    self.face.set_position(self.saved);
                    self.move_idea.set_mode(cx, 0);
                    self.move_idea.set_position(cx, self.saved);
                    self.move_idea.set_range_3c(25.0);
                    return;
                }
                self.face.set_basis(&self.orient_3d8);
                self.move_idea.set_position(cx, self.dest);
                self.move_idea.set_range_3c(15.0);
                self.move_idea.set_mode(cx, 1);
                self.max_speed_46c = 18.0;
            }
            8 => {
                self.build_final_approach_spline(cx);
                cx.set_cruise_lock(0, 3);
                self.spline_idea.set_field_158(2);
            }
            9 => {
                cx.set_cruise_lock(0, 3);
                self.move_idea.set_position(cx, self.saved);
                self.move_idea.offset_4c = self.offset_430;
                self.move_idea
                    .set_range_3c(r(e(self.field_408) * MOUNT_RADIUS_SCALE));
                if self.type_flags & 0xc0 != 0 {
                    self.face.set_direction(self.facing);
                    self.move_idea.set_field_44(2);
                    return;
                }
                if self.type_flags & 0x800 != 0 {
                    self.move_idea.set_mode(cx, 0);
                    self.move_idea.set_field_59(0);
                    self.move_idea.set_angle_38(f32::from_bits(0x3eb2_b8c3));
                    self.move_idea.set_field_44(2);
                    self.face.set_position(self.saved);
                    return;
                }
                cx.state.lock = true;
                self.move_idea.set_field_44(0);
                if self.combat_344 != 0 {
                    self.face.set_position(self.saved);
                    self.move_idea.set_mode(cx, 0);
                    return;
                }
                self.face.set_basis(&self.orient_40c);
                self.move_idea.set_mode(cx, 1);
                self.max_speed_46c = 18.0;
                if self.type_flags & 0x20 != 0 {
                    return;
                }
                GunGroups::set_shield_active(&mut *cx.world, cx.ship, false);
            }
            0xb => self.timer_340 = 0.0,
            0xc | 0xd => {
                self.base.flags |= 0x1;
                self.dock_disengage(cx);
                self.base.flags &= 0xfd;
                cx.set_directive_result(ids::DOCK, if new_state == 0xc { 0 } else { 2 });
            }
            _ => {}
        }
    }

    /// `EnterState(2)` for a lane ring or a gate: a point 136 m before the
    /// first dock point along the way out, offset sideways by two thirds
    /// of the ring's size and spread up or down at random.
    fn lane_approach_point<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        let center = Self::own_center(cx);
        let delta = sub(center, self.dest);
        let Some(w) = self.watched_view(cx) else {
            return;
        };
        let rot = w.orientation;
        let col0 = column0(&rot);
        let size = e(max_scalar_attribute(&w));
        let mut offset = Vector::new(
            r((e(col0.x) * size * 2.0) * ONE_THIRD),
            r((e(r(e(col0.y) * size)) * 2.0) * ONE_THIRD),
            r(e(r(e(r(e(col0.z) * size)) * 2.0)) * ONE_THIRD),
        );
        let facing = e(offset.y) * e(delta.y) + e(offset.x) * e(delta.x) + e(offset.z) * e(delta.z);
        if facing <= 0.0 {
            offset = neg(offset);
        }
        let rand_unit = f64::from(cx.rand()) * 3.051_757_812_5e-05;
        let spread = r((128.0 - -128.0) * rand_unit + -128.0);
        let col1 = column1(&rot);
        let jitter_x = r(e(r(e(col1.x) * e(spread))) + e(offset.x));
        let jitter_y = r(e(col1.y) * e(spread) + e(offset.y));
        let jitter_z = e(col1.z) * e(spread) + e(offset.z);
        let back = 64.0 - 200.0;
        let back_x = back * e(self.facing.x);
        let back_y = r(back * e(self.facing.y));
        let back_z = r(back * e(self.facing.z));
        let base_x = back_x + e(self.dest.x);
        let base_y = e(back_y) + e(self.dest.y);
        let base_z = r(e(back_z) + e(self.dest.z));
        self.intercept = Vector::new(
            r(base_x + e(jitter_x)),
            r(base_y + e(jitter_y)),
            r(e(base_z) + jitter_z),
        );
        if self.type_flags & 0x80 != 0 {
            self.field_338 = r(e(max_scalar_attribute(&w)) * 0.5);
        } else if self.type_flags & 0x40 != 0 {
            self.field_338 = r(e(max_scalar_attribute(&w)) * ONE_THIRD);
        }
    }

    /// `BuildFinalApproachSpline`: the segment from the ship (ahead along
    /// its nose) to the first dock point (beyond it along the way out).
    fn build_final_approach_spline<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        let Some(own) = cx.own() else {
            return;
        };
        let mut offset = Vector::ZERO;
        if cx
            .state
            .sys
            .formation
            .formation_leader(&*cx.world)
            .is_none()
        {
            let local = own.formation_offset.unwrap_or(Vector::ZERO);
            offset = transform_matrix(&own.orientation, local);
        }
        let center = own.center;
        let p1 = add(center, offset);
        let dock_point = add(self.dest, self.field_444);
        let p2 = add(dock_point, offset);
        let span = sub(p2, p1);
        let length =
            ((e(span.z) * e(span.z) + e(span.y) * e(span.y)) + e(span.x) * e(span.x)).sqrt();
        let lead_in = r(length * 0.5);
        let lead_out = r(length * 2.5);
        let forward = column2(&own.orientation);
        let start = add(scale(forward, lead_in), p1);
        let p0 = add(start, offset);
        let behind = Vector::new(
            r(e(lead_out) * e(self.facing.x)),
            r(e(lead_out) * e(self.facing.y)),
            r(e(lead_out) * e(self.facing.z)),
        );
        let end = add(behind, p2);
        let p3 = add(end, offset);
        let arr = |v: Vector| [v.x, v.y, v.z];
        self.spline = CrSplineSegment::new(arr(p0), arr(p1), arr(p2), arr(p3));
        self.spline.calculate_arclength();
        self.spline_idea.set_spline(&*cx, self.spline);
    }

    /// `AdvancePreamble`: this tick's linear and angular speed caps (the
    /// state's cap, the formation's envelope, the proximity watcher's) to
    /// the three ideas.
    fn advance_preamble<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        let mut linear = self.max_speed_46c;
        let mut angular = NO_LIMIT;
        let (valid, envelope) = formation_follower_envelope(&*cx);
        if valid {
            if !(linear < envelope.speed) {
                linear = envelope.speed;
            }
            if !(angular < envelope.angular_speed) {
                angular = envelope.angular_speed;
            }
        }
        if cx.view_of(self.prox.watched).is_some() {
            let cap = self.prox.speed_cap(&*cx, 1, 1.0);
            if !(cap > e(linear)) {
                linear = r(cap);
            }
        }
        self.move_idea.set_throttle_scale(&*cx, linear);
        self.face.set_throttle_scale(&*cx, angular);
        self.spline_idea.set_throttle_scale(&*cx, linear);
    }

    /// `CheckCanProceed`: states 2, 3, 6 and 8 give up beyond 1050 m of
    /// the object (a lane ring or gate behind the ship stops states 3 and
    /// 6 at once; state 8 only while a request is out or the port was
    /// granted).
    fn check_can_proceed<W: AiEnv + ?Sized>(&self, cx: &Cx<'_, W>) -> bool {
        match self.mode {
            3 | 6 => {
                if self.type_flags & 0xc0 != 0 {
                    let delta = sub(Self::own_center(cx), self.dest);
                    let along = e(delta.z) * e(self.facing.z)
                        + e(delta.y) * e(self.facing.y)
                        + e(delta.x) * e(self.facing.x);
                    if !(along < 0.0) {
                        return false;
                    }
                }
            }
            8 => {
                if self.flag_39a == 0 && self.cleared_334 == 0 {
                    return false;
                }
            }
            2 => {}
            _ => return true,
        }
        let (Some(own), Some(w)) = (cx.own(), self.watched_view(cx)) else {
            return false;
        };
        is_within_intercept_range_of_iface(&own, &w, r(e(NO_LIMIT) * 1.05))
    }

    /// `Disengage` (slot 4).
    fn dock_disengage<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        if self.mode == 0 {
            if self.cleared_334 == 0 {
                return;
            }
            self.sub_440 = 0;
            self.field_330 = -1;
            self.cleared_334 = 0;
            self.cancel_owner_request(cx, 0);
            return;
        }
        cx.state.lock = false;
        if self.pilot {
            Self::release_pilot(cx);
            self.pilot = false;
        }
        if self.lane() {
            self.move_idea.set_field_44(2);
            if self.abort_398 != 0 && self.flag_399 == 0 {
                cx.world.command(cx.ship, AiCommand::RequestExitTradeLane);
            }
            if self.flag_39a != 0 {
                self.cancel_owner_request(cx, 2);
            }
        } else {
            let player = cx.own().is_some_and(|v| v.is_player);
            let level = if player && self.mode == 1 { 2 } else { 0 };
            self.move_idea.set_field_44(level);
            if self.base.flags & 0x1 == 0 {
                self.sub_440 = 0;
                self.field_330 = -1;
                self.cleared_334 = 0;
                self.cancel_owner_request(cx, 0);
            }
        }
        self.flag_39a = 0;
        self.abort_398 = 0;
        Idea::<W>::commit(&mut self.afterburner, cx);
        self.face.commit(cx);
        self.move_idea.commit(cx);
        cx.set_cruise_lock(1, 3);
        GunGroups::set_shield_active(&mut *cx.world, cx.ship, true);
        self.base.disengage(cx.state);
        self.mode = 0;
    }

    /// `RecomputeApproach` (slot 3).
    fn recompute_approach<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        self.flag_399 = 0;
        self.flag_39a = 0;
        self.flag_39b = 0;
        self.pilot = false;
        self.base.engage();
        let Some(watched) = self.watched_view(cx) else {
            self.enter_state(cx, 0xd);
            return;
        };
        self.field_43c = -1;
        self.combat_344 = 0;
        let full = with_own_motion(&*cx, |m| speed(m, 1.0, ThrustEquipType::Engine));
        let coast = with_own_motion(&*cx, |m| query_fixed_falloff_effect(m, full));
        self.field_33c = r(e(coast) * e(1.4f32));
        cx.set_cruise_lock(1, 3);
        if self.lane() {
            let ring = cx.world.handle_of(watched.id);
            let lane = self.target_ship.and_then(|id| cx.world.handle_of(id));
            let answer = cx.world.lane_start(cx.ship, ring, lane);
            self.target_ship = answer.lane.and_then(|h| cx.world.inspect(h)).map(|v| v.id);
            if let Some(start) = answer.start {
                self.dest = start;
            }
            if let Some(dir) = answer.direction {
                self.facing = dir;
            }
            self.orient_3d8 = look_matrix_yup(self.facing);
            publish_target_id(cx, watched.id);
        } else if self.type_flags & 0x40 != 0 {
            self.field_43c = 0;
            self.refresh_approach_state(cx);
        }
        let mut next = 1;
        if let Some(own) = cx.own()
            && own.is_player
        {
            let target = self.watched_view(cx).map_or(Vector::ZERO, |v| v.position);
            let dx = e(target.x) - e(own.position.x);
            let dy = e(target.y) - e(own.position.y);
            let dz = e(target.z) - e(own.position.z);
            let dist_sq = dx * dx + dy * dy + dz * dz;
            if !(dist_sq < e(10000.0f32 * 10000.0f32)) {
                cx.world.command(
                    cx.ship,
                    AiCommand::Slot {
                        slot: 27,
                        ints: vec![0, 0],
                        floats: Vec::new(),
                    },
                );
                next = 0xd;
            }
        }
        self.enter_state(cx, next);
        self.move_idea.offset_4c = Vector::ZERO;
        self.move_idea.set_angle_38(PI_F);
        self.max_speed_46c = NO_LIMIT;
    }

    /// `UpdateApproachTail`: the state's tick.
    #[allow(clippy::too_many_lines)]
    fn update_approach_tail<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        self.base.result = 1;
        self.advance_preamble(cx);
        let dt = self.base.tick_delta;

        if self.mode == 1 {
            let done = cx
                .with_peer(ids::GOTO, |b, cx| {
                    b.base_mut().tick_delta = dt;
                    b.update(cx)
                })
                .unwrap_or(3);
            if done != 2 {
                return self.base.result;
            }
            self.enter_state(cx, if self.type_flags & 0xc0 != 0 { 2 } else { 3 });
            self.base.result = 1;
            return 1;
        }

        let mut aborted = false;
        if self.mode != 0xc {
            let health = self.watched_view(cx).map_or(1.0, |v| v.hit_points.0);
            if !(health > 0.0) {
                aborted = true;
            }
        }
        if aborted || (cx.view_of(self.prox.watched).is_some() && self.prox.zone_flags & 2 == 0) {
            self.move_idea.set_field_44(0);
            self.move_idea.commit(cx);
            self.face.commit(cx);
            self.base.result = 1;
            return 1;
        }

        if !self.check_can_proceed(cx) {
            self.dock_disengage(cx);
            self.recompute_approach(cx);
            self.base.result = 1;
            return 1;
        }

        match self.mode {
            2 => {
                let player = cx.own().is_some_and(|v| v.is_player);
                if self.target_ship.is_none() && !player && self.lane() {
                    self.enter_state(cx, 0xc);
                    self.base.result = 1;
                    return 1;
                }
                let mut past_dock = false;
                if self.type_flags & 0xc0 != 0 {
                    let delta = sub(Self::own_center(cx), self.dest);
                    let along = e(delta.z) * e(self.facing.z)
                        + e(delta.y) * e(self.facing.y)
                        + e(delta.x) * e(self.facing.x);
                    past_dock = along < 0.0;
                }
                if !past_dock {
                    let _ = self.face.update(cx);
                    if self.move_idea.update(cx, dt) != 1 {
                        return self.base.result;
                    }
                }
                self.enter_state(cx, if self.type_flags & 0x8c0 == 0 { 6 } else { 3 });
                self.base.result = 1;
                1
            }
            3 => {
                let mut slot_mode = 0;
                let mut target_query = 0;
                if self.lane() {
                    self.saved = self.dest;
                    target_query = cx.view_of(self.target_ship).map_or(0, |v| v.id);
                    slot_mode = 2;
                }
                if self.cleared_334 != 0 {
                    self.enter_state(cx, 6);
                    self.base.result = 1;
                    return 1;
                }
                self.enter_state(cx, 4);
                self.flag_39a = 1;
                let engine_query = self.watched_id(cx);
                let answers = cx.request(ids::DOCK, slot_mode, engine_query, target_query, 0);
                for a in answers {
                    if !Behavior::<W>::on_request_result(self, cx, a.kind, a.response, a.arg) {
                        cx.end_request();
                    }
                }
                self.base.result = 1;
                1
            }
            6 => {
                if self.type_flags & 0x800 != 0 {
                    self.enter_state(cx, 9);
                    self.base.result = 1;
                    return 1;
                }
                let move_result = self.move_idea.update(cx, dt);
                let face_result = self.face.update(cx);
                if self.type_flags & 0xc0 != 0 && face_result == 1 {
                    self.enter_state(cx, 8);
                    self.base.result = 1;
                    return 1;
                }
                if move_result != 1 {
                    return self.base.result;
                }
                self.enter_state(cx, if self.combat_344 != 0 { 9 } else { 7 });
                self.base.result = 1;
                1
            }
            7 | 10 => {
                if self.mode == 7 {
                    let _ = self.face.update(cx);
                    if self.move_idea.update(cx, dt) == 1 {
                        self.enter_state(cx, 9);
                    }
                }
                self.base.result = 0;
                0
            }
            8 => {
                if Idea::<W>::update(&mut self.spline_idea, cx, dt) == 1 {
                    Idea::<W>::commit(&mut self.spline_idea, cx);
                    self.enter_state(cx, 9);
                    self.base.result = 1;
                    return 1;
                }
                if !(self.spline_idea.face.remaining_angle < f32::from_bits(0x3e49_0fdb)) {
                    self.build_final_approach_spline(cx);
                    self.base.result = 1;
                    return 1;
                }
                self.base.result
            }
            9 => self.mount_tick(cx, dt),
            0xb => {
                self.timer_340 = r(e(self.timer_340) + e(dt));
                let player = cx.own().is_some_and(|v| v.is_player);
                let threshold = if !cx.world.single_player() && player {
                    10.0
                } else {
                    3.0
                };
                if threshold < self.timer_340 {
                    self.dock_disengage(cx);
                    self.recompute_approach(cx);
                    self.base.result = 1;
                    return 1;
                }
                self.base.result
            }
            0xc | 0xd => {
                self.base.result = 2;
                2
            }
            _ => self.base.result,
        }
    }

    /// State 9 of `UpdateApproachTail`: at the mount, into the lane, or
    /// waiting for a terminal that does not take the ship in.
    fn mount_tick<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>, dt: f32) -> i32 {
        self.base.result = 0;
        let _ = self.face.update(cx);
        let move_result = self.move_idea.update(cx, dt);
        if self.lane() {
            if move_result != 1 {
                let retreat = Vector::new(
                    r(e(self.facing.x) * -25.0 + e(self.saved.x)),
                    r(e(self.facing.y) * -25.0 + e(self.saved.y)),
                    r(e(r(e(self.facing.z) * -25.0)) + e(self.saved.z)),
                );
                let own = cx.own();
                if !is_point_outside_heading_cone(retreat, own.as_ref(), 0.0) {
                    return 0;
                }
            }
            self.move_idea.commit(cx);
            self.face.commit(cx);
            cx.world.command(
                cx.ship,
                AiCommand::SetTarget {
                    target: None,
                    subtarget: 0,
                },
            );
            let entrance = self.watched.and_then(|id| cx.world.handle_of(id));
            let exit = self.target_ship.and_then(|id| cx.world.handle_of(id));
            let refused = cx.world.command(
                cx.ship,
                AiCommand::GoTradeLane {
                    entrance,
                    exit,
                    fast_exit: false,
                    speed: 0.0,
                },
            ) != 0;
            if refused {
                self.enter_state(cx, 0xd);
                return 0;
            }
            if let Some(own) = cx.own() {
                cx.state.leader_notes.push(LeaderNotification {
                    leader: own.id,
                    extra: 0,
                    duration: 0.0,
                });
            }
            self.move_idea.set_field_44(2);
            self.enter_state(cx, 10);
            self.abort_398 = 1;
            self.flag_39a = 1;
            return 0;
        }
        if move_result != 1 {
            return 0;
        }
        if !takes_ship_in(self.sub_440) {
            self.enter_state(cx, 0xb);
            return 0;
        }
        self.move_idea.commit(cx);
        self.face.commit(cx);
        cx.world.command(
            cx.ship,
            AiCommand::SetTarget {
                target: None,
                subtarget: 0,
            },
        );
        self.enter_state(cx, 0xc);
        0
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for DockBehavior {
    fn base(&self) -> &BehaviorBase {
        &self.base
    }

    fn base_mut(&mut self) -> &mut BehaviorBase {
        &mut self.base
    }

    fn as_any_mut(&mut self) -> Option<&mut dyn core::any::Any> {
        Some(self)
    }

    fn as_any(&self) -> Option<&dyn core::any::Any> {
        Some(self)
    }

    /// (1) `SetContext`: the flags it clears; the ideas and the `hpmount`
    /// offset in [`Self::attach`].
    fn bind(&mut self, owner_id: Option<u32>) -> bool {
        let ok = self.base.bind(owner_id);
        if ok {
            self.abort_398 = 0;
            self.flag_39b = 0;
            Idea::<W>::bind(&mut self.afterburner, owner_id);
        }
        ok
    }

    /// The world half of `SetContext`: the ideas bound, the ship's
    /// `hpmount` in its own frame.
    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.move_idea.set_context(&*cx);
        self.move_idea.set_mode(cx, 0);
        self.face.set_context(&*cx);
        self.spline_idea.set_context(cx);
        self.field_444 = Vector::ZERO;
        self.base.flags &= !0x2;
        let Some(own) = cx.own() else {
            return;
        };
        let Some((mount, _)) = cx.world.hardpoint(cx.ship, b"hpmount") else {
            self.offset_430 = Vector::ZERO;
            return;
        };
        let delta = sub(mount, own.center);
        self.offset_430 = transpose_transform(&own.orientation, delta);
    }

    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.recompute_approach(cx);
    }

    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.dock_disengage(cx);
    }

    /// (5) `SetModeFromFlags`: 1 with a directive, else 2; 3 while +0x29.
    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut mode = if self.base.flags & 0x2 != 0 { 1 } else { 2 };
        if self.base.flag29 != 0 {
            mode = 3;
        }
        self.base.mode = mode;
        mode
    }

    /// (6) `CheckClearanceProgress`: whether the player's target is a
    /// still object the ship could dock with.
    fn evaluate(&mut self, cx: &mut Cx<'_, W>, _context: f32) -> EvaluateResult {
        if self.abort_398 != 0 {
            return EvaluateResult::Blocked;
        }
        let Some(candidate) = cx.target_view() else {
            return EvaluateResult::Skip;
        };
        let kind = candidate.type_flags;
        if kind & 0x0060_0de0 != 0 {
            if kind & 0x8c0 == 0 {
                if kind & 0x120 == 0 {
                    return EvaluateResult::Skip;
                }
                let has = cx
                    .world
                    .handle_of(candidate.id)
                    .is_some_and(|h| cx.world.has_dock_hardpoints(h));
                if !has {
                    return EvaluateResult::Skip;
                }
            }
        } else {
            if kind & 0x3 == 0 || candidate.base.unwrap_or(0) == 0 {
                return EvaluateResult::Skip;
            }
            let found = cx
                .world
                .handle_of(candidate.id)
                .and_then(|h| find_object_docking_at_base(&*cx.world, h));
            if found.is_none() {
                return EvaluateResult::Skip;
            }
        }
        if !(candidate.hit_points.0 > 0.0) {
            return EvaluateResult::Skip;
        }
        if len_sq_xyz(candidate.velocity) < e(0.3f32) {
            EvaluateResult::Blocked
        } else {
            EvaluateResult::Skip
        }
    }

    /// (7) `Update`.
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if self.base.flags & 0x1 != 0 {
            self.base.result = 2;
            return 2;
        }
        let Some(target) = self.watched_view(cx) else {
            self.base.result = 3;
            return 3;
        };
        if !self.base.bound {
            self.base.result = 3;
            return 3;
        }
        if cx.own().is_some_and(|v| v.is_player) && self.lane() {
            if target.hit_points.0 > 0.0 {
                self.flag_39b = 0;
            } else if self.flag_39b == 0 {
                self.flag_39b = 1;
                cx.world.command(
                    cx.ship,
                    AiCommand::Slot {
                        slot: 27,
                        ints: vec![1, 0],
                        floats: Vec::new(),
                    },
                );
            }
        }
        if len_sq_xyz(target.velocity) > e(0.3f32) {
            self.face.commit(cx);
            self.move_idea.set_field_44(0);
            self.move_idea.commit(cx);
            self.base.result = 1;
            return 1;
        }
        self.prox.recompute_zone_flags(&*cx, target.position);
        self.prox.notify_zone_change(cx, target.position);
        self.update_approach_tail(cx)
    }

    /// (10) `Slot10`: the dock op; the object (or the ring docking at a
    /// planet), its type, the lane's other ring, a granted port and the
    /// proximity watcher.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let DirectiveOp::Dock(op) = op else {
            self.base.flags = (self.base.flags & 0xfd) | 0x1;
            return true;
        };
        self.op = op.clone();
        self.mode = 0;
        self.type_flags = 0;
        let verify = |cx: &Cx<'_, W>, id: u32| -> Option<u32> {
            (id != 0)
                .then(|| cx.view_of(Some(id)).map(|v| v.id))
                .flatten()
        };
        self.watched = verify(cx, op.dock_object);
        let Some(watched) = self.watched else {
            self.base.flags = (self.base.flags & 0xfd) | 0x1;
            return true;
        };
        if let Some(h) = cx.world.handle_of(watched)
            && let Some(ring) = find_object_docking_at_base(&*cx.world, h)
        {
            self.watched = cx.world.inspect(ring).map(|v| v.id);
        }
        self.type_flags = object_type_or_zero(self.watched_view(cx).as_ref());
        self.target_ship = verify(cx, op.field_10);
        self.cleared_334 = op.field_14;
        self.field_408 = -1.0;
        if op.field_14 != 0 {
            self.field_330 = op.port;
            self.field_43c = op.port;
            self.refresh_approach_state(cx);
        } else {
            self.sub_440 = 0;
            self.field_330 = -1;
        }
        let prox = verify(cx, op.field_1c);
        self.prox.init(
            &*cx,
            prox,
            op.field_20,
            op.field_24,
            f32::from_bits(op.field_28),
        );
        self.base.flags = (self.base.flags & 0xfe) | 0x2;
        true
    }

    /// (11) `IsClearanceState`.
    fn docking_probe(&self) -> bool {
        !matches!(self.mode, 6..=8 | 10)
    }

    /// (12) `SubmitGotoDirective`: a direction goes to the pilot; a zero
    /// one gives it the approach op again.
    fn set_direction(&mut self, cx: &mut Cx<'_, W>, dir: Vector) {
        if !self.pilot {
            return;
        }
        if !(len_sq_xyz(dir) == 0.0) {
            cx.with_peer(ids::GOTO, |b, cx| b.set_direction(cx, dir));
            return;
        }
        let op = self.pilot_op(cx, self.prox.far);
        cx.with_peer(ids::GOTO, |b, cx| {
            b.accept_directive(cx, &DirectiveOp::Goto(op));
        });
    }

    /// (13) the pilot's throttle.
    fn set_throttle(&mut self, cx: &mut Cx<'_, W>, throttle: f32) {
        if self.pilot {
            cx.with_peer(ids::GOTO, |b, cx| b.set_throttle(cx, throttle));
        }
    }

    /// (14) the pilot's cruise.
    fn set_cruise(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        if self.pilot {
            cx.with_peer(ids::GOTO, |b, cx| b.set_cruise(cx, on));
        }
    }

    /// (16) `Behavior::set_afterburner` while the pilot flies.
    fn set_afterburner(&mut self, cx: &mut Cx<'_, W>, on: bool) {
        if !self.pilot {
            return;
        }
        let (cruise, lane) = cx
            .world
            .inspect(cx.ship)
            .and_then(|v| v.cruise)
            .unwrap_or((false, false));
        if cruise && !lane {
            return;
        }
        if !on {
            cx.set_thrusters_active(false);
            return;
        }
        cx.set_thrusters_active(true);
        cx.set_throttle(1.0, if lane { 2 } else { 0 });
    }

    /// (17) `Behavior::slide_strafe_burst` while the pilot flies.
    fn slide_strafe_burst(&mut self, cx: &mut Cx<'_, W>, dir: StrafeDir) {
        if self.pilot {
            cx.set_strafe_dir(dir);
        }
    }

    /// (21) `Slot21`: the watchers let go of an object that left.
    fn object_left(&mut self, cx: &mut Cx<'_, W>, id: u32) {
        let _ = cx;
        if self.watched == Some(id) {
            self.watched = None;
        }
        if self.prox.watched == Some(id) {
            self.prox.watched = None;
        }
    }

    /// (24) `GetWatchedRawBlock`.
    fn target(&self) -> Option<u32> {
        self.watched_live()
    }

    /// (26) `GetWatchedRawBlock`.
    fn dock_target(&self) -> Option<u32> {
        self.watched_live()
    }

    /// `OnResult(RequestType, Response, Arg3)`: the server's answer to the
    /// dock or lane request.
    fn on_request_result(
        &mut self,
        cx: &mut Cx<'_, W>,
        kind: i32,
        response: i32,
        arg: i32,
    ) -> bool {
        match kind {
            0 => {
                if response == 3 {
                    return true;
                }
                self.field_43c = arg;
                if self.watched_view(cx).is_none() || arg == -1 {
                    self.enter_state(cx, 0xd);
                    self.flag_39a = 0;
                    return false;
                }
                self.refresh_approach_state(cx);
                if self.type_flags & 0x840 != 0 {
                    self.enter_state(cx, 6);
                } else {
                    self.enter_state(cx, 2);
                }
                false
            }
            2 => {
                if response == 4 {
                    if arg == -1 {
                        self.enter_state(cx, 0xd);
                        self.flag_39a = 0;
                        return false;
                    }
                    self.field_408 = 16.0;
                    self.enter_state(cx, 6);
                    return false;
                }
                if response == 1 {
                    self.enter_state(cx, 0xd);
                    self.flag_39a = 0;
                }
                false
            }
            _ => false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn look_matrix_yup_looks_along_the_direction() {
        let m = look_matrix_yup(Vector::new(0.0, 0.0, -2.0));
        let forward = column2(&m);
        assert_eq!((forward.x, forward.y, forward.z), (0.0, 0.0, 1.0));
        let up = column1(&m);
        assert!((up.y - 1.0).abs() < 1e-6);
        // A vertical direction falls back to the +Z reference.
        let v = look_matrix_yup(Vector::new(0.0, 5.0, 0.0));
        assert_eq!(column2(&v).y, -1.0);
    }

    #[test]
    fn terminal_types_and_clearance_states() {
        assert!(takes_ship_in(3) && takes_ship_in(5));
        assert!(!takes_ship_in(1) && !takes_ship_in(6));
        let mut d = DockBehavior::new(ids::DOCK);
        assert!(<DockBehavior as Behavior<crate::object::World>>::docking_probe(&d));
        d.mode = 8;
        assert!(!<DockBehavior as Behavior<crate::object::World>>::docking_probe(&d));
    }
}
