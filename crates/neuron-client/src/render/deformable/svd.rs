//! The singular value decomposition the IK solvers invert their Jacobian
//! with (`Mat.h` in the reference's diagnostics): `FSvdMatrix`, a 12x12
//! cell block with its row and column counts, Golub-Reinsch `svdcmp`
//! (`SvdCmp`), the pseudo-inverse and the products, each in the
//! reference's operation order and float stores.
//!
//! Cells are addressed flat, twelve per row, as the reference addresses
//! them; a read outside the block (which only a NaN or infinite input
//! reaches) gives 0 and a write there is dropped (divergence D396).

#[allow(clippy::cast_possible_truncation)]
fn f(x: f64) -> f32 {
    x as f32
}

/// `G_SvdMaxDim`.
pub const MAX_DIM: usize = 12;
/// `G_SvdMaxIter`.
pub const MAX_ITER: i32 = 30;

/// `FSvdMatrix`.
#[derive(Clone, Debug, PartialEq)]
pub struct SvdMatrix {
    /// The cells, twelve per row.
    pub cells: [f32; MAX_DIM * MAX_DIM],
    /// `RowCount` (+0x240).
    pub rows: i32,
    /// `ColCount` (+0x244).
    pub cols: i32,
}

impl Default for SvdMatrix {
    fn default() -> Self {
        Self {
            cells: [0.0; MAX_DIM * MAX_DIM],
            rows: 0,
            cols: 0,
        }
    }
}

impl SvdMatrix {
    /// A zeroed matrix of the given counts.
    #[must_use]
    pub fn new(rows: i32, cols: i32) -> Self {
        Self {
            rows,
            cols,
            ..Self::default()
        }
    }

    /// The cell at `row`, `col` (flat addressing).
    #[must_use]
    pub fn at(&self, row: i32, col: i32) -> f32 {
        get(&self.cells, row * 12 + col)
    }

    /// Sets the cell at `row`, `col`.
    pub fn set(&mut self, row: i32, col: i32, value: f32) {
        put(&mut self.cells, row * 12 + col, value);
    }
}

fn get(cells: &[f32], index: i32) -> f32 {
    usize::try_from(index)
        .ok()
        .and_then(|i| cells.get(i))
        .copied()
        .unwrap_or(0.0)
}

fn put(cells: &mut [f32], index: i32, value: f32) {
    if let Some(cell) = usize::try_from(index).ok().and_then(|i| cells.get_mut(i)) {
        *cell = value;
    }
}

/// `SvdPythag`: `sqrt(a^2 + b^2)` without overflow, in `f64`; the ratio is
/// stored to a float (`G_SvdSqrArg`) before it is squared.
#[must_use]
pub fn pythag(left: f32, right: f32) -> f64 {
    let abs_left = f64::from(left).abs();
    let abs_right = right.abs();
    if abs_left > f64::from(abs_right) {
        let ratio = f64::from(abs_right) / abs_left;
        let stored = f64::from(f(ratio));
        if ratio == 0.0 {
            return abs_left * (0.0f64 + 1.0).sqrt();
        }
        return abs_left * (stored * stored + 1.0).sqrt();
    }
    if f64::from(abs_right) == 0.0 {
        return 0.0;
    }
    let ratio = abs_left / f64::from(abs_right);
    let stored = f64::from(f(ratio));
    if ratio == 0.0 {
        return (0.0f64 + 1.0).sqrt() * f64::from(abs_right);
    }
    (stored * stored + 1.0).sqrt() * f64::from(abs_right)
}

/// `SvdIsNegligible`: `value + anorm` equals `anorm`; the sum is not rounded
/// to a float before the comparison (it stays on the FPU at 53 bits).
fn negligible(value: f32, anorm: f32) -> bool {
    f64::from(value) + f64::from(anorm) == f64::from(anorm)
}

/// The sign test before each square root: below zero or unordered.
fn sign_c0(value: f32) -> bool {
    !(f64::from(value) >= 0.0)
}

/// `SvdNegSignOf`.
fn neg_sign_of(value: f64, negative: bool) -> f64 {
    let mut magnitude = value.abs();
    if negative {
        magnitude = -magnitude;
    }
    -magnitude
}

