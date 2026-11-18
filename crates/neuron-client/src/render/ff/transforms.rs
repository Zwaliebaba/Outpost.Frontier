//! Transforms as DX8RENDERER keeps them (`FDx8RendererTransformCache` and
//! the transform methods in `CppSource/DX8RENDERER/DX8RENDERER.cpp`).
//!
//! Each transform is stored as the 16 floats of a `D3DMATRIX`. The callers
//! pass two shapes: the engine's 12-float affine transform (a 3x3 rotation
//! then a translation, `neuron_core::math::Transform`), which is stored
//! transposed into the rotation part with the translation in the fourth
//! row, and a 16-float matrix, which is stored transposed. The getters undo
//! exactly those moves, so a value read back is the value stored, bit for
//! bit.
//!
//! The view transform carries a Z flip (a product with `diag(1, 1, -1)` on
//! the left): `set_view` stores the flipped matrix and `view` flips it back.
//! `set_modelview` puts the flip into the world transform while the
//! pipeline property `VIEWSPACE_LIGHTS` is on, and the first time it runs
//! it sets an identity view, flipped while that property is off.

use std::collections::BTreeMap;
use std::fmt;

use neuron_core::math::{Transform, mul_transform};

use super::state::STAGES;

/// A `D3DMATRIX`: row-major, row vectors.
pub type Matrix16 = [f32; 16];

/// The engine's affine transform as 12 floats.
pub type Affine12 = [f32; 12];

const IDENTITY12: Affine12 = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// The 4x4 identity, Direct3D's default for every transform.
pub const IDENTITY: Matrix16 = [
    1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0,
];

/// `StoreAffine`.
#[must_use]
pub fn store_affine(m: &Affine12) -> Matrix16 {
    [
        m[0], m[3], m[6], 0.0, //
        m[1], m[4], m[7], 0.0, //
        m[2], m[5], m[8], 0.0, //
        m[9], m[10], m[11], 1.0,
    ]
}

/// `LoadAffine`.
#[must_use]
pub fn load_affine(v: &Matrix16) -> Affine12 {
    [
        v[0], v[4], v[8], v[1], v[5], v[9], v[2], v[6], v[10], v[12], v[13], v[14],
    ]
}

/// `StoreTransposed` (and `LoadTransposed`, which is the same move).
#[must_use]
pub fn transpose16(m: &Matrix16) -> Matrix16 {
    let mut out = [0.0; 16];
    for row in 0..4 {
        for col in 0..4 {
            out[col * 4 + row] = m[row * 4 + col];
        }
    }
    out
}

/// `MulZFlip12`: `diag(1, 1, -1)` times `m` through the math engine's
/// `MulTransform`.
#[must_use]
pub fn z_flip(m: &Affine12) -> Affine12 {
    let flip = Transform::from_words([1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, -1.0, 0.0, 0.0, 0.0]);
    mul_transform(&flip, &Transform::from_words(*m)).to_words()
}

/// Why `set_perspective` refused its arguments (`GR_GENERIC`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PerspectiveError {
    /// The near plane is not above 0.
    Near,
    /// The far plane is not above 0.
    Far,
    /// The planes are equal.
    EqualPlanes,
}

impl fmt::Display for PerspectiveError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(match self {
            Self::Near => "the near plane is not above zero",
            Self::Far => "the far plane is not above zero",
            Self::EqualPlanes => "the near and far planes are equal",
        })
    }
}

impl std::error::Error for PerspectiveError {}

/// The world, view, projection, texture and palette transforms.
#[derive(Clone, Debug)]
pub struct Transforms {
    world: Matrix16,
    view: Matrix16,
    projection: Matrix16,
    /// `View.bApplyMarker`: whether `set_modelview` has set the view.
    view_set: bool,
    textures: [Matrix16; STAGES],
    /// `SetWorldN`, by index: the bone palette.
    palette: BTreeMap<u32, Matrix16>,
}

impl Transforms {
    /// `ApplyIdentityTransforms` and the texture identities of
    /// `ApplyDefaultTextureStages`, as `CreateBuffers` leaves them.
    #[must_use]
    pub fn new(viewspace_lights: bool) -> Transforms {
        let mut t = Transforms {
            world: [0.0; 16],
            view: [0.0; 16],
            projection: [0.0; 16],
            view_set: false,
            textures: [store_affine(&IDENTITY12); STAGES],
            palette: BTreeMap::new(),
        };
        t.apply_identity(viewspace_lights);
        t
    }

    /// `ApplyIdentityTransforms`: identity view (flipped while
    /// `VIEWSPACE_LIGHTS` is off), identity world (flipped while it is on),
    /// identity projection; the view counts as set.
    pub fn apply_identity(&mut self, viewspace_lights: bool) {
        let flipped = store_affine(&z_flip(&IDENTITY12));
        let plain = store_affine(&IDENTITY12);
        self.view = if viewspace_lights { plain } else { flipped };
        self.world = if viewspace_lights { flipped } else { plain };
        self.projection = plain;
        self.view_set = true;
    }

