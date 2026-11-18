//! Startup movie playback, the platform-free part (work package P4.15; the
//! C++ `movie` library's approach 0, `BuildAndRunWmAsfGraph`,
//! `MoviePlaybackWndProc`, `UpdateMovie`, `StopMovie` in
//! `CppSource/movie/MoviePlay.cpp` and `MediaPlayer.cpp`).
//!
//! The C++ builds a DirectShow graph over the WM ASF Reader and lets its
//! video window draw into the game window. Here (migration plan, P4.15) a
//! Media Foundation source reader (`platform::media`) decodes the frames
//! into a texture the renderer draws and the audio into PCM the audio
//! device streams; this module holds the rules that do not depend on the
//! platform: where the picture goes, which keys stop it, how long
//! `UpdateMovie` waits, the WAV wrapper of the decoded audio, and which
//! frame shows at a time.

/// `UpdateMovie` sleeps this long before it answers (`Sleep(0x12c)`).
pub const UPDATE_SLEEP_MS: u32 = 300;

/// The virtual keys whose `WM_KEYDOWN` stops the movie: `VK_SHIFT` and
/// `VK_ESCAPE`. Every other key-down is swallowed while a movie plays.
pub const STOP_KEYS: [u32; 2] = [0x10, 0x1b];

/// Whether a key-down of virtual key `vk` stops the movie.
#[must_use]
pub fn stops_movie(vk: u32) -> bool {
    STOP_KEYS.contains(&vk)
}

/// Where the picture goes in a client area of `client` pixels for a movie
/// whose largest ideal size is `ideal`: the ideal size, clamped to the
/// client area, centred (`SetDestinationPosition(Left, Top, UsedWidth,
/// UsedHeight)`). Returns left, top, width, height.
#[must_use]
pub fn placement(ideal: (i32, i32), client: (i32, i32)) -> (i32, i32, i32, i32) {
    let width = ideal.0.min(client.0);
    let height = ideal.1.min(client.1);
    // C's division truncates toward zero, as Rust's does.
    (
        (client.0 - width) / 2,
        (client.1 - height) / 2,
        width,
        height,
    )
}

/// A PCM format of decoded movie audio.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PcmFormat {
    pub channels: u16,
    pub rate: u32,
    pub bits: u16,
}

/// A canonical 44-byte-header WAV file of `pcm` in `format`, for the audio
/// device's streams, which take WAV bytes.
#[must_use]
pub fn wav_file(format: PcmFormat, pcm: &[u8]) -> Vec<u8> {
    let block_align = format.channels * (format.bits / 8);
    let byte_rate = format.rate * u32::from(block_align);
    let data_len = u32::try_from(pcm.len()).unwrap_or(u32::MAX);
    let mut out = Vec::with_capacity(44 + pcm.len());
    out.extend_from_slice(b"RIFF");
    out.extend_from_slice(&data_len.saturating_add(36).to_le_bytes());
    out.extend_from_slice(b"WAVEfmt ");
    out.extend_from_slice(&16u32.to_le_bytes());
    out.extend_from_slice(&1u16.to_le_bytes());
    out.extend_from_slice(&format.channels.to_le_bytes());
    out.extend_from_slice(&format.rate.to_le_bytes());
    out.extend_from_slice(&byte_rate.to_le_bytes());
    out.extend_from_slice(&block_align.to_le_bytes());
    out.extend_from_slice(&format.bits.to_le_bytes());
    out.extend_from_slice(b"data");
    out.extend_from_slice(&data_len.to_le_bytes());
    out.extend_from_slice(&pcm[..data_len as usize]);
    out
}

/// A decoded video frame: BGRA rows top to bottom, without padding, and its
/// presentation time in 100-nanosecond units.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct VideoFrame {
    pub time: i64,
    pub width: u32,
    pub height: u32,
    pub bgra: Vec<u8>,
}

/// Frame pacing: the frame to show at a playback time is the last decoded
/// one whose time has come.
#[derive(Debug, Default)]
pub struct FramePacer {
    pending: Option<VideoFrame>,
}

impl FramePacer {
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// Feeds decoded frames from `next` (in presentation order) up to
    /// playback time `now` (100 ns units) and returns the newest frame due,
    /// if a new one is due; a frame not yet due is kept for a later call.
    pub fn due(
        &mut self,
        now: i64,
        mut next: impl FnMut() -> Option<VideoFrame>,
    ) -> Option<VideoFrame> {
        let mut shown = None;
        loop {
            let frame = match self.pending.take() {
                Some(f) => f,
                None => match next() {
                    Some(f) => f,
                    None => return shown,
                },
            };
            if frame.time > now {
                self.pending = Some(frame);
                return shown;
            }
            shown = Some(frame);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_picture_is_clamped_and_centred() {
        assert_eq!(placement((640, 480), (1024, 768)), (192, 144, 640, 480));
        assert_eq!(placement((1280, 720), (1024, 768)), (0, 24, 1024, 720));
        // Odd margins round down, as C's division does.
        assert_eq!(placement((640, 480), (1023, 767)), (191, 143, 640, 480));
    }

    #[test]
    fn shift_and_escape_stop_and_nothing_else() {
        assert!(stops_movie(0x10));
        assert!(stops_movie(0x1b));
        assert!(!stops_movie(0x0d));
        assert!(!stops_movie(0x20));
    }

    #[test]
    fn wav_headers_describe_the_pcm() {
        let pcm = [1u8, 2, 3, 4, 5, 6, 7, 8];
        let w = wav_file(
            PcmFormat {
                channels: 2,
                rate: 44_100,
                bits: 16,
            },
            &pcm,
        );
        assert_eq!(&w[..4], b"RIFF");
        assert_eq!(u32::from_le_bytes(w[4..8].try_into().unwrap()), 44);
        assert_eq!(&w[8..16], b"WAVEfmt ");
        assert_eq!(u16::from_le_bytes([w[22], w[23]]), 2);
        assert_eq!(u32::from_le_bytes(w[28..32].try_into().unwrap()), 176_400);
        assert_eq!(u16::from_le_bytes([w[32], w[33]]), 4);
        assert_eq!(&w[36..40], b"data");
        assert_eq!(&w[44..], &pcm);
    }

    #[test]
    fn the_pacer_shows_the_newest_due_frame_and_keeps_the_next() {
        let frame = |time| VideoFrame {
            time,
            width: 1,
            height: 1,
            bgra: vec![0; 4],
        };
        let mut source =
            vec![frame(0), frame(400_000), frame(800_000), frame(1_200_000)].into_iter();
        let mut p = FramePacer::new();
        assert_eq!(p.due(0, || source.next()).map(|f| f.time), Some(0));
        // Nothing new is due before 40 ms.
        assert_eq!(p.due(300_000, || source.next()), None);
        // At 90 ms the 40 ms frame is skipped for the 80 ms one.
        assert_eq!(
            p.due(900_000, || source.next()).map(|f| f.time),
            Some(800_000)
        );
        assert_eq!(
            p.due(5_000_000, || source.next()).map(|f| f.time),
            Some(1_200_000)
        );
        assert_eq!(p.due(6_000_000, || source.next()), None);
    }
}
