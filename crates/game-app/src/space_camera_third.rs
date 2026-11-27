//! The chase camera (`space_camera_third.cpp`, the HUD camera's shake and
//! `CameraOrientFromForwardAndUp` of `space_camera_game.cpp`, and the ship
//! ini's camera keys of `ship_camera_params.cpp`; work package P5.4b).
//!
//! - [`ShipCameraTable`]: the camera block of each ship archetype's client
//!   companion (`ICliObj` +0x68 to +0x87), read from the `[Data] ships`
//!   files. game-sim keeps no companions (D117), so the client reads the
//!   keys it needs itself.
//! - [`ThirdPersonCamera`]: `FThirdPersonCamera`'s own state, with its
//!   attach (slot 0x58), update (slot 0x6c), the level toggle (event 0x3f)
//!   and the chase solver. The projection and the watched object are the
//!   [`GameCamera`] of [`SpaceCameraMode::ThirdPerson`].
//! - [`CameraShake`]: `G_CameraShakeAmount` and its decay, applied to the
//!   orientation the solver produces (`HudCameraApplyShake`). The random
//!   draws take the caller's generator (as D513).
//!
//! In single player the watched ship is the shared world's object (record
//! 0002); the behaviour manager the solver levels through is the one
//! `GetBehaviorManager` answers, made on first use as for the flight
//! commands. The client ship game-object's state (+0x158, the jump) and
//! its hit flags (+0x18c) are not ported yet: they read as zero, so the
//! jump's up override and the hit shake do not happen.
//!
//! [`SpaceCameraMode::ThirdPerson`]: crate::space_camera::SpaceCameraMode::ThirdPerson

// The negated comparisons keep the reference's NaN outcomes; the constants
// are the exe's floats, written out in full.
#![allow(clippy::neg_cmp_op_on_partial_ord, clippy::excessive_precision)]

use std::collections::HashMap;

use game_sim::ai::manager::ManagerState;
use game_sim::data::DataSource;
use game_sim::data::config::{DataConfig, DataKind};
use game_sim::object::{ObjHandle, ObjectKind, World};
use neuron_core::id::create_id;
use neuron_core::ini::IniReader;
use neuron_core::math::{
    Matrix, Vector, matrix_to_quaternion, mul_matrix, quat_slerp, quaternion_to_matrix,
    transform_matrix, transpose_transform,
};
use neuron_core::rng::CrtRand;

use crate::space_camera::GameCamera;

/// The source the camera's warnings name.
pub const COCKPIT_CPP: &str = "Client\\space\\Cockpit.cpp";

/// The degree-to-radian factor of the ship ini and the cockpit loader
/// (.rdata 0x005c77f4).
const DEGREES_TO_RADIANS: f32 = 0.017_453_292;

const MIN_DELTA_TIME: f32 = 9.999_999_747_378_752e-6;
const SLACK_ACCEL_SCALE: f32 = 0.019_999_999_552_965_164;
const SLACK_THRESHOLD: f64 = 0.01;
const SLACK_CAP_SCALE: f32 = 0.039_999_999_105_930_33;
const LEVEL_TURN_MIN_DOT: f32 = -0.990_000_009_536_743_2;
const LEVEL_TURN_MAX_DOT: f64 = 0.999_899_983_406_066_9;
const LEVEL_UP_AGREE_DOT: f64 = 0.995;
const LEVEL_AXIS_MAX_DOT: f64 = 0.9999;
const MIN_TURN_ANGLE: f64 = 0.0001;
const DOWN_TURN_YAW_SCALE: f32 = 0.800_000_011_920_929;

/// `G_CameraShakeDecay`.
pub const SHAKE_DECAY: f32 = 0.001;

fn e(v: f32) -> f64 {
    f64::from(v)
}

#[allow(clippy::cast_possible_truncation)]
fn r(v: f64) -> f32 {
    v as f32
}

fn col(m: &Matrix, c: usize) -> Vector {
    Vector::new(m.m[0][c], m.m[1][c], m.m[2][c])
}

fn set_col(m: &mut Matrix, c: usize, v: Vector) {
    m.m[0][c] = v.x;
    m.m[1][c] = v.y;
    m.m[2][c] = v.z;
}

/// `Vec3CrossStored`: `a x b`, each term on the FPU, stored.
fn cross_stored(a: Vector, b: Vector) -> Vector {
    let z = e(a.x) * e(b.y) - e(a.y) * e(b.x);
    let y = e(a.z) * e(b.x) - e(b.z) * e(a.x);
    let x = e(a.y) * e(b.z) - e(a.z) * e(b.y);
    Vector::new(r(x), r(y), r(z))
}

