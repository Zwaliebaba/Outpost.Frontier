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
//!
//! The second half is the streamer itself over a voice queue: the stream
//! states, start, stop, the pump, the prefill and the refill pass
//! ([`Stream`], [`Streamer`], [`StreamVoice`]).

use neuron_core::num::trunc_to_i32_via_i64;

use super::decode::StreamSource;

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

// --- The streamer over a voice queue ---------------------------------------
//
// `FSoundStreamer` and its handles (`FStreamingSound`) without the ring
// buffer: a stream decodes from its `StreamSource` into blocks it queues
// on a `StreamVoice`; "buffered" is what the voice has queued and not yet
// played. The states, `StartHandle`, `StreamingSoundStop`, `PumpHandle`,
// `PrefillAndPlay`, `FillStreamEntry`, the refill pass of `PumpStreams` and
// `MovePendingActive` keep the reference's order. A stream that reaches the
// end of a source that does not loop drains: it stops and finishes once the
// voice has played everything queued (divergence D227).

/// The state of a stream (`FStreamingSound::State`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamState {
    /// 0: failed to play, or never loaded.
    Failed,
    /// 1: loaded, not started.
    Loaded,
    /// 2: started, prefilling before the voice plays.
    Starting,
    /// 3: playing.
    Playing,
    /// 6: the source ended; the queue plays out.
    Draining,
    /// 8: played to its end.
    Finished,
    /// 9: stopped by the client.
    Stopped,
}

impl StreamState {
    /// The reference's state number.
    #[must_use]
    pub fn code(self) -> u32 {
        match self {
            StreamState::Failed => 0,
            StreamState::Loaded => 1,
            StreamState::Starting => 2,
            StreamState::Playing => 3,
            StreamState::Draining => 6,
            StreamState::Finished => 8,
            StreamState::Stopped => 9,
        }
    }

    /// States 2, 3 and 6: on the active list, volume and pan pushed.
    #[must_use]
    pub fn is_active(self) -> bool {
        matches!(
            self,
            StreamState::Starting | StreamState::Playing | StreamState::Draining
        )
    }

    /// `QueryStatus`: 2 for loaded, starting or stopped, 1 for playing or
    /// draining, 4 for finished, 0 otherwise (the reference's 3 belongs to
    /// a state nothing sets).
    #[must_use]
    pub fn status(self) -> u32 {
        match self {
            StreamState::Loaded | StreamState::Starting | StreamState::Stopped => 2,
            StreamState::Playing | StreamState::Draining => 1,
            StreamState::Finished => 4,
            StreamState::Failed => 0,
        }
    }
}

/// The device side of a stream: a voice that plays queued PCM blocks.
pub trait StreamVoice {
    /// Queue a block of PCM after the queued ones; `last` when no block
    /// follows. False when the device refused it.
    fn submit(&mut self, pcm: Vec<u8>, last: bool) -> bool;
    /// The queued bytes not yet played.
    fn queued_bytes(&mut self) -> u32;
    /// Start or resume playback (`Play` with `DSBPLAY_LOOPING` on the
    /// ring); false when it failed.
    fn play(&mut self) -> bool;
    /// Pause; the queue stays.
    fn stop(&mut self);
    /// `SetVolume`, in hundredths of a decibel.
    fn set_volume(&mut self, volume: i32);
    /// `SetPan`, in hundredths of a decibel.
    fn set_pan(&mut self, pan: i32);
}

/// Why a stream cannot be made.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamError {
    /// The streamer's buffer length is zero or negative (`LoadWav` checks
    /// `Gain`).
    BufferLength,
}

impl core::fmt::Display for StreamError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str("the stream buffer length is not positive")
    }
}

impl std::error::Error for StreamError {}

/// One stream (`FStreamingSound`).
#[derive(Debug)]
pub struct Stream<V> {
    source: StreamSource,
    voice: V,
    state: StreamState,
    want_play: bool,
    volume: i32,
    pan: i32,
    seconds: f32,
    capacity: u32,
}

