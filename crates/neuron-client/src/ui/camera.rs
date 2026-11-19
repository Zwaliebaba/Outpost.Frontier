//! The Win3D camera: the projection part of the in-flight camera base
//! (`FSpaceCamera`, `space_camera.cpp`) that the interface is seen through,
//! with the screen-to-ray step of the Win3D input handler (`win3d.cpp`).
//!
//! The interface lives in a 3D space in front of a camera at the origin
//! looking down -Z (`G_Win3DSceneCamera`, constructed at static
//! initialisation; `Win3DStartupManager` gives it the surface as its
//! viewport, near and far planes of 0.1 and 1000 and a horizontal half
//! angle of 27.216 degrees). Pages and controls are placed in that space
//! (`UIPage::MasterWin` unprojects the surface corners to size the page),
//! the mouse is a ray through the cursor pixel, and controls are hit by
//! that ray.
//!
//! With the reference's port-only widescreen support (on in the reference
//! build) the camera goes "Hor+" ([`super::widescreen`]): the stored half
//! angle is the one a 4:3 surface of the same height shows, and a viewport
//! spanning the full surface widens its frustum by the surface's aspect
//! over 4:3, so the 4:3 interface stays the same size in the middle.
//!
//! Arithmetic follows decision T11: `f64` evaluation, rounding at each
//! store the reference makes to a `float`. The tangent and arc tangent go
//! through the host's `tan` and `atan2` where the reference has `FPTAN`
//! and `FPATAN`; their results are rounded to `f32` before any store.

use neuron_core::math::{
    Matrix, Transform, Vector, inverse_transform, transform_matrix, transpose_transform,
};

use super::widescreen::{Surface, hor_plus_scale};

/// `kHalfFovXRadians`: 35 degrees.
const HALF_FOV_X_RADIANS: f64 = 0.610_865_233_466_029_2;
/// `kRadiansToDegrees`.
const RADIANS_TO_DEGREES: f64 = 57.295_777_918_682_05;
/// `kDegreesToRadians`, a `float` constant.
const DEGREES_TO_RADIANS: f32 = 0.017_453_292;

/// The Win3D camera's horizontal half angle (bits 0x41D9BA5E, 27.216
/// degrees).
pub const WIN3D_HALF_FOV_X: f32 = f32::from_bits(0x41D9_BA5E);
/// The Win3D camera's near plane.
pub const WIN3D_Z_NEAR: f32 = 0.1;
/// The Win3D camera's far plane.
pub const WIN3D_Z_FAR: f32 = 1000.0;

fn round(v: f64) -> f32 {
    v as f32
}

/// The projection state of `FSpaceCamera` (`+0x04` to `+0xbb`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Camera {
    /// `+0x04` orientation, row-major.
    pub world: Matrix,
    /// `+0x28`.
    pub world_pos: Vector,
    view_dirty: bool,
    view: Matrix,
    view_pos: Vector,
    pub screen_center_x: f32,
    pub screen_center_y: f32,
    pub half_width: f32,
    pub half_height: f32,
    /// `HalfWidth / tan(half fov x)`: pixels per unit at depth 1.
    pub x_scale: f32,
    /// `-(HalfHeight / tan(half fov y))`.
    pub y_scale: f32,
    pub plane_x: [f32; 3],
    pub plane_y: [f32; 3],
    pub half_fov_x_deg: f32,
    pub fov_y_deg: f32,
    pub aspect: f32,
    pub z_near: f32,
    pub z_far: f32,
    /// Left, top, right, bottom, inclusive.
    pub viewport: [i32; 4],
    /// The surface the Hor+ factor is taken against.
    pub surface: Surface,
}

impl Default for Camera {
    fn default() -> Self {
        Camera::new(Surface::default())
    }
}

/// `NormalizePlane`.
fn normalize_plane(v: &mut [f32; 3]) {
    let (a, b, c) = (f64::from(v[0]), f64::from(v[1]), f64::from(v[2]));
    let inv = 1.0 / (a * a + b * b + c * c).sqrt();
    v[0] = round(inv * a);
    v[1] = round(inv * b);
    v[2] = round(inv * c);
}

/// `TangentToDegrees`.
fn tangent_to_degrees(tangent: f64) -> f32 {
    round(tangent.atan2(1.0) * RADIANS_TO_DEGREES)
}

