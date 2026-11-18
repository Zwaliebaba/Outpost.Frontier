//! Param curves: the `IParamCurve` objects of `CppSource/SceneScript/
//! ParamCurve.cpp`, which map an event's progress (0 to 1) to the value
//! that drives its animation.
//!
//! `SceneScript.dll` registers eleven factory names for one class with
//! three evaluators: a Catmull-Rom spline through the keys, a linear
//! interpolation, and a Hermite spline whose tangents a mode sets (step,
//! linear, ramp up, ramp down, smooth, bump out, bump in) or the keys give
//! (free form). The name `LinearPCurve` is registered twice; the component
//! manager hands out the most recent registration, the Hermite linear
//! curve, so that is what [`Curve::new`] makes for it.
//!
//! A curve keeps its keys three times, as the reference does: by id (a
//! sorted map whose ids 0 and 1 are the end points at times 0 and 1), as
//! an evaluation list rebuilt from the map and sorted by time with the
//! reference's merge sort, and, for the Hermite kinds, as a knot array
//! rebuilt by insertion with the tangents recomputed after each insertion.
//!
//! Arithmetic follows design/decisions.md T11. The reference keeps its
//! result on the x87 stack (it returns `float` in `ST(0)` and callers use
//! the unrounded value), so [`Curve::evaluate`] returns an `f64`.

// The reference's x87 comparisons send unordered operands (NaN) to one
// side; the port spells each outcome as the comparison that is true
// exactly when the reference takes that side, which needs negated
// comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use std::collections::BTreeMap;

use neuron_core::num;

/// The evaluator of a curve kind.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Eval {
    CatmullRom,
    Lerp,
    Hermite(u8),
}

/// A row of the reference's `SpecFor` table.
#[derive(Debug)]
struct Spec {
    factory: &'static str,
    display: &'static str,
    eval: Eval,
    clamp_non_negative: bool,
}

/// `SpecFor`'s table, in its order. Rows 2 and 6 share the factory name
/// `LinearPCurve`.
const SPECS: [Spec; 12] = [
    Spec {
        factory: "CatmullRomPCurve",
        display: "CatmullRom",
        eval: Eval::CatmullRom,
        clamp_non_negative: false,
    },
    Spec {
        factory: "SceneScriptParamCurve",
        display: "CatmullRom(non-negative)",
        eval: Eval::CatmullRom,
        clamp_non_negative: true,
    },
    Spec {
        factory: "LinearPCurve",
        display: "Linear",
        eval: Eval::Lerp,
        clamp_non_negative: false,
    },
    Spec {
        factory: "SceneScriptLPCurve",
        display: "Linear(non-negative)",
        eval: Eval::Lerp,
        clamp_non_negative: true,
    },
    Spec {
        factory: "SmoothPCurve",
        display: "Smooth",
        eval: Eval::Hermite(4),
        clamp_non_negative: false,
    },
    Spec {
        factory: "StepPCurve",
        display: "Step",
        eval: Eval::Hermite(0),
        clamp_non_negative: false,
    },
    Spec {
        factory: "LinearPCurve",
        display: "Linear",
        eval: Eval::Hermite(1),
        clamp_non_negative: false,
    },
    Spec {
        factory: "RampUpPCurve",
        display: "Ramp Up",
        eval: Eval::Hermite(2),
        clamp_non_negative: false,
    },
    Spec {
        factory: "RampDownPCurve",
        display: "Ramp Down",
        eval: Eval::Hermite(3),
        clamp_non_negative: false,
    },
    Spec {
        factory: "BumpOutPCurve",
        display: "Bump Out",
        eval: Eval::Hermite(5),
        clamp_non_negative: false,
    },
    Spec {
        factory: "BumpInPCurve",
        display: "Bump In",
        eval: Eval::Hermite(6),
        clamp_non_negative: false,
    },
    Spec {
        factory: "FreeFormPCurve",
        display: "Free Form",
        eval: Eval::Hermite(7),
        clamp_non_negative: false,
    },
];

/// The smooth row, which every other Hermite kind is first built as.
const SMOOTH: usize = 4;

/// `G_MinDeltaT`: the smallest time step the tangent modes divide by.
const MIN_DELTA_T: f32 = 1.0e-4;

