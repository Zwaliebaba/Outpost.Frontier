//! Texture animation: the playback records of the texture library
//! (`FTexturePlayback` in `CppSource/Shading/TextureLibrary.h` and its
//! helpers `PlaybackDuration`, `InitializePlayback`,
//! `RecomputePlaybackDuration`, `SetPlaybackFrame`, `TruncatePlaybackFrame`,
//! `SetPlaybackElapsed`, `AdvancePlayback` and `ReadPlaybackFrame` in
//! `TextureLibrary.cpp`).
//!
//! A playback selects a frame of its source record from the time that has
//! passed: frame `floor(fmod(rate * elapsed, frames))`. The wrap mode
//! decides what happens at the end: 0 holds the last moment (clamp), 1
//! loops, 2 plays forwards and backwards (ping-pong).
//!
//! The reference does this arithmetic on the x87 in 53-bit precision
//! (inline assembly in `PlaybackDuration`, `TruncatePlaybackFrame` and
//! `AdvancePlayback`); here it is `f64` arithmetic in the same order,
//! rounded to `f32` where the reference stores a `float` (decision T11).
//! The elapsed time is a `double`; the time handed to `SetPlaybackElapsed`
//! is a `float`.

use neuron_core::image::TextureFrame;

/// Rates at or below this magnitude leave the duration as it was
/// (`std::fabs(Rate) > 1e-5f`).
pub const RATE_EPSILON: f32 = 1e-5;

/// `0.001f`, the millisecond factor of `AdvancePlayback`.
const SECONDS_PER_MILLISECOND: f32 = 0.001;

/// What a playback's source record says now: its frame count and its tile
/// rate (`TileRate`, frames per second), both read live by the reference.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Source {
    /// `Frames.size()`.
    pub frames: u32,
    /// `TileRate`.
    pub rate: f32,
}

/// The wrap modes of [`Playback::wrap`].
pub mod wrap {
    /// Stop at the end (and at the start).
    pub const CLAMP: i32 = 0;
    /// Start again (the default).
    pub const LOOP: i32 = 1;
    /// Reverse at either end.
    pub const PING_PONG: i32 = 2;
}

/// One playback record (`FTexturePlayback`).
#[derive(Clone, Debug, PartialEq)]
pub struct Playback {
    /// The source record, an index into the texture library's records.
    pub source: usize,
    /// The rate `SetLocalRate` stored.
    pub local_rate: f32,
    /// Set by `SetLocalRate`: the local rate replaces the source's.
    pub use_local_rate: bool,
    /// +1 forwards, -1 backwards (ping-pong).
    pub direction: f32,
    /// The selected frame; not bounded (see [`Playback::read_frame`]).
    pub current_frame: u32,
    /// The frame count cached at creation, or on the first advance or read
    /// when it was 0.
    pub frame_count: u32,
    /// Seconds.
    pub elapsed: f64,
    /// `frames / rate - 1e-6` seconds.
    pub duration: f64,
    /// References held through the playback's id.
    pub ref_count: u32,
    /// See [`wrap`].
    pub wrap: i32,
}

/// `PlaybackDuration`: `fild` the count, `fdiv` the `float` rate, `fsub`
/// the `double` 1e-6.
#[must_use]
pub fn duration(count: u32, rate: f32) -> f64 {
    f64::from(count) / f64::from(rate) - 1e-6
}

/// `TruncatePlaybackFrame`: the low 32 bits of the x87 conversion to a
/// signed 64-bit integer with truncation; NaN and values outside the
/// 64-bit range give the integer indefinite, whose low half is 0.
#[must_use]
pub fn truncate_frame(value: f64) -> u32 {
    const LIMIT: f64 = 9_223_372_036_854_775_808.0; // 2^63
    if value.is_nan() || !(-LIMIT..LIMIT).contains(&value) {
        return 0;
    }
    // In range: the conversion is exact after truncation.
    (value.trunc() as i64) as u32
}

