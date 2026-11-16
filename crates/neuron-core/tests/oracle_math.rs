//! Oracle vectors for work package P1.4, `neuron-core::math` against the
//! `3DMathEngine` component of `x86math.dll`.
//!
//! The inputs are `tests/fixtures/oracle/math/*.hex`, written by
//! `make_math_fixtures.py`; the C++ harness (`CppSource/Oracle/
//! oracle_math.cpp`) renders the same lines through the real component.
//! Every float is an `f32` bit pattern in eight hex digits, a list of them
//! concatenated.
//!
//! The math kernels are Tier B. The port models the reference's 53-bit x87
//! arithmetic and should reproduce it exactly, but the comparison is per
//! value within the bound each suite states below, so the committed file
//! can be the oracle's exact output. Any NaN matches any NaN (x87 and SSE
//! propagate NaN payloads differently); otherwise the bound is a distance
//! in units in the last place. Flags (`*_ok`) and the echoed input compare
//! exactly. `OUTPOST_BLESS_VECTORS=1` rewrites the files from the port.

use std::fmt::Write as _;
use std::fs;
use std::path::Path;

mod support;

use support::{check_suite, input_lines};

use neuron_core::math::{
    self, Matrix, Quaternion, Transform, Vector, det, general_inverse, general_inverse_in_place,
    inverse, inverse_in_place, inverse_rotate, inverse_rotate_in_place, inverse_transform,
    inverse_transform_list, inverse_transform_list_in_place, matrix_to_quaternion, mul_matrix,
    mul_matrix_assign, mul_matrix_assign_rhs, mul_quat, mul_quat_assign, mul_quat_assign_rhs,
    mul_transform, mul_transform_assign, mul_transform_assign_rhs, quat_slerp,
    quaternion_to_matrix, rotate, rotate_in_place, scale, transform_list_matrix,
    transform_list_matrix_in_place, transform_list_xform, transform_list_xform_in_place,
    transform_matrix, transform_matrix_in_place, transform_quat, transform_xform,
    transform_xform_in_place, transpose_transform, transpose_transform_in_place,
    transpose_transform_list, transpose_transform_list_in_place,
};

/// A single kernel: one rounding after an expression evaluated on the
/// 53-bit stack (verification.md default).
const SINGLE: u32 = 1;
/// Kernels with an intermediate `float` rounding feeding a further
/// expression, or a reciprocal or square root in the chain: inverses
/// (1/det), quaternion conversions and the normalised interpolation
/// (sqrt, reciprocal), `TransformQuat` (rounded temporaries), `InvSqrt`
/// (two Newton steps).
const CHAINED: u32 = 2;

fn words(line: &str) -> Vec<f32> {
    line.split(' ')
        .map(|w| f32::from_bits(u32::from_str_radix(w, 16).unwrap_or_else(|e| panic!("{w}: {e}"))))
        .collect()
}

/// Output words as hex. Every NaN is written as `7fc00000`, as the harness
/// does: x87 NaN sign propagation is not part of the port.
fn hex_words(values: &[f32]) -> String {
    values.iter().fold(String::new(), |mut out, v| {
        let bits = if v.is_nan() { 0x7FC0_0000 } else { v.to_bits() };
        let _ = write!(out, "{bits:08x}");
        out
    })
}

fn take<const N: usize>(w: &[f32], at: usize) -> [f32; N] {
    core::array::from_fn(|i| w[at + i])
}

fn matrix(w: &[f32], at: usize) -> Matrix {
    Matrix::from_words(take(w, at))
}

fn transform(w: &[f32], at: usize) -> Transform {
    Transform::from_words(take(w, at))
}

fn vector(w: &[f32], at: usize) -> Vector {
    Vector::from_words(take(w, at))
}

fn quat(w: &[f32], at: usize) -> Quaternion {
    Quaternion::from_words(take(w, at))
}

fn flag(value: bool) -> &'static str {
    if value { "1" } else { "0" }
}

