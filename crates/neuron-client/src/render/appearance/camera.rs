//! The camera FLAppearance draws the backdrop and the starsphere through:
//! `ICamera` as [`Camera`], and `FCameraBlob`, the wrapper that borrows the
//! caller's camera, as [`CameraBlob`] bound to it in [`BoundCamera`].
//!
//! The wrapper either replaces the depth range with its own (1 to 200,000,
//! "fixed depth", the backdrop) and classifies spheres against its own side
//! planes, or replaces the position, orientation and view transform with
//! its own (the camera's orientation at the origin, the starsphere). The
//! reference keeps the wrapper in the appearance between draws; so does
//! [`super::Appearance`].

use neuron_core::math::{Matrix, Vector, transpose_transform};

use crate::render::mesh::MeshCamera;

/// The near plane of the fixed depth range (`kCameraNearConst`).
pub const FIXED_NEAR: f32 = 1.0;
/// The far plane of the fixed depth range (`kCameraFarConst`).
pub const FIXED_FAR: f32 = 200_000.0;

/// `ICamera`, the slots 3 to 16 `FCameraBlob` has too, one method per
/// slot. Pointers the reference returns are returned by value.
pub trait Camera {
    /// `GetPosition`.
    fn position(&mut self) -> [f32; 3];
    /// `GetOrientation`: nine floats, row-major axes.
    fn orientation(&mut self) -> [f32; 9];
    /// `GetViewport`: left, top, right, bottom.
    fn viewport(&mut self) -> [i32; 4];
    /// `GetHalfFovXDeg`.
    fn half_fov_x_deg(&mut self) -> f32;
    /// `GetFovYDeg`.
    fn fov_y_deg(&mut self) -> f32;
    /// `GetZNear`.
    fn z_near(&mut self) -> f32;
    /// `GetZFar`.
    fn z_far(&mut self) -> f32;
    /// `GetAspect`.
    fn aspect(&mut self) -> f32;
    /// `GetXScaleOverNear` (an unrounded x87 result: `f64`).
    fn x_scale_over_near(&mut self) -> f64;
    /// `GetYScaleOverNear`.
    fn y_scale_over_near(&mut self) -> f64;
    /// `Project(&x, &y, &z, world)`: the result; `out` keeps what the
    /// camera does not write.
    fn project(&mut self, world: [f32; 3], out: &mut [f32; 3]) -> i32;
    /// `Unproject(out, x, y)`.
    fn unproject(&mut self, out: &mut [f32; 3], x: f32, y: f32);
    /// `ClassifySphere(centre, radius)`: 1 outside, 3 straddling, 4 inside.
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32;
    /// `GetViewTransform`.
    fn view_transform(&mut self) -> [f32; 12];
}

/// A [`Camera`] as the render manager reads it ([`MeshCamera`]).
pub struct MeshView<'a>(pub &'a mut dyn Camera);

impl MeshCamera for MeshView<'_> {
    fn view_transform(&mut self) -> [f32; 12] {
        self.0.view_transform()
    }

    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        self.0.classify_sphere(centre, radius)
    }

    fn half_fov_x_deg(&mut self) -> f32 {
        self.0.half_fov_x_deg()
    }

    fn viewport(&mut self) -> [i32; 4] {
        self.0.viewport()
    }

    fn x_scale_over_near(&mut self) -> f64 {
        self.0.x_scale_over_near()
    }

    fn z_near(&mut self) -> f32 {
        self.0.z_near()
    }

    fn position(&mut self) -> [f32; 3] {
        self.0.position()
    }
}

const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// `FCameraBlob`'s own state (the inner camera is borrowed per draw, see
/// [`BoundCamera`]).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CameraBlob {
    /// Byte 8: the fixed depth range and the wrapper's own sphere test.
    pub fixed_depth: bool,
    /// Byte 9: the wrapper's own position, orientation and view transform.
    pub own_transform: bool,
    /// The orientation (nine floats) and the position (three) it answers
    /// with its own transform (+0x0C).
    pub transform: [f32; 12],
    /// The view transform it answers with its own transform (+0x3C).
    pub view: [f32; 12],
    /// The side-plane axis of the sphere test across x (+0x6C).
    pub right: [f32; 3],
    /// The side-plane axis across y (+0x78).
    pub up: [f32; 3],
}

impl Default for CameraBlob {
    fn default() -> Self {
        CameraBlob::new()
    }
}

impl CameraBlob {
    /// As constructed: identity transforms, the axes x and y, no flag.
    #[must_use]
    pub fn new() -> CameraBlob {
        CameraBlob {
            fixed_depth: false,
            own_transform: false,
            transform: IDENTITY,
            view: IDENTITY,
            right: [1.0, 0.0, 0.0],
            up: [0.0, 1.0, 0.0],
        }
    }

    /// `SetOrientationAndTranslation`: nine floats, then three.
    pub fn set_orientation_and_translation(
        &mut self,
        orientation: [f32; 9],
        translation: [f32; 3],
    ) {
        self.transform[..9].copy_from_slice(&orientation);
        self.transform[9..].copy_from_slice(&translation);
    }

    /// `BuildInverseTransform`: the transposed orientation with the
    /// position taken through it and negated (`TransposeTransform` of the
    /// 3D math engine).
    #[must_use]
    pub fn inverse_transform(&self) -> [f32; 12] {
        let a = &self.transform;
        let rotation = Matrix::from_words([a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]]);
        let dest = transpose_transform(&rotation, Vector::new(a[9], a[10], a[11]));
        [
            a[0], a[3], a[6], a[1], a[4], a[7], a[2], a[5], a[8], -dest.x, -dest.y, -dest.z,
        ]
    }
}

