//! The value types of `x86math.h`: `FVector`, `FMatrix`, `FTransform` and
//! `FQuaternion`. Common's `Vector`, `Matrix`, `Transform` and `Quaternion`
//! (`common_ini.h`, `common_csys.h`, `common_physys.h`) have the same
//! layouts, so one set of types serves both.
//!
//! The types are `#[repr(C)]` with the C++ field order because they appear
//! in asset and wire formats. Each also converts to and from its words in
//! memory order, which is how the kernels address them.

/// Three `f32` components (`FVector`, 12 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector {
    /// +0x00.
    pub x: f32,
    /// +0x04.
    pub y: f32,
    /// +0x08.
    pub z: f32,
}

impl Vector {
    /// The zero vector.
    pub const ZERO: Self = Self::new(0.0, 0.0, 0.0);

    /// A vector from its components.
    #[must_use]
    pub const fn new(x: f32, y: f32, z: f32) -> Self {
        Self { x, y, z }
    }

    /// The components in memory order.
    #[must_use]
    pub const fn to_words(self) -> [f32; 3] {
        [self.x, self.y, self.z]
    }

    /// A vector from its components in memory order.
    #[must_use]
    pub const fn from_words(words: [f32; 3]) -> Self {
        Self::new(words[0], words[1], words[2])
    }
}

/// A 3x3 row-major matrix (`FMatrix`, 36 bytes). `m[row][col]` is the word
/// at `row * 3 + col`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Matrix {
    /// The rows.
    pub m: [[f32; 3]; 3],
}

impl Matrix {
    /// The identity (`MatrixSetIdentity` in `common_csys.h`).
    pub const IDENTITY: Self = Self::from_rows([[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]]);

    /// A matrix from its rows.
    #[must_use]
    pub const fn from_rows(m: [[f32; 3]; 3]) -> Self {
        Self { m }
    }

    /// The nine words in memory order.
    #[must_use]
    pub const fn to_words(self) -> [f32; 9] {
        let m = self.m;
        [
            m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2],
        ]
    }

    /// A matrix from its nine words in memory order.
    #[must_use]
    pub const fn from_words(w: [f32; 9]) -> Self {
        Self::from_rows([[w[0], w[1], w[2]], [w[3], w[4], w[5]], [w[6], w[7], w[8]]])
    }

    /// The transpose (`TransposeMatrix` in `x86math.cpp`): a copy, no
    /// arithmetic.
    #[must_use]
    pub const fn transpose(self) -> Self {
        let m = self.m;
        Self::from_rows([
            [m[0][0], m[1][0], m[2][0]],
            [m[0][1], m[1][1], m[2][1]],
            [m[0][2], m[1][2], m[2][2]],
        ])
    }
}

/// A rotation (or general linear part) followed by a translation
/// (`FTransform`, 48 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Transform {
    /// +0x00.
    pub rotation: Matrix,
    /// +0x24.
    pub translation: Vector,
}

impl Transform {
    /// The identity rotation and a zero translation.
    pub const IDENTITY: Self = Self::new(Matrix::IDENTITY, Vector::ZERO);

    /// A transform from its parts.
    #[must_use]
    pub const fn new(rotation: Matrix, translation: Vector) -> Self {
        Self {
            rotation,
            translation,
        }
    }

    /// The twelve words in memory order.
    #[must_use]
    pub const fn to_words(self) -> [f32; 12] {
        let r = self.rotation.to_words();
        let t = self.translation;
        [
            r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], t.x, t.y, t.z,
        ]
    }

    /// A transform from its twelve words in memory order.
    #[must_use]
    pub const fn from_words(w: [f32; 12]) -> Self {
        Self::new(
            Matrix::from_words([w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8]]),
            Vector::new(w[9], w[10], w[11]),
        )
    }
}

/// A quaternion with the scalar first (`FQuaternion`, 16 bytes).
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Quaternion {
    /// +0x00.
    pub w: f32,
    /// +0x04.
    pub x: f32,
    /// +0x08.
    pub y: f32,
    /// +0x0C.
    pub z: f32,
}

impl Default for Quaternion {
    /// The identity rotation.
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl Quaternion {
    /// The identity rotation.
    pub const IDENTITY: Self = Self::new(1.0, 0.0, 0.0, 0.0);

    /// A quaternion from its components, scalar first.
    #[must_use]
    pub const fn new(w: f32, x: f32, y: f32, z: f32) -> Self {
        Self { w, x, y, z }
    }

    /// The components in memory order (W, X, Y, Z).
    #[must_use]
    pub const fn to_words(self) -> [f32; 4] {
        [self.w, self.x, self.y, self.z]
    }

    /// A quaternion from its components in memory order.
    #[must_use]
    pub const fn from_words(w: [f32; 4]) -> Self {
        Self::new(w[0], w[1], w[2], w[3])
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layouts_match_the_cpp_value_types() {
        assert_eq!(size_of::<Vector>(), 0x0C);
        assert_eq!(size_of::<Matrix>(), 0x24);
        assert_eq!(size_of::<Transform>(), 0x30);
        assert_eq!(size_of::<Quaternion>(), 0x10);
        assert_eq!(core::mem::offset_of!(Transform, translation), 0x24);
        assert_eq!(core::mem::offset_of!(Quaternion, w), 0);
        assert_eq!(core::mem::offset_of!(Quaternion, z), 0x0C);
    }

    #[test]
    fn words_round_trip() {
        let t = Transform::from_words(core::array::from_fn(|i| i as f32));
        assert_eq!(t.rotation.m[1][2], 5.0);
        assert_eq!(t.translation.y, 10.0);
        assert_eq!(Transform::from_words(t.to_words()), t);
        let m = t.rotation;
        assert_eq!(m.transpose().m[2][1], m.m[1][2]);
        assert_eq!(m.transpose().transpose(), m);
    }
}
