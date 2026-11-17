//! Frame helpers of Common the object model uses: `LookMatrix`
//! (`common_csys.cpp:207`), `RandomVector` (`common_misc.cpp:481`) and
//! `RotateWorldToObjectSpace` (`common_cobject_internal.h`).

use neuron_core::math::{Matrix, Vector, transpose_transform};
use neuron_core::rng::CrtRand;

fn e(v: f32) -> f64 {
    f64::from(v)
}

fn r(v: f64) -> f32 {
    v as f32
}

/// `LookMatrix(direction)`: an orientation whose −Z column points along
/// `direction` (the third column is minus the normalised direction); a
/// direction shorter than 1e-4 looks along (1, 0, 0).
#[must_use]
pub fn look_matrix(direction: Vector) -> Matrix {
    let (x, y, z) = (e(direction.x), e(direction.y), e(direction.z));
    let length = ((x * x + y * y) + z * z).sqrt();
    let (look_x, look_y, look_z) = if length > 1e-4 {
        let inv = -1.0 / length;
        (r(inv * x), r(inv * y), r(inv * z))
    } else {
        (1.0f32, 0.0f32, 0.0f32)
    };
    let (side_x, side_y) = if !(e(look_x).abs() < 1e-4) || !(e(look_y).abs() < 1e-4) {
        (-e(look_x), e(look_y))
    } else {
        (0.0, 1.0)
    };
    let c0 = r(e(look_y) * 0.0 - e(look_z) * side_x);
    let c1 = r(e(look_z) * side_y - e(look_x) * 0.0);
    let c2 = r(side_x * e(look_x) - side_y * e(look_y));
    let inv_n = r(1.0 / ((e(c2) * e(c2) + e(c1) * e(c1)) + e(c0) * e(c0)).sqrt());
    let c0n = e(c0) * e(inv_n);
    let c1n = e(c1) * e(inv_n);
    let c2n = r(e(c2) * e(inv_n));
    let mut m = [[0.0f32; 3]; 3];
    m[2][2] = look_z;
    m[2][1] = c2n;
    m[0][0] = r(e(look_z) * c1n - e(c2n) * e(look_y));
    m[1][0] = r(e(c2n) * e(look_x) - e(look_z) * c0n);
    m[2][0] = r(c0n * e(look_y) - c1n * e(look_x));
    m[0][1] = r(c0n);
    m[1][1] = r(c1n);
    m[0][2] = look_x;
    m[1][2] = look_y;
    Matrix::from_rows(m)
}

/// `RandomVector(radius)`: three `rand() - 0x3fff` draws scaled to the
/// radius (no rejection; three equal draws of 0x3fff divide by zero, as in
/// the reference).
#[must_use]
pub fn random_vector(radius: f32, rng: &mut CrtRand) -> Vector {
    let x = (rng.rand() - 0x3fff) as f32;
    let y = (rng.rand() - 0x3fff) as f32;
    let z = f64::from(rng.rand() - 0x3fff);
    let scale = e(radius) / (z * z + e(y) * e(y) + e(x) * e(x)).sqrt();
    Vector::new(r(e(x) * scale), r(e(y) * scale), r(scale * z))
}

/// `RotateWorldToObjectSpace(orientation, v)`: the transposed orientation
/// applied to `v` (`TransformMatrix` of the transpose).
#[must_use]
pub fn rotate_world_to_object(orientation: &Matrix, v: Vector) -> Vector {
    transpose_transform(orientation, v)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn look_matrix_points_minus_z_along_the_direction() {
        let m = look_matrix(Vector::new(0.0, 0.0, -10.0));
        assert_eq!(m, Matrix::IDENTITY);
        let m = look_matrix(Vector::new(1.0, 0.0, 0.0));
        assert_eq!((m.m[0][2], m.m[1][2], m.m[2][2]), (-1.0, 0.0, 0.0));
        let degenerate = look_matrix(Vector::ZERO);
        assert_eq!(degenerate.m[0][2], 1.0);
    }

    #[test]
    fn random_vector_has_the_radius() {
        let mut rng = CrtRand::with_seed(7);
        let v = random_vector(2.0, &mut rng);
        let len = (e(v.x) * e(v.x) + e(v.y) * e(v.y) + e(v.z) * e(v.z)).sqrt();
        assert!((len - 2.0).abs() < 1e-5);
    }
}
