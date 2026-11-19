//! A perspective camera behind the engine's camera interface
//! ([`Camera`]; work package P4.16, for Gate G4's model viewer).
//!
//! [`ViewCamera`] is the client's room camera (`FRoomCamera`,
//! `CppSource/OutpostFrontier/Client/room_camera.cpp`), the plain
//! perspective camera of the reference: a viewport, a horizontal half
//! angle, near and far planes, an orientation whose columns are the
//! camera's axes in the world (it looks down its -Z) and a position.
//! Construction is `RoomCameraConstruct`, the setters are
//! `RoomCameraSetHalfFovX`, `RoomCameraSetZNear` and `RoomCameraSetZFar`,
//! and the interface answers as the room camera does: `ClassifySphere`
//! (outside, too small, straddling, inside) on a camera-space centre,
//! which is how the render manager culls (`RenderInstance`), `Project`,
//! `Unproject` and `GetViewTransform`. Arithmetic follows decision T11:
//! the reference's float stores round, the rest is `f64`; the tangents
//! and arc tangents come from the standard library (Tier B).
//!
//! The reference is built with its port-only widescreen option
//! (`OUTPOST_FRONTIER_WIDESCREEN_SUPPORT`, on in `Directory.Build.props`):
//! on a viewport as wide as a surface wider than 4:3, the stored half
//! angle stays the 4:3 one and the tangent, the X plane and the X scale
//! widen by the surface's aspect over 4:3 (`Hor+`,
//! [`hor_plus_scale`](crate::ui::widescreen::hor_plus_scale)).
//!
//! [`Engine::frame`](super::Engine::frame) applies the frame's camera
//! before the scene as `Win3DApplyCamera` does (`win3d_render.cpp`): the
//! perspective from `GetFovYDeg`, `GetAspect`, `GetZNear` and `GetZFar`,
//! and `GetViewTransform` as the modelview.

// The reference's compares answer false on a NaN; the negations keep that.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::math::{Matrix, Transform, Vector, inverse_transform, transpose_transform};

use crate::render::appearance::Camera;
use crate::ui::widescreen::{Surface, hor_plus_scale};

/// `.rdata 0x005c901c`: pi / 360 as a float, used on a doubled angle.
const PI_OVER_360: f32 = 0.008_726_646;
/// `.rdata 0x005c77f4`: pi / 180 as a float.
const PI_OVER_180: f32 = 0.017_453_292;
/// `.rdata 0x005c8928`: 180 / pi.
const RAD_TO_DEG: f64 = 57.295_777_918_682_05;
/// `.rdata 0x005e139c`: -4/3, the provisional Y scale.
const MINUS_FOUR_THIRDS: f32 = -1.333_333_4;
/// `.rdata 0x005dbc98`: the "centre on the eye plane" epsilon.
const CLASSIFY_EPSILON: f64 = 0.0001;
/// `.rdata 0x005df814`: -pi as a float.
const MINUS_PI: f32 = -std::f32::consts::PI;

/// `ClassifySphere`'s answers.
pub mod class {
    /// Outside the frustum.
    pub const OUTSIDE: i32 = 1;
    /// Inside, but smaller than one pixel on screen.
    pub const TOO_SMALL: i32 = 2;
    /// Straddling a plane.
    pub const STRADDLING: i32 = 3;
    /// Inside.
    pub const INSIDE: i32 = 4;
}

/// Rounds an x87 result to a float store.
#[allow(clippy::cast_possible_truncation)]
fn f(v: f64) -> f32 {
    v as f32
}

/// Normalises in place, summing `(x*x + y*y) + z*z`.
fn normalize_xyz(v: &mut [f32; 3]) {
    let [x, y, z] = v.map(f64::from);
    let len = (x * x + y * y + z * z).sqrt();
    let inv = 1.0 / len;
    *v = [f(inv * x), f(inv * y), f(inv * z)];
}

/// Normalises in place, summing `(y*y + z*z) + x*x` (the second
/// normalise of `RoomCameraSetZNear`).
fn normalize_yzx(v: &mut [f32; 3]) {
    let [x, y, z] = v.map(f64::from);
    let len = (y * y + z * z + x * x).sqrt();
    let inv = 1.0 / len;
    *v = [f(inv * x), f(inv * y), f(inv * z)];
}