/// One output line: the input echoed, then `key: value` pairs in order.
fn render(input: &str, fields: &[(&str, String)]) -> String {
    let mut out = format!("{{\"in\":\"{input}\"");
    for (key, value) in fields {
        let _ = write!(out, ",\"{key}\":\"{value}\"");
    }
    out.push('}');
    out
}

/// The `"key":"value"` pairs of a rendered line, in order.
fn pairs(line: &str) -> Vec<(String, String)> {
    let inner = line
        .strip_prefix("{\"")
        .and_then(|l| l.strip_suffix("\"}"))
        .unwrap_or_else(|| panic!("not a vector line: {line}"));
    inner
        .split("\",\"")
        .map(|pair| {
            let (key, value) = pair
                .split_once("\":\"")
                .unwrap_or_else(|| panic!("bad pair {pair}"));
            (key.to_owned(), value.to_owned())
        })
        .collect()
}

/// The distance between two floats in units in the last place, on the
/// ordered line where -0 and +0 coincide; `None` when exactly one is NaN.
fn ulps(a: f32, b: f32) -> Option<u64> {
    if a.is_nan() || b.is_nan() {
        return (a.is_nan() && b.is_nan()).then_some(0);
    }
    let ordered = |v: f32| {
        let magnitude = i64::from(v.to_bits() & 0x7FFF_FFFF);
        if v.is_sign_negative() {
            -magnitude
        } else {
            magnitude
        }
    };
    Some(ordered(a).abs_diff(ordered(b)))
}

/// Compares `lines` with the committed suite within `bound(key)` ulps for
/// float fields (`None`: compare the text), or writes the suite when
/// `OUTPOST_BLESS_VECTORS` is set.
fn check_suite_within(name: &str, lines: &[String], bound: impl Fn(&str) -> Option<u32>) {
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        check_suite(name, lines);
        return;
    }
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        let at = format!("{name} line {}", index + 1);
        let (want, got) = (pairs(want), pairs(got));
        let want_keys: Vec<&String> = want.iter().map(|p| &p.0).collect();
        let got_keys: Vec<&String> = got.iter().map(|p| &p.0).collect();
        assert_eq!(want_keys, got_keys, "{at}: fields");
        for ((key, want), (_, got)) in want.iter().zip(&got) {
            let Some(limit) = bound(key) else {
                assert_eq!(want, got, "{at}: {key}");
                continue;
            };
            assert_eq!(want.len(), got.len(), "{at}: {key} length");
            for (i, (w, g)) in words_of(want).zip(words_of(got)).enumerate() {
                let distance = ulps(w, g);
                assert!(
                    distance.is_some_and(|d| d <= u64::from(limit)),
                    "{at}: {key} word {i}: committed {:08x}, port {:08x} ({distance:?} ulps, bound {limit})",
                    w.to_bits(),
                    g.to_bits()
                );
            }
        }
    }
}

fn words_of(text: &str) -> impl Iterator<Item = f32> + '_ {
    (0..text.len()).step_by(8).map(move |i| {
        f32::from_bits(u32::from_str_radix(&text[i..i + 8], 16).unwrap_or_else(|e| panic!("{e}")))
    })
}

fn is_flag(key: &str) -> bool {
    key.ends_with("_ok")
}

#[test]
fn matrix_suite() {
    let lines: Vec<String> = input_lines("math/matrix.hex")
        .iter()
        .map(|input| {
            let w = words(input);
            let (a, b, s) = (matrix(&w, 0), matrix(&w, 9), w[18]);
            let inv = inverse(&a);
            let mut inv_alias = a;
            let inv_alias_ok = inverse_in_place(&mut inv_alias);
            let mut mul_a = a;
            mul_matrix_assign(&mut mul_a, &b);
            let mut mul_b = b;
            mul_matrix_assign_rhs(&a, &mut mul_b);
            render(
                input,
                &[
                    ("det", hex_words(&[det(&a)])),
                    ("inv", hex_words(&inv.matrix.to_words())),
                    ("inv_ok", flag(inv.invertible).into()),
                    ("inv_alias", hex_words(&inv_alias.to_words())),
                    ("inv_alias_ok", flag(inv_alias_ok).into()),
                    ("scale", hex_words(&scale(&a, s).to_words())),
                    ("mul", hex_words(&mul_matrix(&a, &b).to_words())),
                    ("mul_a", hex_words(&mul_a.to_words())),
                    ("mul_b", hex_words(&mul_b.to_words())),
                    ("quat", hex_words(&matrix_to_quaternion(&a).to_words())),
                ],
            )
        })
        .collect();
    check_suite_within("math_matrix.jsonl", &lines, |key| match key {
        "in" | "inv_ok" | "inv_alias_ok" => None,
        "inv" | "inv_alias" | "quat" => Some(CHAINED),
        _ => Some(SINGLE),
    });
}

