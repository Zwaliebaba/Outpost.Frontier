//! Channel sampling: the `IChannelArchetype` slots 6 to 10 of
//! `CppSource/SceneRuntime/SceneRuntime.cpp` (`ChannelArchetypeSample`,
//! `SamplePair`, `GetFrame`, `CollectRange`, `CollectAt`) and the helpers
//! they inline (`ChannelFrameIndexAt`, `ChannelFrameTime`,
//! `ChannelFramePayload`, `ChannelRelocatedKey`).
//!
//! A frame is named by its index; [`frame_words`] reads its payload. Times
//! are compared in double precision, as the x87 compares them. Frame
//! times follow `ChannelFrameTime`: a stamp when the interval is not `>= 0`
//! (negative or NaN), else `index * interval`.
//!
//! Where the reference reads outside the frame buffer (a stamped channel
//! sampled before its first stamp reads the stamp before the buffer, a
//! channel without frames reads past it), the port answers the frame it
//! found or zero words (divergence register).

use crate::anim::Channel;

/// The result `Sample` and `SamplePair` return for a time outside
/// `0..=duration` (`0xFFFFFFFE`).
pub const SAMPLE_OUT_OF_RANGE: u32 = 0xFFFF_FFFE;

/// `1e-7f` and `-1e-7f`, the slack `CollectRange` allows at both ends.
const TIME_EPS: f32 = 1e-7;

/// The two frames a time falls between and the fraction between them.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Sample {
    /// The frame at or before the time.
    pub a: u32,
    /// The frame after it, or `a` when the time is on a frame.
    pub b: u32,
    /// The fraction from `a` to `b`, rounded to `float`.
    pub alpha: f32,
}

/// [`sample_pair`]: the frames, their times and the fraction.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SamplePair {
    /// The frames and the fraction.
    pub sample: Sample,
    /// The time of `a`, rounded to `float`.
    pub time_lo: f32,
    /// The time of `b`, rounded to `float`.
    pub time_hi: f32,
}

/// One key an event channel answers: the frame's time and the offset of
/// its event data in [`Channel::frames`] (the reference stores the
/// address there).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EventKey {
    /// The time.
    pub time: f32,
    /// `ChannelRelocatedKey` as an offset.
    pub offset: u32,
}

/// What `CollectRange` and `CollectAt` return: the keys they wrote (at most
/// the capacity), the count (all the keys they found) and the result.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Collected {
    /// The keys written.
    pub keys: Vec<EventKey>,
    /// Every key found; larger than `keys.len()` when the capacity ran out.
    pub count: u32,
    /// 0, or the failure code (-1 for a channel that is not an event
    /// channel or a full buffer in `CollectAt`, -2 for a time out of
    /// range).
    pub result: u32,
}

fn stamped(channel: &Channel) -> bool {
    super::not_ge(f64::from(channel.interval()), 0.0)
}

