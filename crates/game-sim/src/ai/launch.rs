//! The launch behaviour (`LaunchBehavior`, behaviour 8,
//! `Common/common_launchbehavior.cpp`, `include/common_launchbehavior.h`):
//! a ship placed at a base's dock leaves it along the docking sphere's
//! points, the reverse of the dock behaviour's last states.
//!
//! The states of `ModeState164`: 1 a 3.5 s wait, 2 the dock points read
//! (`get_dock_hardpoints`) and the ship's `hpmount` offset, 3 the launch
//! sequence chosen, 4 out to the first dock point (skipped when the three
//! points line up), 5 out to the second, 6 a turn about for a terminal that
//! launches backwards. The whole launch gives up after 15 s for a player,
//! 60 s for an NPC.

use neuron_core::math::{Matrix, Transform, Vector, transpose_transform};

use super::behavior::{AiEnv, Behavior, BehaviorBase, Cx, DirectiveOp, ids};
use super::face_idea::FaceIdea;
use super::fp::{add, column2, e, normalize3, r, sub};
use super::gun_groups::GunGroups;
use super::move_idea::MoveIdea;
use super::raycast::clamped_dot3;

/// `float` π (`0x40490fdb`), widened: the angle `RotateBasisRowsXZ` turns
/// by.
const PI_D: f64 = 3.141_592_741_012_573_2;

/// `RotateBasisRowsXZ(Rows, Angle)`: each row's x and z rotated by the
/// angle about y (the x87 `FSIN` and `FCOS` of the angle; the port's
/// `f64::sin` and `f64::cos`, D187).
fn rotate_basis_rows_xz(m: &Matrix, angle: f64) -> Matrix {
    let (sin, cos) = angle.sin_cos();
    let mut rows = m.m;
    for row in &mut rows {
        let x = e(row[0]);
        let z = e(row[2]);
        row[0] = r(x * cos - sin * z);
        row[2] = r(cos * z + x * sin);
    }
    Matrix::from_rows(rows)
}

/// `LaunchBehavior` (0x234 bytes), field by field.
#[derive(Clone, Debug, PartialEq)]
pub struct LaunchBehavior {
    base: BehaviorBase,
    /// +0x2c the move idea (`SubObjectA`).
    pub move_idea: MoveIdea,
    /// +0xa4 the face idea (`SubObjectB`).
    pub face: FaceIdea,
    /// +0x164 the state (`ModeState164`).
    pub mode: i32,
    /// +0x168 the base launched from (`Watcher168`).
    pub watched: Option<u32>,
    /// +0x170 the system at engage time.
    pub system: u32,
    /// +0x174 the base's type flags.
    pub target_type: u32,
    /// +0x178 the docking sphere (-1 wait, -2 give up).
    pub index: i32,
    /// +0x17c the three dock points line up (state 4 is skipped).
    pub straight: u8,
    /// +0x180 the wait of state 1 (3.5 s).
    pub wait: f32,
    /// +0x184 the time waited.
    pub waited: f32,
    /// +0x188 the time since engage.
    pub elapsed: f32,
    /// +0x18c the time limit (15 s for a player, 60 s for an NPC).
    pub limit: f32,
    /// +0x190 the ship's `hpmount` in its own frame.
    pub mount_offset: Vector,
    /// +0x19c the second dock point.
    pub point2: Vector,
    /// +0x1a8 its orientation (`LaunchBasis`, rows).
    pub launch_basis: Matrix,
    /// +0x1cc the first dock point.
    pub point1: Vector,
    /// +0x1d8 its orientation (`ApproachBasis2`).
    pub basis2: Matrix,
    /// +0x1fc the mount.
    pub mount: Vector,
    /// +0x208 its orientation (`ApproachBasis3`).
    pub basis3: Matrix,
    /// +0x22c the throttle cap of state 4 (15).
    pub throttle_22c: f32,
    /// +0x230 a berth: half throttle once out.
    pub berth: u8,
    /// +0x231 a terminal that launches backwards (types 2 to 5).
    pub turn_about: u8,
}

fn zero_matrix() -> Matrix {
    Matrix::from_rows([[0.0; 3]; 3])
}

impl LaunchBehavior {
    /// `LaunchBehavior::LaunchBehavior(TypeId)`.
    #[must_use]
    pub fn new(id: i32) -> Self {
        Self {
            base: BehaviorBase::new(id),
            move_idea: MoveIdea::new(),
            face: FaceIdea::new(),
            mode: 0,
            watched: None,
            system: 0,
            target_type: 0,
            index: 0,
            straight: 0,
            wait: 3.5,
            waited: 0.0,
            elapsed: 0.0,
            limit: 0.0,
            mount_offset: Vector::ZERO,
            point2: Vector::ZERO,
            launch_basis: zero_matrix(),
            point1: Vector::ZERO,
            basis2: zero_matrix(),
            mount: Vector::ZERO,
            basis3: zero_matrix(),
            throttle_22c: 15.0,
            berth: 0,
            turn_about: 0,
        }
    }