impl Camera {
    /// `SpaceCameraConstruct` over zeroed storage (the Win3D camera is a
    /// static object): identity orientation, aspect 4:3, half extents 320
    /// by 240, planes 3 and 250,000, a half angle of 35 degrees.
    #[must_use]
    pub fn new(surface: Surface) -> Camera {
        let mut cam = Camera {
            world: Matrix::IDENTITY,
            world_pos: Vector::ZERO,
            view_dirty: true,
            view: Matrix::IDENTITY,
            view_pos: Vector::ZERO,
            screen_center_x: 0.0,
            screen_center_y: 0.0,
            half_width: 320.0,
            half_height: 240.0,
            x_scale: 0.0,
            y_scale: 0.0,
            plane_x: [0.0; 3],
            plane_y: [0.0; 3],
            half_fov_x_deg: 35.0,
            fov_y_deg: 0.0,
            aspect: f32::from_bits(0x3FAA_AAAB),
            z_near: 3.0,
            z_far: 250_000.0,
            viewport: [0; 4],
            surface,
        };
        let tan_x = HALF_FOV_X_RADIANS.tan();
        let skew = f64::from(0.75f32) * tan_x;
        let skew_stored = round(skew);
        cam.fov_y_deg = tangent_to_degrees(skew);
        cam.rebuild_plane_x(tan_x);
        cam.rebuild_plane_y(f64::from(skew_stored));
        cam
    }

    /// The camera `Win3DStartupManager` sets up for a surface: viewport
    /// `{0, 0, w - 1, h - 1}`, planes 0.1 and 1000, the half angle
    /// [`WIN3D_HALF_FOV_X`].
    #[must_use]
    pub fn win3d(surface: Surface) -> Camera {
        let mut cam = Camera::new(surface);
        cam.set_viewport([0, 0, surface.width - 1, surface.height - 1]);
        cam.set_near_far(WIN3D_Z_NEAR, WIN3D_Z_FAR);
        cam.set_half_fov_x(WIN3D_HALF_FOV_X);
        cam
    }

    /// `RebuildPlaneX`.
    fn rebuild_plane_x(&mut self, tan_x: f64) {
        self.plane_x = [1.0, 0.0, round(tan_x)];
        normalize_plane(&mut self.plane_x);
        if tan_x > 0.0 {
            self.x_scale = round(f64::from(self.half_width) / tan_x);
        }
    }

    /// `RebuildPlaneY`.
    fn rebuild_plane_y(&mut self, tan_y: f64) {
        self.plane_y = [0.0, 1.0, round(tan_y)];
        normalize_plane(&mut self.plane_y);
        if tan_y > 0.0 {
            self.y_scale = round(-(f64::from(self.half_height) / tan_y));
        }
    }

    /// `SpaceCameraHorPlusScale`: the Hor+ factor of the stored viewport.
    #[must_use]
    pub fn hor_plus_scale(&self) -> f64 {
        let width = i64::from(self.viewport[2]) - i64::from(self.viewport[0]) + 1;
        hor_plus_scale(&self.surface, width)
    }

    fn tan_x(&self) -> f64 {
        let tan = (f64::from(self.half_fov_x_deg) * f64::from(DEGREES_TO_RADIANS)).tan();
        tan * self.hor_plus_scale()
    }

    /// `SpaceCameraSetHalfFovX`.
    pub fn set_half_fov_x(&mut self, half_fov_x_deg: f32) {
        self.half_fov_x_deg = half_fov_x_deg;
        let tan_x = self.tan_x();
        let tan_y = tan_x / f64::from(self.aspect);
        let tan_y_stored = round(tan_y);
        self.fov_y_deg = tangent_to_degrees(tan_y);
        self.rebuild_plane_x(tan_x);
        self.rebuild_plane_y(f64::from(tan_y_stored));
    }

    /// `SpaceCameraSetFovY`: the vertical angle, the horizontal half angle
    /// of its tangent times the aspect (over the Hor+ factor), and both
    /// planes from the unrounded tangents.
    pub fn set_fov_y(&mut self, fov_y_deg: f32) {
        self.fov_y_deg = fov_y_deg;
        let tan_y = (f64::from(fov_y_deg) * f64::from(DEGREES_TO_RADIANS)).tan();
        let tan_x = tan_y * f64::from(self.aspect);
        self.half_fov_x_deg = tangent_to_degrees(tan_x / self.hor_plus_scale());
        self.rebuild_plane_x(tan_x);
        self.rebuild_plane_y(tan_y);
    }

