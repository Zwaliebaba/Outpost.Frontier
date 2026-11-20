//! Motion paths: `CV_CRSplinePath` and `CV_CROrientationSplinePath` of
//! `CppSource/SceneScript/SplinePath.cpp`.
//!
//! A path is a Catmull-Rom spline through its points with a ghost point
//! at each end, sampled 16 times per segment for its arc length; the
//! parameter of the motion-path interface is the fraction of that length.
//! The orientation variant also keeps a quaternion per point and blends
//! them with the reference's squad construction. Both answer the three
//! interfaces of the reference: `ICRSpline` (points, times, orientations,
//! open or closed), `IMotionPath` (position, tangent, orientation and
//! transform at a parameter, total length) and `IPersistString` (the text
//! form `OPEN, {x,y,z}, {w,x,y,z}, ...` the scripts' `path_data` holds).
//!
//! Methods keep the reference's result codes (0 for success, negative
//! codes for its failures) and its output parameters, which a failing
//! call leaves as they were. Arithmetic follows design/decisions.md T11;
//! the quaternion logarithm and exponential use `acos`, `sin` and `cos`,
//! so orientations are Tier B.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use crate::math::{self, Matrix, Quaternion};

use super::entity::{PathFactory, PathInit, PathObject};
use super::spline_math::{Vec3, catmull_rom_3d, catmull_rom_tangent_3d, lerp_3d, sample_parameter};
use crate::msvcrt::ucrt_atof;

/// `GR_OK`.
pub const OK: i32 = 0;
/// `GR_GENERIC`, also the reference's -1 for a missing core.
pub const GENERIC: i32 = -1;
/// An index past the stored points.
pub const BAD_INDEX: i32 = -2;
/// An orientation call on the position-only variant.
pub const NO_ORIENTATION: i32 = -7;
/// An orientation read before the orientations exist.
pub const ORIENTATION_NOT_READY: i32 = -8;

/// Samples per segment (`G_CrSamples`) and the parameter step between
/// them (`G_CrStep`).
const SAMPLES: usize = 16;
const STEP: f32 = 0.0625;
/// `G_QuatSinEps`: below this sine the logarithm and exponential copy.
const SIN_EPS: f32 = 0.001;

/// A quaternion, scalar first (`FQuat`).
pub type Quat = [f32; 4];

const IDENTITY: Quat = [1.0, 0.0, 0.0, 0.0];

fn ext(v: f32) -> f64 {
    f64::from(v)
}

fn round(v: f64) -> f32 {
    v as f32
}

/// `FLD dword` then `FSTP dword`: a signalling NaN comes out quiet.
fn fld_fstp(v: f32) -> f32 {
    if v.is_nan() {
        f32::from_bits(v.to_bits() | 0x0040_0000)
    } else {
        v
    }
}

fn mul_quat(a: Quat, b: Quat) -> Quat {
    math::mul_quat(&Quaternion::from_words(a), &Quaternion::from_words(b)).to_words()
}

fn slerp(a: Quat, b: Quat, t: f32) -> Quat {
    math::quat_slerp(&Quaternion::from_words(a), &Quaternion::from_words(b), t).to_words()
}

/// A motion path.
#[derive(Clone, Debug)]
pub struct SplinePath {
    orientation: bool,
    /// `bInnerAllocated_`: the core exists (after the first load).
    inner: bool,
    points: Vec<Vec3>,
    times: Vec<f32>,
    quats: Vec<Quat>,
    rows: Vec<Vec<f32>>,
    live_count: i32,
    point_count: i32,
    /// Set to 1 by the constructor and every nonempty load, never reset.
    first_live: i32,
    last_live: i32,
    sample_table_rows: i32,
    total_length: f32,
    orientation_ready: bool,
    closed: bool,
}

impl SplinePath {
    /// A new, empty path; `orientation` makes `CV_CROrientationSplinePath`.
    #[must_use]
    pub fn new(orientation: bool) -> SplinePath {
        SplinePath {
            orientation,
            inner: false,
            points: Vec::new(),
            times: Vec::new(),
            quats: Vec::new(),
            rows: Vec::new(),
            live_count: 0,
            point_count: 0,
            first_live: 1,
            last_live: 0,
            sample_table_rows: 0,
            total_length: 0.0,
            orientation_ready: false,
            closed: false,
        }
    }

    /// The path the parser created for a motion-path entity, filled as it
    /// filled it.
    #[must_use]
    pub fn from_desc(desc: &PathObject) -> SplinePath {
        let mut path = SplinePath::new(desc.factory == PathFactory::CrOrientationSpline);
        match &desc.init {
            PathInit::Empty => {}
            PathInit::Text(text) => {
                path.load_persist(text.as_deref());
            }
            PathInit::Points(points) => {
                path.load_points(points);
            }
        }
        path
    }

    /// Whether this is the orientation variant.
    #[must_use]
    pub fn is_orientation(&self) -> bool {
        self.orientation
    }