/// `Vec3NormaliseInPlace`: scaled by `1.0f / |v|`, unrounded.
fn normalise_stored(v: Vector) -> Vector {
    let sum = e(v.x) * e(v.x) + e(v.y) * e(v.y);
    let len = (sum + e(v.z) * e(v.z)).sqrt();
    let inv = 1.0 / len;
    Vector::new(r(inv * e(v.x)), r(inv * e(v.y)), r(inv * e(v.z)))
}

/// `Vec3DotZyx`: `(a.z b.z + a.y b.y) + a.x b.x`.
fn dot_zyx(a: Vector, b: Vector) -> f64 {
    (e(a.z) * e(b.z) + e(a.y) * e(b.y)) + e(a.x) * e(b.x)
}

/// The solver's pitch loop: columns 1 and 2 of each row.
fn rotate_pitch(m: &mut Matrix, angle: f64) {
    let c = e(r(angle.cos()));
    let s = angle.sin();
    for row in &mut m.m {
        let (b, z) = (e(row[1]), e(row[2]));
        row[1] = r(s * z + b * c);
        row[2] = r(c * z - b * s);
    }
}

/// The solver's yaw loop: columns 0 and 2 of each row.
fn rotate_yaw(m: &mut Matrix, angle: f64) {
    let c = e(r(angle.cos()));
    let s = angle.sin();
    for row in &mut m.m {
        let (a, z) = (e(row[0]), e(row[2]));
        row[0] = r(c * a - s * z);
        row[2] = r(c * z + a * s);
    }
}

/// `CameraOrientFromForwardAndUp`: `m`'s forward (column 2) kept, the
/// right made from `up x forward` (column 0 of `m` when the two are within
/// `acos 0.999`), the up from `forward x right`.
#[must_use]
pub fn orient_from_forward_and_up(m: &Matrix, up: Vector) -> Matrix {
    let (a2, a5, a8) = (e(m.m[0][2]), e(m.m[1][2]), m.m[2][2]);
    let a8d = e(a8);
    let dot = a2 * e(up.x) + a8d * e(up.z) + a5 * e(up.y);
    let (rx, ry, rz) = if dot.abs() < 0.999 {
        let x = a8d * e(up.y) - a5 * e(up.z);
        let y = a2 * e(up.z) - a8d * e(up.x);
        let z = a5 * e(up.x) - a2 * e(up.y);
        let zf = r(z);
        let sum = z * e(zf) + y * y + x * x;
        let inv = e(r(1.0 / sum.sqrt()));
        (r(x * inv), r(y * inv), r(e(zf) * inv))
    } else {
        (m.m[0][0], m.m[1][0], m.m[2][0])
    };
    let c0 = r(e(rz) * a5 - e(ry) * a8d);
    let c1 = r(a8d * e(rx) - e(rz) * a2);
    let c2 = r(e(ry) * a2 - e(rx) * a5);
    Matrix {
        m: [[rx, c0, m.m[0][2]], [ry, c1, m.m[1][2]], [rz, c2, a8]],
    }
}

/// `FShipCameraParams`: the camera block of a ship archetype's client
/// companion. Zero for a ship whose section sets none of the keys, as the
/// reference's zero-filled companion.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShipCameraParams {
    /// +0x68, +0x6c `camera_offset`.
    pub offset: [f32; 2],
    /// +0x70 `camera_angular_acceleration`.
    pub angular_acceleration: f32,
    /// +0x74 `camera_angular_slerp_multiplier`.
    pub angular_slerp_multiplier: f32,
    /// +0x78 `camera_horizontal_turn_angle`, radians.
    pub horizontal_turn_angle: f32,
    /// +0x7c `camera_vertical_turn_up_angle`, radians.
    pub vertical_turn_up_angle: f32,
    /// +0x80 `camera_vertical_turn_down_angle`, radians.
    pub vertical_turn_down_angle: f32,
    /// +0x84 `camera_turn_look_ahead_slerp_amount`.
    pub turn_look_ahead_slerp_amount: f32,
}

/// `ShipCameraClampTurnAngle`: degrees clamped to [0, 90] (a NaN kept),
/// then radians.
#[must_use]
pub fn clamp_turn_angle(degrees: f32) -> f32 {
    if degrees < 0.0 {
        return 0.0;
    }
    if degrees > 90.0 {
        return r(e(90.0) * e(DEGREES_TO_RADIANS));
    }
    r(e(degrees) * e(DEGREES_TO_RADIANS))
}