impl<V: StreamVoice> Stream<V> {
    /// The end of `LoadWav`: the buffer of `seconds` (the streamer's
    /// length), volume and pan 0 pushed, state loaded.
    ///
    /// # Errors
    ///
    /// A buffer length that is zero or negative (NaN passes, as in the
    /// reference); the voice is dropped.
    pub fn new(source: StreamSource, mut voice: V, seconds: f32) -> Result<Stream<V>, StreamError> {
        if seconds <= 0.0 {
            return Err(StreamError::BufferLength);
        }
        let capacity = source.buffer_bytes(seconds);
        voice.set_volume(0);
        voice.set_pan(0);
        Ok(Stream {
            source,
            voice,
            state: StreamState::Loaded,
            want_play: false,
            volume: 0,
            pan: 0,
            seconds,
            capacity,
        })
    }

    /// The state.
    #[must_use]
    pub fn state(&self) -> StreamState {
        self.state
    }

    /// The voice.
    pub fn voice_mut(&mut self) -> &mut V {
        &mut self.voice
    }

    /// The queue capacity in bytes (`BufferBytes`).
    #[must_use]
    pub fn capacity(&self) -> u32 {
        self.capacity
    }

    /// The source.
    #[must_use]
    pub fn source(&self) -> &StreamSource {
        &self.source
    }

    fn rate_and_block(&self) -> (u32, u16) {
        let f = self.source.format();
        (f.bytes_per_sec, f.block_align)
    }

    /// `StartHandle`: wants to play; loaded, finished or stopped streams
    /// start again.
    pub fn start(&mut self) {
        self.want_play = true;
        if matches!(
            self.state,
            StreamState::Loaded | StreamState::Finished | StreamState::Stopped
        ) {
            self.state = StreamState::Starting;
        }
    }

    /// `StreamingSoundStop`.
    pub fn stop(&mut self) {
        self.want_play = false;
        if self.state.is_active() {
            self.voice.stop();
            self.state = StreamState::Stopped;
        }
    }

    /// `SetVolume`.
    pub fn set_volume(&mut self, volume: i32) {
        self.volume = volume;
        self.voice.set_volume(volume);
    }

    /// `SetPan`.
    pub fn set_pan(&mut self, pan: i32) {
        self.pan = pan;
        self.voice.set_pan(pan);
    }

    /// The level the refill pass measures, for a starting or playing
    /// stream.
    pub fn level(&mut self) -> Option<StreamLevel> {
        if !matches!(self.state, StreamState::Starting | StreamState::Playing) {
            return None;
        }
        let (bytes_per_sec, block) = self.rate_and_block();
        Some(StreamLevel {
            buffered: self.voice.queued_bytes(),
            bytes_per_sec,
            block,
        })
    }

    /// `FillStreamEntry`: up to `want` bytes, no more than the queue has
    /// room for, from the source. A source that ended (or failed) turns
    /// the stream to draining.
    pub fn fill(&mut self, want: u32) {
        if !matches!(self.state, StreamState::Starting | StreamState::Playing) {
            return;
        }
        let free = self.capacity.saturating_sub(self.voice.queued_bytes());
        let want = want.min(free);
        if want == 0 {
            return;
        }
        let (pcm, end) = match self.source.read(want) {
            Ok(block) => (block.pcm, block.end),
            Err(_) => (Vec::new(), true),
        };
        if !pcm.is_empty() {
            self.voice.submit(pcm, end);
        }
        if end {
            self.state = StreamState::Draining;
        }
    }

    /// `PrefillAndPlay`.
    fn prefill_and_play(&mut self) {
        self.state = StreamState::Starting;
        self.want_play = true;
        let (rate, block) = self.rate_and_block();
        let buffered = self.voice.queued_bytes();
        if let Some(bytes) = prefill_bytes(buffered, rate, self.seconds, block) {
            self.fill(bytes);
            let buffered = self.voice.queued_bytes();
            if !starts_after_prefill(buffered, rate, self.seconds) {
                return;
            }
        }
        if !self.voice.play() {
            self.state = StreamState::Failed;
            return;
        }
        if self.state == StreamState::Starting {
            self.state = StreamState::Playing;
        }
    }