    /// `FreePath`.
    fn free(&mut self) {
        self.points = Vec::new();
        self.times = Vec::new();
        self.quats = Vec::new();
        self.rows = Vec::new();
        self.live_count = 0;
        self.point_count = 0;
        self.last_live = 0;
        self.sample_table_rows = 0;
        self.total_length = 0.0;
        self.inner = false;
        self.orientation_ready = false;
    }

    /// `ICRSpline::Slot4LoadPoints`: a new core through the points, open,
    /// with the constructor's extrapolated ghosts; identity orientations.
    pub fn load_points(&mut self, points: &[Vec3]) -> i32 {
        self.free();
        self.closed = false;
        let count = points.len();
        if count != 0 {
            self.live_count = count as i32;
            self.first_live = 1;
            self.last_live = count as i32;
            self.point_count = count as i32 + 3;
            let size = count + 3;
            self.points = vec![[0.0; 3]; size];
            self.times = vec![0.0; size];
            self.rows = vec![Vec::new(); size];
            self.sample_table_rows = self.last_live;
            self.quats = vec![[0.0; 4]; size];
            self.points[1..=count].copy_from_slice(points);
            self.apply_ctor_ghosts();
            self.rebuild_sample_tables();
        }
        self.inner = true;
        if self.orientation {
            self.fill_identity_quats();
        }
        OK
    }

    /// `FillIdentityQuats`.
    fn fill_identity_quats(&mut self) {
        let last = self.last_live as u32;
        if last == 0 {
            return;
        }
        self.orientation_ready = true;
        for index in 0..last {
            self.set_orientation_internal(index, IDENTITY);
        }
    }

    /// `ApplyCtorGhosts`: each ghost mirrors the Catmull-Rom point at 0.1
    /// (start) or 0.9 (end) of the segment between the end point and its
    /// neighbour, both doubled, through the end point.
    fn apply_ctor_ghosts(&mut self) {
        self.last_live = self.live_count;
        let p = &mut self.points;
        p[0] = catmull_rom_3d(p[1], p[1], p[2], p[2], 0.1);
        let difference = sub_ghost(p[0], p[1]);
        p[0] = sub_ghost(p[1], difference);
        let n = self.live_count as usize;
        let end = n + 1;
        p[end] = catmull_rom_3d(p[n - 1], p[n - 1], p[n], p[n], 0.9);
        let (last, ghost) = (p[n], p[end]);
        let dx = ext(last[0]) - ext(ghost[0]);
        let dy = ext(last[1]) - ext(ghost[1]);
        let dz = round(ext(last[2]) - ext(ghost[2]));
        p[end] = [
            round(dx + ext(last[0])),
            round(dy + ext(last[1])),
            round(ext(dz) + ext(last[2])),
        ];
    }

    /// `ApplyPersistGhosts`: an open path duplicates its ends; a closed
    /// one wraps round, its last segment returning to the first point.
    fn apply_persist_ghosts(&mut self) {
        if self.live_count <= 0 {
            return;
        }
        let first = self.first_live as usize;
        let p = &mut self.points;
        if self.closed {
            self.last_live = (self.point_count as u32).wrapping_sub(2) as i32;
            let last = self.last_live as usize;
            p[last] = p[first];
            p[last + 1] = p[first + 1];
            p[0] = p[last - 1];
        } else {
            self.last_live = (self.point_count as u32).wrapping_sub(3) as i32;
            let last = self.last_live as usize;
            p[0] = p[first];
            p[last + 1] = p[last];
        }
    }

    /// `RebuildSampleTables`: per segment, 17 cumulative chord lengths of
    /// the 16 samples (from the length before the segment); the times at
    /// the segment ends; then both normalised by the total length.
    fn rebuild_sample_tables(&mut self) {
        // The reference writes through null storage here when no point
        // was ever loaded (divergence D246).
        if self.times.is_empty() {
            return;
        }
        self.sample_table_rows = self.last_live;
        self.total_length = 0.0;
        let mut row = 1usize;
        for segment in self.first_live as u32..self.last_live as u32 {
            let s = segment as usize;
            let (p0, p1, p2, p3) = (
                self.points[s - 1],
                self.points[s],
                self.points[s + 1],
                self.points[s + 2],
            );
            let mut segment_length = 0.0_f32;
            let incoming = self.total_length;
            let mut table = vec![0.0_f32; SAMPLES + 1];
            let mut prev = catmull_rom_3d(p0, p1, p2, p3, 0.0);
            table[0] = incoming;
            for (sample, entry) in table.iter_mut().enumerate().skip(1) {
                let cur = catmull_rom_3d(p0, p1, p2, p3, sample as f32 * STEP);
                let d = [0, 1, 2].map(|i| ext(cur[i]) - ext(prev[i]));
                prev = cur;
                let chord = ((d[2] * d[2] + d[1] * d[1]) + d[0] * d[0]).sqrt();
                let sum = chord + ext(segment_length);
                segment_length = round(sum);
                *entry = round(sum + ext(incoming));
            }
            self.rows[row] = table;
            self.total_length = round(ext(self.total_length) + ext(segment_length));
            self.times[s + 1] = self.total_length;
            row += 1;
        }
        let inverse = 1.0 / ext(self.total_length);
        for table in &mut self.rows[1..row] {
            for value in table.iter_mut() {
                *value = round(inverse * ext(*value));
            }
        }
        self.times[1] = 0.0;
        self.times[0] = 0.0;
        self.times[self.last_live as u32 as usize + 1] = 0.0;
        let total = ext(self.total_length);
        for time in &mut self.times {
            *time = round(ext(*time) / total);
        }
    }

