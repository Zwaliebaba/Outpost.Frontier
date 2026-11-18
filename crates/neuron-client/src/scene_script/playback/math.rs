//! The arithmetic helpers of `SceneScriptScene.cpp` that the timeline
//! uses: the scene frame and its inverse, the math engine calls (through
//! `neuron_core::math`, the port of the same engine), the look-at frame
//! built from axis numbers, and the interpolations of the property
//! animations, each in the reference's operation order
//! (design/decisions.md T11).

use neuron_core::math::{self, Matrix, Quaternion, Transform, Vector};
use neuron_core::num;

pub(super) type Vec3 = [f32; 3];
pub(super) type Mat9 = [f32; 9];
pub(super) type Xform = [f32; 12];

pub(super) const IDENTITY9: Mat9 = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0];

pub(super) fn ext(v: f32) -> f64 {
    f64::from(v)
}

pub(super) fn round(v: f64) -> f32 {
    v as f32
}

/// `FLD dword` then `FSTP dword`: a signalling NaN comes out quiet.
pub(super) fn fld_fstp(v: f32) -> f32 {
    if v.is_nan() {
        f32::from_bits(v.to_bits() | 0x0040_0000)
    } else {
        v
    }
}

/// A coordinate system (`FSceneScriptCsys`): rotation, then position.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Csys {
    pub orient: Mat9,
    pub pos: Vec3,
}

impl Csys {
    /// The identity frame at the origin.
    pub const IDENTITY: Csys = Csys {
        orient: IDENTITY9,
        pos: [0.0; 3],
    };

    pub(super) fn words(&self) -> Xform {
        let mut out = [0.0; 12];
        out[..9].copy_from_slice(&self.orient);
        out[9..].copy_from_slice(&self.pos);
        out
    }
}

pub(super) fn pack(orient: &Mat9, pos: &Vec3) -> Xform {
    let mut out = [0.0; 12];
    out[..9].copy_from_slice(orient);
    out[9..].copy_from_slice(pos);
    out
}

/// `TransposeTransform` (+0x38): `transpose(m) * v`.
pub(super) fn transpose_transform(m: &Mat9, v: &Vec3) -> Vec3 {
    math::transpose_transform(&Matrix::from_words(*m), Vector::from_words(*v)).to_words()
}

/// `TransformXform` (+0x28).
pub(super) fn transform(xform: &Xform, v: &Vec3) -> Vec3 {
    math::transform_xform(&Transform::from_words(*xform), Vector::from_words(*v)).to_words()
}

/// `Rotate` (+0x30).
pub(super) fn rotate(xform: &Xform, v: &Vec3) -> Vec3 {
    math::rotate(&Transform::from_words(*xform), Vector::from_words(*v)).to_words()
}

/// `MulMatrix` (+0x20).
pub(super) fn mul_matrix(a: &Mat9, b: &Mat9) -> Mat9 {
    math::mul_matrix(&Matrix::from_words(*a), &Matrix::from_words(*b)).to_words()
}

/// `MulTransform` (+0x1C).
pub(super) fn mul_transform(a: &Xform, b: &Xform) -> Xform {
    math::mul_transform(&Transform::from_words(*a), &Transform::from_words(*b)).to_words()
}

/// `MatrixToQuaternion` (+0x50).
pub(super) fn matrix_to_quat(m: &Mat9) -> [f32; 4] {
    math::matrix_to_quaternion(&Matrix::from_words(*m)).to_words()
}

/// `QuaternionToMatrix` (+0x54).
pub(super) fn quat_to_matrix(q: &[f32; 4]) -> Mat9 {
    math::quaternion_to_matrix(&Quaternion::from_words(*q)).to_words()
}

/// `QuatSlerpValue` (+0x68).
pub(super) fn slerp(a: &[f32; 4], b: &[f32; 4], t: f32) -> [f32; 4] {
    math::quat_slerp(&Quaternion::from_words(*a), &Quaternion::from_words(*b), t).to_words()
}