fn word_at(channel: &Channel, at: u64) -> Option<f32> {
    let at = usize::try_from(at).ok()?;
    let bytes = channel.frames().get(at..at.checked_add(4)?)?;
    Some(f32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
}

/// `ChannelFrameTime`: the frame's stamp, or `index * interval`. `None`
/// for a stamp outside the frames.
#[must_use]
pub fn frame_time(channel: &Channel, index: u32) -> Option<f64> {
    if stamped(channel) {
        return word_at(channel, u64::from(channel.stride()) * u64::from(index)).map(f64::from);
    }
    Some(f64::from(index) * f64::from(channel.interval()))
}

/// The frame time, with a stamp outside the frames read as 0.
fn time_or_zero(channel: &Channel, index: u32) -> f64 {
    frame_time(channel, index).unwrap_or(0.0)
}

/// The first `N` payload words of frame `index` (`ChannelFramePayload`):
/// after the stamp when the interval is negative. Words past the frame
/// buffer read as 0.
#[must_use]
pub fn frame_words<const N: usize>(channel: &Channel, index: u32) -> [f32; N] {
    let skip = if channel.interval() < 0.0 { 4 } else { 0 };
    let base = u64::from(channel.stride()) * u64::from(index) + skip;
    core::array::from_fn(|i| word_at(channel, base + 4 * i as u64).unwrap_or(0.0))
}

/// `ChannelFrameIndexAt`: for a positive interval the frame after the time
/// (at most the last one), else a binary search over the frame times for
/// the first frame after the time.
#[must_use]
pub fn frame_index_at(channel: &Channel, time: f32) -> u32 {
    let interval = channel.interval();
    if f64::from(interval) > 0.0 {
        let last = channel.frame_count().wrapping_sub(1);
        let truncated = crate::num::trunc_to_i64(f64::from(time) / f64::from(interval));
        let [a, b, c, d, ..] = truncated.to_le_bytes();
        let index = u32::from_le_bytes([a, b, c, d]).wrapping_add(1);
        return last.min(index);
    }
    let mut lo = 0u32;
    let mut hi = channel.frame_count().wrapping_sub(1);
    let mut mid = hi >> 1;
    if hi == 0 {
        return mid;
    }
    // A missing stamp ends the search where it is: the reference reads
    // outside the buffer there. At most 32 halvings for a sorted range.
    loop {
        let Some(at) = frame_time(channel, mid) else {
            return mid;
        };
        if at <= f64::from(time) {
            lo = mid.wrapping_add(1);
        } else {
            hi = mid;
        }
        mid = hi.wrapping_add(lo) >> 1;
        if lo >= hi {
            return mid;
        }
    }
}

/// The frames `Sample` and `SamplePair` find: `(lo, hi, time of lo, time
/// of hi)`, with `lo == hi` when the time is on a frame (or before the
/// first stamp, where the reference reads the stamp before the buffer).
fn locate(channel: &Channel, time: f32) -> Result<(u32, u32, f64, f64), u32> {
    let duration = channel.duration();
    if super::not_ge(f64::from(time), 0.0) || super::not_le(f64::from(time), f64::from(duration)) {
        return Err(SAMPLE_OUT_OF_RANGE);
    }
    let hi = frame_index_at(channel, time);
    let time_hi = time_or_zero(channel, hi);
    let lo = hi.wrapping_sub(1);
    match (time_hi > f64::from(time))
        .then(|| frame_time(channel, lo))
        .flatten()
    {
        Some(time_lo) => Ok((lo, hi, time_lo, time_hi)),
        None => Ok((hi, hi, time_hi, time_hi)),
    }
}

/// `Sample(time)`: the frames around `time` and the fraction, or
/// [`SAMPLE_OUT_OF_RANGE`] when `time` is not in `0..=duration`.
///
/// # Errors
///
/// [`SAMPLE_OUT_OF_RANGE`].
pub fn sample(channel: &Channel, time: f32) -> Result<Sample, u32> {
    let (a, b, time_lo, time_hi) = locate(channel, time)?;
    let alpha = if a == b {
        0.0
    } else {
        ((f64::from(time) - time_lo) / (time_hi - time_lo)) as f32
    };
    Ok(Sample { a, b, alpha })
}

/// `SamplePair(time)`: as [`sample`], with the two frame times. Its
/// fraction divides by the low time rounded to `float`.
///
/// # Errors
///
/// [`SAMPLE_OUT_OF_RANGE`].
pub fn sample_pair(channel: &Channel, time: f32) -> Result<SamplePair, u32> {
    let (a, b, time_lo, time_hi) = locate(channel, time)?;
    let lo = time_lo as f32;
    let alpha = if a == b {
        0.0
    } else {
        ((f64::from(time) - f64::from(lo)) / (time_hi - f64::from(lo))) as f32
    };
    Ok(SamplePair {
        sample: Sample { a, b, alpha },
        time_lo: lo,
        time_hi: time_hi as f32,
    })
}

/// `GetFrame(last)`: frame 0, or the last frame; `None` (the reference's
/// -1) for the last frame of a channel whose count minus one is negative.
#[must_use]
pub fn frame(channel: &Channel, last: bool) -> Option<u32> {
    if !last {
        return Some(0);
    }
    let index = channel.frame_count().wrapping_sub(1);
    (i32::from_ne_bytes(index.to_ne_bytes()) >= 0).then_some(index)
}

/// `ChannelRelocatedKey`: the end of the frames plus the frame's dword.
fn relocated_key(channel: &Channel, index: u32) -> u32 {
    let reloc = frame_words::<1>(channel, index)[0].to_bits();
    channel
        .stride()
        .wrapping_mul(channel.frame_count())
        .wrapping_add(reloc)
}

/// `CollectRange(t0, t1, capacity)`: the keys of an event channel strictly
/// after `t0` up to before `t1`, walking backwards when `t1 < t0`.
///
/// The reference starts its two bounds from the bit patterns of `t0` and
/// `duration + 1e-7` and only replaces them when a frame qualifies; a
/// bound left at a bit pattern makes it walk (and read) far outside the
/// frames. The port collects nothing there (divergence register).
#[must_use]
pub fn collect_range(channel: &Channel, t0: f32, t1: f32, capacity: u32) -> Collected {
    let mut out = Collected::default();
    if channel.flags() != crate::anim::FLAG_SCALAR_ONLY {
        out.result = 0xFFFF_FFFF;
        return out;
    }
    let dur_eps = (f64::from(channel.duration()) + f64::from(TIME_EPS)) as f32;
    let inside = |t: f32| t >= -TIME_EPS && t <= dur_eps;
    if !inside(t0) || !inside(t1) || t0 == t1 {
        out.result = SAMPLE_OUT_OF_RANGE;
        return out;
    }
    let count = i64::from(channel.frame_count());
    let mut start: Option<i64> = None;
    let mut stop: Option<i64> = None;
    let step: i64;
    if t0 < t1 {
        step = 1;
        for index in 0..count {
            let at = time_or_zero(channel, index as u32);
            if at <= f64::from(t0) {
                start = Some(index);
            }
            if super::not_lt(at, f64::from(t1)) {
                break;
            }
            stop = Some(index);
        }
        start = start.map(|s| s + 1);
        stop = stop.map(|s| s + 1);
    } else {
        step = -1;
        for index in (0..count).rev() {
            let at = time_or_zero(channel, index as u32);
            if at >= f64::from(t0) {
                start = Some(index);
            }
            if super::not_gt(at, f64::from(t1)) {
                break;
            }
            stop = Some(index);
        }
        start = start.map(|s| s - 1);
        stop = stop.map(|s| s - 1);
    }
    let (Some(mut start), Some(stop)) = (start, stop) else {
        return out;
    };
    // Both bounds are frames here, so the walk ends within the frames
    // unless it starts past its stop.
    if (stop - start) * step < 0 {
        return out;
    }
    while start != stop {
        if out.count < capacity {
            out.keys.push(EventKey {
                time: time_or_zero(channel, start as u32) as f32,
                offset: relocated_key(channel, start as u32),
            });
        }
        start += step;
        out.count = out.count.wrapping_add(1);
    }
    out
}

/// `CollectAt(time, capacity)`: the keys of an event channel whose time is
/// exactly `time`; result -1 when more were found than fit.
#[must_use]
pub fn collect_at(channel: &Channel, time: f32, capacity: u32) -> Collected {
    let mut out = Collected::default();
    if channel.flags() != crate::anim::FLAG_SCALAR_ONLY {
        out.result = 0xFFFF_FFFF;
        return out;
    }
    for index in 0..channel.frame_count() {
        if time_or_zero(channel, index) == f64::from(time) {
            if out.count < capacity {
                out.keys.push(EventKey {
                    time,
                    offset: relocated_key(channel, index),
                });
            } else {
                out.result = 0xFFFF_FFFF;
            }
            out.count = out.count.wrapping_add(1);
        }
    }
    out
}