    /// `ICRSpline::Slot3`: replaces live point `index`.
    pub fn set_point(&mut self, index: u32, point: Vec3) -> i32 {
        if !self.inner || index >= self.last_live as u32 {
            return GENERIC;
        }
        if index >= self.point_count as u32 {
            return BAD_INDEX;
        }
        let mut storage = index as usize + 1;
        let last = self.last_live as usize;
        let first = self.first_live as usize;
        if self.closed && storage == last {
            storage = first;
        }
        let p = &mut self.points;
        p[storage] = point;
        if self.closed {
            p[last] = p[first];
            p[last + 1] = p[first + 1];
            p[0] = p[last - 1];
        } else {
            p[0] = p[1];
            p[last + 1] = p[last];
        }
        self.rebuild_sample_tables();
        OK
    }

    /// `ICRSpline::Slot5`: the live point count (one more when closed).
    #[must_use]
    pub fn live_points(&self) -> u32 {
        if self.inner { self.last_live as u32 } else { 0 }
    }

    /// `ICRSpline::Slot6`: stored point `index` (after the start ghost).
    pub fn point(&self, index: u32, out: &mut Vec3) -> i32 {
        if !self.inner || index > self.last_live as u32 {
            return GENERIC;
        }
        self.get_point(index, out)
    }

    fn get_point(&self, index: u32, out: &mut Vec3) -> i32 {
        if index >= self.point_count as u32 {
            return BAD_INDEX;
        }
        *out = self.points[index as usize + 1];
        OK
    }

    /// `ICRSpline::Slot7`: the normalised time of stored point `index`.
    pub fn time(&self, index: u32, out: &mut f32) -> i32 {
        if !self.inner || index > self.last_live as u32 {
            return GENERIC;
        }
        if index >= self.point_count as u32 {
            return BAD_INDEX;
        }
        *out = self.times[index as usize + 1];
        OK
    }

    /// `ICRSpline::Slot8`: opens or closes the path.
    pub fn set_closed(&mut self, closed: bool) -> i32 {
        self.closed = closed;
        self.apply_persist_ghosts();
        self.rebuild_sample_tables();
        OK
    }

    /// `ICRSpline::Slot9`.
    #[must_use]
    pub fn closed(&self) -> bool {
        self.closed
    }

    /// `SetOrientationInternal`, then the quaternion ghosts.
    fn set_orientation_internal(&mut self, index: u32, q: Quat) -> i32 {
        if !self.orientation_ready {
            return ORIENTATION_NOT_READY;
        }
        if index >= self.point_count as u32 {
            return GENERIC;
        }
        let mut storage = index as usize + 1;
        let (first, last) = (self.first_live as usize, self.last_live as usize);
        let quats = &mut self.quats;
        if self.closed {
            if storage == last {
                storage = first;
            }
            quats[storage] = q;
            quats[last] = quats[first];
            quats[last + 1] = quats[first + 1];
            quats[0] = quats[last - 1];
        } else {
            quats[storage] = q;
            quats[0] = quats[1];
            quats[last + 1] = quats[last];
        }
        OK
    }

