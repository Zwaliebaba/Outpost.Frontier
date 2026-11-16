//! `ICRSplineSegment`: a uniform Catmull-Rom segment, its evaluation, arc
//! length and parameter search (`common_crspline.cpp`).
//!
//! The arithmetic follows the reference compiled for x87 with
//! `/fp:precise`, run at 53-bit precision (decision T11): every expression
//! is evaluated in `f64`, and a value is rounded to `f32` where the C++
//! stores it in a `float` (an assignment, a cast, an argument or a return).
//! Only `+ - * /` and `sqrt` occur, so the results are exact as long as no
//! intermediate leaves the `f64` exponent range.

/// A point or direction, `x, y, z`.
pub type Vec3 = [f32; 3];

/// `ICRSplineSegment`. `p1` to `p2` is the curve; `p0` and `p3` shape its
/// tangents.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct CrSplineSegment {
    /// The control point before the segment.
    pub p0: Vec3,
    /// The start: the curve at 0.
    pub p1: Vec3,
    /// The end: the curve at 1.
    pub p2: Vec3,
    /// The control point after the segment.
    pub p3: Vec3,
    /// The length [`calculate_arclength`](Self::calculate_arclength)
    /// writes.
    pub arc_length: f32,
    /// The number of chords the length functions sum; 20 by default.
    pub sample_count: u32,
}

/// The sample count the C++ constructor sets.
pub const DEFAULT_SAMPLE_COUNT: u32 = 20;

/// One component of `calculate_spline_vector`, in the reference's term
/// order: `0.5 * (2 p1 + ((p2 - p0) t + ((2 p0 - 5 p1 + 4 p2 - p3) t2 +
/// (3 p1 - p0 - 3 p2 + p3) t3)))`.
fn component(p0: f32, p1: f32, p2: f32, p3: f32, t: f32, t2: f32, t3: f32) -> f32 {
    let (p0, p1, p2, p3) = (f64::from(p0), f64::from(p1), f64::from(p2), f64::from(p3));
    let two_p1 = (p1 * 2.0) as f32;
    let linear = ((-p0 + p2) * f64::from(t)) as f32;
    let quadratic = ((p0 + p0 - p1 * 5.0 + p2 * 4.0 - p3) * f64::from(t2)) as f32;
    let cubic = ((p1 * 3.0 + -p0 - p2 * 3.0 + p3) * f64::from(t3)) as f32;
    let rest = (f64::from(linear) + (f64::from(quadratic) + f64::from(cubic))) as f32;
    ((f64::from(two_p1) + f64::from(rest)) * 0.5) as f32
}

/// `(double)a - b` for each component.
fn difference(a: Vec3, b: Vec3) -> [f64; 3] {
    [
        f64::from(a[0]) - f64::from(b[0]),
        f64::from(a[1]) - f64::from(b[1]),
        f64::from(a[2]) - f64::from(b[2]),
    ]
}

impl CrSplineSegment {
    /// A segment through the four points with [`DEFAULT_SAMPLE_COUNT`]
    /// samples and no length yet.
    #[must_use]
    pub const fn new(p0: Vec3, p1: Vec3, p2: Vec3, p3: Vec3) -> Self {
        Self {
            p0,
            p1,
            p2,
            p3,
            arc_length: 0.0,
            sample_count: DEFAULT_SAMPLE_COUNT,
        }
    }

    /// `calculate_spline_vector`: the point at `t` (0 is `p1`, 1 is `p2`).
    #[must_use]
    pub fn point(&self, t: f32) -> Vec3 {
        let td = f64::from(t);
        let t2 = (td * td) as f32;
        let t3 = (td * td * td) as f32;
        let axis = |i: usize| component(self.p0[i], self.p1[i], self.p2[i], self.p3[i], t, t2, t3);
        [axis(0), axis(1), axis(2)]
    }

    /// `calculate_spline_tangent`: the unit chord between `t - 0.001` and
    /// `t + 0.001`. A zero chord gives NaN, as in the C++.
    #[must_use]
    pub fn tangent(&self, t: f32) -> Vec3 {
        let a = self.point((f64::from(t) - 0.001) as f32);
        let b = self.point((f64::from(t) + 0.001) as f32);
        let [dx, dy, dz] = difference(b, a);
        let dz_stored = dz as f32;
        let inv_len = (1.0 / (dz * f64::from(dz_stored) + dy * dy + dx * dx).sqrt()) as f32;
        [
            (dx * f64::from(inv_len)) as f32,
            (dy * f64::from(inv_len)) as f32,
            (f64::from(dz_stored) * f64::from(inv_len)) as f32,
        ]
    }