/// `InvertParentCsys` and the inline inverse of the entity and event
/// consumers: the transposed rotation and minus the transposed
/// position. `InvertParentCsys` transposes through the x87 stack (all
/// words but one); the inline inverse copies raw words.
pub(super) fn invert_csys(c: &Csys, through_stack: bool) -> Xform {
    let t = transpose_transform(&c.orient, &c.pos);
    let neg = t.map(|v| -v);
    let o = &c.orient;
    let mut transposed = [o[0], o[3], o[6], o[1], o[4], o[7], o[2], o[5], o[8]];
    if through_stack {
        for (index, word) in transposed.iter_mut().enumerate() {
            if index != 1 {
                *word = fld_fstp(*word);
            }
        }
    }
    pack(&transposed, &neg)
}

/// `MulXformByInverseCsys`: `inverse(c) * x`.
pub(super) fn mul_by_inverse_csys(c: &Csys, x: &Xform) -> Xform {
    mul_transform(&invert_csys(c, true), x)
}

/// `TransformFloorHardpoint`: as [`mul_by_inverse_csys`] with the raw
/// transpose.
pub(super) fn floor_hardpoint(c: &Csys, x: &Xform) -> Xform {
    mul_transform(&invert_csys(c, false), x)
}

/// `NormalizeAxis3`, `Normalize3`: `v / sqrt((x^2 + y^2) + z^2)`, the
/// reciprocal unrounded.
pub(super) fn normalize(v: Vec3) -> Vec3 {
    let (x, y, z) = (ext(v[0]), ext(v[1]), ext(v[2]));
    let r = 1.0 / ((x * x + y * y) + z * z).sqrt();
    v.map(|c| round(ext(c) * r))
}

/// `Cross3`, `CrossAxisSide3`, `CrossAxisUp3`: `a × b`, each component
/// rounded once.
pub(super) fn cross(a: &Vec3, b: &Vec3) -> Vec3 {
    let (a0, a1, a2) = (ext(a[0]), ext(a[1]), ext(a[2]));
    let (b0, b1, b2) = (ext(b[0]), ext(b[1]), ext(b[2]));
    [
        round(a1 * b2 - a2 * b1),
        round(a2 * b0 - b2 * a0),
        round(a0 * b1 - a1 * b0),
    ]
}

const AXIS_TABLE: [Vec3; 6] = [
    [1.0, 0.0, 0.0],
    [0.0, 1.0, 0.0],
    [0.0, 0.0, 1.0],
    [-1.0, 0.0, 0.0],
    [0.0, -1.0, 0.0],
    [0.0, 0.0, -1.0],
];

fn write_column(m: &mut Mat9, column: usize, v: &Vec3) {
    for (row, value) in v.iter().enumerate() {
        m[column + row * 3] = *value;
    }
}

fn read_column(m: &Mat9, column: usize) -> Vec3 {
    [0, 1, 2].map(|row| fld_fstp(m[column + row * 3]))
}