    /// `ICRSpline::Slot10`: the orientation at live point `index`.
    pub fn set_orientation(&mut self, index: u32, q: Quat) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        if !self.inner || index >= self.last_live as u32 {
            return GENERIC;
        }
        self.set_orientation_internal(index, q)
    }

    /// `ICRSpline::Slot11`: every live orientation; the count must be the
    /// live point count.
    pub fn set_orientations(&mut self, quats: &[Quat]) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        if !self.inner || quats.len() as u64 != u64::from(self.last_live as u32) {
            return GENERIC;
        }
        for (index, q) in quats.iter().enumerate() {
            self.set_orientation_internal(index as u32, *q);
        }
        OK
    }

    /// `ICRSpline::Slot12`: stored orientation `index`.
    pub fn orientation(&self, index: u32, out: &mut Quat) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        if !self.inner || index > self.last_live as u32 {
            return GENERIC;
        }
        self.get_orientation(index, out)
    }

    fn get_orientation(&self, index: u32, out: &mut Quat) -> i32 {
        if !self.orientation_ready {
            return ORIENTATION_NOT_READY;
        }
        if index >= self.point_count as u32 {
            return BAD_INDEX;
        }
        *out = self.quats[index as usize + 1];
        OK
    }

    /// `ICRSpline::Slot13`: point `index` from two 3×3 records, the first's
    /// rotation and the second's first row as the position.
    pub fn set_from_matrices(&mut self, index: u32, records: &[f32; 18]) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        let matrix: [f32; 9] = records[..9].try_into().expect("nine words");
        let quat = math::matrix_to_quaternion(&Matrix::from_words(matrix)).to_words();
        let position = [records[9], records[10], records[11]];
        if self.set_point(index, position) != OK {
            return GENERIC;
        }
        self.set_orientation(index, quat)
    }

    /// `ICRSpline::Slot14`: loads the path from transforms (rotation then
    /// position, twelve words each).
    pub fn load_transforms(&mut self, records: &[[f32; 12]]) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        if !self.inner {
            return GENERIC;
        }
        let positions: Vec<Vec3> = records.iter().map(|r| [r[9], r[10], r[11]]).collect();
        let quats: Vec<Quat> = records
            .iter()
            .map(|r| {
                let m: [f32; 9] = r[..9].try_into().expect("nine words");
                math::matrix_to_quaternion(&Matrix::from_words(m)).to_words()
            })
            .collect();
        self.load_points(&positions);
        self.set_orientations(&quats);
        OK
    }

    /// `ICRSpline::Slot15`: the transform at stored point `index`. As in
    /// the reference it writes the record and returns -1.
    pub fn transform_at(&self, index: u32, out: &mut [f32; 12]) -> i32 {
        if !self.orientation {
            return NO_ORIENTATION;
        }
        if !self.inner || (self.last_live as u32) < index {
            return GENERIC;
        }
        // The reference uses whatever its locals held when a getter fails
        // (divergence D246): here the identity and the origin.
        let mut quat = IDENTITY;
        let mut position = [0.0; 3];
        self.get_orientation(index, &mut quat);
        self.get_point(index, &mut position);
        out[9..12].copy_from_slice(&position);
        let m = math::quaternion_to_matrix(&Quaternion::from_words(quat)).to_words();
        out[..9].copy_from_slice(&m);
        GENERIC
    }

    /// `IMotionPath::Slot7Length`: the total arc length.
    pub fn length(&self, out: &mut f32) -> i32 {
        if !self.inner {
            return GENERIC;
        }
        *out = self.total_length;
        OK
    }

    /// `FindPathSegment`: the segment whose end time is the first later
    /// than `t`.
    fn find_segment(&self, t: f32) -> Option<usize> {
        let first = self.first_live as u32;
        let last = self.last_live as u32;
        let mut index = first;
        let mut segment = 0u32;
        while index < last {
            if self.times[index as usize] > t {
                break;
            }
            segment += 1;
            index += 1;
        }
        (segment >= first && segment < last).then_some(segment as usize)
    }

    /// `FindSample`: the sample interval of the segment that holds `t`
    /// (an exact hit selects the interval before) and the fraction into
    /// it, rounded to `float`.
    fn find_sample(&self, segment: usize, t: f32) -> Option<(i32, f32)> {
        if segment as i64 >= i64::from(self.sample_table_rows) {
            return None;
        }
        let table = &self.rows[segment];
        let mut index = (table.len() / 2) as i32;
        let mut step = index;
        loop {
            let previous = index;
            step /= 2;
            if t < table[previous as usize] {
                step = -step;
            }
            index = previous + step;
            step = (previous - index).abs();
            if index == previous {
                break;
            }
        }
        if !(t > table[index as usize]) {
            index -= 1;
        }
        let (i0, i1) = (table[index as usize], table[index as usize + 1]);
        let ratio = round((ext(t) - ext(i0)) / (ext(i1) - ext(i0)));
        Some((index, ratio))
    }

    /// `EvalTwoSample`: the point (or unit tangent) at the two samples
    /// round `t`, blended. Points take the end points outside (0, 1).
    fn eval_two_sample(&self, t: f32, out: &mut Vec3, tangent: bool) -> bool {
        if self.live_count <= 0 {
            return false;
        }
        if !tangent {
            if t <= 0.0 {
                *out = self.points[self.first_live as usize];
                return true;
            }
            if t >= 1.0 {
                *out = self.points[self.last_live as usize];
                return true;
            }
        } else if self.last_live <= self.first_live {
            return false;
        }
        let Some(segment) = self.find_segment(t) else {
            return false;
        };
        let Some((sample, fraction)) = self.find_sample(segment, t) else {
            return false;
        };
        let p = &self.points;
        let (p0, p1, p2, p3) = (p[segment - 1], p[segment], p[segment + 1], p[segment + 2]);
        let u0 = sample_parameter(sample, STEP);
        let u1 = sample_parameter(sample + 1, STEP);
        let kernel = if tangent {
            catmull_rom_tangent_3d
        } else {
            catmull_rom_3d
        };
        let a = kernel(p0, p1, p2, p3, u0);
        let b = kernel(p0, p1, p2, p3, u1);
        *out = lerp_3d(a, b, fraction);
        true
    }

    /// `EvalOrientation`: the end orientations outside (0, 1); inside, the
    /// squad orientation at the two samples round `t`, blended.
    fn eval_orientation(&self, t: f32, out: &mut Quat) -> bool {
        if !self.orientation_ready {
            return false;
        }
        if t <= 0.0 {
            *out = self.quats[self.first_live as usize];
            return true;
        }
        if t >= 1.0 {
            *out = self.quats[self.last_live as usize];
            return true;
        }
        let Some(segment) = self.find_segment(t) else {
            return false;
        };
        let Some((sample, fraction)) = self.find_sample(segment, t) else {
            return false;
        };
        let q = &self.quats;
        let (q0, q1, q2, q3) = (q[segment - 1], q[segment], q[segment + 1], q[segment + 2]);
        let u0 = sample_parameter(sample, STEP);
        let u1 = sample_parameter(sample + 1, STEP);
        let a = cr_orientation(q0, q1, q2, q3, u0);
        let b = cr_orientation(q0, q1, q2, q3, u1);
        *out = slerp(a, b, fraction);
        true
    }

    /// `IMotionPath::Slot4Evaluate`: the point at parameter `t`. As in the
    /// reference, a core without the segment leaves `out` as it was and
    /// still answers 0.
    pub fn evaluate(&self, t: f32, out: &mut Vec3) -> i32 {
        if !self.inner {
            return GENERIC;
        }
        self.eval_two_sample(t, out, false);
        OK
    }

    /// `IMotionPath::Slot6LookAt`: the unit tangent at `t`.
    pub fn look_at(&self, t: f32, out: &mut Vec3) -> i32 {
        if !self.inner {
            return GENERIC;
        }
        self.eval_two_sample(t, out, true);
        OK
    }

    /// `IMotionPath::Slot5`: the orientation at `t` as a quaternion: the
    /// blended one for the orientation variant, else the frame facing
    /// along the reversed tangent with the world Y as its up reference.
    pub fn orientation_at(&self, t: f32, out: &mut Quat) -> i32 {
        if !self.inner {
            return GENERIC;
        }
        if self.orientation {
            self.eval_orientation(t, out);
            return OK;
        }
        let mut tangent = [0.0; 3];
        if !self.eval_two_sample(t, &mut tangent, true) {
            return GENERIC;
        }
        let m = slot5_matrix(tangent);
        *out = math::matrix_to_quaternion(&Matrix::from_words(m)).to_words();
        OK
    }

    /// `IMotionPath::Slot3Transform`: the transform at `t`, rotation then
    /// position. The position-only variant builds the rotation from the
    /// tangent and the rotation `out` already holds.
    pub fn transform(&self, t: f32, out: &mut [f32; 12]) -> i32 {
        if !self.inner {
            return GENERIC;
        }
        let mut position = [0.0; 3];
        if self.orientation {
            // A failed point leaves the reference's local as it was
            // (divergence D246): here the origin.
            self.eval_two_sample(t, &mut position, false);
            out[9..12].copy_from_slice(&position);
            let mut q = IDENTITY;
            self.eval_orientation(t, &mut q);
            let m = math::quaternion_to_matrix(&Quaternion::from_words(q)).to_words();
            out[..9].copy_from_slice(&m);
            return OK;
        }
        if !self.eval_two_sample(t, &mut position, false) {
            return GENERIC;
        }
        out[9..12].copy_from_slice(&position);
        let mut tangent = [0.0; 3];
        if !self.eval_two_sample(t, &mut tangent, true) {
            return GENERIC;
        }
        look_at_from_tangent(tangent, out);
        OK
    }

    /// `IPersistString::Slot5EstimateSize`.
    #[must_use]
    pub fn estimate_size(&self) -> u32 {
        let count = if self.inner { self.last_live as u32 } else { 0 };
        if self.orientation {
            count
                .wrapping_mul(40)
                .wrapping_add(count.wrapping_mul(51))
                .wrapping_add(10)
        } else {
            count.wrapping_mul(36).wrapping_add(10)
        }
    }

    /// `IPersistString::Slot3Load`: reads `CLOSED` or anything else (open)
    /// at the start, then `{x,y,z}` points, each followed by a `{w,x,y,z}`
    /// quaternion for the orientation variant. A quaternion that does not
    /// read ends quaternion reading and gives every point the identity.
    /// Fewer than two points fail (-1) and leave the path as it was.
    pub fn load_persist(&mut self, text: Option<&[u8]>) -> i32 {
        let Some(text) = text else {
            return GENERIC;
        };
        let text = text.split(|&b| b == 0).next().unwrap_or_default();
        let closed = text.starts_with(b"CLOSED");
        let mut points: Vec<Vec3> = Vec::new();
        let mut quats: Vec<Quat> = Vec::new();
        let mut read_quats = self.orientation;
        let mut missing_quat = false;
        let mut cursor = Some(0usize);
        while let Some(at) = cursor {
            let mut position = Some(at);
            let mut point = [0.0; 4];
            if !parse_floats(text, &mut position, &mut point, 3) {
                cursor = position;
                continue;
            }
            cursor = position;
            points.push([point[0], point[1], point[2]]);
            if read_quats {
                let saved = cursor;
                let mut quat = [0.0; 4];
                if parse_floats(text, &mut cursor, &mut quat, 4) {
                    quats.push(quat);
                } else {
                    cursor = saved;
                    read_quats = false;
                    missing_quat = true;
                }
            }
        }
        if points.len() <= 1 {
            return GENERIC;
        }
        self.load_points(&points);
        if self.orientation {
            let packed: Vec<Quat> = if missing_quat {
                vec![IDENTITY; points.len()]
            } else {
                quats
            };
            self.set_orientations(&packed);
        }
        self.closed = closed;
        self.apply_persist_ghosts();
        self.rebuild_sample_tables();
        OK
    }

    /// `IPersistString::Slot4Save`: the text [`Self::load_persist`] reads,
    /// with the points of the open path (the call reopens a closed one, as
    /// the reference does, and leaves it open). Fails (-1) without points
    /// or when `size` is smaller than [`Self::estimate_size`] or than the
    /// text before the last point.
    pub fn save(&mut self, size: u32) -> Result<Vec<u8>, i32> {
        match self.save_text(size) {
            (OK, Some(text)) => Ok(text),
            _ => Err(GENERIC),
        }
    }

    /// `IPersistString::Slot4Save` with what it leaves in the caller's
    /// buffer: the result, and the text written, `None` when the call
    /// writes nothing (no points, or `size` below [`Self::estimate_size`]).
    /// When the text reaches `size` before the last point the call fails
    /// with the text written so far; the reference writes that text past
    /// the end of its caller's `size + 1` bytes when it is longer
    /// (divergence D246), and here it is all returned.
    pub fn save_text(&mut self, size: u32) -> (i32, Option<Vec<u8>>) {
        let saved_count = self.live_points();
        let required = self.estimate_size();
        if saved_count == 0 || required > size {
            return (GENERIC, None);
        }
        let mut text = Vec::new();
        text.extend_from_slice(if self.closed { b"CLOSED, " } else { b"OPEN, " });
        self.closed = false;
        self.apply_persist_ghosts();
        self.rebuild_sample_tables();
        let mut written = 0;
        while written < saved_count {
            let mut p = [0.0; 3];
            self.get_point(written, &mut p);
            text.extend_from_slice(
                format!(
                    "{{{},{},{}}}, ",
                    printf_f(p[0]),
                    printf_f(p[1]),
                    printf_f(p[2])
                )
                .as_bytes(),
            );
            if self.orientation {
                let mut q = [0.0; 4];
                self.get_orientation(written, &mut q);
                text.extend_from_slice(
                    format!(
                        "{{{},{},{},{}}}, ",
                        printf_f(q[0]),
                        printf_f(q[1]),
                        printf_f(q[2]),
                        printf_f(q[3])
                    )
                    .as_bytes(),
                );
            }
            if text.len() as u64 >= u64::from(size) {
                break;
            }
            written += 1;
        }
        let status = if written == saved_count { OK } else { GENERIC };
        (status, Some(text))
    }
}

