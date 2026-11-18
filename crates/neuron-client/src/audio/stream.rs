//! The timing arithmetic of the streamer
//! (`CppSource/SoundStreamer/SoundStreamer.cpp`): buffer sizes, the
//! prefill before a stream starts, the 40 ms refill schedule that tops up
//! the emptiest stream, and the gapless trim of decoded MP3 data.
//!
//! The reference streams into a looping ring buffer and measures the
//! data ahead of the play cursor. The port's device queues buffers
//! instead; "buffered" here is the queued bytes not yet played, which is
//! what the ring distance measured. The ring itself (lock regions, the
//! drain markers and the silence written behind the end) is not ported
//! (decision T4, divergence D227).

use neuron_core::num::trunc_to_i32_via_i64;

/// The worker's sleep between refill passes, in milliseconds.
pub const REFILL_PERIOD_MS: u32 = 40;

/// A stream with less than this much queued is filled up to it, seconds
/// (`3.0f * 0.04f`, stored as a float).
#[must_use]
pub fn refill_threshold() -> f32 {
    (3.0 * f64::from(0.04_f32)) as f32
}

/// The longest queue the refill pass considers for the emptiest stream,
/// seconds.
pub const EMPTIEST_LIMIT: f32 = 60.0;

/// The low 32 bits of `(__int64)value` (MSVCRT `_ftol`).
fn ftol(value: f64) -> u32 {
    trunc_to_i32_via_i64(value).cast_unsigned()
}

/// `PcmFrameBytes`: `bits * channels / 8`, the product as a signed 32-bit
/// value divided toward zero.
#[must_use]
pub fn pcm_frame_bytes(bits: u16, channels: u16) -> u32 {
    let product = (u32::from(bits) * u32::from(channels)).cast_signed();
    ((product + ((product >> 31) & 7)) >> 3).cast_unsigned()
}

/// `AlignUp`: up to a multiple of `align`; 0 for an alignment of 0. The
/// sum wraps.
#[must_use]
pub fn align_up(value: u32, align: u32) -> u32 {
    if align == 0 {
        return 0;
    }
    (value.wrapping_add(align - 1) / align).wrapping_mul(align)
}

/// `AlignDown`: down to a multiple of `align`. The reference divides by
/// the alignment unchecked; the port gives 0 for an alignment of 0
/// (divergence D225).
#[must_use]
pub fn align_down(value: u32, align: u32) -> u32 {
    value
        .checked_div(align)
        .map_or(0, |q| q.wrapping_mul(align))
}

/// The buffer of a PCM stream: `avg bytes per second * seconds`, the low
/// word of the 64-bit truncation (`LoadWav`, `OpenAcmStream`).
#[must_use]
pub fn buffer_bytes(avg_bytes_per_sec: u32, seconds: f32) -> u32 {
    ftol(f64::from(avg_bytes_per_sec) * f64::from(seconds))
}

/// A stream's byte rate as a float (`FILD qword` then `FSTP float`).
fn rate_float(bytes_per_sec: u32) -> f32 {
    bytes_per_sec as f32
}

/// The queued time of a stream, seconds: `buffered / float(rate)` on the
/// stack. A zero rate gives infinity or NaN, as in the reference.
#[must_use]
pub fn queued_seconds(buffered: u32, bytes_per_sec: u32) -> f64 {
    f64::from(buffered) / f64::from(rate_float(bytes_per_sec))
}

/// The bytes that fill a stream to `seconds`: `rate * seconds` truncated,
/// rounded up to whole blocks; 0 for a block of 0.
#[must_use]
pub fn fill_bytes(seconds: f32, bytes_per_sec: u32, block: u16) -> u32 {
    if block == 0 {
        return 0;
    }
    let raw = ftol(f64::from(rate_float(bytes_per_sec)) * f64::from(seconds));
    align_up(raw, u32::from(block))
}

/// One active stream as the refill pass sees it, after it was pumped.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamLevel {
    /// Queued bytes not yet played.
    pub buffered: u32,
    /// The decoded byte rate.
    pub bytes_per_sec: u32,
    /// The decoded block align.
    pub block: u16,
}