#[test]
fn transform_suite() {
    let lines: Vec<String> = input_lines("math/transform.hex")
        .iter()
        .map(|input| {
            let w = words(input);
            let (a, b) = (transform(&w, 0), transform(&w, 12));
            let mut mul_a = a;
            mul_transform_assign(&mut mul_a, &b);
            let mut mul_b = b;
            mul_transform_assign_rhs(&a, &mut mul_b);
            let ginv = general_inverse(&a);
            let mut alias = a;
            let (alias_w, alias_ok) = general_inverse_in_place(&mut alias);
            render(
                input,
                &[
                    ("mul", hex_words(&mul_transform(&a, &b).to_words())),
                    ("mul_a", hex_words(&mul_a.to_words())),
                    ("mul_b", hex_words(&mul_b.to_words())),
                    ("ginv", hex_words(&ginv.transform.to_words())),
                    ("ginv_w", hex_words(&[ginv.w])),
                    ("ginv_ok", flag(ginv.invertible).into()),
                    ("ginv_alias", hex_words(&alias.to_words())),
                    ("ginv_alias_w", hex_words(&[alias_w])),
                    ("ginv_alias_ok", flag(alias_ok).into()),
                ],
            )
        })
        .collect();
    check_suite_within("math_transform.jsonl", &lines, |key| {
        if key == "in" || is_flag(key) {
            None
        } else if key.starts_with("ginv") {
            Some(CHAINED)
        } else {
            Some(SINGLE)
        }
    });
}

#[test]
fn vector_suite() {
    let lines: Vec<String> = input_lines("math/vector.hex")
        .iter()
        .map(|input| {
            let w = words(input);
            let (x, v) = (transform(&w, 0), vector(&w, 12));
            let m = x.rotation;
            let alias = |f: &dyn Fn(&mut Vector)| {
                let mut out = v;
                f(&mut out);
                hex_words(&out.to_words())
            };
            render(
                input,
                &[
                    ("xform", hex_words(&transform_xform(&x, v).to_words())),
                    ("xform_alias", alias(&|o| transform_xform_in_place(&x, o))),
                    ("matrix", hex_words(&transform_matrix(&m, v).to_words())),
                    ("matrix_alias", alias(&|o| transform_matrix_in_place(&m, o))),
                    ("rotate", hex_words(&rotate(&x, v).to_words())),
                    ("rotate_alias", alias(&|o| rotate_in_place(&x, o))),
                    ("inv_rotate", hex_words(&inverse_rotate(&x, v).to_words())),
                    (
                        "inv_rotate_alias",
                        alias(&|o| inverse_rotate_in_place(&x, o)),
                    ),
                    (
                        "transpose",
                        hex_words(&transpose_transform(&m, v).to_words()),
                    ),
                    (
                        "transpose_alias",
                        alias(&|o| transpose_transform_in_place(&m, o)),
                    ),
                    ("inv_xform", hex_words(&inverse_transform(&x, v).to_words())),
                    (
                        "inv_xform_alias",
                        alias(&|o| *o = inverse_transform(&x, *o)),
                    ),
                ],
            )
        })
        .collect();
    check_suite_within("math_vector.jsonl", &lines, |key| {
        (key != "in").then_some(SINGLE)
    });
}