/// `%f` of the Universal C Runtime: the value correctly rounded to six
/// decimals; NaN as `nan`, or `-nan(ind)` for the negative default NaN and
/// `-nan` for other negative ones.
fn printf_f(v: f32) -> String {
    let v = f64::from(v);
    if v.is_nan() {
        let bits = v.to_bits();
        return match (bits >> 63 != 0, bits & ((1 << 51) - 1) == 0) {
            (false, _) => "nan".to_owned(),
            (true, true) => "-nan(ind)".to_owned(),
            (true, false) => "-nan".to_owned(),
        };
    }
    format!("{v:.6}")
}

/// `SubtractGhostVector`: `a - b`, rounded.
fn sub_ghost(a: Vec3, b: Vec3) -> Vec3 {
    [0, 1, 2].map(|i| round(ext(a[i]) - ext(b[i])))
}

/// `ParsePersistFloats`: `needed` numbers, each the first run of
/// `-0123456789.` after the cursor, read with `atof` from its start and
/// rounded to `float`; the cursor moves one byte past each run. A `}`
/// right after a number that is not the last ends the text, as does a
/// missing first number. The reference steps past the terminator after a
/// number that ends the text and reads on (divergence D246); here the
/// text ends there.
fn parse_floats(
    text: &[u8],
    cursor: &mut Option<usize>,
    out: &mut [f32; 4],
    needed: usize,
) -> bool {
    const NUMBER: &[u8] = b"-0123456789.";
    let mut local = [0.0_f32; 4];
    let mut index = 0;
    while index < needed {
        let from = cursor.unwrap_or(text.len()).min(text.len());
        let Some(start) = text[from..].iter().position(|b| NUMBER.contains(b)) else {
            if index == 0 {
                *cursor = None;
            }
            return false;
        };
        let number = from + start;
        let span = text[number..]
            .iter()
            .take_while(|b| NUMBER.contains(b))
            .count();
        local[index] = ucrt_atof(&text[number..]) as f32;
        let after = number + span;
        *cursor = Some(after);
        if text.get(after) == Some(&b'}') && index != needed - 1 {
            *cursor = None;
            return false;
        }
        index += 1;
        *cursor = Some(after + 1);
    }
    out[..needed].copy_from_slice(&local[..needed]);
    true
}