/// `FLD dword` then `FSTP dword`: a signalling NaN comes out quiet.
fn fld_fstp(value: f32) -> f32 {
    if value.is_nan() {
        f32::from_bits(value.to_bits() | 0x0040_0000)
    } else {
        value
    }
}

/// A key (`FParamKey`): time, value, two extra words (the free-form
/// tangents) and the id.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Key {
    t: f32,
    value: f32,
    extra0: f32,
    extra1: f32,
    id: i32,
}

/// A Hermite knot (`FHermiteKnot`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Knot {
    value: f32,
    t: f32,
    m_in: f32,
    m_out: f32,
}

impl Knot {
    /// `ConstructHermiteRecord`: value and time copied raw, the tangents
    /// through the x87 stack.
    fn construct(source: &Knot) -> Knot {
        Knot {
            value: source.value,
            t: source.t,
            m_in: fld_fstp(source.m_in),
            m_out: fld_fstp(source.m_out),
        }
    }
}

/// A param curve.
#[derive(Clone, Debug)]
pub struct Curve {
    spec: &'static Spec,
    /// `EvaluationKeys_`.
    list: Vec<Key>,
    /// `KeysOwner_.Items`, by signed id.
    map: BTreeMap<i32, Key>,
    /// `Knots_`, its length `KnotCount_`.
    knots: Vec<Knot>,
    /// `KnotCapacity_`, which survives rebuilds.
    knot_capacity: u32,
    next_id: i32,
}

/// The names `SceneScript.dll` registers, in its registration order.
pub const CURVE_CLASSES: [&str; 11] = super::event::CURVE_CLASSES;

impl Curve {
    /// The curve the component manager creates for a factory name, or
    /// `None` for a name `SceneScript.dll` does not register. Names
    /// compare exactly.
    #[must_use]
    pub fn new(factory: &str) -> Option<Curve> {
        let index = if factory == "LinearPCurve" {
            6
        } else {
            SPECS.iter().position(|s| s.factory == factory)?
        };
        Some(Curve::with_spec(&SPECS[index]))
    }

    /// The constructor: the two end keys, a rebuild as a smooth curve for
    /// every Hermite kind but smooth, then a rebuild as itself.
    fn with_spec(spec: &'static Spec) -> Curve {
        let derived = matches!(spec.eval, Eval::Hermite(mode) if mode != 4);
        let mut curve = Curve {
            spec: if derived { &SPECS[SMOOTH] } else { spec },
            list: Vec::new(),
            map: BTreeMap::new(),
            knots: Vec::new(),
            knot_capacity: 0,
            next_id: 2,
        };
        let first = Key {
            t: 0.0,
            value: 0.0,
            extra0: 0.0,
            extra1: 0.0,
            id: 0,
        };
        let second = Key {
            t: 1.0,
            value: 1.0,
            extra0: 0.0,
            extra1: 0.0,
            id: 1,
        };
        curve.list.push(first);
        curve.list.push(second);
        curve.map.insert(0, first);
        curve.map.insert(1, second);
        curve.rebuild();
        if derived {
            curve.spec = spec;
            curve.rebuild();
        }
        curve
    }

    /// The curve the parser made from a script's `param_curve`: created by
    /// name, then given each key in order (`IParamCurve::AddKey`).
    #[must_use]
    pub fn from_desc(desc: &super::event::ParamCurve) -> Option<Curve> {
        let mut curve = Curve::new(desc.class)?;
        for key in &desc.keys {
            curve.add_key(key[0], key[1], key[2], key[3]);
        }
        Some(curve)
    }

