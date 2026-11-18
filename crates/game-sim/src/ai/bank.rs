//! The steering helpers `FaceIdea::Update` (and `DodgeIdea::Update`) shares
//! from `Common/common_aisteer.cpp`: `LimitAxisThrottleForTurn`, which caps
//! an axis throttle so the ship does not overshoot its turn, and
//! `ComputeBankedSteeringFrame`, which rolls the wanted frame into a bank.
//!
//! The arc cosines, sines and cosines come from the Rust standard library
//! (decision T11), so the results are Tier B.

use neuron_core::math::{Matrix, Vector, transform_matrix, transpose_transform};

use super::fp::{
    add, column0, column1, column2, dot_zyx, e, from_columns, len_sq_xyz, neg, normalize_or_zero,
    normalize3, r, scale,
};
use super::raycast::clamped_dot3;
use super::steer::{build_orientation_basis, build_perpendicular_frame};
use super::world::{MotionModel, ObjectView};

/// `LimitProjectOntoColumn` (`common_aisteer.cpp:443`): `source` projected
/// on the unit `column`, the cosine and the length rounded.
#[must_use]
pub fn limit_project_onto_column(source: Vector, column: Vector) -> Vector {
    let source_dir = normalize3(source);
    let column_dir = normalize3(column);
    let cosine = r(clamped_dot3(source_dir, column_dir));
    let length = len_sq_xyz(source).sqrt();
    scale(column_dir, r(length * e(cosine)))
}

/// `LimitClampedAcos` (`common_aisteer.cpp:465`): the angle between two
/// unit vectors, the dot clamped (above 1 unrounded, below -1 rounded).
#[must_use]
pub fn limit_clamped_acos(a: Vector, b: Vector) -> f32 {
    let dot = dot_zyx(a, b);
    let mut clamped = r(dot);
    if dot > 1.0 {
        clamped = 1.0;
    } else if clamped < -1.0 {
        clamped = -1.0;
    }
    r(e(clamped).acos())
}