    /// `calculate_arclength`: sets [`arc_length`](Self::arc_length) to the
    /// sum of `sample_count` chords, each times the step `1 / sample_count`.
    /// The first chord starts at `p0`, not at the curve's start.
    pub fn calculate_arclength(&mut self) {
        let mut prev = self.p0;
        let step = (1.0 / f64::from(self.sample_count)) as f32;
        self.arc_length = 0.0;
        for i in 1..=self.sample_count {
            let v = self.point((f64::from(i) * f64::from(step)) as f32);
            let [dx, dy, dz] = difference(v, prev);
            prev = v;
            self.arc_length = ((dz * dz + dy * dy + dx * dx).sqrt() * f64::from(step)
                + f64::from(self.arc_length)) as f32;
        }
    }

    /// `get_spline_length`. The C++ computes each sample's parameter with an
    /// unsigned integer division, so every sample but the last is at 0 and
    /// the last at 1: the result is `|p2 - p1|`, summed as the C++ does.
    #[must_use]
    pub fn spline_length(&self) -> f32 {
        let mut prev = self.p1;
        let mut total = 0.0f32;
        for i in 1..=self.sample_count {
            let t = i / self.sample_count;
            let v = self.point(t as f32);
            let [dx, dy, dz] = difference(v, prev);
            prev = v;
            total = ((dz * dz + dy * dy + dx * dx).sqrt() + f64::from(total)) as f32;
        }
        total
    }

    /// `get_t_from_point`: steps the parameter from `start` by `step` while
    /// the squared distance to `point` does not grow, and returns the last
    /// accepted parameter.
    ///
    /// The C++ loops forever when the search can make no more progress: once
    /// the distance is NaN (unordered compares keep going), or when the
    /// parameter no longer changes and the distance does not grow. Those
    /// cases return `None`.
    #[must_use]
    pub fn t_from_point(&self, point: Vec3, start: f32, step: f32) -> Option<f32> {
        let s = difference(self.point(start), point);
        let mut best = (s[0] * s[0] + s[2] * s[2] + s[1] * s[1]) as f32;
        let mut t = start;
        loop {
            if best.is_nan() {
                return None;
            }
            let next_t = (f64::from(t) + f64::from(step)) as f32;
            let [dx, dy, dz] = difference(self.point(next_t), point);
            let next = dz * dz + dy * dy + dx * dx;
            if next > f64::from(best) {
                return Some(t);
            }
            let next_best = next as f32;
            if next_t.to_bits() == t.to_bits() && next_best.to_bits() == best.to_bits() {
                return None;
            }
            best = next_best;
            t = next_t;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn segment() -> CrSplineSegment {
        CrSplineSegment::new(
            [0.0, 0.0, 0.0],
            [1.0, 0.0, 0.0],
            [2.0, 1.0, 0.0],
            [3.0, 3.0, 1.0],
        )
    }

    #[test]
    fn ends_are_the_inner_points() {
        let s = segment();
        assert_eq!(s.point(0.0), s.p1);
        assert_eq!(s.point(1.0), s.p2);
    }

    #[test]
    fn straight_line_tangent_and_lengths() {
        let mut s =
            CrSplineSegment::new([0.0; 3], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0], [3.0, 0.0, 0.0]);
        assert_eq!(s.point(0.5), [1.5, 0.0, 0.0]);
        assert_eq!(s.tangent(0.5), [1.0, 0.0, 0.0]);
        assert_eq!(s.spline_length(), 1.0);
        s.calculate_arclength();
        // The first chord runs from p0 to the first sample.
        assert!((s.arc_length - 0.1).abs() < 1e-6, "{}", s.arc_length);
    }

    #[test]
    fn search_stops_when_the_distance_grows() {
        let s = segment();
        let target = s.point(0.4);
        let t = s.t_from_point(target, 0.0, 0.05);
        assert!(t.is_some_and(|t| (t - 0.4).abs() < 0.03), "{t:?}");
    }

    #[test]
    fn hopeless_searches_end() {
        let flat = CrSplineSegment::new([1.0; 3], [1.0; 3], [1.0; 3], [1.0; 3]);
        assert_eq!(flat.t_from_point([0.0; 3], 0.0, 0.0), None);
        assert_eq!(segment().t_from_point([f32::NAN; 3], 0.0, 0.1), None);
        assert_eq!(flat.t_from_point([0.0; 3], 0.0, 1e30), None);
    }
}