impl ShipCameraParams {
    /// `ShipCameraParamsReadValue`: true when the current value is one of
    /// the camera keys, in the reference's order.
    pub fn read_value(&mut self, reader: &mut IniReader) -> bool {
        if reader.is_value(b"camera_offset") {
            self.offset = [r(reader.value_float(0)), r(reader.value_float(1))];
        } else if reader.is_value(b"camera_angular_slerp_multiplier") {
            self.angular_slerp_multiplier = r(reader.value_float(0));
        } else if reader.is_value(b"camera_angular_acceleration") {
            self.angular_acceleration = r(reader.value_float(0));
        } else if reader.is_value(b"camera_horizontal_turn_angle") {
            self.horizontal_turn_angle = clamp_turn_angle(r(reader.value_float(0)));
        } else if reader.is_value(b"camera_vertical_turn_up_angle") {
            self.vertical_turn_up_angle = clamp_turn_angle(r(reader.value_float(0)));
        } else if reader.is_value(b"camera_vertical_turn_down_angle") {
            self.vertical_turn_down_angle = clamp_turn_angle(r(reader.value_float(0)));
        } else if reader.is_value(b"camera_turn_look_ahead_slerp_amount") {
            self.turn_look_ahead_slerp_amount = r(reader.value_float(0));
        } else {
            return false;
        }
        true
    }
}

/// The ship companions' camera blocks by ship archetype id.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ShipCameraTable {
    ships: HashMap<u32, ShipCameraParams>,
}

impl ShipCameraTable {
    /// The camera blocks of every `[Data] ships` file of `config`, in file
    /// order.
    #[must_use]
    pub fn from_config(source: &dyn DataSource, config: &DataConfig) -> Self {
        let mut table = Self::default();
        for entry in &config.entries {
            if DataKind::of(&entry.key) == Some(DataKind::Ships) {
                table.load_ships(source, &entry.value);
            }
        }
        table
    }

    /// One ships file: every `[Ship]` section's camera keys. A ship named
    /// again keeps its first block, as `LoadShips` drops the second record.
    /// Returns the sections added.
    pub fn load_ships(&mut self, source: &dyn DataSource, path: &[u8]) -> usize {
        let Some(mut reader) = game_sim::data::open_ini(source, path) else {
            return 0;
        };
        self.read(&mut reader)
    }

    /// The `[Ship]` sections of an open reader.
    pub fn read(&mut self, reader: &mut IniReader) -> usize {
        let mut added = 0;
        while reader.read_header() {
            if !reader.is_header(b"Ship") {
                continue;
            }
            let mut nickname = None;
            let mut params = ShipCameraParams::default();
            while reader.read_value() {
                if reader.is_value(b"nickname") {
                    nickname = Some(create_id(&reader.value_string_at(0)));
                } else {
                    params.read_value(reader);
                }
            }
            if let Some(id) = nickname
                && !self.ships.contains_key(&id)
            {
                self.ships.insert(id, params);
                added += 1;
            }
        }
        added
    }

    /// The companion's camera block of a ship archetype; `None` when the
    /// archetype has no companion.
    #[must_use]
    pub fn get(&self, archetype: u32) -> Option<&ShipCameraParams> {
        self.ships.get(&archetype)
    }

    /// The number of ships.
    #[must_use]
    pub fn len(&self) -> usize {
        self.ships.len()
    }

    /// No ships.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.ships.is_empty()
    }
}

/// What the watched ship's game object reports to the shake each frame
/// (`FClientShipGameobj` +0x18c bits 0 and 1, the jump effect's
/// amplitude).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ShakeHits {
    /// Bit 0: a hull hit.
    pub hull: bool,
    /// Bit 1: a shield hit.
    pub shield: bool,
    /// `FJumpShipEffect::ShakeAmplitude` while the ship jumps.
    pub jump: Option<f64>,
}

/// `G_CameraShakeAmount`: the HUD cameras' shake.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CameraShake {
    /// The amplitude, radians.
    pub amount: f32,
    /// `G_CameraShakeDecay`, per call.
    pub decay: f32,
}

impl Default for CameraShake {
    fn default() -> Self {
        Self {
            amount: 0.0,
            decay: SHAKE_DECAY,
        }
    }
}

impl CameraShake {
    /// `HudCameraUpdateShake`: the decay, then (with a watched object) the
    /// hits.
    pub fn update(&mut self, watched: bool, hits: ShakeHits) {
        if self.amount > 0.0 {
            let decayed = e(self.amount) - e(self.decay);
            self.amount = r(decayed);
            if decayed < e(9.999_999_747_378_752e-5) {
                self.amount = 0.0;
            }
        }
        if !watched {
            return;
        }
        if hits.hull {
            self.amount = 0.02;
        }
        if hits.shield && !(self.amount > 0.005) {
            self.amount = 0.005;
        }
        if let Some(jump) = hits.jump
            && !(e(self.amount) > jump)
        {
            self.amount = r(jump);
        }
    }