    /// `SpaceCameraSetViewport`: the inclusive rectangle, the aspect, the
    /// half extents, the screen centre and the planes for the stored half
    /// angle.
    pub fn set_viewport(&mut self, rect: [i32; 4]) {
        self.viewport = rect;
        let width = rect[2].wrapping_sub(rect[0]).wrapping_add(1);
        let height = rect[3].wrapping_sub(rect[1]).wrapping_add(1);
        let aspect = round(f64::from(width) / f64::from(height));
        self.aspect = aspect;
        let half_width = round(f64::from(width) * 0.5);
        self.half_width = half_width;
        let half_height = f64::from(height) * 0.5;
        self.half_height = round(half_height);
        self.screen_center_x = round(f64::from(rect[0]) + f64::from(half_width));
        self.screen_center_y = round(f64::from(rect[1]) + half_height);
        let tan_x = self.tan_x();
        let tan_y = tan_x / f64::from(aspect);
        let tan_y_stored = round(tan_y);
        self.fov_y_deg = tangent_to_degrees(tan_y);
        self.rebuild_plane_x(tan_x);
        self.rebuild_plane_y(f64::from(tan_y_stored));
    }

    /// `CameraSetNearFar`: the planes only.
    pub fn set_near_far(&mut self, z_near: f32, z_far: f32) {
        self.z_near = z_near;
        self.z_far = z_far;
    }

    /// `GetXScaleOverNear`.
    #[must_use]
    pub fn x_scale_over_near(&self) -> f64 {
        f64::from(self.x_scale) / f64::from(self.z_near)
    }

    /// `GetYScaleOverNear`.
    #[must_use]
    pub fn y_scale_over_near(&self) -> f64 {
        f64::from(self.y_scale) / f64::from(self.z_near)
    }

    /// The world transform (orientation and position).
    #[must_use]
    pub fn world_transform(&self) -> Transform {
        Transform::new(self.world, self.world_pos)
    }

    /// Moves the camera; the view transform is rebuilt on its next read.
    pub fn set_world(&mut self, orientation: Matrix, position: Vector) {
        self.world = orientation;
        self.world_pos = position;
        self.view_dirty = true;
    }

    /// `GetViewTransform`: the transposed orientation and `-(R^T p)`,
    /// rebuilt when dirty.
    pub fn view_transform(&mut self) -> Transform {
        if self.view_dirty {
            let out = transpose_transform(&self.world, self.world_pos);
            self.view = self.world.transpose();
            self.view_pos = Vector::new(-out.x, -out.y, -out.z);
            self.view_dirty = false;
        }
        Transform::new(self.view, self.view_pos)
    }

    /// Overwrites the cached view transform and keeps it clean (the
    /// widescreen shear of `hud_widescreen.cpp`).
    pub(crate) fn set_view_cache(&mut self, view: Transform) {
        self.view = view.rotation;
        self.view_pos = view.translation;
        self.view_dirty = false;
    }

    /// Marks the view transform for a rebuild.
    pub(crate) fn mark_view_dirty(&mut self) {
        self.view_dirty = true;
    }

    /// `FSpaceCamera::Project`: the screen point and depth of a world
    /// point, `None` unless it lies on or beyond the near plane.
    #[must_use]
    pub fn project(&self, world: Vector) -> Option<(f32, f32, f32)> {
        let local = inverse_transform(&self.world_transform(), world);
        // Unordered fails, as the reference's FCOMP / TEST AH,1 does.
        if !(-f64::from(self.z_near) >= f64::from(local.z)) {
            return None;
        }
        let inv_z = -1.0 / f64::from(local.z);
        let x = round(
            f64::from(local.x) * f64::from(self.x_scale) * inv_z + f64::from(self.screen_center_x),
        );
        let y = round(
            f64::from(local.y) * f64::from(self.y_scale) * inv_z + f64::from(self.screen_center_y),
        );
        Some((x, y, -local.z))
    }

    /// The camera-space point on the near plane under a screen pixel
    /// (`FSpaceCamera::Unproject`'s first step, `Win3DScreenToNearPlane`).
    #[must_use]
    pub fn screen_to_near_plane(&self, screen_x: f32, screen_y: f32) -> Vector {
        let z_near = f64::from(self.z_near);
        Vector::new(
            round(
                (f64::from(screen_x) - f64::from(self.screen_center_x))
                    * (z_near / f64::from(self.x_scale)),
            ),
            round(
                (f64::from(screen_y) - f64::from(self.screen_center_y))
                    * (z_near / f64::from(self.y_scale)),
            ),
            round(-z_near),
        )
    }