    /// `Slot10Clsid`: the factory name.
    #[must_use]
    pub fn class_name(&self) -> &'static str {
        self.spec.factory
    }

    /// `Slot11Display`: the display name.
    #[must_use]
    pub fn display_name(&self) -> &'static str {
        self.spec.display
    }

    /// `GetKeyCount`: the keys by id.
    #[must_use]
    pub fn key_count(&self) -> u32 {
        self.map.len() as u32
    }

    /// `Slot9IdByIndex`: the id at a position of the evaluation list, or
    /// -1.
    #[must_use]
    pub fn id_by_index(&self, index: i32) -> i32 {
        self.list
            .get(index as u32 as usize)
            .map_or(-1, |key| key.id)
    }

    /// `Slot5ReadKey`: time, value and the two extra words of a key.
    #[must_use]
    pub fn read_key(&self, id: i32) -> Option<[f32; 4]> {
        self.map
            .get(&id)
            .map(|k| [k.t, k.value, k.extra0, k.extra1])
    }

    /// `AddKey`: a time below 0 or above 1 is refused (-1); a time of 0
    /// or 1 replaces that end key and returns its id; any other time,
    /// NaN included, adds a key and returns its id.
    pub fn add_key(&mut self, f0: f32, f1: f32, f2: f32, f3: f32) -> i32 {
        if f0 < 0.0 || f0 > 1.0 {
            return -1;
        }
        if f0 == 0.0 {
            self.replace(0, [f0, f1, f2, f3]);
            self.rebuild();
            return 0;
        }
        if f0 == 1.0 {
            self.replace(1, [f0, f1, f2, f3]);
            self.rebuild();
            return 1;
        }
        let id = self.next_id;
        let record = Key {
            t: f0,
            value: f1,
            extra0: f2,
            extra1: f3,
            id,
        };
        self.list.push(record);
        // A wrapped id overwrites its namesake; the list keeps both.
        self.map.insert(id, record);
        loop {
            self.next_id = self.next_id.wrapping_add(1);
            if !matches!(self.next_id, -1..=1) {
                break;
            }
        }
        self.rebuild();
        id
    }

    /// `Slot4Replace`: new words for a key. The end keys keep their times;
    /// another key's time is clamped to [0, 1] and refused when it is then
    /// exactly 0 or 1. A NaN time is kept.
    pub fn replace(&mut self, id: i32, words: [f32; 4]) -> bool {
        if !self.map.contains_key(&id) {
            return false;
        }
        let t = match id {
            0 => 0.0,
            1 => 1.0,
            _ if words[0] > 1.0 => 1.0,
            _ if words[0] < 0.0 => 0.0,
            // Through the x87 stack.
            _ => fld_fstp(words[0]),
        };
        if (t == 0.0 && id != 0) || (t == 1.0 && id != 1) {
            return false;
        }
        let key = self.map.get_mut(&id).expect("checked above");
        key.t = t;
        key.value = words[1];
        key.extra1 = words[3];
        key.extra0 = words[2];
        self.rebuild();
        true
    }

    /// `Slot7DeleteKey`: removes a key other than the end keys. As in the
    /// reference, the evaluation list loses its last node, whichever key
    /// it held; the rebuild then rewrites the list from the map.
    pub fn delete_key(&mut self, id: i32) -> bool {
        if id == 0 || id == 1 || self.map.remove(&id).is_none() {
            return false;
        }
        self.list.pop();
        self.rebuild();
        true
    }

    /// `Slot13Rebuild`: the list from the map in id order, sorted by time;
    /// the non-negative clamp; for the Hermite kinds the knots, one
    /// insertion and one tangent pass per key.
    fn rebuild(&mut self) {
        for (node, key) in self.list.iter_mut().zip(self.map.values()) {
            *node = *key;
        }
        sort_evaluation_list(&mut self.list);
        if self.spec.clamp_non_negative {
            self.clamp_non_negative();
        }
        let Eval::Hermite(mode) = self.spec.eval else {
            return;
        };
        self.knots.clear();
        let keys: Vec<Key> = self.map.values().copied().collect();
        for key in keys {
            self.insert_knot(Knot {
                value: key.value,
                t: key.t,
                m_in: key.extra0,
                m_out: key.extra1,
            });
            apply_tangents(&mut self.knots, mode);
        }
    }

    /// `ClampNonNegative`: in time order, a value below the one before it
    /// (0 before the first) takes that value, in the list and in the map.
    fn clamp_non_negative(&mut self) {
        let mut tracker = 0.0_f32;
        for node in &mut self.list {
            if node.value < tracker {
                node.value = tracker;
                if let Some(payload) = self.map.get_mut(&node.id) {
                    payload.value = tracker;
                }
            }
            tracker = node.value;
        }
    }

    /// `InsertDerivedRecord`: before the first knot whose time is not
    /// below the new one's. The array grows 0, 1, 2, 4, ...; which knots
    /// pass through the x87 stack (quieting a signalling NaN tangent)
    /// depends on whether it grows.
    fn insert_knot(&mut self, record: Knot) {
        let count = self.knots.len();
        let at = self
            .knots
            .iter()
            .position(|k| record.t <= k.t)
            .unwrap_or(count);
        if self.knot_capacity as usize == count {
            let growth = if count <= 1 { 1 } else { count as u32 };
            self.knot_capacity = count as u32 + growth;
            let mut grown = Vec::with_capacity(count + 1);
            grown.extend(self.knots[..at].iter().map(Knot::construct));
            grown.push(Knot::construct(&record));
            grown.extend(self.knots[at..].iter().map(Knot::construct));
            self.knots = grown;
        } else if at == count {
            self.knots.push(Knot::construct(&record));
        } else {
            let last = Knot::construct(&self.knots[count - 1]);
            self.knots.push(last);
            for index in (at + 1..count).rev() {
                self.knots[index] = self.knots[index - 1];
            }
            self.knots[at] = record;
        }
    }

    /// `Evaluate`: the curve at `t`, unrounded.
    #[must_use]
    pub fn evaluate(&self, t: f32) -> f64 {
        match self.spec.eval {
            Eval::Hermite(_) => self.evaluate_hermite(t),
            Eval::Lerp => self.evaluate_lerp(t),
            Eval::CatmullRom => self.evaluate_catmull_rom(t),
        }
    }

    /// `Slot12WrapEvaluate`: the curve at `(a mod b) / b`, the quotient
    /// rounded to `float`. `None` where the reference's `IDIV` faults
    /// (`b` is 0, or `a` is `i32::MIN` and `b` is -1; divergence D245).
    #[must_use]
    pub fn wrap_evaluate(&self, a: i32, b: i32) -> Option<f64> {
        let remainder = a.checked_rem(b)?;
        let normalized = (f64::from(remainder) / f64::from(b)) as f32;
        Some(self.evaluate(normalized))
    }

    /// The Catmull-Rom walk: the segment that ends at the first key later
    /// than `t`, or the value of a key at exactly `t`; 0 before the first
    /// key and past the last.
    fn evaluate_catmull_rom(&self, t: f32) -> f64 {
        for (index, key) in self.list.iter().enumerate() {
            if key.t > t {
                if index == 0 {
                    break;
                }
                return self.evaluate_catmull(index - 1, index, t);
            }
            if key.t == t {
                return f64::from(key.value);
            }
        }
        0.0
    }

    /// `EvaluateCatmull`: the segment's parameter rounded to `float`; the
    /// outside points are the keys beyond the segment, or the segment's
    /// own ends where there are none.
    fn evaluate_catmull(&self, left: usize, right: usize, t: f32) -> f64 {
        let (t0, t1) = (f64::from(self.list[left].t), f64::from(self.list[right].t));
        let u = ((f64::from(t) - t0) / (t1 - t0)) as f32;
        let p1 = self.list[left].value;
        let p2 = self.list[right].value;
        let p0 = if left == 0 {
            p1
        } else {
            self.list[left - 1].value
        };
        let p3 = self.list.get(right + 1).map_or(p2, |k| k.value);
        catmull_rom_1d(p0, p1, p2, p3, u)
    }

    /// `EvaluateLerp`: between the first key later than `t` and the one
    /// before it; 0 before the first key and from the last key on.
    fn evaluate_lerp(&self, t: f32) -> f64 {
        for (index, key) in self.list.iter().enumerate() {
            if key.t > t {
                if index == 0 {
                    break;
                }
                let l = &self.list[index - 1];
                let ratio = (f64::from(t) - f64::from(l.t)) / (f64::from(key.t) - f64::from(l.t));
                return f64::from(l.value) + ratio * (f64::from(key.value) - f64::from(l.value));
            }
        }
        0.0
    }

    /// `HermiteEvaluate`: the end values outside the knots; inside, a
    /// segment found from a guess proportional to time, then walked.
    fn evaluate_hermite(&self, t: f32) -> f64 {
        let knots = &self.knots;
        let count = knots.len();
        if count == 1 {
            return f64::from(knots[0].value);
        }
        let last = count - 1;
        let (first_t, last_t) = (knots[0].t, knots[last].t);
        if t < first_t {
            return f64::from(knots[0].value);
        }
        if t > last_t {
            return f64::from(knots[last].value);
        }
        let guess = (f64::from(t) - f64::from(first_t)) / (f64::from(last_t) - f64::from(first_t))
            * last as f64;
        // FISTP qword with truncation; the low word is the index, which
        // lies in the knots for every time that reaches here.
        let seed = (num::trunc_to_i64(guess) as u32 as usize).min(last);
        let (left, right) = if knots[seed].t > t {
            let mut c = seed;
            if c > 0 {
                loop {
                    if knots[c].t < t {
                        break;
                    }
                    c -= 1;
                    if c == 0 {
                        break;
                    }
                }
            }
            (c, c + 1)
        } else {
            let mut c = seed;
            while c <= last && !(knots[c].t > t) {
                c += 1;
            }
            (c - 1, c)
        };
        // Past the last knot only with a NaN time or key time; the
        // reference reads beyond its array there (divergence D245).
        let b = knots.get(right).unwrap_or(&knots[last]);
        hermite_1d(&knots[left], b, t)
    }
}