    fn draw(&self, rand: &mut CrtRand) -> (f64, f64) {
        let amp = e(self.amount);
        let neg = e(-self.amount);
        let angle = f64::from(rand.rand()) * e(3.051_757_812_5e-5) * (amp - neg) + neg;
        (e(r(angle.cos())), angle.sin())
    }

    /// `HudCameraApplyShake`: the update, then (a positive amplitude) two
    /// random turns of `m`, rows 0 and 2, then rows 0 and 1.
    pub fn apply(&mut self, m: &mut Matrix, watched: bool, hits: ShakeHits, rand: &mut CrtRand) {
        self.update(watched, hits);
        if !(self.amount > 0.0) {
            return;
        }
        let (c, s) = self.draw(rand);
        for k in 0..3 {
            let (a, z) = (e(m.m[0][k]), e(m.m[2][k]));
            m.m[0][k] = r(s * z + a * c);
            m.m[2][k] = r(c * z - a * s);
        }
        let (c, s) = self.draw(rand);
        for k in 0..3 {
            let (a, b) = (e(m.m[0][k]), e(m.m[1][k]));
            m.m[0][k] = r(c * a - s * b);
            m.m[1][k] = r(c * b + a * s);
        }
    }
}

/// What the solver reads of the watched ship, before it touches the
/// manager.
struct ShipView {
    rotation: Matrix,
    position: Vector,
    velocity: Vector,
    axis_throttle: Vector,
}

/// `FThirdPersonCamera` +0x100 to +0x214: the chase camera's own state.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ThirdPersonCamera {
    /// +0x100 the `HpCockpit` hardpoint's position (fallback `(0, 0, -4)`).
    pub hardpoint_pos: Vector,
    /// +0x10c its orientation (never read back).
    pub hardpoint_orient: Matrix,
    /// +0x130 the placement the solver produced.
    pub translation_cache: Vector,
    /// +0x13c the auto-level toggle (event 0x3f).
    pub level_enabled: bool,
    /// +0x140 the look offset `{0, camera_offset[0], camera_offset[1]}`.
    pub offset: Vector,
    /// +0x14c the second offset (zeroed on every attach).
    pub offset2: Vector,
    /// +0x158 the orientation as of the last solve.
    pub reset_orient: Matrix,
    /// +0x17c the target's orientation, second copy.
    pub target_orient_b: Matrix,
    /// +0x1a0 the level axis, in the ship's frame.
    pub level_axis: Vector,
    /// +0x1ac the last frame's turn rate (rad/s).
    pub prev_angular_rate: f32,
    /// +0x1b0 the target's orientation, first copy.
    pub target_orient_a: Matrix,
    /// +0x1d4.
    pub angular_acceleration: f32,
    /// +0x1d8.
    pub angular_slerp_multiplier: f32,
    /// +0x1dc radians.
    pub horizontal_turn_angle: f32,
    /// +0x1e0 radians.
    pub vertical_turn_up_angle: f32,
    /// +0x1e4 radians.
    pub vertical_turn_down_angle: f32,
    /// +0x1e8.
    pub turn_look_ahead_slerp_amount: f32,
    /// +0x1ec the last frame's speed.
    pub prev_speed: f32,
    /// +0x1f0.
    pub look_ahead_multiplier: f32,
    /// +0x1f4.
    pub acceleration_limit: f32,
    /// +0x1f8 the speed the camera follows.
    pub smoothed_speed: f32,
    /// +0x1fc the integrated lag, never negative.
    pub speed_slack: f32,
    /// +0x200.
    pub turn_rate_scale: f32,
    /// +0x204 the up the camera levels towards.
    pub up_axis: Vector,
    /// +0x214 set by the attach; the next update skips its solve.
    pub initialised: bool,
}

impl Default for ThirdPersonCamera {
    /// `ThirdPersonCameraConstruct`.
    fn default() -> Self {
        Self {
            hardpoint_pos: Vector::ZERO,
            hardpoint_orient: Matrix::IDENTITY,
            translation_cache: Vector::ZERO,
            level_enabled: true,
            offset: Vector::new(0.0, 7.0, 60.0),
            offset2: Vector::ZERO,
            reset_orient: Matrix::IDENTITY,
            target_orient_b: Matrix { m: [[0.0; 3]; 3] },
            level_axis: Vector::new(0.0, 0.0, -1.0),
            prev_angular_rate: 0.0,
            target_orient_a: Matrix { m: [[0.0; 3]; 3] },
            angular_acceleration: 0.049_087_385,
            angular_slerp_multiplier: 2.0,
            horizontal_turn_angle: 0.2,
            vertical_turn_up_angle: 0.1,
            vertical_turn_down_angle: 0.2,
            turn_look_ahead_slerp_amount: 0.5,
            prev_speed: 0.0,
            look_ahead_multiplier: 0.05,
            acceleration_limit: 40.0,
            smoothed_speed: 0.0,
            speed_slack: 0.0,
            turn_rate_scale: 2.0,
            up_axis: Vector::new(0.0, 1.0, 0.0),
            initialised: false,
        }
    }
}

