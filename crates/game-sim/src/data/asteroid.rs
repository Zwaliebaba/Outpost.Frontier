//! Asteroid field ids and cube orientations (`CmnAsteroid`), ported from
//! `CppSource/Common/common_casteroid.cpp`.
//!
//! Asteroid fields are filled with cubes on a 128-unit grid; every cube and
//! every rock in it has an id packed from its position, which client and
//! server must compute alike (Tier A). Each cube is turned by a snap
//! rotation drawn from the CRT generator seeded with the cube id, so the
//! draw sequence is Tier A while the rotation itself goes through sine and
//! cosine (Tier B).
//!
//! The field and cube runtime (`CAsteroidField`, `CAsteroidCube`,
//! `CAsteroidSystem`) belongs with the objects (P2.4).

use neuron_core::math::Matrix;
use neuron_core::num::trunc_to_i32_via_i64;

/// `CmnAsteroid::closest_cube_coord`: the centre of the cube a coordinate
/// falls in. The C++ divides by zero for a cube size of 0; the port returns
/// `-half_size` there (divergence D113).
#[must_use]
pub fn closest_cube_coord(coord: i32, cube_size: i32, half_size: i32) -> i32 {
    let snapped = coord
        .checked_div(cube_size)
        .map_or(0, |q| q.wrapping_mul(cube_size));
    if coord > 0 {
        snapped.wrapping_add(half_size)
    } else {
        snapped.wrapping_sub(half_size)
    }
}

/// `NudgeTruncate`: 0.1 away from zero (towards minus for 0 and NaN), then
/// truncated through 64 bits.
fn nudge_truncate(v: f32) -> i32 {
    let nudged = if v > 0.0 {
        f64::from(v) + f64::from(0.1_f32)
    } else {
        f64::from(v) - f64::from(0.1_f32)
    };
    trunc_to_i32_via_i64(nudged)
}

/// `CmnAsteroid::compute_cube_id`: the cube id of a position, 0 outside the
/// packable range.
#[must_use]
pub fn compute_cube_id(pos: [f32; 3]) -> u32 {
    let [x, y, z] = pos.map(nudge_truncate);
    let mut grid_x = x.unsigned_abs() >> 7;
    let mut grid_y = y.unsigned_abs() >> 7;
    let mut grid_z = z.unsigned_abs() >> 7;
    if grid_x >= 0x400 || grid_y >= 0x200 || grid_z >= 0x400 {
        return 0;
    }
    if x < 0 {
        grid_x |= 0x400;
    }
    if z < 0 {
        grid_z |= 0x400;
    }
    if y < 0 {
        grid_y |= 0x200;
    }
    (grid_z << 10) | grid_y | (grid_x << 0x15)
}

/// `CAsteroidCube::compute_asteroid_id`: the id of a rock at an offset in
/// its cube, `None` outside the packable range.
#[must_use]
pub fn compute_asteroid_id(offset: [f32; 3]) -> Option<u32> {
    let [x, y, z] = offset.map(nudge_truncate);
    let mut id_x = x.unsigned_abs() >> 1;
    let mut id_y = y.unsigned_abs() >> 1;
    let mut id_z = z.unsigned_abs() >> 1;
    if id_x >= 0x200 || id_y >= 0x200 || id_z >= 0x200 {
        return None;
    }
    if x < 0 {
        id_x |= 0x200;
    }
    if y < 0 {
        id_y |= 0x200;
    }
    if z < 0 {
        id_z |= 0x200;
    }
    Some(id_z | (id_y << 10) | (id_x << 0x14))
}

/// `AxisRotations`: four preset angles (radians) per axis.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AxisRotations {
    /// `Angles[axis][index]`.
    pub angles: [[f32; 4]; 3],
}

impl Default for AxisRotations {
    /// The `CAsteroidField` default: 0, pi/2, pi and 3pi/2 on every axis.
    fn default() -> Self {
        let snap = [
            0.0,
            core::f32::consts::FRAC_PI_2,
            core::f32::consts::PI,
            3.0 * core::f32::consts::FRAC_PI_2,
        ];
        Self { angles: [snap; 3] }
    }
}

/// The C runtime generator step `NextLcgRaw`, on a local seed.
fn next(state: &mut u32) -> u32 {
    *state = state.wrapping_mul(214_013).wrapping_add(2_531_011);
    (*state >> 16) & 0x7FFF
}