/// `tan((angle + angle) * pi/360)` in `f64`.
fn tan_of_doubled(angle: f32) -> f64 {
    ((f64::from(angle) + f64::from(angle)) * f64::from(PI_OVER_360)).tan()
}

/// The degrees of `atan2(tangent, 1)`.
fn atan_deg(tangent: f64) -> f32 {
    f(tangent.atan2(1.0) * RAD_TO_DEG)
}

/// The room camera's state (`FRoomCamera`'s fields from +0x04).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ViewCamera {
    /// Left, top, right, bottom (inclusive edges).
    pub viewport: [i32; 4],
    pub half_fov_x_deg: f32,
    pub half_fov_y_deg: f32,
    pub z_near: f32,
    pub z_far: f32,
    pub tan_half_fov_x: f32,
    pub tan_half_fov_y: f32,
    pub aspect: f32,
    pub half_width: f32,
    pub half_height: f32,
    pub x_scale: f32,
    pub y_scale: f32,
    pub plane_x: [f32; 3],
    pub plane_y: [f32; 3],
    /// Row-major; its columns are the camera's axes in the world.
    pub orientation: [f32; 9],
    pub position: [f32; 3],
    /// The render surface, for the widescreen factor.
    pub surface: Surface,
}

impl ViewCamera {
    /// `RoomCameraConstruct` with an identity orientation at the origin,
    /// over a viewport of `width` by `height` pixels at `(0, 0)`.
    #[must_use]
    pub fn new(
        (width, height): (u32, u32),
        half_fov_x_deg: f32,
        z_near: f32,
        z_far: f32,
    ) -> ViewCamera {
        let w = i32::try_from(width.max(1)).unwrap_or(i32::MAX);
        let h = i32::try_from(height.max(1)).unwrap_or(i32::MAX);
        let mut cam = ViewCamera::from_view_rect([0, 0, w - 1, h - 1], Surface::new(w, h));
        let tan = f(tan_of_doubled(half_fov_x_deg));
        cam.tan_half_fov_x = f(f64::from(tan) * cam.hor_plus());
        cam.half_fov_x_deg = atan_deg(f64::from(tan));
        cam.plane_x = [1.0, 0.0, cam.tan_half_fov_x];
        normalize_xyz(&mut cam.plane_x);
        cam.x_scale = f(f64::from(cam.half_width) / f64::from(cam.tan_half_fov_x));
        cam.aspect = f(f64::from(cam.tan_half_fov_x) / f64::from(cam.tan_half_fov_y));

        // The Rect's extents, not its edges.
        let rect_aspect = f64::from(w) / f64::from(h);
        cam.aspect = f(rect_aspect);
        let tan_y = f64::from(cam.tan_half_fov_x) / rect_aspect;
        cam.tan_half_fov_y = f(tan_y);
        cam.half_fov_y_deg = atan_deg(tan_y);
        cam.plane_y = [0.0, 1.0, cam.tan_half_fov_y];
        normalize_xyz(&mut cam.plane_y);
        cam.z_near = z_near;
        cam.y_scale = f(-(f64::from(cam.half_height) / f64::from(cam.tan_half_fov_y)));

        let half_x = (f64::from(cam.half_fov_x_deg) + f64::from(cam.half_fov_x_deg)) * 0.5;
        cam.half_fov_x_deg = f(half_x);
        cam.tan_half_fov_x = f((half_x * f64::from(PI_OVER_180)).tan() * cam.hor_plus());
        cam.rebuild_plane_x();
        cam.aspect = f(f64::from(cam.tan_half_fov_x) / f64::from(cam.tan_half_fov_y));

        let half_y = (f64::from(cam.half_fov_y_deg) + f64::from(cam.half_fov_y_deg)) * 0.5;
        cam.half_fov_y_deg = f(half_y);
        cam.tan_half_fov_y = f((half_y * f64::from(PI_OVER_180)).tan());
        cam.rebuild_plane_y();
        cam.aspect = f(f64::from(cam.tan_half_fov_x) / f64::from(cam.tan_half_fov_y));
        cam.z_far = z_far;
        cam
    }