    /// `FetchSystemId`: the ship's system.
    fn system_id<W: AiEnv + ?Sized>(cx: &Cx<'_, W>) -> u32 {
        cx.world.system_of(cx.ship).unwrap_or(0)
    }

    /// The base's three dock transforms of the sphere (`get_dock_hardpoints`);
    /// an output the call did not write is zero (D178).
    fn read_points<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) -> Option<i32> {
        let h = self.watched.and_then(|id| cx.world.handle_of(id))?;
        let answer = cx.world.dock_hardpoints(h, self.index);
        let zero = Transform::new(zero_matrix(), Vector::ZERO);
        let a = answer.mount.unwrap_or(zero);
        let b = answer.point1.unwrap_or(zero);
        let c = answer.point2.unwrap_or(zero);
        self.mount = a.translation;
        self.basis3 = a.rotation;
        self.point1 = b.translation;
        self.basis2 = b.rotation;
        self.point2 = c.translation;
        self.launch_basis = c.rotation;
        Some(answer.terminal.unwrap_or(0))
    }

    /// `ComputeLaunchApproachTransform`: the dock points and the ship's
    /// `hpmount` offset for the move idea.
    fn compute_launch_approach_transform<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) {
        let _ = self.read_points(cx);
        let Some((mount, _)) = cx.world.hardpoint(cx.ship, b"hpmount") else {
            return;
        };
        let Some(own) = cx.own() else {
            return;
        };
        let delta = sub(mount, own.center);
        self.mount_offset = transpose_transform(&own.orientation, delta);
        self.move_idea.offset_4c = self.mount_offset;
    }

    /// `StartLaunchSequence`: whether the points line up, and the
    /// terminal's way out.
    fn start_launch_sequence<W: AiEnv + ?Sized>(&mut self, cx: &mut Cx<'_, W>) -> bool {
        if cx.view_of(self.watched).is_none() {
            return false;
        }
        let terminal = self.read_points(cx).unwrap_or(0);
        let dir1 = normalize3(sub(self.point1, self.mount));
        let dir2 = normalize3(sub(self.point2, self.point1));
        let dot1 = r(clamped_dot3(dir1, dir2));
        let basis2_z = column2(&self.basis2);
        let dot2 = r(clamped_dot3(basis2_z, dir1));
        let launch_z = column2(&self.launch_basis);
        let basis3_z = column2(&self.basis3);
        let sum = add(add(basis3_z, basis2_z), launch_z);
        let dir3 = normalize3(sum);
        let dot3 = clamped_dot3(dir1, dir3);
        self.straight = u8::from(e(dot1) > 0.999 && e(dot2) > 0.99 && dot3 > 0.9999);
        match terminal {
            1 | 6..=8 => {
                if terminal == 1 {
                    self.berth = 1;
                }
                self.basis3 = rotate_basis_rows_xz(&self.basis3, PI_D);
                self.basis2 = rotate_basis_rows_xz(&self.basis2, PI_D);
                self.launch_basis = rotate_basis_rows_xz(&self.launch_basis, PI_D);
            }
            2..=5 => {
                self.straight = 0;
                self.turn_about = 1;
            }
            _ => {}
        }
        true
    }

    /// The end of the launch: done, flag bits and result 2.
    fn finish(&mut self) -> i32 {
        self.base.flags = (self.base.flags & !0x2) | 0x1;
        self.base.result = 2;
        2
    }
}

impl<W: AiEnv + ?Sized> Behavior<W> for LaunchBehavior {
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

    /// The world half of `SetContext`: the ideas bound, the time limit.
    fn attach(&mut self, cx: &mut Cx<'_, W>) {
        if !self.base.bound {
            return;
        }
        self.move_idea.set_context(&*cx);
        self.move_idea.set_mode(cx, 0);
        self.face.set_context(&*cx);
        let player = cx.own().is_some_and(|v| v.is_player);
        self.limit = if player { 15.0 } else { 60.0 };
    }

    /// (3) `Engage`.
    fn engage(&mut self, cx: &mut Cx<'_, W>) {
        self.base.engage();
        self.system = Self::system_id(cx);
        self.target_type = cx.view_of(self.watched).map_or(0, |v| v.type_flags);
        GunGroups::set_shield_active(&mut *cx.world, cx.ship, false);
        self.mode = 1;
        self.straight = 0;
        self.waited = 0.0;
        self.elapsed = 0.0;
        cx.set_cruise_lock(0, 3);
    }

    /// (4) `Disengage`.
    fn disengage(&mut self, cx: &mut Cx<'_, W>) {
        self.move_idea.commit(cx);
        self.face.commit(cx);
        self.base.disengage(cx.state);
        GunGroups::set_shield_active(&mut *cx.world, cx.ship, true);
        cx.set_cruise_lock(1, 3);
        self.base.flags = (self.base.flags & !0x2) | 0x1;
        cx.set_directive_result(ids::LAUNCH, 0);
    }