/// The wrapper bound to the caller's camera for one draw (`BindArg` to
/// `ClearArg`): `FCameraBlob`'s camera slots.
pub struct BoundCamera<'a> {
    /// The wrapper's state.
    pub blob: &'a mut CameraBlob,
    /// The caller's camera.
    pub inner: &'a mut dyn Camera,
}

fn axis_length(axis: &[f32; 3]) -> f64 {
    let x = f64::from(axis[0]);
    let y = f64::from(axis[1]);
    let z = f64::from(axis[2]);
    ((x * x + y * y) + z * z).sqrt()
}

fn normalize(axis: &mut [f32; 3]) {
    let scale = f64::from(FIXED_NEAR) / axis_length(axis);
    for value in axis.iter_mut() {
        *value = (scale * f64::from(*value)) as f32;
    }
}

impl BoundCamera<'_> {
    /// `FillLocalAxes`: the side-plane axes from the inner camera's
    /// viewport width, the wrapper's x scale and its aspect.
    pub fn fill_local_axes(&mut self) {
        let viewport = self.inner.viewport();
        let width = viewport[2].wrapping_sub(viewport[0]);
        let half_width = (f64::from(width) * 0.5) as f32;
        let horizontal = (f64::from(half_width) / self.x_scale_over_near()) as f32;
        let vertical = (f64::from(horizontal) / f64::from(self.aspect())) as f32;
        self.blob.right = [1.0, 0.0, horizontal];
        normalize(&mut self.blob.right);
        self.blob.up = [0.0, 1.0, vertical];
        normalize(&mut self.blob.up);
    }

    /// `GetXScaleOverNear` and `GetYScaleOverNear`: the inner result
    /// rounded to a float, times the inner near plane under the fixed
    /// depth range.
    fn scale_over_near(&mut self, first: f64) -> f64 {
        let first = first as f32;
        if !self.blob.fixed_depth {
            return f64::from(first);
        }
        f64::from(self.inner.z_near()) * f64::from(first)
    }
}

impl Camera for BoundCamera<'_> {
    fn position(&mut self) -> [f32; 3] {
        if self.blob.own_transform {
            return [
                self.blob.transform[9],
                self.blob.transform[10],
                self.blob.transform[11],
            ];
        }
        self.inner.position()
    }

    fn orientation(&mut self) -> [f32; 9] {
        if self.blob.own_transform {
            let mut out = [0.0; 9];
            out.copy_from_slice(&self.blob.transform[..9]);
            return out;
        }
        self.inner.orientation()
    }

    fn viewport(&mut self) -> [i32; 4] {
        self.inner.viewport()
    }

    fn half_fov_x_deg(&mut self) -> f32 {
        self.inner.half_fov_x_deg()
    }

    fn fov_y_deg(&mut self) -> f32 {
        self.inner.fov_y_deg()
    }

    fn z_near(&mut self) -> f32 {
        if self.blob.fixed_depth {
            return FIXED_NEAR;
        }
        self.inner.z_near()
    }

    fn z_far(&mut self) -> f32 {
        if self.blob.fixed_depth {
            return FIXED_FAR;
        }
        self.inner.z_far()
    }

    fn aspect(&mut self) -> f32 {
        self.inner.aspect()
    }

    fn x_scale_over_near(&mut self) -> f64 {
        let first = self.inner.x_scale_over_near();
        self.scale_over_near(first)
    }

    fn y_scale_over_near(&mut self) -> f64 {
        let first = self.inner.y_scale_over_near();
        self.scale_over_near(first)
    }

    fn project(&mut self, world: [f32; 3], out: &mut [f32; 3]) -> i32 {
        self.inner.project(world, out)
    }

    fn unproject(&mut self, out: &mut [f32; 3], x: f32, y: f32) {
        self.inner.unproject(out, x, y);
    }

    /// Under the fixed depth range: the near and far planes, then the side
    /// planes through the wrapper's axes, in the reference's order of
    /// tests; otherwise the inner camera's.
    fn classify_sphere(&mut self, centre: [f32; 3], radius: f32) -> i32 {
        if !self.blob.fixed_depth {
            return self.inner.classify_sphere(centre, radius);
        }
        let near = f64::from(self.z_near());
        let far = f64::from(self.z_far());
        let r = f64::from(radius);
        let cx = f64::from(centre[0]);
        let cy = f64::from(centre[1]);
        let cz = f64::from(centre[2]);
        if r - near < cz {
            return 1;
        }
        let mut rank = 4;
        if !(r + cz < -near) {
            rank = 3;
        }
        if r + cz < -far {
            return 1;
        }
        if !(r - far < cz) {
            rank = 3;
        }
        let neg_r = f64::from(-radius);
        let right = self.blob.right;
        let sum_x = cx.abs() * f64::from(right[0]) + cz * f64::from(right[2]);
        if sum_x > r {
            return 1;
        }
        if !(sum_x < neg_r) {
            rank = 3;
        }
        let up = self.blob.up;
        let sum_y = cy.abs() * f64::from(up[1]) + cz * f64::from(up[2]);
        if sum_y > r {
            return 1;
        }
        if sum_y < neg_r {
            return rank;
        }
        3
    }

    fn view_transform(&mut self) -> [f32; 12] {
        if self.blob.own_transform {
            return self.blob.view;
        }
        self.inner.view_transform()
    }
}