/// `SvdCmp`: decomposes `a` (counts from `source`) in place into `U`,
/// with the singular values in `w` and `V` in `v`. `false` when the
/// diagonalisation does not converge in [`MAX_ITER`] steps.
#[allow(clippy::too_many_lines, clippy::many_single_char_names)]
pub fn svd_cmp(
    source: &SvdMatrix,
    a: &mut [f32; 144],
    w: &mut [f32; 12],
    v: &mut [f32; 144],
) -> bool {
    let m = source.rows;
    let n = source.cols;
    let ac = |a: &[f32; 144], r: i32, c: i32| get(a, r * 12 + c);
    let wi = |w: &[f32; 12], i: i32| get(w, i);
    let mut rv1 = [0.0f32; 12];
    let mut g = 0.0f32;
    let mut anorm = 0.0f32;
    let mut scale = 0.0f64;
    let mut l = 0i32;
    let mut i = 0i32;
    while i < n {
        l = i + 1;
        put(&mut rv1, i, f(scale * f64::from(g)));
        g = 0.0;
        scale = 0.0;
        let mut sum = 0.0f64;
        if i < m {
            for k in i..m {
                scale += f64::from(ac(a, k, i)).abs();
            }
            if scale != 0.0 {
                let inv = 1.0 / scale;
                for k in i..m {
                    let scaled = inv * f64::from(ac(a, k, i));
                    put(a, k * 12 + i, f(scaled));
                    sum += scaled * scaled;
                }
                let fv = ac(a, i, i);
                g = f(neg_sign_of(sum.sqrt(), sign_c0(fv)));
                let h = f64::from(fv) * f64::from(g) - sum;
                put(a, i * 12 + i, f(f64::from(fv) - f64::from(g)));
                if l < n {
                    let h_inv = 1.0 / h;
                    for j in l..n {
                        let mut dot = 0.0f64;
                        for k in i..m {
                            dot += f64::from(ac(a, k, j)) * f64::from(ac(a, k, i));
                        }
                        let factor = f(h_inv * dot);
                        for k in i..m {
                            let value =
                                f(f64::from(factor) * f64::from(ac(a, k, i))
                                    + f64::from(ac(a, k, j)));
                            put(a, k * 12 + j, value);
                        }
                    }
                }
                for k in i..m {
                    let value = f(scale * f64::from(ac(a, k, i)));
                    put(a, k * 12 + i, value);
                }
            }
        }
        put(w, i, f(scale * f64::from(g)));

        g = 0.0;
        scale = 0.0;
        sum = 0.0;
        if i < m && i != n && l < n {
            for k in l..n {
                scale += f64::from(ac(a, i, k)).abs();
            }
            if scale != 0.0 {
                let inv = 1.0 / scale;
                for k in l..n {
                    let scaled = inv * f64::from(ac(a, i, k));
                    put(a, i * 12 + k, f(scaled));
                    sum += scaled * scaled;
                }
                let sum_f = f(sum);
                let fv = ac(a, i, l);
                let gd = neg_sign_of(f64::from(sum_f).sqrt(), sign_c0(fv));
                g = f(gd);
                let h = f(gd * f64::from(fv) - f64::from(sum_f));
                put(a, i * 12 + l, f(f64::from(fv) - f64::from(g)));
                let h_inv = 1.0 / f64::from(h);
                for k in l..n {
                    put(&mut rv1, k, f(h_inv * f64::from(ac(a, i, k))));
                }
                if l < m {
                    for j in l..m {
                        let mut dot = 0.0f64;
                        for k in l..n {
                            dot += f64::from(ac(a, i, k)) * f64::from(ac(a, j, k));
                        }
                        for k in l..n {
                            let value = f(dot * f64::from(get(&rv1, k)) + f64::from(ac(a, j, k)));
                            put(a, j * 12 + k, value);
                        }
                    }
                }
                for k in l..n {
                    let value = f(scale * f64::from(ac(a, i, k)));
                    put(a, i * 12 + k, value);
                }
            }
        }
        let max_arg = f(f64::from(wi(w, i)).abs() + f64::from(get(&rv1, i)).abs());
        if !(anorm > max_arg) {
            anorm = max_arg;
        }
        i = l;
    }

    // The right-hand transforms into V.
    i = n - 1;
    while i >= 0 {
        if i < n {
            if g != 0.0 {
                let g_inv = 1.0 / f64::from(g);
                for j in l..n {
                    let value = f(f64::from(ac(a, i, j)) / f64::from(ac(a, i, l)) * g_inv);
                    put(v, j * 12 + i, value);
                }
                for j in l..n {
                    let mut dot = 0.0f64;
                    for k in l..n {
                        dot += f64::from(get(v, k * 12 + j)) * f64::from(ac(a, i, k));
                    }
                    for k in l..n {
                        let value =
                            f(dot * f64::from(get(v, k * 12 + i)) + f64::from(get(v, k * 12 + j)));
                        put(v, k * 12 + j, value);
                    }
                }
            }
            for j in l..n {
                put(v, j * 12 + i, 0.0);
                put(v, i * 12 + j, 0.0);
            }
        }
        put(v, i * 12 + i, 1.0);
        g = get(&rv1, i);
        l = i;
        i -= 1;
    }

    // The left-hand transforms into A.
    i = m.min(n) - 1;
    while i >= 0 {
        l = i + 1;
        let w_val = wi(w, i);
        for j in l..n {
            put(a, i * 12 + j, 0.0);
        }
        if w_val != 0.0 {
            let g_inv = f(1.0 / f64::from(w_val));
            for j in l..n {
                let mut dot = 0.0f64;
                for k in l..m {
                    dot += f64::from(ac(a, k, i)) * f64::from(ac(a, k, j));
                }
                let factor = f(dot / f64::from(ac(a, i, i)) * f64::from(g_inv));
                for k in i..m {
                    let value =
                        f(f64::from(factor) * f64::from(ac(a, k, i)) + f64::from(ac(a, k, j)));
                    put(a, k * 12 + j, value);
                }
            }
            for k in i..m {
                let value = f(f64::from(g_inv) * f64::from(ac(a, k, i)));
                put(a, k * 12 + i, value);
            }
        } else {
            for k in i..m {
                put(a, k * 12 + i, 0.0);
            }
        }
        let value = f(f64::from(ac(a, i, i)) + 1.0);
        put(a, i * 12 + i, value);
        i -= 1;
    }

    // The diagonalisation.
    let mut k = n - 1;
    while k >= 0 {
        let mut its = 1;
        loop {
            let mut cancel = true;
            let mut nm;
            l = k;
            while l >= 0 {
                nm = l - 1;
                if negligible(get(&rv1, l).abs(), anorm) {
                    cancel = false;
                    break;
                }
                if negligible(wi(w, nm).abs(), anorm) {
                    break;
                }
                l = nm;
            }
            nm = l - 1;
            if cancel && l <= k {
                let mut cos = 0.0f32;
                let mut sin = 1.0f64;
                for row in l..=k {
                    let fv = f(sin * f64::from(get(&rv1, row)));
                    let value = f(f64::from(cos) * f64::from(get(&rv1, row)));
                    put(&mut rv1, row, value);
                    if negligible(fv.abs(), anorm) {
                        break;
                    }
                    let gw = wi(w, row);
                    let hyp = pythag(fv, gw);
                    put(w, row, f(hyp));
                    let h_inv = 1.0 / hyp;
                    cos = f(f64::from(gw) * h_inv);
                    sin = -(h_inv * f64::from(fv));
                    for j in 0..m {
                        let y = f64::from(ac(a, j, nm));
                        let z = f64::from(ac(a, j, row));
                        put(a, j * 12 + nm, f(y * f64::from(cos) + z * sin));
                        put(a, j * 12 + row, f(z * f64::from(cos) - y * sin));
                    }
                }
            }

            let z = wi(w, k);
            if l == k {
                if f64::from(z) < 0.0 {
                    put(w, k, -z);
                    for j in 0..n {
                        let value = -get(v, j * 12 + k);
                        put(v, j * 12 + k, value);
                    }
                }
                break;
            }
            if its == MAX_ITER {
                return false;
            }

            // The shift from the bottom 2x2 minor.
            let mut x = wi(w, l);
            nm = k - 1;
            let mut y = wi(w, nm);
            let mut gv = get(&rv1, nm);
            let mut h = get(&rv1, k);
            let (yd, zd, gd, hd) = (f64::from(y), f64::from(z), f64::from(gv), f64::from(h));
            let mut fv = f(((yd - zd) * (zd + yd) + (gd - hd) * (hd + gd)) / (yd * hd + yd * hd));
            let p = pythag(fv, 1.0).abs();
            let g_signed = if sign_c0(fv) { -p } else { p };
            fv = f(
                ((f64::from(y) / (f64::from(fv) + g_signed) - f64::from(h)) * f64::from(h)
                    + (f64::from(x) - f64::from(z)) * (f64::from(x) + f64::from(z)))
                    / f64::from(x),
            );
            let mut cos = 1.0f32;
            let mut sin = 1.0f32;

            // The QR sweep.
            for j in l..=nm {
                let next = j + 1;
                let g_next = get(&rv1, next);
                y = wi(w, next);
                h = f(f64::from(sin) * f64::from(g_next));
                gv = f(f64::from(cos) * f64::from(g_next));
                let mut zz = pythag(fv, h);
                put(&mut rv1, j, f(zz));
                let mut z_inv = 1.0 / zz;
                cos = f(f64::from(fv) * z_inv);
                sin = f(z_inv * f64::from(h));
                fv = f(f64::from(x) * f64::from(cos) + f64::from(gv) * f64::from(sin));
                gv = f(f64::from(cos) * f64::from(gv) - f64::from(x) * f64::from(sin));
                h = f(f64::from(y) * f64::from(sin));
                y = f(f64::from(y) * f64::from(cos));
                for jj in 0..n {
                    let vx = f64::from(get(v, jj * 12 + j));
                    let vz = f64::from(get(v, jj * 12 + next));
                    put(v, jj * 12 + j, f(vx * f64::from(cos) + vz * f64::from(sin)));
                    put(
                        v,
                        jj * 12 + next,
                        f(vz * f64::from(cos) - vx * f64::from(sin)),
                    );
                }
                zz = pythag(fv, h);
                put(w, j, f(zz));
                if zz != 0.0 {
                    z_inv = 1.0 / zz;
                    cos = f(f64::from(fv) * z_inv);
                    sin = f(z_inv * f64::from(h));
                }
                fv = f(f64::from(y) * f64::from(sin) + f64::from(cos) * f64::from(gv));
                x = f(f64::from(y) * f64::from(cos) - f64::from(sin) * f64::from(gv));
                for jj in 0..m {
                    let ay = f64::from(ac(a, jj, j));
                    let az = f64::from(ac(a, jj, next));
                    put(a, jj * 12 + j, f(ay * f64::from(cos) + az * f64::from(sin)));
                    put(
                        a,
                        jj * 12 + next,
                        f(az * f64::from(cos) - ay * f64::from(sin)),
                    );
                }
            }
            put(w, k, x);
            put(&mut rv1, l, 0.0);
            put(&mut rv1, k, fv);
            its += 1;
            if its > MAX_ITER {
                break;
            }
        }
        k -= 1;
    }
    true
}

