//! The keyed values at run time: evaluation of the ramps
//! (`CppSource/particlefx/particlefx_ramp.cpp`).
//!
//! `neuron_core::fx_data` holds the ramps as loaded; the types here are
//! built from them and add what evaluation needs: the interpolator a mode
//! selects and the evaluation cache of the flat ramps. A flat ramp
//! (`FxRampSingle`, `FxRampColor`) remembers the segment its last
//! evaluation used and the reciprocal of that segment's span; a later
//! evaluation keeps the segment while the time lies inside it, which
//! decides the result when keys share a time. Property sets are shared by
//! a node prototype and all its clones, so the cache is too; it lives in
//! [`Cell`]s.
//!
//! Arithmetic follows decision T11: what the reference keeps on the x87
//! stack is `f64` here, every store to a `float` is a rounding to `f32`.
//! The flat ramps use the interpolator table the reference picks on a CPU
//! with SSE (every CPU the port runs on): entries 7 to 13, whose mode 4
//! differs from the x87 table (D372).

use std::cell::Cell;

use neuron_core::fx_data::ramp::{self as fx, pack_rgb, unpack_rgb};
use neuron_core::num::{round_even_to_i32, trunc_to_i32_via_i64};

/// `RampFtol`: truncation toward zero to 64 bits, the low 32 bits kept.
fn ftol(value: f64) -> i32 {
    trunc_to_i32_via_i64(value)
}

/// Rounds an x87 value to a `float` store.
#[allow(clippy::cast_possible_truncation)]
pub(crate) fn round(value: f64) -> f32 {
    value as f32
}

const ONE: f64 = 1.0;
const THREE: f64 = 3.0;
const C255: f64 = 255.0;

// --- FxRampSingle interpolators (the SSE table) ----------------------------------------

fn single_ease_in(a: f32, b: f32, t: f32) -> f64 {
    (f64::from(b) - f64::from(a)) * (f64::from(t) * f64::from(t)) + f64::from(a)
}

fn single_ease_out(a: f32, b: f32, t: f32) -> f64 {
    let one_minus = ONE - f64::from(t);
    let w = ONE - one_minus * one_minus;
    (f64::from(b) - f64::from(a)) * w + f64::from(a)
}

/// `G_RampSingleInterpolators[7 + mode]`: the step, linear, ease-in,
/// ease-out, SSE smooth step, ease by rise and ease by fall bodies. The
/// reference indexes the table without a bound; a mode above 6 reads past
/// it, and here gives the step (divergence register, D370).
fn single_interpolate(mode: u32, a: f32, b: f32, t: f32) -> f64 {
    match mode {
        1 => (f64::from(b) - f64::from(a)) * f64::from(t) + f64::from(a),
        2 => single_ease_in(a, b, t),
        3 => single_ease_out(a, b, t),
        4 => {
            // The SSE body stores the weight and the result as floats.
            let t = f64::from(t);
            let w = f64::from(round((t * -2.0 + 3.0) * t * t));
            f64::from(round((f64::from(b) - f64::from(a)) * w + f64::from(a)))
        }
        5 if a < b => single_ease_in(a, b, t),
        5 => single_ease_out(a, b, t),
        6 if b < a => single_ease_in(a, b, t),
        6 => single_ease_out(a, b, t),
        _ => f64::from(a),
    }
}

// --- FxRampColor interpolators (the SSE table) -----------------------------------------

/// `RampColorBlend`: per channel `((b - a) * W >> 8) + a` with
/// `W = ftol(weight * 255)`, masked back into `0x00RRGGBB`.
fn color_blend(a: u32, b: u32, weight: f64) -> u32 {
    let w = ftol(weight * C255);
    let channel = |shift: u32| {
        let ca = ((a >> shift) & 0xFF).cast_signed();
        let cb = ((b >> shift) & 0xFF).cast_signed();
        ((cb - ca).wrapping_mul(w) >> 8)
            .wrapping_add(ca)
            .cast_unsigned()
    };
    ((channel(16) << 16) & 0xFF_0000) | ((channel(8) << 8) & 0xFF00) | (channel(0) & 0xFF)
}

