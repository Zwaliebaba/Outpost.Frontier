//! IVP's own vector, matrix, quaternion and time helpers
//! (`CppSource/IVP/ivp_utility/ivp_math.h`), in the reference's operation
//! order on the 53-bit model (decision T11).
//!
//! IVP keeps its own spelling of every product and sum, different from
//! `x86math` (`crate::math`), so the free flight cannot use those kernels.
//! A value the reference keeps on the x87 stack is an `f64` here; each
//! `FSTP dword` is an `as f32`. Trigonometry goes through the Rust
//! standard library, so results that pass through `FSIN` or `acos` are
//! Tier B (T11).

/// `FLD dword`.
#[inline]
pub(crate) fn e(value: f32) -> f64 {
    f64::from(value)
}

/// `FSTP dword` or an assignment to `float`.
#[inline]
pub(crate) fn r(value: f64) -> f32 {
    value as f32
}

/// IVP's split time stamp `IVP_Time` (`ivp_core.h`): a whole part and a
/// fraction, both `f32`, kept so that the fraction stays in `[0, 1]`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct IvpTime {
    /// `time_base`: whole seconds.
    pub base: f32,
    /// `time_offset`: the fraction.
    pub offset: f32,
}

impl IvpTime {
    /// A stamp from its halves.
    #[must_use]
    pub const fn new(base: f32, offset: f32) -> Self {
        Self { base, offset }
    }

    /// `((self.base - since.base) + self.offset) - since.offset` on the
    /// stack, the difference every IVP routine forms.
    #[must_use]
    pub fn since(self, since: IvpTime) -> f64 {
        ((e(self.base) - e(since.base)) + e(self.offset)) - e(since.offset)
    }

    /// The normalisation loop of the event manager and the PSI event
    /// (`ivp_environment.cpp:1385`, `:1491`): `while (lo > 1.0f) { hi += 1;
    /// lo -= 1; }` on the stack, then both halves stored.
    #[must_use]
    pub fn normalized(hi: f64, lo: f64) -> Self {
        let mut hi = hi;
        let mut lo = lo;
        while lo > 1.0 {
            hi += 1.0;
            lo -= 1.0;
        }
        Self::new(r(hi), r(lo))
    }

    /// The int and fraction split of `update_time_of_last_motion`
    /// (`ivp_core.cpp:482`): `Sum = offset + base`, truncated by `ftol`, the
    /// fraction from the unrounded sum.
    #[must_use]
    pub fn split_sum(self) -> Self {
        let sum = e(self.offset) + e(self.base);
        let whole = crate::num::trunc_to_i32(sum);
        Self::new(whole as f32, r(sum - f64::from(whole)))
    }
}

/// A 3x4 matrix `IVP_U_Matrix`: three rows of the rotation and the
/// translation `vv`. The rows' fourth floats are not modelled.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct IvpMatrix {
    /// The rotation rows.
    pub rows: [[f32; 3]; 3],
    /// The translation.
    pub vv: [f32; 3],
}