    /// `PumpHandle`: a starting stream that wants to play prefills and
    /// plays; a finished or stopped one stops wanting to; a draining one
    /// finishes once its queue has played. Active streams get their volume
    /// and pan again; returns whether the stream is active.
    pub fn pump(&mut self) -> bool {
        match self.state {
            StreamState::Starting if self.want_play => self.prefill_and_play(),
            StreamState::Finished | StreamState::Stopped => self.want_play = false,
            StreamState::Draining if self.voice.queued_bytes() == 0 => {
                self.stop();
                self.state = StreamState::Finished;
            }
            _ => {}
        }
        if self.state.is_active() {
            self.voice.set_volume(self.volume);
            self.voice.set_pan(self.pan);
            true
        } else {
            false
        }
    }
}

/// A stream handle of the [`Streamer`].
pub type StreamId = u32;

/// The streamer (`FSoundStreamer`): an active list the refill pass
/// pumps and a pending list, as the reference keeps them.
#[derive(Debug)]
pub struct Streamer<V> {
    seconds: f32,
    active: Vec<(StreamId, Stream<V>)>,
    pending: Vec<(StreamId, Stream<V>)>,
    next_id: StreamId,
}

impl<V: StreamVoice> Streamer<V> {
    /// A streamer whose streams buffer `seconds`
    /// ([`super::config::stream_buffer_seconds`] of the request).
    #[must_use]
    pub fn new(seconds: f32) -> Streamer<V> {
        Streamer {
            seconds,
            active: Vec::new(),
            pending: Vec::new(),
            next_id: 1,
        }
    }

    /// The buffer length of new streams.
    #[must_use]
    pub fn seconds(&self) -> f32 {
        self.seconds
    }

    /// `CreateStream` after the file was read: the stream goes on the
    /// pending list, started first when `start` (flag bit 0).
    ///
    /// # Errors
    ///
    /// See [`Stream::new`].
    pub fn create(
        &mut self,
        source: StreamSource,
        voice: V,
        start: bool,
    ) -> Result<StreamId, StreamError> {
        let mut stream = Stream::new(source, voice, self.seconds)?;
        if start {
            stream.start();
        }
        let id = self.next_id;
        self.next_id = self.next_id.wrapping_add(1).max(1);
        self.pending.push((id, stream));
        Ok(id)
    }

    fn find(&mut self, id: StreamId) -> Option<&mut Stream<V>> {
        self.active
            .iter_mut()
            .chain(self.pending.iter_mut())
            .find(|(i, _)| *i == id)
            .map(|(_, s)| s)
    }

    /// The stream of `id`.
    #[must_use]
    pub fn stream(&self, id: StreamId) -> Option<&Stream<V>> {
        self.active
            .iter()
            .chain(self.pending.iter())
            .find(|(i, _)| *i == id)
            .map(|(_, s)| s)
    }

    /// `DestroyStream`: stopped and removed; its voice is handed back for
    /// the device to release.
    pub fn destroy(&mut self, id: StreamId) -> Option<V> {
        self.stop(id);
        let list = if self.active.iter().any(|(i, _)| *i == id) {
            &mut self.active
        } else {
            &mut self.pending
        };
        let index = list.iter().position(|(i, _)| *i == id)?;
        Some(list.remove(index).1.voice)
    }

    /// `StartStream`: `StartHandle`, then one pump.
    pub fn start(&mut self, id: StreamId) -> bool {
        let Some(stream) = self.find(id) else {
            return false;
        };
        stream.start();
        stream.pump();
        true
    }

    /// `StopStream`.
    pub fn stop(&mut self, id: StreamId) -> bool {
        let Some(stream) = self.find(id) else {
            return false;
        };
        stream.stop();
        true
    }

    /// `SetVolume`, hundredths of a decibel.
    pub fn set_volume(&mut self, id: StreamId, volume: i32) -> bool {
        self.find(id).map(|s| s.set_volume(volume)).is_some()
    }