    /// `FSpaceCamera::Unproject`: the near-plane point under a screen pixel
    /// turned by the orientation (a direction, not normalised).
    #[must_use]
    pub fn unproject(&self, screen_x: f32, screen_y: f32) -> Vector {
        transform_matrix(&self.world, self.screen_to_near_plane(screen_x, screen_y))
    }

    /// `FSpaceCamera::ClassifySphere` of a camera-space centre: 1 outside,
    /// 3 straddling a plane, 4 inside. The view-axis edges are stored as
    /// floats; the side distances stay unrounded.
    #[must_use]
    pub fn classify_sphere(&self, centre: [f32; 3], radius: f32) -> i32 {
        let near_edge = round(f64::from(-centre[2]) - f64::from(radius));
        let far_edge = round(f64::from(radius) - f64::from(centre[2]));
        let r = f64::from(radius);
        if far_edge < self.z_near {
            return 1;
        }
        let mut result = 4;
        if near_edge < self.z_near {
            result = 3;
        }
        for (axis, plane) in [(0, self.plane_x), (1, self.plane_y)] {
            let dist = f64::from(centre[axis].abs()) * f64::from(plane[axis])
                + f64::from(plane[2]) * f64::from(centre[2]);
            if dist > r {
                return 1;
            }
            if dist > -r {
                result = 3;
            }
        }
        if near_edge > self.z_far {
            return 1;
        }
        if far_edge > self.z_far {
            result = 3;
        }
        result
    }

    /// The mouse ray of the Win3D input handler's move event: the cursor
    /// pixel unprojected and normalised (`NormalizeInPlace`, no zero test:
    /// a zero vector gives NaNs as in the reference).
    #[must_use]
    pub fn mouse_ray(&self, cursor_x: i32, cursor_y: i32) -> Vector {
        // The pixel goes through `float` first (FILD, FSTP dword).
        let world = self.unproject(cursor_x as f32, cursor_y as f32);
        normalize(world)
    }
}

/// `NormalizeInPlace` of the Win3D input handler: scale by `1 / |v|`.
#[must_use]
pub fn normalize(v: Vector) -> Vector {
    let (x, y, z) = (f64::from(v.x), f64::from(v.y), f64::from(v.z));
    let inv = 1.0 / (x * x + y * y + z * z).sqrt();
    Vector::new(round(inv * x), round(inv * y), round(inv * z))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_constructed_camera_matches_the_header_values() {
        let cam = Camera::new(Surface::default());
        // HalfWidth / tan(35 degrees), -(HalfHeight / (0.75 tan(35))).
        assert!((cam.x_scale - 457.0).abs() < 0.1);
        assert!((cam.y_scale + 457.0).abs() < 0.1);
        assert!((cam.fov_y_deg - 27.71).abs() < 0.01);
    }

    #[test]
    fn a_4_by_3_surface_centres_the_screen() {
        let cam = Camera::win3d(Surface::new(1024, 768));
        assert_eq!(cam.screen_center_x, 512.0);
        assert_eq!(cam.screen_center_y, 384.0);
        assert_eq!(cam.half_width, 512.0);
        assert_eq!(cam.aspect, (1024.0f64 / 768.0) as f32);
        // The centre pixel looks straight ahead.
        let ray = cam.mouse_ray(512, 384);
        assert_eq!((ray.x, ray.y, ray.z), (0.0, 0.0, -1.0));
        // A projected point comes back to its pixel.
        let dir = cam.unproject(100.0, 50.0);
        let world = Vector::new(dir.x * 30.0, dir.y * 30.0, dir.z * 30.0);
        let (x, y, _) = cam.project(world).unwrap();
        assert!((x - 100.0).abs() < 1e-3 && (y - 50.0).abs() < 1e-3);
        assert!(cam.project(Vector::new(0.0, 0.0, 1.0)).is_none());
    }

    #[test]
    fn hor_plus_keeps_the_scale_of_a_4_by_3_surface_of_the_same_height() {
        let narrow = Camera::win3d(Surface::new(1024, 768));
        let wide = Camera::win3d(Surface::new(1366, 768));
        assert!((wide.x_scale - narrow.x_scale).abs() < 0.01);
        assert!((wide.y_scale - narrow.y_scale).abs() < 0.01);
        let mut off = Surface::new(1366, 768);
        off.widescreen = false;
        let vert_minus = Camera::win3d(off);
        assert!(vert_minus.x_scale > wide.x_scale + 100.0);
    }
}