/// `QuatInverseScale`: `1 / |q|^2`.
fn inverse_scale(q: Quat) -> f32 {
    let (w, x, y, z) = (ext(q[0]), ext(q[1]), ext(q[2]), ext(q[3]));
    round(1.0 / (((x * x + y * y) + z * z) + w * w))
}

/// `QuatScale`.
fn quat_scale(q: Quat, s: f32) -> Quat {
    q.map(|c| round(ext(s) * ext(c)))
}

/// `QuatLogbaseline`: `(0, v acos(w) / sin(acos(w)))`, or `(0, v)` when
/// `|w|` is not below 1 or the sine is below [`SIN_EPS`]. With
/// `round_scale` the ratio is rounded to `float` before the products.
fn quat_log(q: Quat, round_scale: bool) -> Quat {
    let copy = [0.0, q[1], q[2], q[3]];
    if !(ext(q[0]).abs() < 1.0) {
        return copy;
    }
    let angle = ext(q[0]).acos();
    let sine = angle.sin();
    if !(sine >= ext(SIN_EPS)) {
        return copy;
    }
    let ratio = angle / sine;
    if round_scale {
        let scale = ext(round(ratio));
        [
            0.0,
            round(scale * ext(q[1])),
            round(scale * ext(q[2])),
            round(scale * ext(q[3])),
        ]
    } else {
        [
            0.0,
            round(ratio * ext(q[1])),
            round(ratio * ext(q[2])),
            round(ratio * ext(q[3])),
        ]
    }
}