fn color_ease_in(a: u32, b: u32, t: f32) -> u32 {
    color_blend(a, b, f64::from(t) * f64::from(t))
}

fn color_ease_out(a: u32, b: u32, t: f32) -> u32 {
    let one_minus = ONE - f64::from(t);
    color_blend(a, b, ONE - one_minus * one_minus)
}

/// The SSE table's mode 4 (`RampColorSmoothMmx`): the weight rounded to
/// nearest by `cvtss2si`, then the MMX word arithmetic.
fn color_smooth_mmx(a: u32, b: u32, t: f32) -> u32 {
    let t = f64::from(t);
    let weight = round((t * -2.0 + 3.0) * t * t * C255);
    // The low word of the integer (PMULLW keeps 16-bit products).
    let weight_b = round_even_to_i32(f64::from(weight)).cast_unsigned() & 0xFFFF;
    let weight_a = 0xFF_u32.wrapping_sub(weight_b) & 0xFFFF;
    let mut result = 0;
    for channel in 0..3 {
        let byte_a = (a >> (channel * 8)) & 0xFF;
        let byte_b = (b >> (channel * 8)) & 0xFF;
        let low = |byte: u32, weight: u32| (byte.wrapping_mul(weight) & 0xFFFF) >> 8;
        let sum = (low(byte_b, weight_b) + low(byte_a, weight_a)).min(0xFF);
        result |= sum << (channel * 8);
    }
    result
}

/// `G_RampColorInterpolators[7 + mode]`; a mode above 6 gives the step, as
/// for the scalar table (D370).
fn color_interpolate(mode: u32, a: u32, b: u32, t: f32) -> u32 {
    match mode {
        1 => color_blend(a, b, f64::from(t)),
        2 => color_ease_in(a, b, t),
        3 => color_ease_out(a, b, t),
        4 => color_smooth_mmx(a, b, t),
        5 if a < b => color_ease_in(a, b, t),
        5 => color_ease_out(a, b, t),
        6 if b < a => color_ease_in(a, b, t),
        6 => color_ease_out(a, b, t),
        _ => a,
    }
}

// --- The flat ramps ----------------------------------------------------------------------

/// The segment search and cache shared by `FxRampSingle::Eval` and
/// `FxRampColor::EvalPacked`. Returns `Err(index)` when the time lies at or
/// before the first key or at or after the last (the key's value is the
/// result), else the segment and the parameter `U`.
#[derive(Clone, Debug, Default, PartialEq)]
struct EvalCache {
    segment: Cell<usize>,
    inv_span: Cell<f32>,
}

impl EvalCache {
    /// `ResetEvalCache`: segment 0 and the reciprocal of the first span
    /// (0 for a single key or an exactly zero span).
    fn reset(times: &[f32]) -> Self {
        let cache = Self::default();
        if let [t0, t1, ..] = times {
            let span = f64::from(*t1) - f64::from(*t0);
            if span != 0.0 {
                cache.inv_span.set(round(ONE / span));
            }
        }
        cache
    }

    fn locate(&self, times: &[f32], time: f32) -> Result<(usize, f32), usize> {
        let Some(&first) = times.first() else {
            // The reference reads key 0 of an empty ramp (D370).
            return Err(usize::MAX);
        };
        if time <= first {
            return Err(0);
        }
        let last = times.len() - 1;
        if time >= times[last] {
            return Err(last);
        }
        if last == 0 {
            // An unordered time on one key: the reference reads past the
            // keys (D370).
            return Err(0);
        }
        let mut index = self.segment.get().min(last - 1);
        let mut recompute = false;
        if time < times[index] {
            index -= 1;
            while time < times[index] {
                index -= 1;
            }
            recompute = true;
        } else if times[index + 1] < time {
            index += 1;
            while times[index + 1] < time {
                index += 1;
            }
            recompute = true;
        }
        self.segment.set(index);
        if recompute {
            let span = f64::from(times[index + 1]) - f64::from(times[index]);
            self.inv_span
                .set(if span == 0.0 { 0.0 } else { round(ONE / span) });
        }
        let u = round((f64::from(time) - f64::from(times[index])) * f64::from(self.inv_span.get()));
        Ok((index, u))
    }
}