/// One pass of `PumpStreams`. `streams` is the active list in order, with
/// `None` for an entry that is not starting or playing (it is skipped but
/// counts toward the list length). Every stream below
/// [`refill_threshold`] is filled up to it, in order; of the others the one
/// with the least queued (under [`EMPTIEST_LIMIT`]) is then filled by
/// `count * 4 * 0.04f * rate` bytes, rounded up to blocks. Returns the
/// fills as (index, bytes) in the order the reference makes them.
#[must_use]
pub fn plan_refill(streams: &[Option<StreamLevel>]) -> Vec<(usize, u32)> {
    let threshold = refill_threshold();
    let mut best = EMPTIEST_LIMIT;
    let mut emptiest = None;
    let mut fills = Vec::new();
    for (index, level) in streams.iter().enumerate() {
        let Some(level) = level else { continue };
        let seconds = queued_seconds(level.buffered, level.bytes_per_sec);
        if seconds < f64::from(threshold) {
            fills.push((
                index,
                fill_bytes(threshold, level.bytes_per_sec, level.block),
            ));
        } else if seconds < f64::from(best) {
            best = seconds as f32;
            emptiest = Some(index);
        }
    }
    if let Some(index) = emptiest {
        let level = streams[index].unwrap_or(StreamLevel {
            buffered: 0,
            bytes_per_sec: 0,
            block: 0,
        });
        let mut bytes = 0;
        if level.block != 0 {
            let count = f64::from(i32::try_from(streams.len()).unwrap_or(i32::MAX));
            let raw =
                count * 4.0 * f64::from(0.04_f32) * f64::from(level.bytes_per_sec.cast_signed());
            bytes = align_up(ftol(raw), u32::from(level.block));
        }
        fills.push((index, bytes));
    }
    fills
}

/// `PrefillAndPlay`, first half: the fill before a stream starts, when its
/// queue is shorter than its buffer length (`seconds`, at most 1); `None`
/// when it can start at once.
#[must_use]
pub fn prefill_bytes(buffered: u32, bytes_per_sec: u32, seconds: f32, block: u16) -> Option<u32> {
    let seconds = if seconds > 1.0 { 1.0 } else { seconds };
    if queued_seconds(buffered, bytes_per_sec) < f64::from(seconds) {
        Some(fill_bytes(seconds, bytes_per_sec, block))
    } else {
        None
    }
}

/// `PrefillAndPlay`, second half: after the prefill, whether the stream
/// starts now. It waits while its buffer length (at most 0.25 s) is
/// greater than the queued time, here divided by the rate as a signed
/// integer; equal or unordered starts.
#[must_use]
pub fn starts_after_prefill(buffered: u32, bytes_per_sec: u32, seconds: f32) -> bool {
    let wait = if seconds > 0.25 { 0.25 } else { seconds };
    let queued = f64::from(buffered) / f64::from(bytes_per_sec.cast_signed());
    wait_is_not_longer(f64::from(wait), queued)
}

/// Not `wait > queued`, so equal and unordered values start: the
/// reference's `FCOMPP` with `TEST AH,0x41` and `JZ`.
#[allow(clippy::neg_cmp_op_on_partial_ord)]
fn wait_is_not_longer(wait: f64, queued: f64) -> bool {
    !(wait > queued)
}

/// The gapless trim of decoded MP3 data (`TrimEncoderDelay`): the encoder
/// delay from the `trim` chunk is dropped from the first bytes, and the
/// output stops at the `fact` chunk's sample count.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct GaplessTrim {
    /// Bytes still to drop; -1 when not armed.
    pub encoder_delay: i32,
    /// Bytes delivered since the data start.
    pub emitted: u32,
    /// The `fact` sample count, if the file has one.
    pub fact_samples: Option<u32>,
    /// Bytes per decoded frame.
    pub frame_bytes: u32,
}

/// What [`GaplessTrim::trim`] did to a block.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Trimmed {
    /// Bytes dropped from the front.
    pub skip: u32,
    /// Bytes kept after them.
    pub count: u32,
    /// The `fact` length was reached: the decoder starts over (or the
    /// stream ends) and the delay is disarmed.
    pub end: bool,
}

impl GaplessTrim {
    /// A stream at its data start, the delay not armed.
    #[must_use]
    pub fn new(fact_samples: Option<u32>, frame_bytes: u32) -> GaplessTrim {
        GaplessTrim {
            encoder_delay: -1,
            emitted: 0,
            fact_samples,
            frame_bytes,
        }
    }

    /// Arms the delay as the first decode at the data start does:
    /// `frame bytes * trim samples`, when not armed.
    pub fn arm(&mut self, trim_samples: u32) {
        if self.encoder_delay == -1 {
            self.encoder_delay = self.frame_bytes.wrapping_mul(trim_samples).cast_signed();
        }
    }