/// What [`compute_cube_orientation`] drew: whether the cube is filled and
/// the three preset indices.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct CubeDraw {
    /// The cube is filled (`compute_cube_orientation` returned true).
    pub filled: bool,
    /// The preset index per axis (0 to 3); zeros for an empty cube.
    pub indices: [usize; 3],
}

/// The draws of `compute_cube_orientation` without the rotation: 0 to 3
/// warm-up steps, the emptiness draw against `freq` (skipped for a negative
/// `freq`; an empty cube when the draw is below it), then one index per
/// axis.
#[must_use]
pub fn cube_draw(seed: u32, freq: f32) -> CubeDraw {
    let mut state = seed;
    let warm = (next(&mut state) >> 13) & 3;
    for _ in 0..warm {
        state = state.wrapping_mul(214_013).wrapping_add(2_531_011);
    }
    if freq >= 0.0 {
        let probability = f64::from(next(&mut state)) * f64::from(1.0_f32 / 32768.0);
        if probability < f64::from(freq) {
            return CubeDraw {
                filled: false,
                indices: [0; 3],
            };
        }
    }
    let mut indices = [0usize; 3];
    for index in &mut indices {
        let draw = trunc_to_i32_via_i64(f64::from(next(&mut state)) * f64::from(4.0_f32 / 32768.0));
        *index = usize::try_from(draw.clamp(0, 3)).unwrap_or(0);
    }
    CubeDraw {
        filled: true,
        indices,
    }
}

/// `CmnAsteroid::compute_cube_orientation`: `false` for an empty cube
/// (the matrix is left alone); otherwise the three preset rotations are
/// applied to `orient` in place, rows mixed in the reference's order.
pub fn compute_cube_orientation(
    seed: u32,
    orient: &mut Matrix,
    rotations: &AxisRotations,
    freq: f32,
) -> bool {
    let draw = cube_draw(seed, freq);
    if !draw.filled {
        return false;
    }
    let angle = |axis: usize| f64::from(rotations.angles[axis][draw.indices[axis]]);
    let m = &mut orient.m;
    // Each step mixes two rows; the cosine is stored to a float, the sine
    // stays unrounded.
    let steps: [(usize, usize, f64); 3] = [(1, 2, 1.0), (0, 2, -1.0), (0, 1, 1.0)];
    for (axis, &(first, second, sign)) in steps.iter().enumerate() {
        let c = f64::from(angle(axis).cos() as f32);
        let s = angle(axis).sin();
        for col in 0..3 {
            let a = f64::from(m[first][col]);
            let b = f64::from(m[second][col]);
            if sign > 0.0 {
                m[first][col] = (a * c - b * s) as f32;
                m[second][col] = (b * c + a * s) as f32;
            } else {
                m[first][col] = (a * c + b * s) as f32;
                m[second][col] = (b * c - a * s) as f32;
            }
        }
    }
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cube_ids() {
        assert_eq!(compute_cube_id([0.0, 0.0, 0.0]), 0);
        assert_eq!(compute_cube_id([128.0, 0.0, 0.0]), 1 << 0x15);
        assert_eq!(compute_cube_id([-128.0, 0.0, 0.0]), (0x401) << 0x15);
        assert_eq!(compute_cube_id([0.0, -127.95, 0.0]), 0x201);
        assert_eq!(compute_cube_id([1.0e9, 0.0, 0.0]), 0);
        assert_eq!(
            compute_asteroid_id([2.0, 0.0, -2.0]),
            Some(0x201 | (1 << 0x14))
        );
        assert_eq!(compute_asteroid_id([1024.0, 0.0, 0.0]), None);
        assert_eq!(closest_cube_coord(300, 256, 128), 384);
        assert_eq!(closest_cube_coord(-300, 256, 128), -384);
    }

    #[test]
    fn orientation_draws() {
        let mut orient = Matrix::IDENTITY;
        let draw = cube_draw(12345, 0.0);
        assert!(draw.filled);
        assert!(compute_cube_orientation(
            12345,
            &mut orient,
            &AxisRotations::default(),
            0.0
        ));
        assert!(!cube_draw(12345, 2.0).filled);
        let mut untouched = Matrix::IDENTITY;
        assert!(!compute_cube_orientation(
            1,
            &mut untouched,
            &AxisRotations::default(),
            2.0
        ));
        assert_eq!(untouched, Matrix::IDENTITY);
    }
}