/// `FxRampSingle` at run time.
#[derive(Clone, Debug, PartialEq)]
pub struct SingleRamp {
    mode: u32,
    times: Vec<f32>,
    values: Vec<f32>,
    cache: EvalCache,
}

impl SingleRamp {
    /// From the loaded ramp: the cache as the last `AddKey` left it.
    #[must_use]
    pub fn new(ramp: &fx::RampSingle) -> Self {
        let times: Vec<f32> = ramp.keys.iter().map(|k| k.time).collect();
        Self {
            mode: ramp.mode,
            cache: EvalCache::reset(&times),
            values: ramp.keys.iter().map(|k| k.value).collect(),
            times,
        }
    }

    /// `Eval(time)`: the interpolator's unrounded result.
    #[must_use]
    pub fn eval(&self, time: f32) -> f64 {
        match self.cache.locate(&self.times, time) {
            Ok((i, u)) => single_interpolate(self.mode, self.values[i], self.values[i + 1], u),
            Err(i) => self.values.get(i).copied().map_or(0.0, f64::from),
        }
    }
}

/// `FxRampColor` at run time: keys packed as `0x00RRGGBB`.
#[derive(Clone, Debug, PartialEq)]
pub struct ColorRamp {
    mode: u32,
    times: Vec<f32>,
    values: Vec<u32>,
    cache: EvalCache,
}

impl ColorRamp {
    /// From the loaded ramp.
    #[must_use]
    pub fn new(ramp: &fx::RampColor) -> Self {
        let times: Vec<f32> = ramp.keys.iter().map(|k| k.time).collect();
        Self {
            mode: ramp.mode,
            cache: EvalCache::reset(&times),
            values: ramp.keys.iter().map(|k| k.value).collect(),
            times,
        }
    }

    /// `EvalPacked(time)`.
    #[must_use]
    pub fn eval_packed(&self, time: f32) -> u32 {
        match self.cache.locate(&self.times, time) {
            Ok((i, u)) => color_interpolate(self.mode, self.values[i], self.values[i + 1], u),
            Err(i) => self.values.get(i).copied().unwrap_or(0),
        }
    }

    /// `EvalRgb(out, time)`: the packed value unpacked.
    #[must_use]
    pub fn eval_rgb(&self, time: f32) -> [f32; 3] {
        unpack_rgb(self.eval_packed(time))
    }
}

/// Keeps `pack_rgb` reachable for callers building colour keys.
#[must_use]
pub fn pack(rgb: [f32; 3]) -> u32 {
    pack_rgb(rgb)
}

// --- FxAnimatedSingle --------------------------------------------------------------------

/// One key of an animated value: value, time, incoming and outgoing slope.
#[derive(Clone, Copy, Debug, PartialEq)]
struct Key {
    value: f32,
    time: f32,
    in_slope: f32,
    out_slope: f32,
}

/// `FxAnimatedSingle` at run time: the weight without keys, else a Hermite
/// track with loop types.
#[derive(Clone, Debug, PartialEq)]
pub struct Animated {
    weight: f32,
    loop_type: u32,
    keys: Option<Vec<Key>>,
}

impl Animated {
    /// From the loaded value.
    #[must_use]
    pub fn new(value: &fx::AnimatedSingle) -> Self {
        Self {
            weight: value.weight,
            loop_type: value.loop_type(),
            keys: value.track.as_ref().map(|track| {
                track
                    .keys
                    .iter()
                    .map(|k| Key {
                        value: k.value,
                        time: k.time,
                        in_slope: k.in_slope,
                        out_slope: k.out_slope,
                    })
                    .collect()
            }),
        }
    }