    /// `SetPan`, hundredths of a decibel.
    pub fn set_pan(&mut self, id: StreamId, pan: i32) -> bool {
        self.find(id).map(|s| s.set_pan(pan)).is_some()
    }

    /// `QueryStatus`; 0 for an unknown handle.
    #[must_use]
    pub fn status(&self, id: StreamId) -> u32 {
        self.stream(id).map_or(0, |s| s.state().status())
    }

    /// One pass of the worker (`PumpStreams` between its sleeps): every
    /// active stream pumped, then the refill plan of [`plan_refill`]
    /// carried out, then `MovePendingActive`.
    pub fn pass(&mut self) {
        if !self.active.is_empty() {
            let levels: Vec<Option<StreamLevel>> = self
                .active
                .iter_mut()
                .map(|(_, s)| {
                    s.pump();
                    s.level()
                })
                .collect();
            for (index, bytes) in plan_refill(&levels) {
                self.active[index].1.fill(bytes);
            }
        }
        self.move_pending_active();
    }

    /// `MovePendingActive`: streams leave the active list when they are not
    /// starting, playing or draining, and join it when they are.
    fn move_pending_active(&mut self) {
        let mut i = 0;
        while i < self.active.len() {
            if self.active[i].1.state().is_active() {
                i += 1;
            } else {
                let entry = self.active.remove(i);
                self.pending.push(entry);
            }
        }
        let mut i = 0;
        while i < self.pending.len() {
            if self.pending[i].1.state().is_active() {
                let entry = self.pending.remove(i);
                self.active.push(entry);
            } else {
                i += 1;
            }
        }
    }

    /// The active and pending handles, in list order.
    #[must_use]
    pub fn handles(&self) -> (Vec<StreamId>, Vec<StreamId>) {
        (
            self.active.iter().map(|(i, _)| *i).collect(),
            self.pending.iter().map(|(i, _)| *i).collect(),
        )
    }