    /// The constructor's body and `InitFromViewRect`.
    fn from_view_rect(viewport: [i32; 4], surface: Surface) -> ViewCamera {
        let half_width = f(f64::from(viewport[2] - viewport[0]) * 0.5);
        let half_height = f(f64::from(viewport[3] - viewport[1]) * 0.5);
        let mut cam = ViewCamera {
            viewport,
            half_fov_x_deg: 0.0,
            half_fov_y_deg: 0.0,
            z_near: 1.0,
            z_far: 100_000.0,
            tan_half_fov_x: 1.0,
            tan_half_fov_y: 0.75,
            aspect: 1.333_333_4,
            half_width,
            half_height,
            x_scale: half_width,
            y_scale: f(f64::from(half_height) * f64::from(MINUS_FOUR_THIRDS)),
            plane_x: [1.0, 0.0, 1.0],
            plane_y: [0.0, 1.0, 0.75],
            orientation: [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0],
            position: [0.0; 3],
            surface,
        };
        cam.half_fov_x_deg = atan_deg(1.0);
        normalize_xyz(&mut cam.plane_x);
        cam.x_scale = f(f64::from(half_width) / f64::from(cam.tan_half_fov_x));
        cam.half_fov_y_deg = atan_deg(f64::from(cam.tan_half_fov_y));
        normalize_xyz(&mut cam.plane_y);
        cam.y_scale = f(-(f64::from(half_height) / f64::from(cam.tan_half_fov_y)));
        cam
    }

    /// `RebuildPlaneX`.
    /// `HorPlusScale`: the widescreen factor of the viewport.
    fn hor_plus(&self) -> f64 {
        let width = i64::from(self.viewport[2]) - i64::from(self.viewport[0]) + 1;
        hor_plus_scale(&self.surface, width)
    }

    fn rebuild_plane_x(&mut self) {
        self.half_fov_x_deg = atan_deg(f64::from(self.tan_half_fov_x) / self.hor_plus());
        self.plane_x = [1.0, 0.0, self.tan_half_fov_x];
        normalize_xyz(&mut self.plane_x);
        self.x_scale = f(f64::from(self.half_width) / f64::from(self.tan_half_fov_x));
    }

    /// `RebuildPlaneY`.
    fn rebuild_plane_y(&mut self) {
        self.half_fov_y_deg = atan_deg(f64::from(self.tan_half_fov_y));
        self.plane_y = [0.0, 1.0, self.tan_half_fov_y];
        normalize_xyz(&mut self.plane_y);
        self.y_scale = f(-(f64::from(self.half_height) / f64::from(self.tan_half_fov_y)));
    }

    /// `SetPosition` (slot 19).
    pub fn set_position(&mut self, position: [f32; 3]) {
        self.position = position;
    }

    /// `SetOrientation` (slot 20): row-major, the camera's axes as columns.
    pub fn set_orientation(&mut self, orientation: [f32; 9]) {
        self.orientation = orientation;
    }

    /// `SetTransform` (slot 21): the orientation, then the position.
    pub fn set_transform(&mut self, transform: &[f32; 12]) {
        self.orientation.copy_from_slice(&transform[..9]);
        self.position.copy_from_slice(&transform[9..]);
    }

    /// `RoomCameraSetHalfFovX`: the horizontal half angle; the aspect comes
    /// from the viewport's inclusive edges.
    pub fn set_half_fov_x(&mut self, half_fov_x_deg: f32) {
        let tan = tan_of_doubled(half_fov_x_deg);
        let tan_x = tan * self.hor_plus();
        self.tan_half_fov_x = f(tan_x);
        self.half_fov_x_deg = atan_deg(tan);
        self.plane_x = [1.0, 0.0, f(tan_x)];
        normalize_xyz(&mut self.plane_x);
        self.x_scale = f(f64::from(self.half_width) / f64::from(self.tan_half_fov_x));
        let width = self.viewport[2] - self.viewport[0] + 1;
        let height = self.viewport[3] - self.viewport[1] + 1;
        let edge_aspect = f64::from(width) / f64::from(height);
        self.aspect = f(edge_aspect);
        let tan_y = f64::from(self.tan_half_fov_x) / edge_aspect;
        self.tan_half_fov_y = f(tan_y);
        self.half_fov_y_deg = atan_deg(tan_y);
        self.plane_y = [0.0, 1.0, f(tan_y)];
        normalize_xyz(&mut self.plane_y);
        self.y_scale = f(-(f64::from(self.half_height) / f64::from(self.tan_half_fov_y)));
    }