/// `BuildAxisFrame`: the rotation whose axis `front_axis` (0 to 5: +X,
/// +Y, +Z, -X, -Y, -Z) points along `direction` and whose axis `up_axis`
/// is as close as it can be to the scene's up axis (the scene's front
/// axis supplies the fallback when they are nearly parallel); the third
/// column is completed by a cross product. Columns the axes do not name
/// keep what `out` held. A scene axis outside 0 to 5 that is not the
/// unset marker 6 indexes past the reference's table (divergence D247);
/// here it is taken as unset.
pub(super) fn build_axis_frame(
    out: &mut Mat9,
    direction: &Vec3,
    front_axis: u32,
    up_axis: u32,
    scene_front: i32,
    scene_up: i32,
) {
    let mut up_index = 1usize;
    let mut front_index = 2usize;
    if scene_up != 6 && (0..6).contains(&scene_up) {
        up_index = scene_up as usize;
        if scene_front != 6 && (0..6).contains(&scene_front) {
            front_index = scene_front as usize;
        }
    }
    let table_front = AXIS_TABLE[front_index];
    let table_up = AXIS_TABLE[up_index];
    let table_side = cross(&table_up, &table_front);
    let mut dir = normalize(*direction);
    let mut up = table_up;
    if front_axis > 2 {
        dir = dir.map(|c| -c);
    }
    if up_axis > 2 {
        up = up.map(|c| -c);
    }
    let dot = (ext(up[2]) * ext(dir[2]) + ext(up[1]) * ext(dir[1])) + ext(up[0]) * ext(dir[0]);
    let ortho_up = if !(dot.abs() > 0.99) {
        let side = normalize(cross(&up, &dir));
        normalize(cross(&dir, &side))
    } else {
        normalize(cross(&dir, &table_side))
    };
    let (mut col0, mut col1, mut col2, mut up_col0) = (false, false, false, false);
    match front_axis {
        0 | 3 => {
            write_column(out, 0, &dir);
            col0 = true;
        }
        1 | 4 => {
            write_column(out, 1, &dir);
            col1 = true;
        }
        2 | 5 => {
            write_column(out, 2, &dir);
            col2 = true;
        }
        _ => {}
    }
    match up_axis {
        0 | 3 => {
            write_column(out, 0, &ortho_up);
            up_col0 = true;
        }
        1 | 4 => {
            write_column(out, 1, &ortho_up);
            col1 = true;
        }
        2 | 5 => {
            write_column(out, 2, &ortho_up);
            col2 = true;
        }
        _ => {}
    }
    if !up_col0 && !col0 {
        let z = [out[2], out[5], out[8]];
        let y = read_column(out, 1);
        let x = normalize(cross(&y, &z));
        write_column(out, 0, &x);
        return;
    }
    if col1 {
        if !col2 {
            let y = read_column(out, 1);
            let x = read_column(out, 0);
            let z = normalize(cross(&x, &y));
            write_column(out, 2, &z);
        }
        return;
    }
    let x = read_column(out, 0);
    let z = read_column(out, 2);
    let y = normalize(cross(&z, &x));
    write_column(out, 1, &y);
}

/// `FogLerpFloat`: `((to - from) u) + from`, `u` a `float`.
pub(super) fn lerp_f(from: f32, to: f32, u: f32) -> f32 {
    round((ext(to) - ext(from)) * ext(u) + ext(from))
}

/// `AudioLerpFloat`, `InterpolateCameraProps`: as [`lerp_f`] with the
/// unrounded progress.
pub(super) fn lerp_x(from: f32, to: f32, progress: f64) -> f32 {
    round((ext(to) - ext(from)) * progress + ext(from))
}

/// `FogLerpInteger`, `ComputeLightColor`: the wrapped difference times
/// `u` plus `from`, chopped to 64 bits, the low word kept.
pub(super) fn lerp_i(from: i32, to: i32, u: f32) -> i32 {
    let difference = f64::from(to.wrapping_sub(from));
    num::trunc_to_i32_via_i64(difference * ext(u) + f64::from(from))
}

/// `ComputeAudioLerpInteger`: the difference zero-extended, times the
/// unrounded progress, plus `from`; chopped as [`lerp_i`].
pub(super) fn lerp_audio_i(from: i32, to: i32, progress: f64) -> i32 {
    let difference = f64::from(to.wrapping_sub(from) as u32);
    num::trunc_to_i32_via_i64(difference * progress + f64::from(from))
}

/// `LightColorToInteger`: `v * 255`, chopped to 64 bits, the low word.
pub(super) fn color_to_int(v: f32) -> i32 {
    num::trunc_to_i32_via_i64(ext(v) * 255.0)
}

/// `InterpolateLightDirection`: `from + (to - from) u` with the X product
/// and the Z difference rounded.
pub(super) fn lerp_direction(from: &Vec3, to: &Vec3, u: f32) -> Vec3 {
    let u = ext(u);
    let dx = ext(to[0]) - ext(from[0]);
    let dy = ext(to[1]) - ext(from[1]);
    let dz = round(ext(to[2]) - ext(from[2]));
    let delta_x = round(dx * u);
    [
        round(ext(delta_x) + ext(from[0])),
        round(dy * u + ext(from[1])),
        round(ext(dz) * u + ext(from[2])),
    ]
}

/// `LerpSpatialPosition`: `((to - from) s) + from` per component.
pub(super) fn lerp_position(from: &Vec3, to: &Vec3, scale: f32) -> Vec3 {
    [0, 1, 2].map(|i| lerp_f(from[i], to[i], scale))
}

/// `AccumulateAxisDelta`: the step since the last tick, `t * angle -
/// applied`, rounded, and the new accumulator from the unrounded step.
pub(super) fn accumulate_axis_delta(t: f32, angle: f32, applied: &mut f32) -> f32 {
    let delta = ext(t) * ext(angle) - ext(*applied);
    *applied = round(delta + ext(*applied));
    round(delta)
}

