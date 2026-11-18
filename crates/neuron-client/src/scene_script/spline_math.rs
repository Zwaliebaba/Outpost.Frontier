//! The spline arithmetic of `CppSource/SceneScript/SplineMath.h`: the
//! uniform Catmull-Rom point and tangent, the linear blend between two
//! samples and the sample parameter.
//!
//! Each helper of the header stores its vector result to `float`, so
//! every named intermediate below is rounded where the reference rounds
//! it (design/decisions.md T11); only the values the header keeps on the
//! x87 stack stay `f64`.

/// A three-component vector in memory order.
pub(super) type Vec3 = [f32; 3];

fn ext(v: f32) -> f64 {
    f64::from(v)
}

fn round(v: f64) -> f32 {
    v as f32
}

/// `Scale`, `ScaleStored`, `DoubleStored`: `factor * v`, rounded.
fn scale(v: Vec3, factor: f32) -> Vec3 {
    v.map(|c| round(ext(factor) * ext(c)))
}

/// `Add`, `AddStored`.
fn add(a: Vec3, b: Vec3) -> Vec3 {
    [0, 1, 2].map(|i| round(ext(a[i]) + ext(b[i])))
}

/// `Subtract`, `SubtractVector`.
fn sub(a: Vec3, b: Vec3) -> Vec3 {
    [0, 1, 2].map(|i| round(ext(a[i]) - ext(b[i])))
}

/// `Negate`, `NegateStored`.
fn neg(a: Vec3) -> Vec3 {
    a.map(|c| -c)
}

/// The quadratic coefficient both kernels share:
/// `((2 p0 - 5 p1) + 4 p2) - p3`.
fn quadratic(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3) -> Vec3 {
    let four_p2 = scale(p2, 4.0);
    let five_p1 = scale(p1, 5.0);
    let twice_p0 = add(p0, p0);
    sub(add(sub(twice_p0, five_p1), four_p2), p3)
}

/// The cubic coefficient both kernels share: `((3 p1 - p0) - 3 p2) + p3`.
fn cubic(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3) -> Vec3 {
    let three_p2 = scale(p2, 3.0);
    let three_p1 = scale(p1, 3.0);
    add(sub(add(three_p1, neg(p0)), three_p2), p3)
}

/// `CatmullRom3D`: `0.5 (2 p1 + (p2 - p0) u + q u^2 + c u^3)`, the two
/// powers of `u` rounded to `float`.
pub(super) fn catmull_rom_3d(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3, u: f32) -> Vec3 {
    let uu = ext(u) * ext(u);
    let u2 = round(uu);
    let u3 = round(uu * ext(u));
    let twice_p1 = scale(p1, 2.0);
    let delta = add(neg(p0), p2);
    let linear = scale(delta, u);
    let quad = scale(quadratic(p0, p1, p2, p3), u2);
    let cub = scale(cubic(p0, p1, p2, p3), u3);
    let sum = add(linear, add(quad, cub));
    let x = ext(twice_p1[0]) + ext(sum[0]);
    let y = ext(twice_p1[1]) + ext(sum[1]);
    let z = round(ext(twice_p1[2]) + ext(sum[2]));
    [round(x * 0.5), round(y * 0.5), round(ext(z) * 0.5)]
}

/// `CatmullRomTangent3D`: the derivative of [`catmull_rom_3d`] in `u`,
/// normalised (an unconditional division by its length, zero included).
pub(super) fn catmull_rom_tangent_3d(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3, u: f32) -> Vec3 {
    let delta = add(neg(p0), p2);
    let twice_u = ext(u) + ext(u);
    let quad = quadratic(p0, p1, p2, p3).map(|c| round(ext(c) * twice_u));
    let three_u2 = ext(u) * ext(u) * 3.0;
    let cub = cubic(p0, p1, p2, p3).map(|c| round(ext(c) * three_u2));
    let sum = add(delta, add(quad, cub));
    let half = sum.map(|c| round(ext(c) * 0.5));
    let (x, y, z) = (ext(half[0]), ext(half[1]), ext(half[2]));
    let inverse = 1.0 / ((x * x + y * y) + z * z).sqrt();
    half.map(|c| round(inverse * ext(c)))
}

/// `Lerp3D`: `a + (b - a) f`; the X product and the Z difference are
/// rounded, the Y terms are not.
pub(super) fn lerp_3d(a: Vec3, b: Vec3, fraction: f32) -> Vec3 {
    let f = ext(fraction);
    let dx = ext(b[0]) - ext(a[0]);
    let dy = ext(b[1]) - ext(a[1]);
    let dz = round(ext(b[2]) - ext(a[2]));
    let x_product = round(dx * f);
    [
        round(ext(a[0]) + ext(x_product)),
        round(ext(a[1]) + dy * f),
        round(ext(a[2]) + ext(dz) * f),
    ]
}

/// `SampleParameter`: the index, zero-extended, times the step.
pub(super) fn sample_parameter(index: i32, step: f32) -> f32 {
    round(f64::from(index as u32) * ext(step))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn catmull_rom_passes_through_its_inner_points() {
        let (p0, p1, p2, p3) = ([0.0; 3], [1.0, 2.0, 3.0], [4.0, 5.0, 6.0], [7.0; 3]);
        assert_eq!(catmull_rom_3d(p0, p1, p2, p3, 0.0), p1);
        assert_eq!(catmull_rom_3d(p0, p1, p2, p3, 1.0), p2);
        let tangent = catmull_rom_tangent_3d(p0, p1, p2, p3, 0.5);
        let length: f32 = tangent.iter().map(|c| c * c).sum();
        assert!((length - 1.0).abs() < 1e-6);
        assert_eq!(lerp_3d(p1, p2, 0.5), [2.5, 3.5, 4.5]);
        assert_eq!(sample_parameter(3, 0.0625), 0.1875);
    }
}