/// `LimitAxisThrottleForTurn` (`common_aisteer.cpp:532`): `command` split
/// into its parts along the ship's three axes; the part in the X/Y plane
/// scaled down near the target frame (by the angle left, the reachable
/// turn, the current axis throttle plus 0.4, and `get_projected_axis_throttle_XY`
/// of `turn_angle` once within reach), the Z part blended by the up-axis
/// angle when `bank_blend`. `None` returns `command`. The motion queries
/// answer 0 without a motion model.
#[must_use]
pub fn limit_axis_throttle_for_turn(
    obj: Option<&ObjectView>,
    motion: Option<&dyn MotionModel>,
    command: Vector,
    target_frame: &Matrix,
    turn_angle: f32,
    reference_angle: f32,
    bank_blend: bool,
) -> Vector {
    let Some(obj) = obj else {
        return command;
    };
    let frame = obj.orientation;
    let (col0, col1, col2) = (column0(&frame), column1(&frame), column2(&frame));
    let p0 = limit_project_onto_column(command, col0);
    let p1 = limit_project_onto_column(command, col1);
    let mut p2 = limit_project_onto_column(command, col2);
    let angle_z = limit_clamped_acos(column2(target_frame), col2);
    let angle_y = limit_clamped_acos(column1(target_frame), col1);

    let local = transpose_transform(&frame, obj.angular_velocity);
    let speed_d = (e(local.x) * e(local.x) + e(local.y) * e(local.y)).sqrt();
    let mut speed_xy = r(speed_d);
    if speed_d < e(0.01) {
        speed_xy = 0.0;
    }
    let angle_for_call = if turn_angle < 0.0 { 0.0 } else { turn_angle };
    let speed_for = if e(speed_xy) < 0.001 { 0.0 } else { speed_xy };
    let time_to_turn = motion.map_or(0.0, |m| {
        m.time_to_accelerate_angularly_xy(0.0, 0.0, speed_for, angle_for_call)
    });
    let reach = if time_to_turn <= 0.0 {
        0.0
    } else {
        e(motion.map_or(0.0, |m| {
            m.angular_distance_travelled_xy(0.0, 0.0, speed_for, time_to_turn)
        }))
    };
    let mut gate = reach * 0.98;
    if gate < e(0.001) {
        gate = e(0.001);
    }
    let mut plane_limit = 1.0_f32;
    if e(angle_z) < gate {
        plane_limit = if turn_angle == 0.0 {
            0.0
        } else {
            motion.map_or(0.0, |m| m.projected_axis_throttle_xy(turn_angle))
        };
    }
    let mut ratio = 1.0_f32;
    if angle_z < reference_angle {
        ratio = r(e(angle_z) / e(reference_angle));
    }

    let axis_throttle = obj.axis_throttle.unwrap_or(Vector::ZERO);
    let q0 = limit_project_onto_column(axis_throttle, col0);
    let q1 = limit_project_onto_column(axis_throttle, col1);
    let command_plane = add(p0, p1);
    let throttle_plane = add(q0, q1);
    let throttle_len = len_sq_xyz(throttle_plane).sqrt();
    let c = command_plane;
    let command_len = ((e(c.z) * e(c.z) + e(c.y) * e(c.y)) + e(c.x) * e(c.x)).sqrt();
    if command_len + e(0.4) > throttle_len {
        let limit = throttle_len + e(0.4);
        if e(ratio) > limit {
            ratio = r(limit);
        }
    }
    if ratio > plane_limit {
        ratio = plane_limit;
    }

    let mut z_factor = 1.0_f32;
    if bank_blend {
        if len_sq_xyz(p2) > 0.01 {
            p2 = normalize3(p2);
        }
        if angle_y < f32::from_bits(0x3ec9_0fdb) {
            let t = e(angle_y) * e(f32::from_bits(0x4022_f984));
            z_factor = r(t * t * e(0.5));
        } else {
            z_factor = 0.5;
            if !(e(angle_y) < 1.570_796_370_506_286_6) {
                z_factor = 1.0;
            }
        }
    }
    let bank_part = scale(p2, z_factor);
    let plane_part = scale(add(p0, p1), ratio);
    add(plane_part, bank_part)
}

/// `(a.x*b.x + a.z*b.z) + a.y*b.y`, unrounded.
fn dot_xzy(a: Vector, b: Vector) -> f64 {
    (e(a.x) * e(b.x) + e(a.z) * e(b.z)) + e(a.y) * e(b.y)
}

/// A dot clamped as the bank helpers do: above 1 unrounded, below -1
/// rounded.
fn clamp_dot(dot: f64) -> f32 {
    let mut c = r(dot);
    if dot > 1.0 {
        c = 1.0;
    } else if c < -1.0 {
        c = -1.0;
    }
    c
}