/// `SvdScaleColumns`: `out[r][c] = w[c] * v[r][c]`.
fn scale_columns(v: &SvdMatrix, w: &[f32; 12]) -> SvdMatrix {
    let mut out = SvdMatrix::new(v.rows, v.cols);
    for r in 0..v.rows {
        for c in 0..v.cols {
            out.set(r, c, get(w, c) * v.at(r, c));
        }
    }
    out
}

/// `SvdMulABt`: `left * right^T` with the reference's bounds (the output
/// keeps `right.rows` columns but the loop runs to `right.cols`).
fn mul_abt(left: &SvdMatrix, right: &SvdMatrix, out: &mut SvdMatrix) {
    out.rows = left.rows;
    out.cols = right.rows;
    for r in 0..left.rows {
        for c in 0..right.cols {
            let mut sum = 0.0f32;
            for k in 0..left.cols {
                sum = f(f64::from(right.at(c, k)) * f64::from(left.at(r, k)) + f64::from(sum));
            }
            out.set(r, c, sum);
        }
    }
}

/// What [`pseudo_inverse`] reports besides its output.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PseudoInverse {
    /// `SvdCmp` converged ("svdcmp: didnt approach a finite soln." when
    /// not).
    pub converged: bool,
}

/// `SvdPseudoInverse`: `V * diag(1/w) * U^T`, singular values below 1e-6
/// dropped.
pub fn pseudo_inverse(source: &SvdMatrix, out: &mut SvdMatrix) -> PseudoInverse {
    out.rows = source.cols;
    out.cols = source.rows;
    let n = source.cols;
    let mut w = [0.0f32; 12];
    let mut v = SvdMatrix::new(source.cols, source.cols);
    let mut work = source.clone();
    let converged = svd_cmp(source, &mut work.cells, &mut w, &mut v.cells);
    for i in 0..n {
        let value = get(&w, i);
        put(
            &mut w,
            i,
            if f64::from(value) < 1.0e-6 {
                0.0
            } else {
                f(1.0 / f64::from(value))
            },
        );
    }
    let scaled = scale_columns(&v, &w);
    mul_abt(&scaled, &work, out);
    PseudoInverse { converged }
}