/// The world, the generator and the shake the chase camera works with.
pub struct ChaseCx<'a> {
    /// The shared world.
    pub world: &'a mut World,
    /// The ship companions' camera blocks.
    pub ships: &'a ShipCameraTable,
    /// The HUD cameras' shake.
    pub shake: &'a mut CameraShake,
    /// The client's `rand`.
    pub rand: &'a mut CrtRand,
    /// What the watched ship reports to the shake.
    pub hits: ShakeHits,
}

/// What the chase camera reported (the reference's `FDUMP`s).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ChaseWarning {
    /// Cockpit.cpp 0x3c9: the attach found no object.
    NoObjectLinked,
    /// Cockpit.cpp 0x3e9: the update found no ship to follow.
    NoViewerTransform,
}

impl ChaseWarning {
    /// The line and text of the report.
    #[must_use]
    pub fn report(self) -> (u32, &'static str) {
        match self {
            Self::NoObjectLinked => (0x3c9, "*** ERROR: no object linked to third person camera"),
            Self::NoViewerTransform => (
                0x3e9,
                "*** WARNING: third person camera unable to calculate viewer transform, missin linked Ship*",
            ),
        }
    }
}

/// The watched object when the world still has it.
fn watched_object(cam: &GameCamera, world: &World) -> Option<ObjHandle> {
    cam.watched.filter(|h| world.get(*h).is_some())
}

/// The watched object when it is a ship (class 0x503).
fn watched_ship(cam: &GameCamera, world: &World) -> Option<ObjHandle> {
    watched_object(cam, world).filter(|h| world.get(*h).is_some_and(|o| o.kind == ObjectKind::Ship))
}

impl ThirdPersonCamera {
    /// `ThirdPersonCameraAttach` (slot 0x58): HUDCamera's attach (the HUD's
    /// retarget is its owner's), the companion's camera block, the
    /// target's orientation and frame, the `HpCockpit` hardpoint, then a
    /// reset solve (`dt` 0) into the camera. With nothing watched it only
    /// warns.
    pub fn attach(
        &mut self,
        cam: &mut GameCamera,
        cx: &mut ChaseCx<'_>,
        object: Option<ObjHandle>,
    ) -> Result<(), ChaseWarning> {
        cam.attach(object);
        self.offset2 = Vector::ZERO;
        let Some(h) = watched_object(cam, cx.world) else {
            self.initialised = true;
            return Err(ChaseWarning::NoObjectLinked);
        };
        if watched_ship(cam, cx.world).is_some() {
            let archetype = cx.world.get(h).map_or(0, |o| o.archetype);
            if let Some(p) = cx.ships.get(archetype) {
                self.angular_slerp_multiplier = p.angular_slerp_multiplier;
                self.angular_acceleration = p.angular_acceleration;
                self.horizontal_turn_angle = p.horizontal_turn_angle;
                self.vertical_turn_up_angle = p.vertical_turn_up_angle;
                self.vertical_turn_down_angle = p.vertical_turn_down_angle;
                self.turn_look_ahead_slerp_amount = p.turn_look_ahead_slerp_amount;
                self.offset = Vector::new(0.0, p.offset[0], p.offset[1]);
            }
        }
        let Some(o) = cx.world.get(h) else {
            return Err(ChaseWarning::NoObjectLinked);
        };
        let (rotation, position, instance) =
            (o.engine.orientation, o.engine.position, o.engine.instance);
        self.target_orient_a = rotation;
        self.target_orient_b = rotation;
        self.up_axis = col(&rotation, 1);
        cam.camera.set_world(rotation, position);
        match cx.world.find_hardpoint_ps(instance, b"HpCockpit") {
            Some((_, frame)) => {
                self.hardpoint_pos = frame.translation;
                self.hardpoint_orient = frame.rotation;
            }
            None => {
                self.hardpoint_pos = Vector::new(0.0, 0.0, -4.0);
                self.hardpoint_orient = Matrix::IDENTITY;
            }
        }
        // The reference copies the view whatever the solve answers: the
        // identity at the origin when the object is no ship.
        let (m, p) = self
            .solve(cam, cx, 0.0, true)
            .unwrap_or((Matrix::IDENTITY, Vector::ZERO));
        cam.camera.set_world(m, p);
        self.translation_cache = p;
        self.initialised = true;
        Ok(())
    }