    /// `ApplyDefaultTextureStages`'s part: every stage transform identity.
    pub fn reset_texture_transforms(&mut self) {
        self.textures = [store_affine(&IDENTITY12); STAGES];
    }

    pub fn set_world(&mut self, m: &Affine12) {
        self.world = store_affine(m);
    }

    #[must_use]
    pub fn world(&self) -> Affine12 {
        load_affine(&self.world)
    }

    pub fn set_view(&mut self, m: &Affine12) {
        self.view = store_affine(&z_flip(m));
    }

    /// `GetView`: the stored view flipped back.
    #[must_use]
    pub fn view(&self) -> Affine12 {
        z_flip(&load_affine(&self.view))
    }

    /// `SetModelview`.
    pub fn set_modelview(&mut self, m: &Affine12, viewspace_lights: bool) {
        self.world = store_affine(&if viewspace_lights { z_flip(m) } else { *m });
        if !self.view_set {
            self.view = store_affine(&if viewspace_lights {
                IDENTITY12
            } else {
                z_flip(&IDENTITY12)
            });
            self.view_set = true;
        }
    }

    /// `GetModelview`: view times world, each with its stored flip undone.
    #[must_use]
    pub fn modelview(&self, viewspace_lights: bool) -> Affine12 {
        let mut world = load_affine(&self.world);
        if viewspace_lights {
            world = z_flip(&world);
        }
        let mut view = load_affine(&self.view);
        if !viewspace_lights {
            view = z_flip(&view);
        }
        mul_transform(&Transform::from_words(view), &Transform::from_words(world)).to_words()
    }

    pub fn set_projection(&mut self, m: &Matrix16) {
        self.projection = transpose16(m);
    }

    #[must_use]
    pub fn projection(&self) -> Matrix16 {
        transpose16(&self.projection)
    }

    /// `SetOrtho`: the differences in `f64`, each element rounded once
    /// (decision T11), written untransposed.
    pub fn set_ortho(&mut self, left: f32, right: f32, bottom: f32, top: f32, near: f32, far: f32) {
        let (l, r, b, t, n, f) = (
            f64::from(left),
            f64::from(right),
            f64::from(bottom),
            f64::from(top),
            f64::from(near),
            f64::from(far),
        );
        let width = r - l;
        let height = t - b;
        let depth = f - n;
        let mut m = [0.0f32; 16];
        m[0] = (2.0 / width) as f32;
        m[5] = (2.0 / height) as f32;
        m[10] = (2.0 / depth) as f32;
        m[12] = (-(r + l) / width) as f32;
        m[13] = (-(b + t) / height) as f32;
        m[14] = (-n / depth) as f32;
        m[15] = 1.0;
        self.projection = m;
    }

    /// `SetPerspective`, written untransposed. The tangent goes through the
    /// standard library (Tier B, decision T11).
    pub fn set_perspective(
        &mut self,
        fov_degrees: f32,
        aspect: f32,
        near: f32,
        far: f32,
    ) -> Result<(), PerspectiveError> {
        if near <= 0.0 {
            return Err(PerspectiveError::Near);
        }
        if far <= 0.0 {
            return Err(PerspectiveError::Far);
        }
        if far == near {
            return Err(PerspectiveError::EqualPlanes);
        }
        let cot = (1.0 / (std::f64::consts::PI / 180.0 * f64::from(fov_degrees)).tan()) as f32;
        let inv_depth = (1.0 / (f64::from(far) - f64::from(near))) as f32;
        let mut m = [0.0f32; 16];
        m[0] = (f64::from(cot) / f64::from(aspect)) as f32;
        m[5] = cot;
        m[10] = (f64::from(far) * f64::from(inv_depth)) as f32;
        m[11] = 1.0;
        m[14] = (-f64::from(far) * f64::from(near) * f64::from(inv_depth)) as f32;
        self.projection = m;
        Ok(())
    }

    /// `SetTextureStageTransform`: stored transposed.
    pub fn set_texture(&mut self, stage: usize, m: &Matrix16) {
        self.textures[stage] = transpose16(m);
    }

    #[must_use]
    pub fn texture(&self, stage: usize) -> Matrix16 {
        transpose16(&self.textures[stage])
    }

    /// `SetWorldN`: palette entry `index`, stored as `StoreAffine` stores.
    pub fn set_world_n(&mut self, index: u32, m: &Affine12) {
        self.palette.insert(index, store_affine(m));
    }

    /// The matrices as the device would receive them, for the draw.
    #[must_use]
    pub fn world_matrix(&self) -> &Matrix16 {
        &self.world
    }

    #[must_use]
    pub fn view_matrix(&self) -> &Matrix16 {
        &self.view
    }

    #[must_use]
    pub fn projection_matrix(&self) -> &Matrix16 {
        &self.projection
    }