    /// `RoomCameraSetZNear`: the near plane, then both planes rebuilt from
    /// the stored half angles.
    pub fn set_z_near(&mut self, z_near: f32) {
        self.z_near = z_near;
        let tan_x = tan_of_doubled(self.half_fov_x_deg);
        self.tan_half_fov_x = f(tan_x * self.hor_plus());
        self.half_fov_x_deg = atan_deg(tan_x);
        self.plane_x = [1.0, 0.0, self.tan_half_fov_x];
        normalize_xyz(&mut self.plane_x);
        self.x_scale = f(f64::from(self.half_width) / f64::from(self.tan_half_fov_x));
        self.aspect = f(f64::from(self.tan_half_fov_x) / f64::from(self.tan_half_fov_y));
        let tan_y = tan_of_doubled(self.half_fov_y_deg);
        self.tan_half_fov_y = f(tan_y);
        self.half_fov_y_deg = atan_deg(tan_y);
        self.plane_y = [0.0, 1.0, self.tan_half_fov_y];
        normalize_yzx(&mut self.plane_y);
        let inv_tan_y = 1.0 / f64::from(self.tan_half_fov_y);
        self.y_scale = f(-(inv_tan_y * f64::from(self.half_height)));
        self.aspect = f(inv_tan_y * f64::from(self.tan_half_fov_x));
    }

    /// `RoomCameraSetZFar`.
    pub fn set_z_far(&mut self, z_far: f32) {
        self.z_far = z_far;
    }

    fn transform(&self) -> Transform {
        let mut words = [0.0; 12];
        words[..9].copy_from_slice(&self.orientation);
        words[9..].copy_from_slice(&self.position);
        Transform::from_words(words)
    }
}

impl Camera for ViewCamera {
    fn position(&mut self) -> [f32; 3] {
        self.position
    }

    fn orientation(&mut self) -> [f32; 9] {
        self.orientation
    }

    fn viewport(&mut self) -> [i32; 4] {
        self.viewport
    }

    fn half_fov_x_deg(&mut self) -> f32 {
        self.half_fov_x_deg
    }

    /// The room camera answers its vertical half angle; `SetPerspective`
    /// takes it as such.
    fn fov_y_deg(&mut self) -> f32 {
        self.half_fov_y_deg
    }

    fn z_near(&mut self) -> f32 {
        self.z_near
    }

    fn z_far(&mut self) -> f32 {
        self.z_far
    }

    fn aspect(&mut self) -> f32 {
        self.aspect
    }

    fn x_scale_over_near(&mut self) -> f64 {
        f64::from(self.x_scale) / f64::from(self.z_near)
    }

    fn y_scale_over_near(&mut self) -> f64 {
        f64::from(self.y_scale) / f64::from(self.z_near)
    }

    fn project(&mut self, world: [f32; 3], out: &mut [f32; 3]) -> i32 {
        let local = inverse_transform(&self.transform(), Vector::from_words(world));
        if !(-self.z_near >= local.z) {
            return 0;
        }
        let k = -1.0 / f64::from(local.z);
        out[0] = f((f64::from(local.x) * f64::from(self.x_scale) * k
            + f64::from(self.viewport[0]))
            + f64::from(self.half_width));
        out[1] = f((f64::from(local.y) * f64::from(self.y_scale) * k
            + f64::from(self.viewport[1]))
            + f64::from(self.half_height));
        out[2] = -local.z;
        1
    }