    /// A value with only a weight.
    #[must_use]
    pub const fn constant(weight: f32) -> Self {
        Self {
            weight,
            loop_type: 0,
            keys: None,
        }
    }

    /// `GetKeyCount`.
    #[must_use]
    pub fn key_count(&self) -> usize {
        self.keys.as_ref().map_or(0, Vec::len)
    }

    /// `GetKeyTime(index)` and the key's value (`GetKeyValue`'s first word).
    #[must_use]
    pub fn key(&self, index: usize) -> Option<(f32, f32)> {
        self.keys.as_ref()?.get(index).map(|k| (k.time, k.value))
    }

    /// `Eval(time)`: the weight without a track, else the track.
    #[must_use]
    pub fn eval(&self, time: f32) -> f64 {
        match &self.keys {
            // A track with no key: the reference reads key 0 (D370).
            Some(keys) if !keys.is_empty() => eval_track(keys, self.loop_type, time),
            _ => f64::from(self.weight),
        }
    }
}

/// `EvalSegment(a, b, t)`: the cubic Hermite segment; a step when `a`'s
/// outgoing slope is the bit pattern `0xFFFFFFFF`. `b` is `None` where the
/// reference reads past the keys; that is only reached when `a`'s time is
/// `t`.
fn eval_segment(a: &Key, b: Option<&Key>, time: f32) -> f64 {
    if a.time == time {
        return f64::from(a.value);
    }
    let Some(b) = b else {
        return f64::from(a.value);
    };
    if b.time == time {
        return f64::from(b.value);
    }
    let span = f64::from(b.time) - f64::from(a.time);
    let u = f64::from(round((f64::from(time) - f64::from(a.time)) / span));
    if a.out_slope.to_bits() == 0xFFFF_FFFF {
        return f64::from(a.value);
    }
    let u2 = u * u;
    let u3 = u * u2;
    let two_u3 = u3 + u3;
    let three_u2 = u2 * 3.0;
    let tangents = (((u3 - (u2 + u2)) + u) * f64::from(a.out_slope) * span)
        + ((u3 - u2) * f64::from(b.in_slope) * span);
    let with_a = tangents + ((two_u3 - three_u2) + 1.0) * f64::from(a.value);
    with_a + (three_u2 - two_u3) * f64::from(b.value)
}

/// The cycle count and remainder of a time outside the track.
fn cycle(time: f32, t0: f32, span: f32) -> (f32, f32) {
    let rel = f64::from(time) - f64::from(t0);
    let rel_f = round(rel);
    let fm = round(rel % f64::from(span));
    let mut q = f64::from(rel_f) / f64::from(span);
    if q < 0.0 {
        q -= 1.0;
    }
    #[allow(clippy::cast_precision_loss)]
    let n = ftol(q) as f32;
    (fm, n)
}