#[test]
fn quat_suite() {
    let lines: Vec<String> = input_lines("math/quat.hex")
        .iter()
        .map(|input| {
            let w = words(input);
            let (q, p, v, t) = (quat(&w, 0), quat(&w, 4), vector(&w, 8), w[11]);
            let mut mul_a = q;
            mul_quat_assign(&mut mul_a, &p);
            let mut mul_b = p;
            mul_quat_assign_rhs(&q, &mut mul_b);
            let slerp = quat_slerp(&q, &p, t);
            render(
                input,
                &[
                    ("mul", hex_words(&mul_quat(&q, &p).to_words())),
                    ("mul_a", hex_words(&mul_a.to_words())),
                    ("mul_b", hex_words(&mul_b.to_words())),
                    ("rot", hex_words(&transform_quat(&q, v).to_words())),
                    ("matrix", hex_words(&quaternion_to_matrix(&q).to_words())),
                    ("slerp", hex_words(&slerp.to_words())),
                    // QuatSlerp(Q, Q, P, t) and QuatSlerp(P, Q, P, t): each
                    // component reads only its own inputs.
                    ("slerp_a", hex_words(&slerp.to_words())),
                    ("slerp_b", hex_words(&slerp.to_words())),
                ],
            )
        })
        .collect();
    check_suite_within("math_quat.jsonl", &lines, |key| match key {
        "in" => None,
        "mul" | "mul_a" | "mul_b" => Some(SINGLE),
        _ => Some(CHAINED),
    });
}

#[test]
fn list_suite() {
    let lines: Vec<String> = input_lines("math/list.hex")
        .iter()
        .map(|input| {
            let w = words(input);
            let x = transform(&w, 0);
            let m = x.rotation;
            let src: Vec<Vector> = w[12..]
                .as_chunks::<3>()
                .0
                .iter()
                .map(|c| Vector::from_words(*c))
                .collect();
            let flat = |vs: &[Vector]| {
                let all: Vec<f32> = vs.iter().flat_map(|v| v.to_words()).collect();
                hex_words(&all)
            };
            let out = |f: &dyn Fn(&[Vector], &mut [Vector])| {
                let mut dst = vec![Vector::ZERO; src.len()];
                f(&src, &mut dst);
                flat(&dst)
            };
            let alias = |f: &dyn Fn(&mut [Vector])| {
                let mut v = src.clone();
                f(&mut v);
                flat(&v)
            };
            render(
                input,
                &[
                    ("xform", out(&|s, d| transform_list_xform(&x, s, d))),
                    (
                        "xform_alias",
                        alias(&|v| transform_list_xform_in_place(&x, v)),
                    ),
                    ("matrix", out(&|s, d| transform_list_matrix(&m, s, d))),
                    (
                        "matrix_alias",
                        alias(&|v| transform_list_matrix_in_place(&m, v)),
                    ),
                    ("transpose", out(&|s, d| transpose_transform_list(&m, s, d))),
                    (
                        "transpose_alias",
                        alias(&|v| transpose_transform_list_in_place(&m, v)),
                    ),
                    ("inverse", out(&|s, d| inverse_transform_list(&x, s, d))),
                    (
                        "inverse_alias",
                        alias(&|v| inverse_transform_list_in_place(&x, v)),
                    ),
                ],
            )
        })
        .collect();
    check_suite_within("math_list.jsonl", &lines, |key| {
        (key != "in").then_some(SINGLE)
    });
}

#[test]
fn sqrt_suite() {
    let lines: Vec<String> = input_lines("math/sqrt.hex")
        .iter()
        .map(|input| {
            let x = words(input)[0];
            render(
                input,
                &[
                    ("inv_sqrt", hex_words(&[math::inv_sqrt(x)])),
                    ("sqrt", hex_words(&[math::sqrt(x)])),
                ],
            )
        })
        .collect();
    check_suite_within("math_sqrt.jsonl", &lines, |key| {
        (key != "in").then_some(CHAINED)
    });
}