    /// `TrimEncoderDelay` on a decoded block of `count` bytes.
    pub fn trim(&mut self, count: u32) -> Trimmed {
        let mut count = count;
        let mut skip = 0;
        let delay = self.encoder_delay;
        if delay > 0 {
            if delay < count.cast_signed() {
                skip = delay.cast_unsigned();
                self.encoder_delay = 0;
                count -= skip;
            } else {
                self.encoder_delay = delay.wrapping_sub(count.cast_signed());
                count = 0;
            }
        }
        let mut end = false;
        if let Some(samples) = self.fact_samples {
            let limit = self.frame_bytes.wrapping_mul(samples);
            if self.emitted.wrapping_add(count).cast_signed() >= limit.cast_signed() {
                count = limit.wrapping_sub(self.emitted);
                self.encoder_delay = -1;
                end = true;
            }
        }
        self.emitted = self.emitted.wrapping_add(count);
        Trimmed { skip, count, end }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn sizes() {
        assert_eq!(pcm_frame_bytes(16, 2), 4);
        assert_eq!(pcm_frame_bytes(12, 1), 1);
        assert_eq!(align_up(10, 4), 12);
        assert_eq!(align_up(12, 4), 12);
        assert_eq!(align_up(5, 0), 0);
        assert_eq!(align_down(10, 4), 8);
        assert_eq!(align_down(10, 0), 0);
        assert_eq!(buffer_bytes(176_400, 1.0), 176_400);
        assert_eq!(buffer_bytes(88_200, 0.5), 44_100);
        assert_eq!(refill_threshold(), 0.12_f32);
    }

    #[test]
    fn refill_fills_the_starving_then_the_emptiest() {
        let level = |buffered| {
            Some(StreamLevel {
                buffered,
                bytes_per_sec: 176_400,
                block: 4,
            })
        };
        // 0.05 s queued (starving), 0.5 s, none, 0.3 s.
        let plan = plan_refill(&[level(8_820), level(88_200), None, level(52_920)]);
        // 0.12f * 176400 = 21167.99... truncated to 21167, up to 21168.
        assert_eq!(plan[0], (0, 21_168));
        // The emptiest of the rest: four entries, 4 * 4 * 0.04f * rate.
        let raw = (4.0 * 4.0 * f64::from(0.04_f32) * 176_400.0) as u32;
        assert_eq!(plan[1], (3, align_up(raw, 4)));
        assert_eq!(plan.len(), 2);
        // Over a minute queued is never the emptiest.
        assert!(plan_refill(&[level(176_400 * 61)]).is_empty());
        // A zero block fills nothing.
        let zero = Some(StreamLevel {
            buffered: 0,
            bytes_per_sec: 1000,
            block: 0,
        });
        assert_eq!(plan_refill(&[zero]), [(0, 0)]);
    }

    #[test]
    fn prefill() {
        assert_eq!(prefill_bytes(0, 176_400, 1.0, 4), Some(176_400));
        assert_eq!(prefill_bytes(0, 176_400, 2.0, 4), Some(176_400));
        assert_eq!(prefill_bytes(176_400, 176_400, 1.0, 4), None);
        assert!(starts_after_prefill(44_100, 176_400, 1.0));
        assert!(!starts_after_prefill(44_099, 176_400, 1.0));
        // A zero rate: the quotient is infinite or NaN, and it starts.
        assert!(starts_after_prefill(0, 0, 1.0));
    }

    #[test]
    fn gapless_trim() {
        let mut t = GaplessTrim::new(Some(1000), 4);
        t.arm(529);
        assert_eq!(t.encoder_delay, 2116);
        assert_eq!(
            t.trim(1000),
            Trimmed {
                skip: 0,
                count: 0,
                end: false
            }
        );
        assert_eq!(t.encoder_delay, 1116);
        assert_eq!(
            t.trim(2000),
            Trimmed {
                skip: 1116,
                count: 884,
                end: false
            }
        );
        assert_eq!(
            t.trim(4000),
            Trimmed {
                skip: 0,
                count: 3116,
                end: true
            }
        );
        assert_eq!((t.emitted, t.encoder_delay), (4000, -1));
        // Arming again only happens once disarmed.
        t.arm(1);
        assert_eq!(t.encoder_delay, 4);
    }
}