/// `EvalTrack(time)` on a track with at least one key.
fn eval_track(keys: &[Key], loop_type: u32, mut time: f32) -> f64 {
    let first = keys[0];
    if keys.len() == 1
        && (time == first.time
            || (time < first.time && loop_type & 0x4 == 0)
            || (time > first.time && loop_type & 0x40 == 0))
    {
        return f64::from(first.value);
    }
    let last = keys.len() - 1;
    let last_key = keys[last];
    let (t0, v0, tl, vl) = (first.time, first.value, last_key.time, last_key.value);
    let span = round(f64::from(tl) - f64::from(t0));
    let mut offset = 0.0_f32;
    if time < t0 {
        let mode = loop_type & 0xF;
        if mode == 0 {
            return f64::from(v0);
        }
        let (fm, n) = cycle(time, t0, span);
        match mode {
            1 => time = round(f64::from(fm) + f64::from(tl)),
            2 => {
                time = round(f64::from(fm) + f64::from(tl));
                offset = round((f64::from(vl) - f64::from(v0)) * f64::from(n));
            }
            3 if ftol(f64::from(n)) & 1 == 0 => time = round(f64::from(fm) + f64::from(tl)),
            3 => time = round(f64::from(t0) - f64::from(fm)),
            4 => {
                return f64::from(v0)
                    - (f64::from(t0) - f64::from(time)) * f64::from(first.in_slope);
            }
            _ => {}
        }
    } else if time > tl {
        let mode = loop_type & 0xF0;
        if mode == 0 {
            return f64::from(vl);
        }
        let (fm, n) = cycle(time, t0, span);
        match mode {
            0x10 => time = round(f64::from(fm) + f64::from(t0)),
            0x20 => {
                time = round(f64::from(fm) + f64::from(t0));
                offset = round((f64::from(vl) - f64::from(v0)) * f64::from(n));
            }
            0x30 if ftol(f64::from(n)) & 1 == 0 => time = round(f64::from(fm) + f64::from(t0)),
            0x30 => time = round(f64::from(tl) - f64::from(fm)),
            0x40 => {
                return f64::from(vl)
                    - (f64::from(tl) - f64::from(time)) * f64::from(last_key.out_slope);
            }
            _ => {}
        }
    }
    #[allow(clippy::cast_precision_loss)]
    let guess = ftol((f64::from(time) - f64::from(t0)) / f64::from(span) * last as f64);
    // A guess outside the keys reads outside them in the reference (D370).
    let mut probe = usize::try_from(guess).unwrap_or(0).min(last);
    let (a, b) = if keys[probe].time > time {
        while probe > 0 && !(keys[probe].time < time) {
            probe -= 1;
        }
        (probe, probe + 1)
    } else {
        while probe <= last && !(keys[probe].time > time) {
            probe += 1;
        }
        (probe.saturating_sub(1), probe)
    };
    eval_segment(&keys[a], keys.get(b), time) + f64::from(offset)
}

// --- Nested ramps ------------------------------------------------------------------------

/// A child a nested scalar ramp evaluates.
pub trait ChildEval {
    /// The child's `Eval(inner)`.
    fn eval_child(&self, inner: f32) -> f64;
}

impl ChildEval for SingleRamp {
    fn eval_child(&self, inner: f32) -> f64 {
        self.eval(inner)
    }
}

impl ChildEval for Animated {
    fn eval_child(&self, inner: f32) -> f64 {
        self.eval(inner)
    }
}

/// `FxRampSingleRamp` and `FxAnimatedSingleRamp` at run time: children
/// keyed by an outer time.
#[derive(Clone, Debug, PartialEq)]
pub struct Nested<T> {
    mode: u32,
    keys: Vec<(f32, T)>,
}

/// A ramp of [`SingleRamp`] children (property kind 0x200).
pub type SingleRampRamp = Nested<SingleRamp>;
/// A ramp of [`Animated`] children (property kind 0x202).
pub type AnimatedRamp = Nested<Animated>;

impl<T> Nested<T> {
    fn build<U>(ramp: &fx::NestedRamp<U>, child: impl Fn(&U) -> T) -> Self {
        Self {
            mode: ramp.mode,
            keys: ramp
                .keys
                .iter()
                .map(|k| (k.time, child(&k.value)))
                .collect(),
        }
    }

    /// The child of key `index` (`GetKeyValue`).
    #[must_use]
    pub fn child(&self, index: usize) -> Option<&T> {
        self.keys.get(index).map(|(_, child)| child)
    }

    /// The segment of `outer`: `Err(index)` for a single child the result
    /// comes from, `Ok(i)` for the blend of children `i` and `i + 1`.
    fn segment(&self, outer: f32) -> Result<usize, usize> {
        let first = self.keys.first().map_or(f32::NAN, |k| k.0);
        if outer <= first || self.keys.len() == 1 {
            return Err(0);
        }
        let last = self.keys.len() - 1;
        if outer >= self.keys[last].0 {
            return Err(last);
        }
        let mut index = 0;
        while index < last && !(outer < self.keys[index + 1].0) {
            index += 1;
        }
        if self.mode == 0 || index == last {
            // `index == last` only for an unordered time, where the
            // reference blends with the child past the end (D370).
            return Err(index);
        }
        Ok(index)
    }