impl Default for IvpMatrix {
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl IvpMatrix {
    /// `IVP_U_Matrix::init`.
    pub const IDENTITY: Self = Self {
        rows: [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
        vv: [0.0; 3],
    };

    /// `vmult`: `M * p`; y stays on the stack, z is spilled, stores z, x, y.
    #[must_use]
    pub fn vmult(&self, p: [f32; 3]) -> [f32; 3] {
        let m = &self.rows;
        let (x, y, z) = (e(p[0]), e(p[1]), e(p[2]));
        let ry = (e(m[1][0]) * x + e(m[1][2]) * z) + e(m[1][1]) * y;
        let rz = r((e(m[2][0]) * x + e(m[2][2]) * z) + e(m[2][1]) * y);
        let rx = (e(m[0][2]) * z + e(m[0][1]) * y) + e(m[0][0]) * x;
        [r(rx), r(ry), rz]
    }

    /// `vimult`: `M^T * p`, same store pattern as [`IvpMatrix::vmult`].
    #[must_use]
    pub fn vimult(&self, p: [f32; 3]) -> [f32; 3] {
        let m = &self.rows;
        let (x, y, z) = (e(p[0]), e(p[1]), e(p[2]));
        let ry = (e(m[0][1]) * x + e(m[2][1]) * z) + e(m[1][1]) * y;
        let rz = r((e(m[0][2]) * x + e(m[2][2]) * z) + e(m[1][2]) * y);
        let rx = (e(m[2][0]) * z + e(m[1][0]) * y) + e(m[0][0]) * x;
        [r(rx), r(ry), rz]
    }

    /// `vmult4`: `M * p + vv`, with x and y partial sums spilled after
    /// every term.
    #[must_use]
    pub fn vmult4(&self, p: [f32; 3]) -> [f32; 3] {
        let m = &self.rows;
        let x = e(p[0]);
        let mut tx = r(e(m[0][0]) * x);
        let mut ty = r(e(m[1][0]) * x);
        let mut ez = x * e(m[2][0]);
        let y = e(p[1]);
        let z = e(p[2]);
        tx = r(e(m[0][1]) * y + e(tx));
        ty = r(e(m[1][1]) * y + e(ty));
        ez += e(m[2][1]) * y;
        tx = r(e(m[0][2]) * z + e(tx));
        let ry = (e(m[1][2]) * z + e(ty)) + e(self.vv[1]);
        let rz = r((e(m[2][2]) * z + ez) + e(self.vv[2]));
        let rx = e(tx) + e(self.vv[0]);
        [r(rx), r(ry), rz]
    }

    /// `vimult4`: `M^T * (p - vv)`.
    #[must_use]
    pub fn vimult4(&self, p: [f32; 3]) -> [f32; 3] {
        let m = &self.rows;
        let dx = e(p[0]) - e(self.vv[0]);
        let dy = e(p[1]) - e(self.vv[1]);
        let dz = e(p[2]) - e(self.vv[2]);
        [
            r((e(m[2][0]) * dz + e(m[1][0]) * dy) + e(m[0][0]) * dx),
            r((e(m[2][1]) * dz + e(m[1][1]) * dy) + e(m[0][1]) * dx),
            r((e(m[2][2]) * dz + e(m[1][2]) * dy) + e(m[0][2]) * dx),
        ]
    }
}

/// `IVP_U_Quat`, stored x, y, z, w (not `FQuaternion`'s w-first order).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct IvpQuat {
    /// x.
    pub x: f32,
    /// y.
    pub y: f32,
    /// z.
    pub z: f32,
    /// w.
    pub w: f32,
}

impl Default for IvpQuat {
    fn default() -> Self {
        Self::IDENTITY
    }
}

impl IvpQuat {
    /// The identity rotation.
    pub const IDENTITY: Self = Self::new(0.0, 0.0, 0.0, 1.0);

    /// A quaternion from x, y, z, w.
    #[must_use]
    pub const fn new(x: f32, y: f32, z: f32, w: f32) -> Self {
        Self { x, y, z, w }
    }

    /// From the math engine's w-first quaternion.
    #[must_use]
    pub fn from_math(q: crate::math::Quaternion) -> Self {
        Self::new(q.x, q.y, q.z, q.w)
    }

    /// To the math engine's w-first quaternion.
    #[must_use]
    pub fn to_math(self) -> crate::math::Quaternion {
        crate::math::Quaternion::new(self.w, self.x, self.y, self.z)
    }

    /// The four components x, y, z, w.
    #[must_use]
    pub const fn to_array(self) -> [f32; 4] {
        [self.x, self.y, self.z, self.w]
    }

    /// From the four components x, y, z, w.
    #[must_use]
    pub const fn from_array(q: [f32; 4]) -> Self {
        Self::new(q[0], q[1], q[2], q[3])
    }

    /// `set_matrix`: the rotation rows of `out`; `vv` is untouched.
    pub fn set_matrix(&self, out: &mut IvpMatrix) {
        let (x, y, z, w) = (self.x, self.y, self.z, self.w);
        let x2 = e(x) + e(x);
        let y2 = r(e(y) + e(y));
        let z2 = r(e(z) + e(z));
        let xx = r(x2 * e(x));
        let xy = r(e(y2) * e(x));
        let xz = r(e(z2) * e(x));
        let yy = r(e(y2) * e(y));
        let yz = r(e(z2) * e(y));
        let zz = r(e(z2) * e(z));
        let wx = x2 * e(w);
        let wy = e(y2) * e(w);
        let wz = e(z2) * e(w);
        out.rows[0][0] = r(1.0 - (e(zz) + e(yy)));
        out.rows[0][1] = r(e(xy) - wz);
        out.rows[0][2] = r(wy + e(xz));
        out.rows[1][0] = r(wz + e(xy));
        out.rows[1][1] = r(1.0 - (e(zz) + e(xx)));
        out.rows[1][2] = r(e(yz) - wx);
        out.rows[2][0] = r(e(xz) - wy);
        out.rows[2][1] = r(wx + e(yz));
        out.rows[2][2] = r(1.0 - (e(yy) + e(xx)));
    }