/// `QuatFirstControlExp`: the exponential of the first squad control,
/// whose sine takes the unrounded norm.
fn quat_first_control_exp(q: Quat) -> Quat {
    let (x, y, z) = (ext(q[1]), ext(q[2]), ext(q[3]));
    let norm = ((x * x + y * y) + z * z).sqrt();
    let norm_f = round(norm);
    let sine = norm.sin();
    let mut result = [0.0, q[1], q[2], q[3]];
    if sine >= ext(SIN_EPS) {
        let scale = round(sine / ext(norm_f));
        for i in 1..4 {
            result[i] = round(ext(scale) * ext(q[i]));
        }
    }
    result[0] = round(ext(norm_f).cos());
    result
}

/// `QuatExp`: `(cos |v|, v sin |v| / |v|)` with the norm rounded to
/// `float`, or `(cos |v|, v)` when the sine is below [`SIN_EPS`].
fn quat_exp(q: Quat) -> Quat {
    let (x, y, z) = (ext(q[1]), ext(q[2]), ext(q[3]));
    let norm = ext(round(((x * x + y * y) + z * z).sqrt()));
    let sine = norm.sin();
    if !(sine >= ext(SIN_EPS)) {
        return [
            round(norm.cos()),
            fld_fstp(q[1]),
            fld_fstp(q[2]),
            fld_fstp(q[3]),
        ];
    }
    let ratio = sine / norm;
    [
        round(norm.cos()),
        round(ratio * x),
        round(ratio * y),
        round(ratio * z),
    ]
}

/// `QuatSquadControl`: `center exp(-(log(inv prev) + log(inv next)) / 4)`.
fn squad_control(prev: Quat, center: Quat, next: Quat, inverse: Quat, first: bool) -> Quat {
    let log_prev = quat_log(mul_quat(inverse, prev), true);
    let log_next = quat_log(mul_quat(inverse, next), first);
    let sum: Quat = [0, 1, 2, 3].map(|i| round(ext(log_prev[i]) + ext(log_next[i])));
    let scaled = quat_scale(sum, -0.25);
    let exp = if first {
        quat_first_control_exp(scaled)
    } else {
        quat_exp(scaled)
    };
    mul_quat(center, exp)
}

/// `EvalCrOrientation`: squad between `q1` and `q2` with controls from
/// their neighbours.
fn cr_orientation(q0: Quat, q1: Quat, q2: Quat, q3: Quat, u: f32) -> Quat {
    let conjugate = |q: Quat| [q[0], -q[1], -q[2], -q[3]];
    let inverse_a = quat_scale(conjugate(q1), inverse_scale(q1));
    let inverse_b = quat_scale(conjugate(q2), inverse_scale(q2));
    let control_a = squad_control(q0, q1, q2, inverse_a, true);
    let control_b = squad_control(q1, q2, q3, inverse_b, false);
    let slerp_ab = slerp(control_a, control_b, u);
    let slerp_q = slerp(q1, q2, u);
    let blend = round((1.0 - ext(u)) * ext(u) * 2.0);
    slerp(slerp_q, slerp_ab, blend)
}