/// `MergeEvaluationLists`: a source key moves before a destination key
/// only when its time is strictly less.
fn merge(destination: &mut Vec<Key>, source: &mut Vec<Key>) {
    let mut merged = Vec::with_capacity(destination.len() + source.len());
    let mut from = source.drain(..).peekable();
    for into in destination.drain(..) {
        while let Some(next) = from.next_if(|s| s.t < into.t) {
            merged.push(next);
        }
        merged.push(into);
    }
    merged.extend(from);
    *destination = merged;
}

/// `SortEvaluationList`: the VC6 `list::sort` bucket merge sort, with its
/// schedule (which decides where NaN times end up) and its saturation at
/// the sixteenth bucket.
fn sort_evaluation_list(values: &mut Vec<Key>) {
    if values.len() < 2 {
        return;
    }
    let mut buckets: [Vec<Key>; 16] = Default::default();
    let mut fill = 0usize;
    let mut pending: std::collections::VecDeque<Key> = values.drain(..).collect();
    while let Some(first) = pending.pop_front() {
        let mut carry = vec![first];
        let mut bucket = 0;
        while bucket < fill && !buckets[bucket].is_empty() {
            merge(&mut buckets[bucket], &mut carry);
            std::mem::swap(&mut buckets[bucket], &mut carry);
            bucket += 1;
        }
        if bucket == 15 {
            merge(&mut buckets[15], &mut carry);
        } else {
            std::mem::swap(&mut buckets[bucket], &mut carry);
            if bucket == fill {
                fill += 1;
            }
        }
    }
    while fill != 0 {
        fill -= 1;
        let mut bucket = std::mem::take(&mut buckets[fill]);
        merge(values, &mut bucket);
    }
}