/// A frame as `GetFrame` and `GetTextureFrame` write it: the renderer
/// handle of the frame's child texture and the texture rectangle as `f32`
/// bit patterns.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameOut {
    /// The texture, 0 for none.
    pub handle: u32,
    /// `u0, v0, u1, v1`.
    pub uv: [u32; 4],
}

impl FrameOut {
    /// `WriteEmptyFrame`: no texture over the whole rectangle.
    pub const EMPTY: FrameOut = FrameOut {
        handle: 0,
        uv: [0, 0, 0x3F80_0000, 0x3F80_0000],
    };
}

impl Playback {
    /// `InitializePlayback` (the id's reference is added by the caller).
    #[must_use]
    pub fn new(source: usize, from: Source) -> Playback {
        let mut playback = Playback {
            source,
            local_rate: 0.0,
            use_local_rate: false,
            direction: 1.0,
            current_frame: 0,
            frame_count: from.frames,
            elapsed: 0.0,
            duration: 0.0,
            ref_count: 0,
            wrap: wrap::LOOP,
        };
        if from.rate.abs() > RATE_EPSILON {
            playback.duration = duration(playback.frame_count, from.rate);
        }
        playback
    }

    /// `EffectivePlaybackRate`.
    #[must_use]
    pub fn rate(&self, from: Source) -> f32 {
        if self.use_local_rate {
            self.local_rate
        } else {
            from.rate
        }
    }

    /// `SetLocalRate`: the duration follows; the elapsed time and the frame
    /// stay.
    pub fn set_local_rate(&mut self, rate: f32, from: Source) {
        self.local_rate = rate;
        self.use_local_rate = true;
        let rate = self.rate(from);
        if rate.abs() > RATE_EPSILON {
            self.duration = duration(from.frames, rate);
        }
    }

    /// `SetPlaybackFrame`: false (and nothing changed) for a frame the
    /// source does not have.
    pub fn set_frame(&mut self, index: u32, from: Source) -> bool {
        if from.frames == 0 || index >= from.frames {
            return false;
        }
        self.current_frame = index;
        true
    }

    /// `SetPlaybackElapsed`: the frame is computed even for an empty
    /// source and stored before the bounds check, whose result is ignored.
    pub fn set_elapsed(&mut self, seconds: f32, from: Source) {
        self.elapsed = f64::from(seconds);
        let count = f64::from(from.frames);
        let cyclic = (f64::from(self.rate(from)) * self.elapsed) % count;
        self.current_frame = truncate_frame(cyclic.floor());
        let current = self.current_frame;
        let _ = self.set_frame(current, from);
    }

    /// `AdvancePlayback` by `milliseconds` (a `float`, as `AdvanceTime` and
    /// `TickAllPlayback` round it).
    pub fn advance(&mut self, milliseconds: f32, from: Source) {
        if self.frame_count == 0 {
            self.frame_count = from.frames;
        }
        if self.frame_count <= 1 {
            return;
        }
        let delta = f64::from(milliseconds) * f64::from(SECONDS_PER_MILLISECOND);
        let seconds: f32 = match self.wrap {
            wrap::CLAMP => {
                let next = delta + self.elapsed;
                self.elapsed = next;
                // fcompp: the duration unless next is below it (NaN too).
                let value = if next < self.duration {
                    next
                } else {
                    self.duration
                };
                // fcom 0: zero unless above zero (NaN too).
                if value > 0.0 { value as f32 } else { 0.0 }
            }
            wrap::LOOP => {
                let next = delta + self.elapsed;
                self.elapsed = next;
                (next % self.duration) as f32
            }
            wrap::PING_PONG => {
                let direction = self.direction;
                let next = f64::from(direction) * delta + self.elapsed;
                self.elapsed = next;
                if direction < 0.0 && next < 0.0 {
                    let remainder = next % self.duration;
                    self.direction = -direction;
                    (-remainder) as f32
                } else if direction > 0.0 && next > self.duration {
                    let remainder = next % self.duration;
                    self.direction = -direction;
                    (self.duration - remainder) as f32
                } else {
                    next as f32
                }
            }
            _ => 0.0,
        };
        self.set_elapsed(seconds, from);
    }