/// `ComputeBankedSteeringFrame` (`common_aisteer.cpp:747`): for a
/// ship-like `elem`, roll `working_frame` so the ship banks into the turn
/// toward `target_dir`, and set the roll part of `bank_vector` (local
/// when `mode` is 0, world otherwise; rewritten in world space). Mode 1
/// does nothing while the frame is within about 11° of the ship's
/// forward axis. Whether the bank branch ran.
pub fn compute_banked_steering_frame(
    elem: &ObjectView,
    motion: Option<&dyn MotionModel>,
    target_dir: Vector,
    working_frame: &mut Matrix,
    bank_vector: &mut Vector,
    mode: u8,
) -> bool {
    if elem.type_flags & 0x30000 == 0 {
        return false;
    }
    let orient = elem.orientation;
    let td = target_dir;
    let frame_forward = column2(working_frame);
    let forward = column2(&orient);
    let mut bank_local = if mode != 0 {
        let aligned = dot_xzy(frame_forward, forward);
        if !(aligned < e(0.98)) {
            return false;
        }
        transpose_transform(&orient, *bank_vector)
    } else {
        *bank_vector
    };

    let mut new_frame = *working_frame;
    let abs_target_align = r(dot_xzy(forward, td).abs());
    let up = column1(&orient);
    let right_axis = column0(&orient);
    let mut steer = false;
    if !(abs_target_align > 0.999) {
        let up_dot = dot_xzy(up, td);
        steer = !(up_dot < 0.0) || elem.is_player;
    }

    let mut steered = false;
    if steer {
        steered = true;
        let mut roll_dir_sign = bank_local.y;
        let mut roll_scale = 0.0_f32;
        if let Some(perp) = build_perpendicular_frame(neg(forward), 0.707, Some(td)) {
            let local_right = transpose_transform(&orient, column0(&perp));
            let roll_axis = normalize_or_zero(Vector::new(-bank_local.y, bank_local.x, 0.0));
            let dot = dot_zyx(local_right, roll_axis).clamp(-1.0, 1.0);
            let b = bank_local;
            let bank_length = ((e(b.z) * e(b.z) + e(b.x) * e(b.x)) + e(b.y) * e(b.y)).sqrt();
            let scaled = -dot * bank_length;
            roll_scale = r(scaled);
            if scaled < -1.0 {
                roll_scale = -1.0;
            }
            if e(roll_scale) > 1.0 {
                roll_scale = 1.0;
            }
        }

        let mut roll_sign_set = false;
        let mut bank_angle = 0.0_f32;
        let abs_roll = e(roll_scale).abs();
        if abs_roll > e(0.01) {
            bank_angle = elem.throttle.unwrap_or(0.0).abs();
            if bank_angle < 0.4 {
                bank_angle = 0.4;
            }
            bank_angle = r(e(elem.max_bank_angle) * e(bank_angle) * abs_roll);
            if roll_scale < 0.0 {
                bank_angle = -bank_angle;
            }
        }
        let cos_abs_bank = r(e(bank_angle).abs().cos());
        let abs_forward_dot = dot_zyx(forward, td).abs();
        if abs_forward_dot < e(0.9999) && abs_roll > e(0.01) {
            let f = forward;
            let right = normalize_or_zero(Vector::new(
                r(e(f.z) * e(td.y) - e(f.y) * e(td.z)),
                r(e(f.x) * e(td.z) - e(f.z) * e(td.x)),
                r(e(f.y) * e(td.x) - e(f.x) * e(td.y)),
            ));
            let angle_cos = clamp_dot(dot_zyx(normalize_or_zero(right), right_axis));
            let up_new = normalize_or_zero(Vector::new(
                r(e(right.z) * e(f.y) - e(right.y) * e(f.z)),
                r(e(right.x) * e(f.z) - e(right.z) * e(f.x)),
                r(e(right.y) * e(f.x) - e(right.x) * e(f.y)),
            ));
            new_frame = from_columns(right, up_new, f);
            let cos_bank = e(bank_angle).cos();
            let sin_bank = e(bank_angle).sin();
            for row in &mut new_frame.m {
                let old0 = e(row[0]);
                let old1 = e(row[1]);
                row[0] = r(old0 * cos_bank + sin_bank * old1);
                row[1] = r(cos_bank * old1 - old0 * sin_bank);
            }
            let rot_cos = clamp_dot(dot_zyx(column0(&new_frame), right_axis));
            let rotated_angle = r(e(rot_cos).acos());

            let mut sign_probe = 0.0_f32;
            let mut angle_test = true;
            if dot_zyx(up, td).abs() < e(0.9999) {
                let uc = Vector::new(
                    r(e(up.y) * e(td.z) - e(up.z) * e(td.y)),
                    r(e(up.z) * e(td.x) - e(up.x) * e(td.z)),
                    r(e(up.x) * e(td.y) - e(up.y) * e(td.x)),
                );
                let sign_dot = dot_zyx(f, uc);
                sign_probe = r(sign_dot);
                if sign_dot > 0.0 && roll_scale > 0.0 {
                    roll_sign_set = true;
                    roll_dir_sign = 1.0;
                    angle_test = false;
                } else if sign_probe < 0.0 && roll_scale < 0.0 {
                    roll_sign_set = true;
                    roll_dir_sign = -1.0;
                    angle_test = false;
                }
            }
            if angle_test {
                if angle_cos > cos_abs_bank {
                    roll_sign_set = true;
                } else {
                    roll_dir_sign = -1.0;
                    if !(sign_probe > 0.0) {
                        roll_dir_sign = 1.0;
                    }
                }
            }
            let angular_speed = motion.map_or(0.0, |m| m.angular_speed_z(1.0));
            let denom = e(motion.map_or(0.0, |m| {
                m.angular_distance_travelled(0.0, angular_speed, 1000.0)
            })) + 0.314_159_274_101_257_3;
            let blend = if !(e(rotated_angle) < denom) {
                1.0
            } else {
                let b = e(rotated_angle) / denom;
                let b = b * b;
                if b < e(0.005) { 0.0 } else { b }
            };
            let signed = blend * e(roll_dir_sign);
            bank_local.z = if roll_sign_set { r(signed) } else { r(-signed) };
        } else {
            bank_local.z = r(-dot_zyx(right_axis, td));
            if let Some(perp) = build_perpendicular_frame(neg(forward), 0.707, Some(td)) {
                new_frame = perp;
            }
        }
    }

    let tail_forward = neg(column2(working_frame));
    *working_frame = build_orientation_basis(&orient, tail_forward, Some(column1(&new_frame)));
    *bank_vector = transform_matrix(&orient, bank_local);
    steered
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn projection_and_angle() {
        let p = limit_project_onto_column(Vector::new(3.0, 4.0, 0.0), Vector::new(2.0, 0.0, 0.0));
        assert!((p.x - 3.0).abs() < 1e-5 && p.y.abs() < 1e-6, "{p:?}");
        let a = limit_clamped_acos(Vector::new(1.0, 0.0, 0.0), Vector::new(0.0, 1.0, 0.0));
        assert!((a - core::f32::consts::FRAC_PI_2).abs() < 1e-6);
    }

    #[test]
    fn limit_passes_without_object_and_caps_near_target() {
        let c = Vector::new(1.0, 0.5, 0.25);
        assert_eq!(
            limit_axis_throttle_for_turn(None, None, c, &Matrix::IDENTITY, 0.0, 1.0, true),
            c
        );
        let ship = ObjectView {
            orientation: Matrix::IDENTITY,
            ..ObjectView::default()
        };
        // Already facing the target frame: the plane part goes to 0 (no
        // turn angle), the Z part is blended to 0 by the zero up angle.
        let out =
            limit_axis_throttle_for_turn(Some(&ship), None, c, &Matrix::IDENTITY, 0.0, 1.0, true);
        assert!(
            out.x.abs() < 1e-6 && out.y.abs() < 1e-6 && out.z.abs() < 1e-6,
            "{out:?}"
        );
    }

    #[test]
    fn bank_needs_a_ship() {
        let mut frame = Matrix::IDENTITY;
        let mut bank = Vector::ZERO;
        let rock = ObjectView {
            orientation: Matrix::IDENTITY,
            ..ObjectView::default()
        };
        assert!(!compute_banked_steering_frame(
            &rock,
            None,
            Vector::new(1.0, 0.0, 0.0),
            &mut frame,
            &mut bank,
            0
        ));
        let ship = ObjectView {
            orientation: Matrix::IDENTITY,
            type_flags: 0x10000,
            max_bank_angle: 0.5,
            throttle: Some(1.0),
            ..ObjectView::default()
        };
        let mut bank = Vector::new(1.0, 0.0, 0.0);
        let steered = compute_banked_steering_frame(
            &ship,
            None,
            Vector::new(1.0, 0.0, 0.0),
            &mut frame,
            &mut bank,
            0,
        );
        assert!(steered);
        assert!(bank.z != 0.0, "{bank:?}");
    }
}