/// `ApplyHermiteTangents`: for each pair of neighbours, the time step
/// (at least [`MIN_DELTA_T`]) and the value step, then the mode's
/// tangents. Mode 7 (free form) keeps the keys' own.
fn apply_tangents(knots: &mut [Knot], mode: u8) {
    if knots.len() < 2 {
        return;
    }
    for index in 0..knots.len() - 1 {
        let (cur, next) = (knots[index], knots[index + 1]);
        let mut delta_t = (f64::from(next.t) - f64::from(cur.t)) as f32;
        let delta_v = (f64::from(next.value) - f64::from(cur.value)) as f32;
        if delta_t < MIN_DELTA_T {
            delta_t = MIN_DELTA_T;
        }
        let half_slope = || (f64::from(delta_v) / (f64::from(delta_t) * 0.5)) as f32;
        let (out_tangent, in_tangent) = match mode {
            0 => (f32::from_bits(0xffff_ffff), next.m_in),
            1 => {
                let slope = (f64::from(delta_v) / f64::from(delta_t)) as f32;
                (slope, slope)
            }
            4 => (0.0, 0.0),
            // Outgoing unless the value falls (ordered).
            5 if cur.value > next.value => (0.0, half_slope()),
            5 => (half_slope(), 0.0),
            2 => (0.0, half_slope()),
            3 => (half_slope(), 0.0),
            // Incoming only while the value rises (ordered).
            6 if cur.value < next.value => (0.0, half_slope()),
            6 => (half_slope(), 0.0),
            _ => continue,
        };
        knots[index].m_out = out_tangent;
        knots[index + 1].m_in = in_tangent;
    }
}