    /// `ThirdPersonCameraUpdate` (slot 0x6c): unless the attach just ran,
    /// the solve into the camera; then HUDCamera's update (the part ported
    /// here is the base's velocity) and the attach latch cleared.
    pub fn update(
        &mut self,
        cam: &mut GameCamera,
        cx: &mut ChaseCx<'_>,
        dt: f32,
    ) -> Result<(), ChaseWarning> {
        let mut result = Ok(());
        if !self.initialised {
            match self.solve(cam, cx, dt, false) {
                Some((m, p)) => cam.camera.set_world(m, p),
                None => result = Err(ChaseWarning::NoViewerTransform),
            }
        }
        cam.update(dt);
        self.initialised = false;
        result
    }

    /// `ThirdPersonCameraOnEvent`'s event 0x3f: the auto-level toggle,
    /// handed to the watched object's behaviour manager
    /// (`update_level_camera`).
    pub fn toggle_level(&mut self, cam: &GameCamera, world: &mut World) {
        self.level_enabled = !self.level_enabled;
        let flag = self.level_enabled;
        if let Some(h) = watched_object(cam, world) {
            let _ = world.with_behavior_manager(h, |m, _| m.state.level_camera = flag);
        }
    }

    /// `ThirdPersonCameraSolve`: the chase camera's frame, `None` when the
    /// watched object is no ship. `reset` seeds the speed tracker, the
    /// level axis and the orientation history from the ship.
    pub fn solve(
        &mut self,
        cam: &GameCamera,
        cx: &mut ChaseCx<'_>,
        dt: f32,
        reset: bool,
    ) -> Option<(Matrix, Vector)> {
        let h = watched_ship(cam, cx.world)?;
        let o = cx.world.get(h)?;
        let inspect = cx.world.inspect(h)?;
        let view = ShipView {
            rotation: o.engine.orientation,
            position: o.engine.position,
            velocity: inspect.velocity,
            axis_throttle: inspect.ship.map_or(Vector::ZERO, |s| s.axis_throttle),
        };
        // The reference's normalised velocity and heading for a speed over
        // 1 are overwritten on every path before they are read.
        let mut out = None;
        let _ = cx.world.with_behavior_manager(h, |m, _| {
            out = Some(self.solve_with(&view, Some(&mut m.state), dt, reset));
        });
        let (mut m, p) = match out {
            Some(v) => v,
            None => self.solve_with(&view, None, dt, reset),
        };
        cx.shake.apply(&mut m, true, cx.hits, cx.rand);
        Some((m, p))
    }