    /// `U` of segment `i` at `outer`.
    fn param(&self, index: usize, outer: f32) -> f64 {
        let floor = f64::from(self.keys[index].0);
        (f64::from(outer) - floor) / (f64::from(self.keys[index + 1].0) - floor)
    }
}

impl SingleRampRamp {
    /// From the loaded ramp.
    #[must_use]
    pub fn new(ramp: &fx::RampSingleRamp) -> Self {
        Self::build(ramp, SingleRamp::new)
    }
}

impl AnimatedRamp {
    /// From the loaded ramp.
    #[must_use]
    pub fn new(ramp: &fx::AnimatedSingleRamp) -> Self {
        Self::build(ramp, Animated::new)
    }
}

/// The ease weight of the nested modes 2 to 4 (`None` for 5 and 6, which
/// compare the children, and for the rest, which blend linearly).
fn weight_for(mode: u32, u: f64) -> Option<f64> {
    match mode {
        2 => Some(u * u),
        3 => Some(ONE - (ONE - u) * (ONE - u)),
        4 => Some(u * ((THREE - (u + u)) * u)),
        5 | 6 => None,
        _ => Some(u),
    }
}

fn ease(rising: bool, u: f64) -> f64 {
    if rising {
        u * u
    } else {
        ONE - (ONE - u) * (ONE - u)
    }
}

impl<T: ChildEval> Nested<T> {
    /// `Eval(outer, inner)`: the children evaluated at `inner` and blended
    /// by the position of `outer` between their keys.
    #[must_use]
    pub fn eval(&self, outer: f32, inner: f32) -> f64 {
        let index = match self.segment(outer) {
            Err(index) => {
                return self.child(index).map_or(0.0, |c| c.eval_child(inner));
            }
            Ok(index) => index,
        };
        // The next child is stored to a float; the floor stays unrounded.
        let next = f64::from(round(self.keys[index + 1].1.eval_child(inner)));
        let floor = self.keys[index].1.eval_child(inner);
        let u = self.param(index, outer);
        let w = weight_for(self.mode, u).unwrap_or_else(|| match self.mode {
            5 => ease(next < floor, u),
            _ => ease(floor < next, u),
        });
        floor * (ONE - w) + next * w
    }
}

/// `FxRampColorRamp` at run time.
#[derive(Clone, Debug, PartialEq)]
pub struct ColorRampRamp(Nested<ColorRamp>);

impl ColorRampRamp {
    /// From the loaded ramp.
    #[must_use]
    pub fn new(ramp: &fx::ColorRampRamp) -> Self {
        Self(Nested::build(ramp, ColorRamp::new))
    }

    /// `EvalRgb(out, outer, inner)`.
    #[must_use]
    pub fn eval_rgb(&self, outer: f32, inner: f32) -> [f32; 3] {
        let ramp = &self.0;
        let index = match ramp.segment(outer) {
            Err(index) => {
                return ramp.child(index).map_or([0.0; 3], |c| c.eval_rgb(inner));
            }
            Ok(index) => index,
        };
        let next = ramp.keys[index + 1].1.eval_rgb(inner);
        let floor = ramp.keys[index].1.eval_rgb(inner);
        let u = ramp.param(index, outer);
        let sum = |c: [f32; 3]| (f64::from(c[0]) + f64::from(c[1])) + f64::from(c[2]);
        let w = weight_for(ramp.mode, u).unwrap_or_else(|| match ramp.mode {
            5 => ease(sum(floor) > sum(next), u),
            _ => ease(sum(next) > sum(floor), u),
        });
        let one_minus = ONE - w;
        let floor_part = floor.map(|c| round(f64::from(c) * one_minus));
        let next0 = round(f64::from(next[0]) * w);
        let next1 = round(f64::from(next[1]) * w);
        let next2 = f64::from(next[2]) * w;
        [
            round(f64::from(next0) + f64::from(floor_part[0])),
            round(f64::from(next1) + f64::from(floor_part[1])),
            round(next2 + f64::from(floor_part[2])),
        ]
    }