    /// (5) `SetModeFromFlags`.
    fn poll(&mut self, _cx: &mut Cx<'_, W>, _timer_c: f32) -> i32 {
        let mut mode = if self.base.flags & 0x2 != 0 { 0 } else { 2 };
        if self.base.result == 2 || self.base.result == 3 {
            mode = 2;
        }
        if self.base.flag29 != 0 {
            self.base.mode = 3;
            return 3;
        }
        self.base.mode = mode;
        mode
    }

    /// (7) `Update`.
    fn update(&mut self, cx: &mut Cx<'_, W>) -> i32 {
        if !self.base.bound || cx.view_of(self.watched).is_none() {
            self.base.result = 3;
            return 3;
        }
        if self.system != Self::system_id(cx) {
            self.base.result = 2;
            return 2;
        }
        if self.elapsed > self.limit {
            self.base.result = 3;
            return 3;
        }
        let dt = self.base.tick_delta;
        self.elapsed = r(e(self.elapsed) + e(dt));
        self.base.result = 0;
        match self.mode {
            1 => {
                if self.waited > self.wait {
                    self.mode = 2;
                }
                self.waited = r(e(self.waited) + e(dt));
                0
            }
            2 | 3 => {
                if self.mode == 2 {
                    self.compute_launch_approach_transform(cx);
                    self.mode = 3;
                }
                match self.index {
                    i if i >= 0 => {
                        self.start_launch_sequence(cx);
                        self.mode = 4;
                        0
                    }
                    -1 => {
                        self.mode = 2;
                        0
                    }
                    -2 => {
                        Behavior::<W>::disengage(self, cx);
                        self.base.result = 3;
                        3
                    }
                    _ => 0,
                }
            }
            4 => {
                if self.straight != 0 {
                    self.move_idea.set_range_3c(25.0);
                    self.move_idea.set_mode(cx, 0);
                    self.mode = 5;
                    return 0;
                }
                self.face.set_basis(&self.basis2);
                let _ = self.face.update(cx);
                self.move_idea.set_position(cx, self.point1);
                self.move_idea.set_range_3c(5.0);
                self.move_idea.set_mode(cx, 1);
                self.move_idea.set_throttle_scale(&*cx, self.throttle_22c);
                self.move_idea.set_field_44(2);
                if self.move_idea.update(cx, dt) != 1 {
                    return 0;
                }
                self.move_idea.set_range_3c(15.0);
                self.mode = 5;
                0
            }
            5 => {
                self.face.set_basis(&self.launch_basis);
                let _ = self.face.update(cx);
                self.move_idea.set_position(cx, self.point2);
                self.move_idea.set_field_44(2);
                let tick = self.move_idea.update(cx, dt);
                let d1 = sub(self.point2, self.point1);
                let center = cx.own().map_or(Vector::ZERO, |v| v.center);
                let d2x = e(center.x) - e(self.point2.x);
                let d2y = e(center.y) - e(self.point2.y);
                let d2z = e(center.z) - e(self.point2.z);
                if tick != 1 {
                    let dot = (e(d1.z) * d2z + e(d1.y) * d2y) + e(d1.x) * d2x;
                    if !(dot > 0.0) {
                        return 0;
                    }
                }
                self.move_idea.commit(cx);
                GunGroups::set_shield_active(&mut *cx.world, cx.ship, true);
                if self.berth != 0 {
                    cx.set_throttle(0.5, 0);
                }
                if self.turn_about != 0 {
                    self.launch_basis = rotate_basis_rows_xz(&self.launch_basis, PI_D);
                    self.mode = 6;
                    return 0;
                }
                self.finish()
            }
            6 => {
                self.face.set_angle_weight_8c(f32::from_bits(0x3ec9_0fdb));
                self.face.set_basis(&self.launch_basis);
                if self.face.update(cx) != 1 {
                    return 0;
                }
                self.finish()
            }
            _ => 0,
        }
    }

    /// (10) `ApplyDirective`: the base and the docking sphere.
    fn accept_directive(&mut self, cx: &mut Cx<'_, W>, op: &DirectiveOp) -> bool {
        let (object, index) = match op {
            DirectiveOp::Launch(l) => (l.launch_object, l.index),
            _ => (0, 0),
        };
        self.watched = (object != 0)
            .then(|| cx.view_of(Some(object)).map(|v| v.id))
            .flatten();
        self.index = index;
        if self.watched.is_some() {
            self.base.flags |= 0x2;
            self.base.result = 1;
            return true;
        }
        self.base.flags &= !0x2;
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_half_turn_mirrors_x_and_z() {
        let m = Matrix::from_rows([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]);
        let t = rotate_basis_rows_xz(&m, PI_D);
        assert!((t.m[0][0] + 1.0).abs() < 1e-6);
        assert!((t.m[2][2] + 1.0).abs() < 1e-6);
        assert_eq!(t.m[1][1], 1.0);
    }
}