    /// `set_from_matrix`, ending in [`IvpQuat::normize`].
    pub fn set_from_matrix(&mut self, m: &IvpMatrix) {
        let m = &m.rows;
        let trace = (e(m[1][1]) + e(m[0][0])) + e(m[2][2]);
        if trace > 0.0 {
            let mut s = (trace + 1.0).sqrt();
            self.w = r(s * 0.5);
            s = 0.5 / s;
            self.x = r((e(m[2][1]) - e(m[1][2])) * s);
            self.y = r((e(m[0][2]) - e(m[2][0])) * s);
            self.z = r((e(m[1][0]) - e(m[0][1])) * s);
            self.normize();
            return;
        }
        let next = [1usize, 2, 0];
        let mut i = 0usize;
        if !(m[0][0] >= m[1][1]) {
            i = 1;
        }
        if !(m[i][i] >= m[2][2]) {
            i = 2;
        }
        let j = next[i];
        let k = next[j];
        let mut s = ((e(m[i][i]) - (e(m[k][k]) + e(m[j][j]))) + 1.0).sqrt();
        let mut q = [0.0f32; 4];
        q[i] = r(s * 0.5);
        if s > 0.0 || s < 0.0 {
            s = 0.5 / s;
        }
        q[3] = r((e(m[k][j]) - e(m[j][k])) * s);
        q[j] = r((e(m[j][i]) + e(m[i][j])) * s);
        q[k] = r((e(m[k][i]) + e(m[i][k])) * s);
        *self = Self::from_array(q);
        self.normize();
    }

    /// `set_interpolate_quat`.
    pub fn set_interpolate_quat(&mut self, q0: &IvpQuat, q1: &IvpQuat, alpha: f32) {
        let mut dot =
            ((e(q0.z) * e(q1.z) + e(q0.y) * e(q1.y)) + e(q1.w) * e(q0.w)) + e(q0.x) * e(q1.x);
        let mut dot_f = r(dot);
        let sign: f32 = if dot > 0.0 {
            1.0
        } else {
            dot = -dot;
            dot_f = r(dot);
            -1.0
        };
        if !(dot >= e(0.999f32)) {
            let omega = e(dot_f).acos();
            let inv_sin = r(1.0 / (1.0 - e(dot_f) * e(dot_f)).sqrt());
            let scale0 = r(((1.0 - e(alpha)) * omega).sin() * e(inv_sin));
            let scale1 = ((omega * e(alpha)).sin() * e(inv_sin)) * e(sign);
            self.x = r(e(scale0) * e(q0.x) + scale1 * e(q1.x));
            self.y = r(e(scale0) * e(q0.y) + scale1 * e(q1.y));
            self.z = r(e(scale0) * e(q0.z) + scale1 * e(q1.z));
            self.w = r(scale1 * e(q1.w) + e(scale0) * e(q0.w));
            return;
        }
        let lerp = |a: f32, b: f32| (e(sign) * e(b) - e(a)) * e(alpha) + e(a);
        self.x = r(lerp(q0.x, q1.x));
        self.y = r(lerp(q0.y, q1.y));
        self.z = r(lerp(q0.z, q1.z));
        let w = lerp(q0.w, q1.w);
        self.w = r(w);
        let half = (((e(self.x) * e(self.x) + e(self.y) * e(self.y)) + e(self.z) * e(self.z))
            + w * w)
            * 0.5;
        let mut g = 1.5 - half;
        g += 0.5 - (g * g) * half;
        g = (0.5 - half * (g * g)) + g;
        self.x = r(g * e(self.x));
        self.y = r(g * e(self.y));
        self.z = r(g * e(self.z));
        self.w = r(g * e(self.w));
    }

