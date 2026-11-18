//! The rules a device voice follows that need no platform code: how an
//! archetype's samples are queued on an XAudio2 source voice (the start
//! position and the loop region of `MixerSetStartOffset` and `Play`), and
//! how much of a stream's queue is still to play.
//!
//! The reference's buffers loop natively over the whole sound; a loop
//! range inside the sound was kept by its notify thread, which jumped back
//! at the range's end. XAudio2's loop region does the same.

use super::mixer::{Archetype, VoiceCaps};

/// One buffer submission: the frame to start at and the loop region
/// (begin and length in frames, a length of 0 looping to the end).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct BufferPlan {
    /// `PlayBegin`.
    pub play_begin: u32,
    /// `LoopBegin` and `LoopLength` of a looping play.
    pub loop_region: Option<(u32, u32)>,
}

/// The loop region of an archetype: its loop range when the voice has
/// position notification (the range lies inside the sound), else the
/// whole sound.
#[must_use]
pub fn loop_region(archetype: &Archetype, caps: VoiceCaps) -> (u32, u32) {
    let frames = archetype.frames;
    if caps.loop_notify {
        let begin = archetype.loop_start.min(frames);
        let end = archetype.loop_end.min(frames);
        if end > begin {
            return (begin, end - begin);
        }
    }
    (0, 0)
}

/// The submission that plays an archetype from `byte` (the mixer's
/// `set_position`, a multiple of the block) once or looping. A start at or
/// past the end of a one-shot plays nothing (`None`); a looping start at or
/// past the end of its loop region starts at the region's beginning, since
/// XAudio2 needs the start before the region's end.
#[must_use]
pub fn buffer_plan(
    archetype: &Archetype,
    caps: VoiceCaps,
    byte: u32,
    looping: bool,
) -> Option<BufferPlan> {
    let block = u32::from(archetype.format.block_align);
    let frames = archetype.frames;
    if block == 0 || frames == 0 {
        return None;
    }
    let start = byte / block;
    if !looping {
        return (start < frames).then_some(BufferPlan {
            play_begin: start,
            loop_region: None,
        });
    }
    let (begin, length) = loop_region(archetype, caps);
    let end = if length == 0 { frames } else { begin + length };
    let play_begin = if start < end { start } else { begin };
    Some(BufferPlan {
        play_begin,
        loop_region: Some((begin, length)),
    })
}

/// A stream voice's queue: bytes submitted, buffers still queued, and the
/// played count, which XAudio2 may reset (`SamplesPlayed` counts "since the
/// voice last started").
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct QueueLevel {
    /// Bytes submitted.
    pub submitted: u64,
    /// Samples counted before the last reset of the voice's counter.
    pub played_base: u64,
    /// The counter at the last look.
    pub last_samples: u64,
}

impl QueueLevel {
    /// The bytes still to play, from the voice's `samples_played` counter
    /// and the sizes of the buffers it still holds (`pending`, the playing
    /// one first): submitted minus played, kept between the queued
    /// buffers without the playing one and all of them.
    pub fn queued(&mut self, samples_played: u64, block_align: u16, pending: &[usize]) -> u32 {
        if samples_played < self.last_samples {
            self.played_base += self.last_samples;
        }
        self.last_samples = samples_played;
        let played = (self.played_base + samples_played) * u64::from(block_align);
        let all: u64 = pending.iter().map(|n| *n as u64).sum();
        let behind = all - pending.first().map_or(0, |n| *n as u64);
        let queued = self.submitted.saturating_sub(played).clamp(behind, all);
        u32::try_from(queued).unwrap_or(u32::MAX)
    }
}

#[cfg(test)]
mod tests {
    use super::super::mixer::{PcmFormat, buffer_flags};
    use super::*;

    fn archetype(frames: u32, loop_start: u32, loop_end: u32) -> Archetype {
        let format = PcmFormat {
            channels: 1,
            bytes_per_sample: 2,
            rate: 22050,
            block_align: 2,
        };
        Archetype::from_pcm(format, frames * 2, loop_start, loop_end, 0x40, true).unwrap()
    }

    #[test]
    fn plans() {
        let a = archetype(100, 0, 100);
        let caps = VoiceCaps::from_buffer_flags(a.buffer_flags);
        assert!(!caps.loop_notify);
        assert_eq!(
            buffer_plan(&a, caps, 20, false),
            Some(BufferPlan {
                play_begin: 10,
                loop_region: None
            })
        );
        assert_eq!(buffer_plan(&a, caps, 200, false), None);
        assert_eq!(
            buffer_plan(&a, caps, 200, true),
            Some(BufferPlan {
                play_begin: 0,
                loop_region: Some((0, 0))
            })
        );
        // A loop range inside the sound.
        let a = archetype(100, 10, 60);
        let caps = VoiceCaps::from_buffer_flags(a.buffer_flags);
        assert!(caps.loop_notify);
        assert_ne!(a.buffer_flags & buffer_flags::CTRL_POSITION_NOTIFY, 0);
        assert_eq!(loop_region(&a, caps), (10, 50));
        let p = buffer_plan(&a, caps, 2 * 59, true).unwrap();
        assert_eq!((p.play_begin, p.loop_region), (59, Some((10, 50))));
        let p = buffer_plan(&a, caps, 2 * 60, true).unwrap();
        assert_eq!(p.play_begin, 10);
    }

    #[test]
    fn queue_levels() {
        let mut q = QueueLevel {
            submitted: 3000,
            ..QueueLevel::default()
        };
        // Nothing played of three 1000-byte buffers.
        assert_eq!(q.queued(0, 2, &[1000, 1000, 1000]), 3000);
        // 250 frames played: 500 bytes into the first buffer.
        assert_eq!(q.queued(250, 2, &[1000, 1000, 1000]), 2500);
        // The counter restarted: the earlier count is kept (350 frames,
        // 700 bytes, played), within the two buffers left.
        assert_eq!(q.queued(100, 2, &[1000, 1000]), 2000);
        // Never below the buffers behind the playing one, nor above all.
        assert_eq!(q.queued(10_000, 2, &[1000, 1000]), 1000);
        let mut fresh = QueueLevel {
            submitted: 500,
            ..QueueLevel::default()
        };
        assert_eq!(fresh.queued(0, 2, &[500]), 500);
        assert_eq!(q.queued(0, 2, &[]), 0);
    }
}