/// `Hermite1D`: an end value at an exact end time, the left value where
/// the left knot's outgoing tangent holds the step marker (all ones),
/// otherwise the cubic Hermite polynomial in the segment's parameter
/// (rounded to `float`), with the tangents scaled by the segment's length.
fn hermite_1d(a: &Knot, b: &Knot, t: f32) -> f64 {
    if a.t == t {
        return f64::from(a.value);
    }
    if b.t == t {
        return f64::from(b.value);
    }
    let d = f64::from(b.t) - f64::from(a.t);
    let s = f64::from(((f64::from(t) - f64::from(a.t)) / d) as f32);
    if a.m_out.to_bits() == 0xffff_ffff {
        return f64::from(a.value);
    }
    let s2 = s * s;
    let s3 = s * s2;
    let h1 = ((s3 - 2.0 * s2) + s) * f64::from(a.m_out);
    let h2 = (s3 - s2) * f64::from(b.m_in);
    let ha = ((2.0 * s3 - 3.0 * s2) + 1.0) * f64::from(a.value);
    let hb = (3.0 * s2 - 2.0 * s3) * f64::from(b.value);
    ((h1 * d + h2 * d) + ha) + hb
}

/// `CatmullRom1D`: the uniform Catmull-Rom polynomial, all on the x87
/// stack.
fn catmull_rom_1d(p0: f32, p1: f32, p2: f32, p3: f32, u: f32) -> f64 {
    let (p0, p1, p2, p3, u) = (
        f64::from(p0),
        f64::from(p1),
        f64::from(p2),
        f64::from(p3),
        f64::from(u),
    );
    let u2 = u * u;
    let u3 = u * u2;
    let quadratic = ((p0 + p0 - p1 * 5.0) + p2 * 4.0) - p3;
    let cubic = ((p1 * 3.0 - p0) - p2 * 3.0) + p3;
    ((((u2 * quadratic) + cubic * u3) + (p2 - p0) * u) + (p1 + p1)) * 0.5
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_registered_name_makes_a_curve() {
        for name in CURVE_CLASSES {
            let curve = Curve::new(name).unwrap();
            assert_eq!(curve.class_name(), name);
            assert_eq!(curve.key_count(), 2);
        }
        assert!(Curve::new("linearpcurve").is_none());
        assert_eq!(
            Curve::new("LinearPCurve").unwrap().spec.eval,
            Eval::Hermite(1)
        );
    }

    #[test]
    fn default_curves_rise_from_0_to_1() {
        let linear = Curve::new("LinearPCurve").unwrap();
        assert_eq!(linear.evaluate(0.25), 0.25);
        let lerp = Curve::new("SceneScriptLPCurve").unwrap();
        assert_eq!(lerp.evaluate(0.5), 0.5);
        // The plain linear evaluator gives 0 from the last key on.
        assert_eq!(lerp.evaluate(1.0), 0.0);
        let step = Curve::new("StepPCurve").unwrap();
        assert_eq!(step.evaluate(0.9), 0.0);
        assert_eq!(step.evaluate(1.0), 1.0);
        let smooth = Curve::new("SmoothPCurve").unwrap();
        assert_eq!(smooth.evaluate(0.5), 0.5);
    }

    #[test]
    fn keys_add_replace_and_delete() {
        let mut curve = Curve::new("FreeFormPCurve").unwrap();
        assert_eq!(curve.add_key(0.5, 2.0, 0.0, 0.0), 2);
        assert_eq!(curve.add_key(-0.5, 2.0, 0.0, 0.0), -1);
        assert_eq!(curve.add_key(1.0, 3.0, 0.0, 0.0), 1);
        assert_eq!(curve.read_key(1), Some([1.0, 3.0, 0.0, 0.0]));
        assert!(!curve.replace(2, [1.0, 0.0, 0.0, 0.0]));
        assert!(curve.replace(2, [0.25, 4.0, 0.0, 0.0]));
        assert_eq!(curve.evaluate(0.25), 4.0);
        assert_eq!(curve.id_by_index(1), 2);
        assert!(curve.delete_key(2));
        assert!(!curve.delete_key(0));
        assert_eq!(curve.key_count(), 2);
        assert_eq!(curve.wrap_evaluate(5, 0), None);
    }
}