/// `SvdTranspose`.
pub fn transpose(source: &SvdMatrix, out: &mut SvdMatrix) {
    out.rows = source.cols;
    out.cols = source.rows;
    for r in 0..source.rows {
        for c in 0..source.cols {
            out.set(c, r, source.at(r, c));
        }
    }
}

/// `SvdMulAB`: each step `cell += left * right` rounded once.
pub fn mul_ab(left: &SvdMatrix, right: &SvdMatrix, out: &mut SvdMatrix) {
    out.rows = left.rows;
    out.cols = right.cols;
    for r in 0..left.rows {
        for c in 0..right.cols {
            let mut sum = 0.0f32;
            for k in 0..left.cols {
                sum = f(f64::from(left.at(r, k)) * f64::from(right.at(k, c)) + f64::from(sum));
            }
            out.set(r, c, sum);
        }
    }
}

/// `SvdMulVec`: `out = left * vec`; the thirteenth word of `out` (its
/// count) becomes `left.rows`.
pub fn mul_vec(left: &SvdMatrix, vec: &[f32; 12], out: &mut [f32; 12], out_count: &mut i32) {
    *out_count = left.rows;
    for r in 0..left.rows {
        let mut sum = 0.0f32;
        for c in 0..left.cols {
            sum = f(f64::from(get(vec, c)) * f64::from(left.at(r, c)) + f64::from(sum));
        }
        put(out, r, sum);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pseudo_inverse_of_a_diagonal() {
        let mut m = SvdMatrix::new(3, 3);
        m.set(0, 0, 2.0);
        m.set(1, 1, 4.0);
        m.set(2, 2, 0.5);
        let mut out = SvdMatrix::default();
        assert!(pseudo_inverse(&m, &mut out).converged);
        assert!((out.at(0, 0) - 0.5).abs() < 1e-6);
        assert!((out.at(1, 1) - 0.25).abs() < 1e-6);
        assert!((out.at(2, 2) - 2.0).abs() < 1e-6);
        assert!(out.at(0, 1).abs() < 1e-6);
    }

    #[test]
    fn pseudo_inverse_of_a_full_matrix() {
        let mut m = SvdMatrix::new(3, 3);
        let values = [4.0, 1.0, 0.5, 1.0, 3.0, 0.2, 0.5, 0.2, 2.0];
        for (i, v) in values.iter().enumerate() {
            #[allow(clippy::cast_possible_wrap, clippy::cast_possible_truncation)]
            m.set(i as i32 / 3, i as i32 % 3, *v);
        }
        let mut inv = SvdMatrix::default();
        pseudo_inverse(&m, &mut inv);
        let mut prod = SvdMatrix::default();
        mul_ab(&m, &inv, &mut prod);
        for r in 0..3 {
            for c in 0..3 {
                let want = if r == c { 1.0 } else { 0.0 };
                assert!(
                    (prod.at(r, c) - want).abs() < 1e-5,
                    "{r} {c} {}",
                    prod.at(r, c)
                );
            }
        }
    }
}