    /// `normize`: unchanged when the squared length is not above 1e-10f.
    pub fn normize(&mut self) {
        let len_sq = ((e(self.x) * e(self.x) + e(self.y) * e(self.y)) + e(self.z) * e(self.z))
            + e(self.w) * e(self.w);
        if !(len_sq > e(1e-10f32)) {
            return;
        }
        let inv = 1.0 / len_sq.sqrt();
        self.x = r(inv * e(self.x));
        self.y = r(inv * e(self.y));
        self.z = r(inv * e(self.z));
        self.w = r(inv * e(self.w));
    }

    /// `fast_normize`: Newton steps until `|1 - f*f*len|` is at most 1e-6f.
    pub fn fast_normize(&mut self) {
        let len_sq = r(
            ((e(self.x) * e(self.x) + e(self.y) * e(self.y)) + e(self.z) * e(self.z))
                + e(self.w) * e(self.w),
        );
        let epsilon = e(1e-6f32);
        if !((1.0 - e(len_sq)).abs() > epsilon) {
            return;
        }
        let mut f = 1.5 - e(len_sq) * 0.5;
        loop {
            f += (1.0 - (f * f) * e(len_sq)) * 0.5;
            if !((1.0 - (f * f) * e(len_sq)).abs() > epsilon) {
                break;
            }
        }
        self.x = r(f * e(self.x));
        self.y = r(f * e(self.y));
        self.z = r(f * e(self.z));
        self.w = r(f * e(self.w));
    }

    /// `set_invert_unit`.
    #[must_use]
    pub fn invert_unit(&self) -> Self {
        Self::new(-self.x, -self.y, -self.z, self.w)
    }

    /// `set_mult_quat`: `q1 * q2`.
    #[must_use]
    pub fn mult(q1: &IvpQuat, q2: &IvpQuat) -> Self {
        let ry = ((e(q1.w) * e(q2.y) + e(q2.w) * e(q1.y)) + e(q1.z) * e(q2.x)) - e(q2.z) * e(q1.x);
        let rz =
            r(((e(q1.w) * e(q2.z) + e(q1.x) * e(q2.y)) + e(q1.z) * e(q2.w)) - e(q2.x) * e(q1.y));
        let rw =
            r(((e(q1.w) * e(q2.w) - e(q1.x) * e(q2.x)) - e(q1.y) * e(q2.y)) - e(q1.z) * e(q2.z));
        let rx = ((e(q1.x) * e(q2.w) + e(q2.z) * e(q1.y)) + e(q1.w) * e(q2.x)) - e(q1.z) * e(q2.y);
        Self::new(r(rx), r(ry), rz, rw)
    }

    /// `set_mult_inv_quat`: `q1 * q2^-1`.
    #[must_use]
    pub fn mult_inv(q1: &IvpQuat, q2: &IvpQuat) -> Self {
        let l = q2.invert_unit();
        let ry = ((e(l.x) * e(q1.z) + e(l.y) * e(q1.w)) + e(l.w) * e(q1.y)) - e(l.z) * e(q1.x);
        let rz = r(((e(l.y) * e(q1.x) + e(l.w) * e(q1.z)) + e(l.z) * e(q1.w)) - e(l.x) * e(q1.y));
        let rw = r(((e(l.w) * e(q1.w) - e(l.x) * e(q1.x)) - e(l.y) * e(q1.y)) - e(l.z) * e(q1.z));
        let rx = ((e(l.w) * e(q1.x) + e(l.z) * e(q1.y)) + e(l.x) * e(q1.w)) - e(l.y) * e(q1.z);
        Self::new(r(rx), r(ry), rz, rw)
    }

    /// `set_inv_mult_quat`: `q1^-1 * q2`.
    #[must_use]
    pub fn inv_mult(q1: &IvpQuat, q2: &IvpQuat) -> Self {
        let l = q1.invert_unit();
        let ry = ((e(l.z) * e(q2.x) + e(l.y) * e(q2.w)) + e(l.w) * e(q2.y)) - e(l.x) * e(q2.z);
        let rz = r(((e(l.w) * e(q2.z) + e(l.z) * e(q2.w)) + e(l.x) * e(q2.y)) - e(l.y) * e(q2.x));
        let rw = r(((e(l.w) * e(q2.w) - e(l.x) * e(q2.x)) - e(l.y) * e(q2.y)) - e(l.z) * e(q2.z));
        let rx = ((e(l.y) * e(q2.z) + e(l.w) * e(q2.x)) + e(l.x) * e(q2.w)) - e(l.z) * e(q2.y);
        Self::new(r(rx), r(ry), rz, rw)
    }