    fn unproject(&mut self, out: &mut [f32; 3], x: f32, y: f32) {
        let a = f(
            (f64::from(x) - (f64::from(self.viewport[0]) + f64::from(self.half_width)))
                / f64::from(self.x_scale),
        );
        let b = f(
            (f64::from(y) - (f64::from(self.viewport[1]) + f64::from(self.half_height)))
                / f64::from(self.y_scale),
        );
        let m = self.orientation.map(f64::from);
        let (a, b) = (f64::from(a), f64::from(b));
        let m1b = f(m[1] * b);
        let m4b = m[4] * b;
        let m7b = m[7] * b;
        let m0a = f(m[0] * a);
        let m3a = m[3] * a;
        let m6a = f(m[6] * a);
        let r0 = f(f64::from(m0a) + f64::from(m1b));
        let r1 = f(m3a + m4b);
        let r2 = f(f64::from(m6a) + m7b);
        let world = [
            f(f64::from(r0) - m[2]),
            f(f64::from(r1) - m[5]),
            f(f64::from(r2) - m[8]),
        ];
        let near = f64::from(self.z_near);
        *out = world.map(|w| f(near * f64::from(w)));
    }

    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        let z = f64::from(centre[2]);
        let r = f64::from(radius);
        let near = f64::from(self.z_near);
        let far = f64::from(self.z_far);
        if r - near < z {
            return class::OUTSIDE;
        }
        let mut result = class::INSIDE;
        let s = r + z;
        if !(s < -near) {
            result = class::STRADDLING;
        }
        if s < -far {
            return class::OUTSIDE;
        }
        if !(r - far < z) {
            result = class::STRADDLING;
        }
        let negative = f64::from(-radius);
        for (axis, plane) in [(0, self.plane_x), (1, self.plane_y)] {
            let dist =
                f64::from(centre[axis]).abs() * f64::from(plane[axis]) + f64::from(plane[2]) * z;
            if dist > r {
                return class::OUTSIDE;
            }
            if !(dist < negative) {
                result = class::STRADDLING;
            }
        }
        if !(z.abs() >= CLASSIFY_EPSILON) {
            return result;
        }
        let q = -(r / z);
        let area = f64::from(self.y_scale) * f64::from(self.x_scale) * q * q * f64::from(MINUS_PI);
        if area < 1.0 {
            return class::TOO_SMALL;
        }
        result
    }

    fn view_transform(&mut self) -> [f32; 12] {
        let rotated = transpose_transform(
            &Matrix::from_words(self.orientation),
            Vector::from_words(self.position),
        );
        let o = self.orientation;
        [
            o[0], o[3], o[6], o[1], o[4], o[7], o[2], o[5], o[8], -rotated.x, -rotated.y,
            -rotated.z,
        ]
    }
}