    /// `EvalPacked(outer, inner)`: between two keys the children are blended
    /// with the weight `ftol(inner * 255)` and no mode, as the reference
    /// does.
    #[must_use]
    pub fn eval_packed(&self, outer: f32, inner: f32) -> u32 {
        let ramp = &self.0;
        let index = match ramp.segment(outer) {
            Err(index) => return ramp.child(index).map_or(0, |c| c.eval_packed(inner)),
            Ok(index) => index,
        };
        let next = ramp.keys[index + 1].1.eval_packed(inner);
        let floor = ramp.keys[index].1.eval_packed(inner);
        // The same blend as the flat interpolators with the roles swapped.
        color_blend(next, floor, f64::from(inner))
    }
}

/// A billboard ramp slot at run time: either nested scalar kind.
#[derive(Clone, Debug, PartialEq)]
pub enum NestedSingle {
    /// A ramp of flat ramps (kind 0x200).
    Ramp(SingleRampRamp),
    /// A ramp of animated values (kind 0x202).
    Animated(AnimatedRamp),
}

impl NestedSingle {
    /// From the loaded slot.
    #[must_use]
    pub fn new(slot: &neuron_core::fx_data::NestedSingle) -> Self {
        match slot {
            neuron_core::fx_data::NestedSingle::Ramp(r) => Self::Ramp(SingleRampRamp::new(r)),
            neuron_core::fx_data::NestedSingle::Animated(r) => Self::Animated(AnimatedRamp::new(r)),
        }
    }

    /// `Eval(outer, inner)` through the slot's interface.
    #[must_use]
    pub fn eval(&self, outer: f32, inner: f32) -> f64 {
        match self {
            Self::Ramp(r) => r.eval(outer, inner),
            Self::Animated(r) => r.eval(outer, inner),
        }
    }
}

// --- FxAnimatedTransform -----------------------------------------------------------------

/// The identity transform, 12 words (rotation rows, translation).
pub const IDENTITY: [f32; 12] = [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0];

/// `0.017453292f`.
const DEG_TO_RAD: f32 = 0.017_453_292;

/// `FxAnimatedTransform` at run time: nine animated channels (translation,
/// rotation in degrees, scale) and the flags of the channels that hold
/// their defaults.
#[derive(Clone, Debug, PartialEq)]
pub struct AnimatedTransform {
    flags: u32,
    channels: Vec<AnimatedRamp>,
}

impl AnimatedTransform {
    /// From the loaded transform.
    #[must_use]
    pub fn new(transform: &fx::AnimatedTransform) -> Self {
        Self {
            flags: transform.channel_flags,
            channels: transform.channels.iter().map(AnimatedRamp::new).collect(),
        }
    }

    /// `IsIdentity`: every channel holds its default.
    #[must_use]
    pub const fn is_identity(&self) -> bool {
        self.flags == fx::ALL_CHANNELS_DEFAULT
    }

    fn channel(&self, index: usize, a: f32, b: f32) -> f32 {
        round(self.channels[index].eval(a, b))
    }