    /// `set_fast_rot_vector`: the third-order sine series per axis.
    #[must_use]
    pub fn fast_rot_vector(v: [f32; 3], dt: f32) -> Self {
        let sixth = e(0.166_666_67_f32);
        let half = e(dt) * 0.5;
        let series = |c: f32| {
            let s = half * e(c);
            s - ((s * s) * s) * sixth
        };
        let sx = r(series(v[0]));
        let sy = r(series(v[1]));
        let sz = series(v[2]);
        let w = r((1.0 - ((sz * sz + e(sy) * e(sy)) + e(sx) * e(sx))).sqrt());
        Self::new(sx, sy, r(sz), w)
    }

    /// `set_rot_vector`: sines of the half angles, scaled to 0.997 when
    /// their squared length exceeds one.
    #[must_use]
    pub fn rot_vector(v: [f32; 3], dt: f32) -> Self {
        let half = e(dt) * 0.5;
        let mut x = r((half * e(v[0])).sin());
        let mut y = r((half * e(v[1])).sin());
        let sz = (half * e(v[2])).sin();
        let mut z = r(sz);
        let mut len_sq = (e(x) * e(x) + e(y) * e(y)) + sz * sz;
        if len_sq > 1.0 {
            let f = e(0.997f32) / len_sq.sqrt();
            let rx = r(f * e(x));
            x = rx;
            let ry = r(f * e(y));
            y = ry;
            let rz = f * e(z);
            z = r(rz);
            len_sq = (rz * rz + e(ry) * e(ry)) + e(rx) * e(rx);
        }
        Self::new(x, y, z, r((1.0 - len_sq).sqrt()))
    }
}

/// `IVP_DoubleAsinVector`: `2 asin(c)` per component.
#[must_use]
pub fn double_asin_vector(v: [f32; 3]) -> [f32; 3] {
    v.map(|c| {
        let a = e(c).asin();
        r(a + a)
    })
}

/// `IVP_U_Point::subtract`.
#[must_use]
pub fn sub(a: [f32; 3], b: [f32; 3]) -> [f32; 3] {
    [a[0] - b[0], a[1] - b[1], a[2] - b[2]]
}

/// `x*x + y*y + z*z` on the stack.
#[must_use]
pub fn quad_length(v: [f32; 3]) -> f64 {
    (e(v[0]) * e(v[0]) + e(v[1]) * e(v[1])) + e(v[2]) * e(v[2])
}

/// `this = a + b * scale` (`add_multiple`), each component one rounding,
/// stores y, z, x.
#[must_use]
pub fn add_multiple(a: [f32; 3], b: [f32; 3], scale: f32) -> [f32; 3] {
    let y = r(e(scale) * e(b[1]) + e(a[1]));
    let z = r(e(scale) * e(b[2]) + e(a[2]));
    let x = r(e(scale) * e(b[0]) + e(a[0]));
    [x, y, z]
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn time_normalisation_and_split() {
        let t = IvpTime::normalized(3.0, 2.25);
        assert_eq!(t, IvpTime::new(5.0, 0.25));
        let t = IvpTime::normalized(0.0, 1.0);
        assert_eq!(t, IvpTime::new(0.0, 1.0), "exactly 1.0 is not carried");
        assert_eq!(IvpTime::new(4.0, 0.5).since(IvpTime::new(3.0, 0.75)), 0.75);
        assert_eq!(IvpTime::new(2.0, 0.75).split_sum(), IvpTime::new(2.0, 0.75));
    }

    #[test]
    fn quaternion_matrix_round_trip() {
        let mut q = IvpQuat::new(0.1, -0.2, 0.3, 0.9);
        q.normize();
        let mut m = IvpMatrix::IDENTITY;
        q.set_matrix(&mut m);
        let mut back = IvpQuat::IDENTITY;
        back.set_from_matrix(&m);
        for (a, b) in [(q.x, back.x), (q.y, back.y), (q.z, back.z), (q.w, back.w)] {
            assert!((a - b).abs() < 1e-6, "{a} {b}");
        }
        let p = [1.0, 2.0, 3.0];
        let rotated = m.vmult(p);
        let unrotated = m.vimult(rotated);
        for i in 0..3 {
            assert!((unrotated[i] - p[i]).abs() < 1e-5);
        }
        m.vv = [5.0, 6.0, 7.0];
        let world = m.vmult4(p);
        let local = m.vimult4(world);
        for i in 0..3 {
            assert!((local[i] - p[i]).abs() < 1e-5);
        }
    }

    #[test]
    fn quaternion_from_matrix_diagonal_branch() {
        let half_turn = IvpQuat::new(1.0, 0.0, 0.0, 0.0);
        let mut m = IvpMatrix::IDENTITY;
        half_turn.set_matrix(&mut m);
        let mut back = IvpQuat::IDENTITY;
        back.set_from_matrix(&m);
        assert_eq!(back, half_turn);
    }

    #[test]
    fn quaternion_products_and_inverse() {
        let mut a = IvpQuat::new(0.3, 0.1, -0.4, 0.8);
        a.normize();
        let mut b = IvpQuat::new(-0.2, 0.5, 0.1, 0.7);
        b.normize();
        let ab = IvpQuat::mult(&a, &b);
        let back = IvpQuat::inv_mult(&a, &ab);
        assert!((back.x - b.x).abs() < 1e-6 && (back.w - b.w).abs() < 1e-6);
        let back = IvpQuat::mult_inv(&ab, &b);
        assert!((back.y - a.y).abs() < 1e-6 && (back.z - a.z).abs() < 1e-6);
        let id = IvpQuat::mult(&a, &a.invert_unit());
        assert!((id.w - 1.0).abs() < 1e-6);
    }

    #[test]
    fn normalisers() {
        let mut q = IvpQuat::new(0.0, 0.0, 0.0, 1.01);
        q.fast_normize();
        assert!((q.w - 1.0).abs() < 1e-6);
        let mut q = IvpQuat::new(0.0, 0.0, 0.0, 1.000_000_1);
        let before = q;
        q.fast_normize();
        assert_eq!(q, before, "within 1e-6 the quaternion is left alone");
        let mut tiny = IvpQuat::new(0.0, 0.0, 0.0, 1e-6);
        tiny.normize();
        assert_eq!(tiny.w, 1e-6);
    }

    #[test]
    fn rotation_vectors() {
        let q = IvpQuat::fast_rot_vector([0.0, 0.0, 1.0], 0.1);
        assert!((q.z - 0.05f32.sin()).abs() < 1e-6);
        let q = IvpQuat::rot_vector([0.0, 2.0, 0.0], 0.1);
        assert!((q.y - 0.1f32.sin()).abs() < 1e-6);
        assert!((q.w - 0.1f32.cos()).abs() < 1e-6);
        let big = IvpQuat::rot_vector([40.0, 40.0, 0.0], 0.1);
        let len = (big.x * big.x + big.y * big.y + big.w * big.w).sqrt();
        assert!((len - 1.0).abs() < 1e-5);
        assert!(big.w > 0.0);
    }

    #[test]
    fn interpolation_ends() {
        let a = IvpQuat::IDENTITY;
        let b = IvpQuat::rot_vector([0.0, 0.0, 2.0], 0.5);
        let mut q = IvpQuat::IDENTITY;
        q.set_interpolate_quat(&a, &b, 1.0);
        assert!((q.z - b.z).abs() < 1e-6);
        q.set_interpolate_quat(&a, &b, 0.0);
        assert!((q.w - 1.0).abs() < 1e-6);
        q.set_interpolate_quat(&a, &a, 0.5);
        assert_eq!(q, a);
    }

    #[test]
    fn small_helpers() {
        assert_eq!(
            double_asin_vector([0.0, 1.0, -1.0])[1],
            std::f32::consts::PI
        );
        assert_eq!(
            add_multiple([1.0, 1.0, 1.0], [1.0, 2.0, 3.0], 2.0),
            [3.0, 5.0, 7.0]
        );
        assert_eq!(quad_length([1.0, 2.0, 2.0]), 9.0);
        assert_eq!(sub([1.0, 2.0, 3.0], [1.0, 1.0, 1.0]), [0.0, 1.0, 2.0]);
    }
}