/// An orbit around a target: the orientation (row-major, the camera's axes
/// as columns, looking down its -Z at the target) and the position of a
/// camera `distance` from `target` at `yaw` about the world's Y axis and
/// `pitch` above the XZ plane, both in radians. At zero yaw and pitch the
/// camera sits on the target's +Z side.
#[must_use]
pub fn orbit(target: [f32; 3], yaw: f32, pitch: f32, distance: f32) -> ([f32; 9], [f32; 3]) {
    let (yaw, pitch) = (f64::from(yaw), f64::from(pitch));
    let back = [
        pitch.cos() * yaw.sin(),
        pitch.sin(),
        pitch.cos() * yaw.cos(),
    ];
    let right = [yaw.cos(), 0.0, -yaw.sin()];
    let up = [
        back[1] * right[2] - back[2] * right[1],
        back[2] * right[0] - back[0] * right[2],
        back[0] * right[1] - back[1] * right[0],
    ];
    let d = f64::from(distance);
    let position = core::array::from_fn(|i| f(f64::from(target[i]) + d * back[i]));
    let orientation = [
        f(right[0]),
        f(up[0]),
        f(back[0]),
        f(right[1]),
        f(up[1]),
        f(back[1]),
        f(right[2]),
        f(up[2]),
        f(back[2]),
    ];
    (orientation, position)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn camera() -> ViewCamera {
        ViewCamera::new((640, 480), 35.0, 0.25, 1_000_000.0)
    }

    #[test]
    fn construction_follows_the_room_camera() {
        let cam = camera();
        assert_eq!(cam.viewport, [0, 0, 639, 479]);
        assert_eq!(cam.half_width, 319.5);
        assert_eq!(cam.half_height, 239.5);
        assert!((cam.half_fov_x_deg - 35.0).abs() < 1e-4);
        assert!((cam.aspect - 4.0 / 3.0).abs() < 1e-3);
        assert!(cam.y_scale < 0.0 && cam.x_scale > 0.0);
        let tan_y = cam.tan_half_fov_x / (640.0 / 480.0);
        assert!((cam.tan_half_fov_y - tan_y).abs() < 1e-3);
        assert_eq!(cam.z_near, 0.25);
        assert_eq!(cam.z_far, 1_000_000.0);
    }

    #[test]
    fn spheres_classify_against_the_frustum() {
        let mut cam = camera();
        // Camera space: -Z forward.
        assert_eq!(cam.classify_sphere([0.0, 0.0, -10.0], 1.0), class::INSIDE);
        assert_eq!(cam.classify_sphere([0.0, 0.0, 10.0], 1.0), class::OUTSIDE);
        assert_eq!(
            cam.classify_sphere([0.0, 0.0, -0.25], 1.0),
            class::STRADDLING
        );
        assert_eq!(
            cam.classify_sphere([100.0, 0.0, -10.0], 1.0),
            class::OUTSIDE
        );
        assert_eq!(
            cam.classify_sphere([0.0, 0.0, -100_000.0], 0.01),
            class::TOO_SMALL
        );
    }

    #[test]
    fn an_orbit_looks_at_its_target() {
        let target = [1.0, 2.0, 3.0];
        let (orientation, position) = orbit(target, 0.7, 0.3, 50.0);
        let mut cam = camera();
        cam.set_orientation(orientation);
        cam.set_position(position);
        let view = Transform::from_words(cam.view_transform());
        let local = neuron_core::math::transform_xform(&view, Vector::from_words(target));
        assert!(local.x.abs() < 1e-3 && local.y.abs() < 1e-3, "{local:?}");
        assert!((local.z + 50.0).abs() < 1e-3, "{local:?}");
        let mut screen = [0.0; 3];
        assert_eq!(cam.project(target, &mut screen), 1);
        assert!((screen[0] - 319.5).abs() < 1e-2 && (screen[1] - 239.5).abs() < 1e-2);
        assert!((screen[2] - 50.0).abs() < 1e-3);
        // The point straight ahead on the near plane unprojects onto the
        // view axis.
        let mut back = [0.0; 3];
        cam.unproject(&mut back, 319.5, 239.5);
        let dir = [back[0] / 0.25, back[1] / 0.25, back[2] / 0.25];
        let forward = [-orientation[2], -orientation[5], -orientation[8]];
        for i in 0..3 {
            assert!((dir[i] - forward[i]).abs() < 1e-3, "{dir:?} {forward:?}");
        }
    }

    #[test]
    fn a_wide_surface_widens_the_horizontal_angle() {
        let wide = ViewCamera::new((1920, 1080), 35.0, 0.25, 1000.0);
        let narrow = camera();
        assert!(
            (wide.half_fov_x_deg - 35.0).abs() < 1e-4,
            "the 4:3 angle is kept"
        );
        let scale = (1920.0 * 3.0) / (1080.0 * 4.0);
        assert!((wide.tan_half_fov_x / narrow.tan_half_fov_x - scale).abs() < 1e-4);
        assert!((wide.tan_half_fov_y - narrow.tan_half_fov_y).abs() < 1e-4);
    }

    #[test]
    fn the_field_of_view_and_planes_can_change() {
        let mut cam = camera();
        cam.set_half_fov_x(45.0);
        assert!((cam.half_fov_x_deg - 45.0).abs() < 1e-4);
        assert!((cam.tan_half_fov_x - 1.0).abs() < 1e-5);
        cam.set_z_near(2.0);
        assert_eq!(cam.z_near, 2.0);
        assert!((cam.half_fov_x_deg - 45.0).abs() < 1e-4);
        cam.set_z_far(500.0);
        assert_eq!(cam.classify_sphere([0.0, 0.0, -600.0], 1.0), class::OUTSIDE);
    }
}