    #[must_use]
    pub fn texture_matrix(&self, stage: usize) -> &Matrix16 {
        &self.textures[stage]
    }

    #[must_use]
    pub fn palette_matrix(&self, index: u32) -> Option<&Matrix16> {
        self.palette.get(&index)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bits(m: &[f32]) -> Vec<u32> {
        m.iter().map(|x| x.to_bits()).collect()
    }

    const M: Affine12 = [
        0.5, -0.25, 2.0, 1.5, 3.0, -1.0, 0.75, 0.0, -2.5, 10.0, -20.0, 30.5,
    ];

    #[test]
    fn affine_and_matrix_stores_round_trip() {
        let mut t = Transforms::new(true);
        t.set_world(&M);
        assert_eq!(bits(&t.world()), bits(&M));
        let m16: Matrix16 = std::array::from_fn(|i| i as f32 - 3.5);
        t.set_projection(&m16);
        assert_eq!(bits(&t.projection()), bits(&m16));
        assert_eq!(t.projection_matrix()[1], m16[4]);
        t.set_texture(1, &m16);
        assert_eq!(bits(&t.texture(1)), bits(&m16));
        assert_eq!(store_affine(&M)[12..16], [10.0, -20.0, 30.5, 1.0]);
    }

    #[test]
    fn the_view_is_stored_flipped() {
        let mut t = Transforms::new(true);
        t.set_view(&M);
        assert_eq!(bits(&t.view()), bits(&M));
        // diag(1, 1, -1) on the left negates the rotation's third row and
        // the translation's z.
        let stored = load_affine(t.view_matrix());
        assert_eq!(stored[..6], M[..6]);
        assert_eq!(stored[6..9], [-M[6], -M[7], -M[8]]);
        assert_eq!(stored[9..11], M[9..11]);
        assert_eq!(stored[11], -M[11]);
    }

    #[test]
    fn modelview_follows_viewspace_lights() {
        // On: the world gets the flip, the view stays identity.
        let mut on = Transforms::new(true);
        on.view_set = false;
        on.set_modelview(&M, true);
        assert_eq!(load_affine(on.view_matrix()), IDENTITY12);
        assert_eq!(bits(&on.modelview(true)), bits(&M));
        // Off: the world is stored as given, the view gets the flip.
        let mut off = Transforms::new(false);
        off.view_set = false;
        off.set_modelview(&M, false);
        assert_eq!(bits(&load_affine(off.world_matrix())), bits(&M));
        assert_eq!(load_affine(off.view_matrix())[8], -1.0);
        assert_eq!(bits(&off.modelview(false)), bits(&M));
    }

    #[test]
    fn identity_transforms_carry_the_flip_by_the_property() {
        let on = Transforms::new(true);
        assert_eq!(load_affine(on.world_matrix())[8], -1.0);
        assert_eq!(load_affine(on.view_matrix())[8], 1.0);
        let off = Transforms::new(false);
        assert_eq!(load_affine(off.world_matrix())[8], 1.0);
        assert_eq!(load_affine(off.view_matrix())[8], -1.0);
    }

    #[test]
    fn perspective_and_ortho_as_the_reference_builds_them() {
        let mut t = Transforms::new(true);
        assert_eq!(
            t.set_perspective(60.0, 1.0, 0.0, 10.0),
            Err(PerspectiveError::Near)
        );
        assert_eq!(
            t.set_perspective(60.0, 1.0, 1.0, -1.0),
            Err(PerspectiveError::Far)
        );
        assert_eq!(
            t.set_perspective(60.0, 1.0, 2.0, 2.0),
            Err(PerspectiveError::EqualPlanes)
        );
        // The C++ takes the tangent of the whole angle it is given.
        t.set_perspective(45.0, 2.0, 1.0, 101.0).unwrap();
        let p = t.projection_matrix();
        let cot = (1.0 / (std::f64::consts::PI / 180.0 * 45.0).tan()) as f32;
        assert_eq!(p[5], cot);
        assert!((p[5] - 1.0).abs() <= f32::EPSILON);
        assert_eq!(p[0], (f64::from(cot) / 2.0) as f32);
        assert_eq!(p[11], 1.0);
        assert_eq!(
            p[10],
            (101.0f64 * f64::from((1.0f64 / 100.0) as f32)) as f32
        );
        assert_eq!(
            p[14],
            (-101.0f64 * f64::from((1.0f64 / 100.0) as f32)) as f32
        );
        t.set_ortho(0.0, 640.0, 480.0, 0.0, 0.0, 1.0);
        let o = t.projection_matrix();
        assert_eq!(o[0], (2.0f64 / 640.0) as f32);
        assert_eq!(o[5], (2.0f64 / -480.0) as f32);
        assert_eq!(o[12], -1.0);
        assert_eq!(o[13], 1.0);
        assert_eq!(o[15], 1.0);
    }
}