    #[allow(clippy::too_many_lines)]
    fn solve_with(
        &mut self,
        ship: &ShipView,
        mut manager: Option<&mut ManagerState>,
        dt: f32,
        reset: bool,
    ) -> (Matrix, Vector) {
        let ship_rot = ship.rotation;
        let mut m = ship_rot;
        let mut pos = ship.position;
        let v = ship.velocity;
        let speed = r(((e(v.x) * e(v.x) + e(v.y) * e(v.y)) + e(v.z) * e(v.z)).sqrt());

        if reset {
            self.smoothed_speed = speed;
            self.prev_speed = speed;
            self.speed_slack = 0.0;
            self.level_axis = Vector::new(0.0, 0.0, -1.0);
            self.reset_orient = ship_rot;
            self.up_axis = col(&self.reset_orient, 1);
            if let Some(state) = manager.as_deref_mut() {
                state.camera_level_status = 1;
                state.camera_up = col(&self.reset_orient, 1);
            }
        }

        // The speed tracker.
        let look_ahead_base = e(speed) * e(self.look_ahead_multiplier);
        let accel = if dt > MIN_DELTA_TIME {
            (e(speed) - e(self.prev_speed)) / e(dt)
        } else {
            0.0
        };
        let slack_sum = (e(speed) - e(self.smoothed_speed)) * e(dt) + e(self.speed_slack);
        self.speed_slack = r(slack_sum);
        if slack_sum < 0.0 {
            self.speed_slack = 0.0;
        }
        let slack = e(self.speed_slack);
        let limit = r((slack * slack * e(SLACK_ACCEL_SCALE) + 1.0) * e(self.acceleration_limit));
        let ramp = if slack > SLACK_THRESHOLD {
            true
        } else {
            accel > e(limit)
        };
        if ramp {
            let ramped = e(limit) * e(dt) + e(self.smoothed_speed);
            self.smoothed_speed = r(ramped);
            let k = e(self.speed_slack) * e(SLACK_CAP_SCALE);
            let cap = r(k * k * e(speed) + e(speed));
            if ramped > e(cap) {
                self.smoothed_speed = cap;
            }
        } else {
            self.smoothed_speed = speed;
        }
        let look_ahead = r(look_ahead_base + e(self.speed_slack));

        // The camera offset: hardpoint + ((offset + offset2) + (0, 0, look-ahead)).
        let add = |a: Vector, b: Vector| {
            Vector::new(r(e(a.x) + e(b.x)), r(e(a.y) + e(b.y)), r(e(a.z) + e(b.z)))
        };
        let va8 = add(
            add(self.offset, self.offset2),
            Vector::new(0.0, 0.0, look_ahead),
        );
        let v78 = add(va8, self.hardpoint_pos);

        // Auto-level.
        if let Some(state) = manager
            && self.level_enabled
            && state.camera_level_status == 1
            && state.level_camera
        {
            if state.user_turning {
                state.camera_up = col(&self.reset_orient, 1);
                let qa8 = matrix_to_quaternion(&orient_from_forward_and_up(&m, self.up_axis));
                let qdc = matrix_to_quaternion(&orient_from_forward_and_up(
                    &m,
                    col(&self.reset_orient, 1),
                ));
                let t = r(e(dt) * e(self.turn_rate_scale));
                let mec = quaternion_to_matrix(&quat_slerp(&qa8, &qdc, t));
                self.up_axis = col(&mec, 1);
                m = mec;
            } else {
                // The jump states (game object +0x158 2 or 3) force the
                // ship's up to (0, 1, 0); that state is not ported.
                let mut v14 = state.ship_up;
                let mut mb8 = orient_from_forward_and_up(&m, v14);
                let mut dot = r(dot_zyx(v14, self.up_axis));
                let up_agree = r((e(mb8.m[2][1]) * e(ship_rot.m[2][1])
                    + e(mb8.m[1][1]) * e(ship_rot.m[1][1]))
                    + e(mb8.m[0][1]) * e(ship_rot.m[0][1]));
                if dot > LEVEL_TURN_MIN_DOT {
                    if e(dot).abs() < LEVEL_TURN_MAX_DOT {
                        let m110 = orient_from_forward_and_up(&m, self.up_axis);
                        let qdc = matrix_to_quaternion(&mb8);
                        let q24 = matrix_to_quaternion(&m110);
                        if dot < 0.0 {
                            dot = 0.0;
                        }
                        let t = r((e(dot) + 1.0) * e(self.turn_rate_scale) * e(dt) * 0.5);
                        let mec = quaternion_to_matrix(&quat_slerp(&q24, &qdc, t));
                        v14 = col(&mec, 1);
                        mb8 = mec;
                    }
                    self.up_axis = v14;
                    m = mb8;
                    state.camera_up = col(&mb8, 1);
                } else if e(up_agree) > LEVEL_UP_AGREE_DOT {
                    self.up_axis = v14;
                }
            }
        }

        if reset {
            self.reset_orient = m;
            let off = transform_matrix(&m, v78);
            pos = add(off, pos);
        } else {
            let q154 = matrix_to_quaternion(&m);
            let local = transpose_transform(&m, ship.axis_throttle);
            let mut mb8 = Matrix::IDENTITY;
            if local.x > 0.0 {
                rotate_pitch(&mut mb8, -(e(local.x) * e(self.vertical_turn_up_angle)));
                rotate_yaw(&mut mb8, -(e(local.y) * e(self.horizontal_turn_angle)));
            } else {
                rotate_pitch(&mut mb8, -(e(local.x) * e(self.vertical_turn_down_angle)));
                let scale = e(local.x).abs() * e(DOWN_TURN_YAW_SCALE) + 1.0;
                rotate_yaw(
                    &mut mb8,
                    scale * -(e(local.y) * e(self.horizontal_turn_angle)),
                );
            }
            m = mul_matrix(&m, &mb8);

            let v14 = transform_matrix(&ship_rot, self.level_axis);
            let v24 = col(&m, 1);
            let axis_dot = dot_zyx(v24, v14);
            let mec = if axis_dot.abs() < LEVEL_AXIS_MAX_DOT {
                let right = normalise_stored(cross_stored(v14, v24));
                let up = normalise_stored(cross_stored(right, v14));
                let mut t = Matrix { m: [[0.0; 3]; 3] };
                set_col(&mut t, 0, right);
                set_col(&mut t, 1, up);
                set_col(&mut t, 2, Vector::new(-v14.x, -v14.y, -v14.z));
                t
            } else {
                m
            };
            let q164 = matrix_to_quaternion(&mec);
            let q134 = matrix_to_quaternion(&m);

            let b3c = col(&m, 2);
            let b24 = col(&mec, 2);
            let cos_sum = (e(b3c.z) * e(b24.z) + e(b3c.y) * e(b24.y)) + e(b24.x) * e(b3c.x);
            let mut cos = r(cos_sum);
            if cos_sum < 0.0 {
                cos = 0.0;
            } else if e(cos) > 1.0 {
                cos = 1.0;
            }
            let angle = r(e(cos).acos());

            let inv_dt = 1.0 / e(dt);
            let inv_dt_stored = r(inv_dt);
            let mut rate = inv_dt * e(angle);
            if rate - e(self.prev_angular_rate) > e(self.angular_acceleration) {
                rate = e(self.angular_acceleration) + e(self.prev_angular_rate);
            }
            let mut fraction = 1.0f32;
            if e(angle) > MIN_TURN_ANGLE {
                fraction = r(e(dt) / e(angle) * rate);
            }
            if fraction > self.angular_slerp_multiplier {
                fraction = self.angular_slerp_multiplier;
                rate = e(inv_dt_stored) * e(fraction) * e(angle);
            }
            self.prev_angular_rate = r(rate);

            m = quaternion_to_matrix(&quat_slerp(&q164, &q134, fraction));
            let off = transform_matrix(&m, v78);
            pos = add(off, pos);

            let back = transpose_transform(&ship_rot, col(&m, 2));
            self.level_axis = Vector::new(-back.x, -back.y, -back.z);

            self.reset_orient = m;
            let q144 = matrix_to_quaternion(&m);
            m = quaternion_to_matrix(&quat_slerp(&q144, &q154, self.turn_look_ahead_slerp_amount));
        }

        self.translation_cache = pos;
        self.prev_speed = speed;
        (m, pos)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_orientation_keeps_forward_and_levels_to_up() {
        let m = Matrix::IDENTITY;
        assert_eq!(
            orient_from_forward_and_up(&m, Vector::new(0.0, 1.0, 0.0)),
            Matrix::IDENTITY
        );
        // Up along forward: column 0 of m is the right.
        let o = orient_from_forward_and_up(&m, Vector::new(0.0, 0.0, 1.0));
        assert_eq!(col(&o, 0), Vector::new(1.0, 0.0, 0.0));
        assert_eq!(col(&o, 2), Vector::new(0.0, 0.0, 1.0));
    }

    #[test]
    fn turn_angles_clamp_in_degrees() {
        assert_eq!(clamp_turn_angle(-5.0), 0.0);
        assert_eq!(clamp_turn_angle(120.0), clamp_turn_angle(90.0));
        assert!((clamp_turn_angle(30.0) - core::f32::consts::FRAC_PI_6).abs() < 1e-6);
        assert!(clamp_turn_angle(f32::NAN).is_nan());
    }

    #[test]
    fn the_shake_decays_and_takes_hits() {
        let mut shake = CameraShake::default();
        let mut rand = CrtRand::default();
        let mut m = Matrix::IDENTITY;
        shake.apply(&mut m, true, ShakeHits::default(), &mut rand);
        assert_eq!(m, Matrix::IDENTITY);
        let hull = ShakeHits {
            hull: true,
            ..ShakeHits::default()
        };
        shake.apply(&mut m, true, hull, &mut rand);
        assert_eq!(shake.amount, 0.02);
        assert_ne!(m, Matrix::IDENTITY);
        // The shield hit raises a smaller amplitude only.
        let shield = ShakeHits {
            shield: true,
            ..ShakeHits::default()
        };
        shake.update(true, shield);
        assert!(shake.amount > 0.018);
        shake.amount = 0.001;
        shake.update(true, shield);
        assert_eq!(shake.amount, 0.005);
        // Without a watched object only the decay runs.
        shake.amount = 0.00105;
        shake.update(false, hull);
        assert_eq!(shake.amount, 0.0);
    }

    #[test]
    fn the_camera_keys_read_as_the_companion() {
        let text = b"[Ship]\nnickname = a\ncamera_offset = 6, 22\n\
            camera_angular_acceleration = 0.05\ncamera_horizontal_turn_angle = 23\n\
            camera_vertical_turn_down_angle = 100\n\
            camera_turn_look_ahead_slerp_amount = 1\n[Ship]\nnickname = b\n\
            [Ship]\nnickname = a\ncamera_offset = 1, 1\n";
        let mut reader = IniReader::new();
        assert!(reader.open_memory(text));
        let mut table = ShipCameraTable::default();
        let added = table.read(&mut reader);
        assert_eq!(added, 2);
        let a = table.get(create_id(b"a")).expect("a");
        assert_eq!(a.offset, [6.0, 22.0]);
        assert_eq!(a.angular_acceleration, 0.05);
        assert_eq!(a.angular_slerp_multiplier, 0.0);
        assert_eq!(a.horizontal_turn_angle, clamp_turn_angle(23.0));
        assert_eq!(a.vertical_turn_down_angle, clamp_turn_angle(90.0));
        assert_eq!(
            table.get(create_id(b"b")),
            Some(&ShipCameraParams::default())
        );
    }
}