    /// `ShutdownDevice`: every stream stopped and removed; the voices are
    /// handed back.
    pub fn shutdown(&mut self) -> Vec<V> {
        for (_, s) in &mut self.active {
            s.stop();
        }
        self.active
            .drain(..)
            .chain(self.pending.drain(..))
            .map(|(_, s)| s.voice)
            .collect()
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

    /// A voice that plays on demand.
    #[derive(Debug, Default)]
    struct Queue {
        blocks: Vec<Vec<u8>>,
        queued: u32,
        played: Vec<u8>,
        playing: bool,
        last: bool,
        log: Vec<String>,
    }

    impl Queue {
        /// Plays `bytes` of the queue (if playing).
        fn advance(&mut self, mut bytes: u32) {
            while bytes > 0 && self.playing && !self.blocks.is_empty() {
                let front = &mut self.blocks[0];
                let n = (bytes as usize).min(front.len());
                self.played.extend(front.drain(..n));
                self.queued -= n as u32;
                bytes -= n as u32;
                if front.is_empty() {
                    self.blocks.remove(0);
                }
            }
        }
    }

    impl StreamVoice for Queue {
        fn submit(&mut self, pcm: Vec<u8>, last: bool) -> bool {
            self.log.push(format!("submit {}", pcm.len()));
            self.queued += pcm.len() as u32;
            self.blocks.push(pcm);
            self.last |= last;
            true
        }
        fn queued_bytes(&mut self) -> u32 {
            self.queued
        }
        fn play(&mut self) -> bool {
            self.log.push("play".into());
            self.playing = true;
            true
        }
        fn stop(&mut self) {
            self.log.push("stop".into());
            self.playing = false;
        }
        fn set_volume(&mut self, _: i32) {}
        fn set_pan(&mut self, _: i32) {}
    }

    /// An 8-bit mono PCM file of `n` bytes at 1000 Hz, counting up.
    fn source(n: usize, looping: bool) -> StreamSource {
        use super::super::wav::tests::{fmt_block, riff};
        let data: Vec<u8> = (0..n).map(|i| i as u8).collect();
        let file = riff(&[(b"fmt ", fmt_block(1, 1, 1000, 1, 8)), (b"data", data)]);
        StreamSource::open(file.into(), looping).unwrap()
    }

    #[test]
    fn a_stream_prefills_plays_refills_and_drains() {
        let mut streamer = Streamer::new(1.0);
        let id = streamer
            .create(source(2500, false), Queue::default(), true)
            .unwrap();
        assert_eq!(streamer.status(id), 2);
        assert_eq!(streamer.handles(), (vec![], vec![id]));
        // The first pass only moves it to the active list.
        streamer.pass();
        assert_eq!(streamer.handles(), (vec![id], vec![]));
        // The next pumps it: a one-second prefill (1000 bytes), then play.
        streamer.pass();
        let s = streamer.stream(id).unwrap();
        assert_eq!((s.state(), s.capacity()), (StreamState::Playing, 1000));
        let voice = streamer.destroy(id).unwrap();
        assert_eq!(voice.log, ["submit 1000", "play", "stop"]);

        // Again, playing through: the refill keeps the queue topped up.
        let id = streamer
            .create(source(2500, false), Queue::default(), true)
            .unwrap();
        streamer.pass();
        streamer.pass();
        let mut finished = false;
        for _ in 0..200 {
            if let Some((_, s)) = streamer.active.iter_mut().find(|(i, _)| *i == id) {
                s.voice_mut().advance(40);
            }
            streamer.pass();
            if streamer.status(id) == 4 {
                finished = true;
                break;
            }
        }
        assert!(finished);
        let voice = streamer.destroy(id).unwrap();
        assert!(voice.last);
        assert!(
            voice.played.len() == 2500
                && voice.played.iter().enumerate().all(|(i, b)| *b == i as u8)
        );
        assert_eq!(voice.log.last().map(String::as_str), Some("stop"));
    }

    #[test]
    fn stop_and_restart_keep_the_queue() {
        let mut streamer = Streamer::new(0.5);
        let id = streamer
            .create(source(100, true), Queue::default(), false)
            .unwrap();
        assert_eq!(streamer.stream(id).unwrap().state(), StreamState::Loaded);
        assert!(streamer.start(id));
        // StartStream pumps at once: prefill 0.5 s = 500 bytes, play.
        assert_eq!(streamer.status(id), 1);
        assert!(streamer.stop(id));
        assert_eq!(streamer.status(id), 2);
        streamer.pass();
        assert_eq!(streamer.handles(), (vec![], vec![id]));
        assert!(streamer.start(id));
        let voice = streamer.destroy(id).unwrap();
        // The queue was full: no second prefill, play resumes.
        assert_eq!(voice.log, ["submit 500", "play", "stop", "play", "stop"]);
        assert_eq!(voice.blocks[0][..5], [0, 1, 2, 3, 4]);
        assert_eq!(voice.blocks[0][100..103], [0, 1, 2]);
        assert!(!voice.last);
        assert_eq!(
            Stream::new(source(1, false), Queue::default(), 0.0).unwrap_err(),
            StreamError::BufferLength
        );
        assert_eq!(streamer.status(999), 0);
    }

    #[test]
    fn streams_shorter_than_a_quarter_second_never_play() {
        // 0.2 s of data with a 1 s buffer: the prefill reaches the end, so
        // the stream drains, and the queue is shorter than the 0.25 s wait,
        // so it is not played; a draining stream is never prefilled again.
        // The reference behaves the same way.
        let mut streamer = Streamer::new(1.0);
        let id = streamer
            .create(source(200, false), Queue::default(), false)
            .unwrap();
        streamer.start(id);
        let s = streamer.stream(id).unwrap();
        assert_eq!(s.state(), StreamState::Draining);
        let voice = streamer.destroy(id).unwrap();
        assert_eq!(voice.log, ["submit 200", "stop"]);
        assert_eq!(StreamState::Draining.code(), 6);
    }
}