    /// `Evaluate(time_a, time_b, out)`: the rotation from the three angle
    /// channels, then the scale, then the translation; channels at their
    /// defaults are skipped as groups.
    #[must_use]
    pub fn evaluate(&self, time_a: f32, time_b: f32) -> [f32; 12] {
        let mut out = IDENTITY;
        if self.flags == fx::ALL_CHANNELS_DEFAULT {
            return out;
        }
        if self.flags & 0x38 != 0x38 {
            let r4 = self.channel(4, time_a, time_b);
            let r5 = self.channel(5, time_a, time_b);
            let r3 = self.channel(3, time_a, time_b);
            euler_rotation(&mut out, r3, r5, r4);
        }
        if self.flags & 0x1C0 != 0x1C0 {
            let mut scale = IDENTITY;
            scale[0] = self.channel(6, time_a, time_b);
            scale[4] = self.channel(7, time_a, time_b);
            scale[8] = self.channel(8, time_a, time_b);
            out = mul(&scale, &out);
        }
        if self.flags & 0x7 != 0x7 {
            let t2 = self.channel(2, time_a, time_b);
            let t1 = self.channel(1, time_a, time_b);
            out[9] = self.channel(0, time_a, time_b);
            out[10] = t1;
            out[11] = t2;
        }
        out
    }
}

/// `MulTransform(dst, a, b)` on 12-word transforms.
#[must_use]
pub fn mul(a: &[f32; 12], b: &[f32; 12]) -> [f32; 12] {
    use neuron_core::math::{Transform, mul_transform};
    mul_transform(&Transform::from_words(*a), &Transform::from_words(*b)).to_words()
}

/// `BuildEulerRotation3x3(out, a, b, c)`: the rotation about y by `c`, then
/// about x by `a` (rows 1 and 2), then about z by `b` (rows 0 and 1). The
/// sine of `c` is taken of the angle rounded to a float, its cosine of the
/// unrounded one.
fn euler_rotation(out: &mut [f32; 12], a: f32, b: f32, c: f32) {
    let k = f64::from(DEG_TO_RAD);
    let rad_c = f64::from(c) * k;
    out[1] = 0.0;
    out[4] = 1.0;
    out[5] = 0.0;
    out[7] = 0.0;
    let rad_c_f = round(rad_c);
    let cos_c = rad_c.cos();
    out[3] = 0.0;
    let sin_c = f64::from(rad_c_f).sin();
    out[0] = round(cos_c);
    out[2] = round(sin_c);
    out[6] = round(-sin_c);
    out[8] = round(cos_c);

    let rad_a = f64::from(a) * k;
    let cos_a = f64::from(round(rad_a.cos()));
    let sin_a = rad_a.sin();
    for col in 0..3 {
        let row1 = f64::from(out[3 + col]);
        out[3 + col] = round(cos_a * row1 - sin_a * f64::from(out[6 + col]));
        out[6 + col] = round(cos_a * f64::from(out[6 + col]) + row1 * sin_a);
    }
    let rad_b = f64::from(b) * k;
    let cos_b = f64::from(round(rad_b.cos()));
    let sin_b = rad_b.sin();
    for col in 0..3 {
        let row0 = f64::from(out[col]);
        out[col] = round(cos_b * row0 - sin_b * f64::from(out[3 + col]));
        out[3 + col] = round(cos_b * f64::from(out[3 + col]) + row0 * sin_b);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn mmx_weight_matches_the_word_arithmetic() {
        // T = 0: weight 0, so the first colour.
        assert_eq!(color_smooth_mmx(0x00FF_8040, 0x0000_0000, 0.0), 0x00FE_7F3F);
        // T = 1: weight 255 on the second colour.
        assert_eq!(color_smooth_mmx(0, 0x00FF_FFFF, 1.0), 0x00FE_FEFE);
    }

    #[test]
    fn flat_cache_keeps_a_shared_key_time_segment() {
        let mut ramp = fx::RampSingle::new();
        ramp.mode = 1;
        for (t, v) in [(0.0, 0.0), (1.0, 1.0), (1.0, 5.0), (2.0, 6.0)] {
            ramp.add_key(t, v);
        }
        let ramp = SingleRamp::new(&ramp);
        // From segment 0, time 1 stays in it: the end of the first segment.
        assert_eq!(ramp.eval(1.0), 1.0);
        assert_eq!(ramp.eval(1.5), 5.5);
        // Now segment 2 is cached: time 1 is its start.
        assert_eq!(ramp.eval(1.0), 5.0);
    }
}
