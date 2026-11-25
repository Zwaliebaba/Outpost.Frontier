//! Frame helpers of Common the object model uses: `LookMatrix`
//! (`common_csys.cpp:207`), `RandomVector` (`common_misc.cpp:481`),
//! `random_inside_sphere` (`common_misc.cpp:527`), `random_inside_ring`
//! (`common_misc.cpp:601`) and
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

/// `random_inside_sphere(radius)` (`Common/common_misc.cpp:527`): three
/// draws; the radius is `u1^(1/3) * radius` (stored), `cos(theta) = 2 u2 -
/// 1` (stored; the sine is taken of the unrounded value's arc cosine),
/// `phi = u3 * 2 pi`, and the result `(sin(phi) S, cos(theta) R', cos(phi)
/// S)` with `S = sin(theta) R'` stored.
#[must_use]
pub fn random_inside_sphere(radius: f32, rng: &mut CrtRand) -> Vector {
    // `GRandUnitScale` (1/32768) and `GRandTwoPi` (2 pi rounded through a
    // float).
    const UNIT: f64 = 3.051_757_812_5e-5;
    const TWO_PI: f64 = 6.283_185_482_025_146_5;
    let u1 = f64::from(rng.rand()) * UNIT;
    let scaled = r(u1.powf(1.0 / 3.0) * e(radius));
    let cos_raw = f64::from(rng.rand()) * UNIT * 2.0 - 1.0;
    let cos_theta = r(cos_raw);
    let sin_scaled = r(cos_raw.acos().sin() * e(scaled));
    let phi = f64::from(rng.rand()) * UNIT * TWO_PI;
    Vector::new(
        r(phi.sin() * e(sin_scaled)),
        r(e(cos_theta) * e(scaled)),
        r(phi.cos() * e(sin_scaled)),
    )
}

/// `random_inside_ring(outer, inner, half_height)`
/// (`Common/common_misc.cpp:601`): three draws; the radius
/// `sqrt(u1) * (outer - inner) + inner` (stored), the height
/// `(h - (-h)) * u2 + (-h)` and `phi = u3 * 2 pi`; the result
/// `(sin(phi) R, height, cos(phi) R)` around the y axis. The x87 `FSIN` and
/// `FCOS` are `f64`'s (D879).
#[must_use]
pub fn random_inside_ring(outer: f32, inner: f32, half_height: f32, rng: &mut CrtRand) -> Vector {
    const UNIT: f64 = 3.051_757_812_5e-5;
    const TWO_PI: f64 = 6.283_185_482_025_146_5;
    let u1 = f64::from(rng.rand()) * UNIT;
    let scaled = r(u1.sqrt() * (e(outer) - e(inner)) + e(inner));
    let neg_half = -half_height;
    let t = f64::from(rng.rand()) * UNIT;
    let phi = f64::from(rng.rand()) * UNIT * TWO_PI;
    Vector::new(
        r(phi.sin() * e(scaled)),
        r((e(half_height) - e(neg_half)) * t + e(neg_half)),
        r(phi.cos() * e(scaled)),
    )
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
    fn random_inside_sphere_draws_three_and_stays_inside() {
        let mut rng = CrtRand::with_seed(7);
        let mut copy = rng.clone();
        let v = random_inside_sphere(5.0, &mut rng);
        for _ in 0..3 {
            let _ = copy.rand();
        }
        assert_eq!(rng, copy);
        let len = (e(v.x) * e(v.x) + e(v.y) * e(v.y) + e(v.z) * e(v.z)).sqrt();
        assert!(len <= 5.0 + 1e-5, "{len}");
        assert_eq!(random_inside_sphere(0.0, &mut rng), Vector::ZERO);
    }

    #[test]
    fn random_vector_has_the_radius() {
        let mut rng = CrtRand::with_seed(7);
        let v = random_vector(2.0, &mut rng);
        let len = (e(v.x) * e(v.x) + e(v.y) * e(v.y) + e(v.z) * e(v.z)).sqrt();
        assert!((len - 2.0).abs() < 1e-5);
    }
}