    /// `ReadPlaybackFrame`: frame `u32::MAX` is the current frame (frame 0
    /// for a source of one frame), a frame past the end is the last.
    /// `None` for an empty source, where the reference writes
    /// [`FrameOut::EMPTY`] and fails.
    ///
    /// The current frame is not bounded: a negative rate or elapsed time
    /// leaves one past the end, which the reference reads outside the
    /// frame table; the port gives [`FrameOut::EMPTY`] there (divergence
    /// D262). `child_handle` gives the handle of a child by its index in
    /// the source's child table.
    pub fn read_frame(
        &mut self,
        frame: u32,
        frames: &[TextureFrame],
        child_handle: impl Fn(u32) -> u32,
    ) -> Option<FrameOut> {
        if self.frame_count == 0 {
            self.frame_count = u32::try_from(frames.len()).unwrap_or(u32::MAX);
        }
        let count = self.frame_count;
        if count == 0 {
            return None;
        }
        let selected = if frame == u32::MAX {
            if count <= 1 { 0 } else { self.current_frame }
        } else if frame >= count {
            count - 1
        } else {
            frame
        };
        Some(frame_out(frames, selected, child_handle).unwrap_or(FrameOut::EMPTY))
    }
}

/// Frame `index` of a frame table with its child's handle; `None` past the
/// end.
pub fn frame_out(
    frames: &[TextureFrame],
    index: u32,
    child_handle: impl Fn(u32) -> u32,
) -> Option<FrameOut> {
    let frame = frames.get(index as usize)?;
    Some(FrameOut {
        handle: child_handle(frame.child),
        uv: frame.uv,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    const SRC: Source = Source {
        frames: 4,
        rate: 2.0,
    };

    #[test]
    fn durations_and_truncation() {
        assert_eq!(duration(4, 2.0), 2.0 - 1e-6);
        assert_eq!(truncate_frame(3.0), 3);
        assert_eq!(truncate_frame(-1.0), u32::MAX);
        assert_eq!(truncate_frame(f64::NAN), 0);
        assert_eq!(truncate_frame(1e300), 0);
        assert_eq!(truncate_frame(4_294_967_297.0), 1);
    }

    #[test]
    fn loop_wraps_and_selects() {
        let mut p = Playback::new(0, SRC);
        assert_eq!(p.wrap, wrap::LOOP);
        p.advance(750.0, SRC);
        assert_eq!(p.current_frame, 1);
        p.advance(1500.0, SRC);
        // 2.25 s wraps by the duration just under 2 s.
        assert_eq!(p.current_frame, 0);
        assert!(p.elapsed > 0.25 && p.elapsed < 0.26);
    }

    #[test]
    fn clamp_holds_the_end() {
        let mut p = Playback::new(0, SRC);
        p.wrap = wrap::CLAMP;
        p.advance(5000.0, SRC);
        assert_eq!(p.current_frame, 3);
        assert_eq!(p.elapsed, f64::from((2.0 - 1e-6f64) as f32));
    }

    #[test]
    fn ping_pong_reverses() {
        let mut p = Playback::new(0, SRC);
        p.wrap = wrap::PING_PONG;
        p.advance(2500.0, SRC);
        assert_eq!(p.direction, -1.0);
        p.advance(2000.0, SRC);
        assert_eq!(p.direction, 1.0);
    }

    #[test]
    fn single_frames_do_not_advance_and_reads_clamp() {
        let one = Source {
            frames: 1,
            rate: 24.0,
        };
        let mut p = Playback::new(0, one);
        p.advance(1000.0, one);
        assert_eq!(p.elapsed, 0.0);
        let frames = [TextureFrame {
            child: 0,
            uv: [0, 0, 1, 1],
        }];
        let out = p.read_frame(7, &frames, |_| 9).unwrap();
        assert_eq!(out.handle, 9);
        p.current_frame = 5;
        assert_eq!(p.read_frame(u32::MAX, &frames, |_| 9).unwrap().handle, 9);
    }
}