/// `BuildSlot5Matrixbaseline`: the frame with forward `f` = minus the
/// unit tangent, side `s` = normalise(`f` × world Y) and up `u` =
/// normalise(`f` × `s`), as columns `s`, `u`, `f`; the first two side
/// components and the up vector stay on the x87 stack until stored.
fn slot5_matrix(tangent: Vec3) -> [f32; 9] {
    let (tx, ty, tz) = (ext(tangent[0]), ext(tangent[1]), ext(tangent[2]));
    let r = 1.0 / ((tz * tz + ty * ty) + tx * tx).sqrt();
    let f = [-round(tx * r), -round(ty * r), -round(tz * r)];
    let (fx, fy, fz) = (ext(f[0]), ext(f[1]), ext(f[2]));
    let a = fy * 0.0;
    let s18 = round(fz - a);
    let s1c = round(fx * 0.0 - fz * 0.0);
    let d20 = a - fx;
    let s20 = round(d20);
    let r2 = round(1.0 / (((d20 * ext(s20)) + ext(s1c) * ext(s1c)) + ext(s18) * ext(s18)).sqrt());
    let u18 = ext(s18) * ext(r2);
    let u1c = ext(s1c) * ext(r2);
    let side_z = round(ext(s20) * ext(r2));
    let c0 = fy * ext(side_z) - fz * u1c;
    let s28 = round(fz * u18 - fx * ext(side_z));
    let c2 = fx * u1c - fy * u18;
    let s2c = round(c2);
    let r3 = round(1.0 / (((c2 * ext(s2c)) + ext(s28) * ext(s28)) + c0 * c0).sqrt());
    let up = [
        round(c0 * ext(r3)),
        round(ext(s28) * ext(r3)),
        round(ext(s2c) * ext(r3)),
    ];
    [
        round(u18),
        up[0],
        f[0],
        round(u1c),
        up[1],
        f[1],
        side_z,
        up[2],
        f[2],
    ]
}

/// `NormalizeLookAtVector`: `v / sqrt((z^2 + y^2) + x^2)`.
fn normalize_look_at(v: Vec3) -> Vec3 {
    let (x, y, z) = (ext(v[0]), ext(v[1]), ext(v[2]));
    let r = 1.0 / ((z * z + y * y) + x * x).sqrt();
    v.map(|c| round(ext(c) * r))
}

/// `CrossLookAtVector`: `b × a`, or `a × b` with `reverse`.
fn cross_look_at(a: Vec3, b: Vec3, reverse: bool) -> Vec3 {
    let (ax, ay, az) = (ext(a[0]), ext(a[1]), ext(a[2]));
    let (bx, by, bz) = (ext(b[0]), ext(b[1]), ext(b[2]));
    if reverse {
        [
            round(ay * bz - az * by),
            round(az * bx - ax * bz),
            round(ax * by - ay * bx),
        ]
    } else {
        [
            round(az * by - ay * bz),
            round(ax * bz - az * bx),
            round(ay * bx - ax * by),
        ]
    }
}

/// `BuildLookAtFromTangent`: column 0 the reversed unit tangent, column 1
/// normalise(old column 1 × it), column 2 minus normalise(that unnormalised
/// cross × old column 0).
fn look_at_from_tangent(tangent: Vec3, out: &mut [f32; 12]) {
    let forward = normalize_look_at(tangent).map(|c| -c);
    let up_ref = [out[1], out[4], out[7]];
    let axis_y_raw = cross_look_at(forward, up_ref, false);
    let axis_y = normalize_look_at(axis_y_raw);
    let existing_x = [out[0], out[3], out[6]];
    let axis_z = cross_look_at(axis_y_raw, existing_x, true);
    let (x, y, z) = (ext(axis_z[0]), ext(axis_z[1]), ext(axis_z[2]));
    let r = 1.0 / ((x * x + y * y) + z * z).sqrt();
    let axis_z = axis_z.map(|c| round(ext(c) * r));
    out[0] = forward[0];
    out[1] = axis_y[0];
    out[3] = forward[1];
    out[6] = forward[2];
    out[4] = axis_y[1];
    out[7] = axis_y[2];
    out[5] = -axis_z[1];
    out[2] = -axis_z[0];
    out[8] = -axis_z[2];
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_straight_path_is_walked_by_length() {
        let mut path = SplinePath::new(false);
        assert_eq!(
            path.load_points(&[[0.0; 3], [1.0, 0.0, 0.0], [2.0, 0.0, 0.0]]),
            OK
        );
        let mut length = 0.0;
        assert_eq!(path.length(&mut length), OK);
        assert!((length - 2.0).abs() < 1e-3);
        let mut p = [9.0; 3];
        assert_eq!(path.evaluate(0.5, &mut p), OK);
        assert!((p[0] - 1.0).abs() < 1e-3, "{p:?}");
        assert_eq!(path.evaluate(1.0, &mut p), OK);
        assert_eq!(p, [2.0, 0.0, 0.0]);
        let mut tangent = [0.0; 3];
        path.look_at(0.25, &mut tangent);
        assert!((tangent[0] - 1.0).abs() < 1e-6);
    }

    #[test]
    fn persist_text_round_trips() {
        let mut path = SplinePath::new(true);
        let text = b"OPEN, {0,0,0}, {1,0,0,0}, {0,0,10}, {0.707107,0,0.707107,0}, ";
        assert_eq!(path.load_persist(Some(text)), OK);
        assert_eq!(path.live_points(), 2);
        let saved = path.save(1000).unwrap();
        assert_eq!(
            saved,
            b"OPEN, {0.000000,0.000000,0.000000}, {1.000000,0.000000,0.000000,0.000000}, \
              {0.000000,0.000000,10.000000}, {0.707107,0.000000,0.707107,0.000000}, "
                .to_vec()
        );
        let mut one = SplinePath::new(false);
        assert_eq!(one.load_persist(Some(b"OPEN, {1,2,3}")), GENERIC);
        assert_eq!(one.load_persist(None), GENERIC);
    }
}