/// `ApplyAxisRotFpu`: a rotation of `delta_deg` degrees about axis
/// `axis` (0 and 3 X, 1 and 4 Y, 2 and 5 Z; any other number the
/// identity), applied before (axes 0 to 2) or after the base rotation.
pub(super) fn axis_rotation(base: &[f32; 4], axis: i32, delta_deg: f32) -> Mat9 {
    let kind = axis as u32;
    let mut rot = IDENTITY9;
    if kind <= 5 {
        let radians = ext(delta_deg) * ext(f32::from_bits(0x3c8e_fa35));
        let c = ext(round(radians.cos()));
        let s = radians.sin();
        match kind {
            0 | 3 => {
                for k in 0..3 {
                    let (r1, r2) = (ext(rot[3 + k]), ext(rot[6 + k]));
                    rot[3 + k] = round(c * r1 - s * r2);
                    rot[6 + k] = round(r1 * s + c * r2);
                }
            }
            1 | 4 => {
                for k in 0..3 {
                    let (r0, r2) = (ext(rot[k]), ext(rot[6 + k]));
                    rot[k] = round(s * r2 + r0 * c);
                    rot[6 + k] = round(c * r2 - r0 * s);
                }
            }
            _ => {
                for k in 0..3 {
                    let (r0, r1) = (ext(rot[k]), ext(rot[3 + k]));
                    rot[k] = round(c * r0 - s * r1);
                    rot[3 + k] = round(c * r1 + r0 * s);
                }
            }
        }
    }
    let base9 = quat_to_matrix(base);
    if kind <= 2 {
        mul_matrix(&rot, &base9)
    } else {
        mul_matrix(&base9, &rot)
    }
}

/// `SpatialWrapScale`: the share of the remaining way this tick covers,
/// so that an animation reaches its target from wherever the entity
/// stands. Returns the progress rounded to `float` and the scale.
pub(super) fn spatial_wrap_scale(last: &mut f32, progress: f64) -> (f32, f32) {
    let t = round(progress);
    let d = progress - ext(*last);
    let delta = round(d);
    if d == 0.0 {
        return (t, 0.0);
    }
    if !(ext(t) < 1.0) {
        return (t, 1.0);
    }
    let remainder = 1.0 - ext(*last);
    let scale = if remainder == 0.0 {
        1.0
    } else {
        round(ext(delta) / remainder)
    };
    *last = t;
    (t, scale)
}

/// `FloorWrapScale`: as [`spatial_wrap_scale`], comparing the unrounded
/// progress with 1.
pub(super) fn floor_wrap_scale(last: &mut f32, progress: f64) -> f32 {
    let d = progress - ext(*last);
    let delta = round(d);
    if d == 0.0 {
        return 0.0;
    }
    if !(progress < 1.0) {
        return 1.0;
    }
    let remainder = 1.0 - ext(*last);
    let scale = if remainder == 0.0 {
        1.0
    } else {
        round(ext(delta) / remainder)
    };
    *last = round(progress);
    scale
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn axis_frame_faces_along_the_direction() {
        let mut m = IDENTITY9;
        build_axis_frame(&mut m, &[0.0, 0.0, 5.0], 2, 1, 6, 6);
        assert_eq!(m, IDENTITY9);
        let mut m = IDENTITY9;
        build_axis_frame(&mut m, &[1.0, 0.0, 0.0], 5, 1, 6, 6);
        assert_eq!([m[2], m[5], m[8]], [-1.0, 0.0, 0.0]);
    }

    #[test]
    fn wrap_scale_covers_the_rest_of_the_way() {
        let mut last = 0.0;
        assert_eq!(spatial_wrap_scale(&mut last, 0.5), (0.5, 0.5));
        assert_eq!(spatial_wrap_scale(&mut last, 0.75), (0.75, 0.5));
        assert_eq!(spatial_wrap_scale(&mut last, 1.0), (1.0, 1.0));
        let mut floor = 0.0;
        assert_eq!(floor_wrap_scale(&mut floor, 0.25), 0.25);
    }
}
