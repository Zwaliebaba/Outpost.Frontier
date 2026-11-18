//! Decoding: the game's sounds to the 16-bit (or 8-bit) PCM the mixer and
//! the streamer play.
//!
//! Every sound file is a RIFF/WAVE file ([`super::wav`]): a few hold PCM,
//! the rest (all music, all voice lines and nearly all effects) MPEG-1, -2
//! or -2.5 Layer III in a `WAVE_FORMAT_MPEGLAYER3` block with the game's
//! `fact` (sample count) and `trim` (encoder delay) chunks. The reference
//! hands MP3 data to the Audio Compression Manager's Fraunhofer codec,
//! found by its display name, which suggests 16-bit PCM of the same rate
//! and channels; `symphonia` decodes it here (decision T13). MP3 decoders
//! are not bit-exact, so the samples differ slightly from the codec's
//! (divergence D273); lengths, trim and the `fact` cut follow the
//! reference.
//!
//! - [`decode_archetype`]: `WavLibLoad` and `WavLibAcmDecode`, the whole
//!   file at once for a sound archetype.
//! - [`StreamSource`]: the streamer's source (`LoadWav`, the ACM block
//!   conversion and `TrimEncoderDelay`), decoded block by block, with the
//!   gapless trim of [`super::stream::GaplessTrim`] and looping.

use std::sync::Arc;

use symphonia::core::codecs::audio::well_known::CODEC_ID_MP3;
use symphonia::core::codecs::audio::{AudioCodecParameters, AudioDecoder, AudioDecoderOptions};
use symphonia::core::errors::Error as SymphoniaError;
use symphonia::core::packet::PacketRef;
use symphonia::core::units::{Duration, Timestamp};
use symphonia::default::codecs::MpaDecoder;

use super::mixer::PcmFormat;
use super::stream::{GaplessTrim, buffer_bytes, pcm_frame_bytes};
use super::wav::{Loader, WAVE_FORMAT_MPEGLAYER3, WAVE_FORMAT_PCM, WavError, WavFile, WaveFormat};

/// Why a sound cannot be decoded.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum DecodeError {
    /// The RIFF/WAVE container.
    Wav(WavError),
    /// A format tag other than PCM and MPEG Layer III; the reference's
    /// MP3 codec cannot convert it either.
    UnsupportedFormat(u16),
    /// The MP3 data holds no frame the decoder accepts, or the decoder
    /// failed for good.
    Mp3(String),
    /// Decoding produced no samples (`"WavLib: failed to decompress
    /// sound."`).
    NoSamples,
}

impl core::fmt::Display for DecodeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        match self {
            DecodeError::Wav(e) => write!(f, "{e}"),
            DecodeError::UnsupportedFormat(tag) => write!(f, "unsupported format tag {tag:#06x}"),
            DecodeError::Mp3(e) => write!(f, "MP3 decoding failed: {e}"),
            DecodeError::NoSamples => f.write_str("decoding produced no samples"),
        }
    }
}

impl std::error::Error for DecodeError {}

impl From<WavError> for DecodeError {
    fn from(e: WavError) -> Self {
        DecodeError::Wav(e)
    }
}

/// The four words `Fmt8` the loaders hand on: channels, bytes per sample
/// (`bits >> 3`), the rate cut to 16 bits and the block align as
/// `bytes * channels` cut to 16 bits.
#[must_use]
pub fn fmt8(channels: u16, bits: u16, rate: u32) -> PcmFormat {
    let bytes = bits >> 3;
    PcmFormat {
        channels,
        bytes_per_sample: bytes,
        rate: rate as u16,
        block_align: bytes.wrapping_mul(channels),
    }
}

/// A decoded archetype: the format and samples `FillFromPcm` receives.
/// The loop range is the whole sound (`FArchetypeWavSink::Consume`).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DecodedSound {
    /// The PCM format.
    pub format: PcmFormat,
    /// Interleaved little-endian samples.
    pub samples: Vec<u8>,
    /// MP3 frames the decoder rejected, decoded as silence.
    pub rejected_frames: usize,
}

impl DecodedSound {
    /// The byte count as the archetype takes it.
    #[must_use]
    pub fn byte_count(&self) -> u32 {
        u32::try_from(self.samples.len()).unwrap_or(u32::MAX)
    }
}

/// `WavLibLoad`: a sound archetype's file decoded whole. PCM passes
/// through; MP3 is decoded, the `trim` samples dropped from the front when
/// positive and fewer than decoded, and the output cut at the `fact` count
/// when positive.
///
/// # Errors
///
/// See [`DecodeError`].
pub fn decode_archetype(bytes: &[u8]) -> Result<DecodedSound, DecodeError> {
    let wav = WavFile::parse(bytes, Loader::Archetype)?;
    let format = wav.format;
    let data = &bytes[wav.data_range.clone()];
    match format.tag {
        WAVE_FORMAT_PCM => Ok(DecodedSound {
            format: fmt8(format.channels, format.bits_per_sample, format.rate),
            samples: data.to_vec(),
            rejected_frames: 0,
        }),
        WAVE_FORMAT_MPEGLAYER3 => {
            let mut frames = Mp3Frames::new(format)?;
            let mut pcm = Vec::new();
            while frames.decode_next(data, &mut pcm)? {}
            if pcm.is_empty() {
                return Err(DecodeError::NoSamples);
            }
            let frame_bytes = pcm_frame_bytes(16, format.channels);
            let mut start = 0usize;
            let mut used = pcm.len();
            if let Some(trim) = wav.trim_samples.map(u32::cast_signed).filter(|t| *t > 0) {
                let skip = frame_bytes.wrapping_mul(trim.cast_unsigned()) as usize;
                if skip < used {
                    start = skip;
                    used -= skip;
                }
            }
            if let Some(fact) = wav.fact_samples.map(u32::cast_signed).filter(|f| *f > 0) {
                let cap = frame_bytes.wrapping_mul(fact.cast_unsigned()) as usize;
                used = used.min(cap);
            }
            pcm.truncate(start + used);
            pcm.drain(..start);
            Ok(DecodedSound {
                format: fmt8(format.channels, 16, format.rate),
                samples: pcm,
                rejected_frames: frames.rejected_frames(),
            })
        }
        tag => Err(DecodeError::UnsupportedFormat(tag)),
    }
}

/// One MPEG audio Layer III frame header.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct FrameHeader {
    /// Frame bytes, the 4-byte header included.
    pub size: usize,
    /// Samples per channel the frame decodes to.
    pub samples: usize,
    /// Sample rate.
    pub rate: u32,
    /// Channels.
    pub channels: u16,
}

const BITRATES_V1: [u32; 15] = [
    0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320,
];
const BITRATES_V2: [u32; 15] = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160];

impl FrameHeader {
    /// The Layer III frame header at the start of `bytes`, if there is one
    /// (free-format and reserved values are not).
    #[must_use]
    pub fn parse(bytes: &[u8]) -> Option<FrameHeader> {
        let h = u32::from_be_bytes(bytes.get(..4)?.try_into().ok()?);
        if h >> 21 != 0x7FF {
            return None;
        }
        let version = (h >> 19) & 3; // 0: 2.5, 2: 2, 3: 1
        let layer = (h >> 17) & 3; // 1: Layer III
        let bitrate = ((h >> 12) & 0xF) as usize;
        let rate_index = ((h >> 10) & 3) as usize;
        if version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate_index == 3 {
            return None;
        }
        let padding = ((h >> 9) & 1) as usize;
        let channels = if (h >> 6) & 3 == 3 { 1 } else { 2 };
        let (rate, kbps, coefficient, samples) = match version {
            3 => (
                [44100, 48000, 32000][rate_index],
                BITRATES_V1[bitrate],
                144_000,
                1152,
            ),
            2 => (
                [22050, 24000, 16000][rate_index],
                BITRATES_V2[bitrate],
                72_000,
                576,
            ),
            _ => (
                [11025, 12000, 8000][rate_index],
                BITRATES_V2[bitrate],
                72_000,
                576,
            ),
        };
        let size = (coefficient * kbps / rate) as usize + padding;
        Some(FrameHeader {
            size,
            samples,
            rate,
            channels,
        })
    }
}

/// An MP3 decoder over the data of one file: frames split by their
/// headers, decoded by `symphonia` to 16-bit PCM.
pub struct Mp3Frames {
    decoder: MpaDecoder,
    channels: u16,
    rate: u32,
    position: usize,
    rejected: usize,
    scratch: Vec<f32>,
}

impl core::fmt::Debug for Mp3Frames {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.debug_struct("Mp3Frames")
            .field("channels", &self.channels)
            .field("rate", &self.rate)
            .field("position", &self.position)
            .finish_non_exhaustive()
    }
}

/// A float sample as a 16-bit sample: scaled by 32768, rounded to nearest
/// and clamped.
#[must_use]
pub fn sample_to_i16(sample: f32) -> i16 {
    let scaled = (f64::from(sample) * 32768.0).round();
    scaled.clamp(-32768.0, 32767.0) as i16
}

impl Mp3Frames {
    /// A decoder for data of `format` (its channels and rate).
    ///
    /// # Errors
    ///
    /// When `symphonia` refuses the codec.
    pub fn new(format: WaveFormat) -> Result<Mp3Frames, DecodeError> {
        let mut params = AudioCodecParameters::new();
        params.for_codec(CODEC_ID_MP3);
        let mut options = AudioDecoderOptions::default();
        options.gapless = false;
        let decoder =
            MpaDecoder::try_new(&params, &options).map_err(|e| DecodeError::Mp3(e.to_string()))?;
        Ok(Mp3Frames {
            decoder,
            channels: format.channels,
            rate: format.rate,
            position: 0,
            rejected: 0,
            scratch: Vec::new(),
        })
    }

    /// Back to the start of the data with a fresh decoder state.
    pub fn restart(&mut self) {
        self.decoder.reset();
        self.position = 0;
    }

    /// Whether the next frame is the first one of the data.
    #[must_use]
    pub fn at_start(&self) -> bool {
        self.position == 0
    }

    /// Frames the decoder rejected since creation (decoded as silence).
    #[must_use]
    pub fn rejected_frames(&self) -> usize {
        self.rejected
    }

    /// Decodes the next frame of `data`, appending its samples to `out`.
    /// Bytes that are not a frame header are skipped; a frame the decoder
    /// rejects gives silence of the frame's length, so later samples keep
    /// their place. False at the end of the data (a frame cut short by the
    /// end is not decoded).
    ///
    /// # Errors
    ///
    /// A frame whose rate or channels differ from the format's, or a
    /// decoder failure that is not a malformed frame.
    pub fn decode_next(&mut self, data: &[u8], out: &mut Vec<u8>) -> Result<bool, DecodeError> {
        let header = loop {
            let Some(rest) = data.get(self.position..) else {
                return Ok(false);
            };
            if rest.len() < 4 {
                self.position = data.len();
                return Ok(false);
            }
            match FrameHeader::parse(rest) {
                Some(h) if h.size >= 4 => break h,
                _ => self.position += 1,
            }
        };
        let end = self.position + header.size;
        let Some(frame) = data.get(self.position..end) else {
            self.position = data.len();
            return Ok(false);
        };
        self.position = end;
        if header.rate != self.rate || header.channels != self.channels {
            return Err(DecodeError::Mp3(format!(
                "frame of {} Hz, {} channels in a {} Hz, {} channel file",
                header.rate, header.channels, self.rate, self.channels
            )));
        }
        let samples = header.samples as u64;
        let packet = PacketRef::new(0, Timestamp::ZERO, Duration::from(samples), frame);
        let count = header.samples * usize::from(self.channels);
        match self.decoder.decode_ref(&packet) {
            Ok(buffer) => {
                buffer.copy_to_vec_interleaved::<f32>(&mut self.scratch);
                out.reserve(self.scratch.len() * 2);
                for s in &self.scratch {
                    out.extend_from_slice(&sample_to_i16(*s).to_le_bytes());
                }
            }
            Err(SymphoniaError::DecodeError(_) | SymphoniaError::IoError(_)) => {
                out.resize(out.len() + count * 2, 0);
                self.rejected += 1;
            }
            Err(e) => return Err(DecodeError::Mp3(e.to_string())),
        }
        Ok(true)
    }
}

/// What [`StreamSource::read`] produced.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Block {
    /// PCM bytes, at most the amount asked for.
    pub pcm: Vec<u8>,
    /// The source is exhausted: no further bytes will come (never set for
    /// a looping source).
    pub end: bool,
}

#[derive(Debug)]
enum Kind {
    Pcm {
        cursor: usize,
    },
    Mp3 {
        frames: Box<Mp3Frames>,
        pending: Vec<u8>,
        taken: usize,
    },
}

/// The decoded output a stream plays (`PcmTag` after `LoadWav`).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StreamFormat {
    /// Channels.
    pub channels: u16,
    /// Bits per sample.
    pub bits_per_sample: u16,
    /// Frames per second.
    pub rate: u32,
    /// Bytes per second (`BytesPerSec`).
    pub bytes_per_sec: u32,
    /// Bytes per frame (`PcmBlock`).
    pub block_align: u16,
}

/// A stream's source: the file's samples, decoded as the streamer pulls
/// them, trimmed and looped as `TrimEncoderDelay` and
/// `AdvanceSourceOrDrain` do.
#[derive(Debug)]
pub struct StreamSource {
    bytes: Arc<[u8]>,
    wav: WavFile,
    kind: Kind,
    trim: GaplessTrim,
    trim_samples: u32,
    looping: bool,
    format: StreamFormat,
    exhausted: bool,
}

impl StreamSource {
    /// `LoadWav` on `bytes`: PCM plays as stored; MP3 decodes to 16-bit
    /// PCM of the file's rate and channels.
    ///
    /// # Errors
    ///
    /// See [`DecodeError`].
    pub fn open(bytes: Arc<[u8]>, looping: bool) -> Result<StreamSource, DecodeError> {
        let wav = WavFile::parse(&bytes, Loader::Stream)?;
        let file = wav.format;
        let (kind, format) = match file.tag {
            WAVE_FORMAT_PCM => (
                Kind::Pcm { cursor: 0 },
                StreamFormat {
                    channels: file.channels,
                    bits_per_sample: file.bits_per_sample,
                    rate: file.rate,
                    bytes_per_sec: file.avg_bytes_per_sec,
                    block_align: file.block_align,
                },
            ),
            WAVE_FORMAT_MPEGLAYER3 => {
                let block = 2u16.wrapping_mul(file.channels);
                (
                    Kind::Mp3 {
                        frames: Box::new(Mp3Frames::new(file)?),
                        pending: Vec::new(),
                        taken: 0,
                    },
                    StreamFormat {
                        channels: file.channels,
                        bits_per_sample: 16,
                        rate: file.rate,
                        bytes_per_sec: file.rate.wrapping_mul(u32::from(block)),
                        block_align: block,
                    },
                )
            }
            tag => return Err(DecodeError::UnsupportedFormat(tag)),
        };
        let frame_bytes = pcm_frame_bytes(format.bits_per_sample, format.channels);
        Ok(StreamSource {
            trim: GaplessTrim::new(wav.fact_samples, frame_bytes),
            trim_samples: wav.trim_samples.unwrap_or(0),
            bytes,
            wav,
            kind,
            looping,
            format,
            exhausted: false,
        })
    }

    /// The output format.
    #[must_use]
    pub fn format(&self) -> StreamFormat {
        self.format
    }

    /// Whether the source loops.
    #[must_use]
    pub fn looping(&self) -> bool {
        self.looping
    }

    /// The buffer a stream of `seconds` holds: `avg * seconds` for PCM;
    /// for MP3 the same for the decoded rate (the reference asks the codec
    /// for the PCM size of `avg * seconds` compressed bytes; divergence
    /// D274).
    #[must_use]
    pub fn buffer_bytes(&self, seconds: f32) -> u32 {
        buffer_bytes(self.format.bytes_per_sec, seconds)
    }

    /// Back to the data start (`AdvanceSourceOrDrain` on a looping
    /// stream): the decoder reopened, the delay disarmed, the count reset.
    fn restart(&mut self) {
        let frame_bytes = self.trim.frame_bytes;
        self.trim = GaplessTrim::new(self.wav.fact_samples, frame_bytes);
        match &mut self.kind {
            Kind::Pcm { cursor } => *cursor = 0,
            Kind::Mp3 {
                frames,
                pending,
                taken,
            } => {
                frames.restart();
                pending.clear();
                *taken = 0;
            }
        }
    }

    /// The end of the data: a looping source starts over, another one is
    /// exhausted.
    fn data_end(&mut self) {
        if self.looping {
            self.restart();
        } else {
            self.exhausted = true;
        }
    }

    /// Up to `want` bytes of PCM.
    ///
    /// # Errors
    ///
    /// A decoder failure; the source is exhausted after it.
    pub fn read(&mut self, want: u32) -> Result<Block, DecodeError> {
        let want = want as usize;
        let mut pcm = Vec::with_capacity(want);
        // A full pass over the data that yields nothing would loop forever.
        let mut empty_restarts = 0;
        while pcm.len() < want && !(self.exhausted && self.drained()) {
            let before = pcm.len();
            let restarted = match &mut self.kind {
                Kind::Pcm { cursor } => {
                    let data = &self.bytes[self.wav.data_range.clone()];
                    let take = (want - pcm.len()).min(data.len() - *cursor);
                    if take == 0 {
                        self.data_end();
                        true
                    } else {
                        let t = self.trim.trim(take as u32);
                        let start = *cursor + t.skip as usize;
                        let count = t.count as usize;
                        pcm.extend_from_slice(&data[start..start + count]);
                        *cursor = start + count;
                        if t.end {
                            self.data_end();
                        }
                        t.end
                    }
                }
                Kind::Mp3 {
                    frames,
                    pending,
                    taken,
                } => {
                    if *taken < pending.len() {
                        let n = (want - pcm.len()).min(pending.len() - *taken);
                        pcm.extend_from_slice(&pending[*taken..*taken + n]);
                        *taken += n;
                        false
                    } else {
                        let data = &self.bytes[self.wav.data_range.clone()];
                        let at_start = frames.at_start();
                        pending.clear();
                        *taken = 0;
                        let more = match frames.decode_next(data, pending) {
                            Ok(more) => more,
                            Err(e) => {
                                self.exhausted = true;
                                return Err(e);
                            }
                        };
                        if more {
                            if at_start {
                                self.trim.arm(self.trim_samples);
                            }
                            let t = self.trim.trim(pending.len() as u32);
                            let start = t.skip as usize;
                            pending.truncate(start + t.count as usize);
                            *taken = start;
                            if t.end {
                                // Hand out what is left, then start over or end.
                                let tail = pending[start..].to_vec();
                                self.data_end();
                                if let Kind::Mp3 { pending, taken, .. } = &mut self.kind {
                                    *pending = tail;
                                    *taken = 0;
                                }
                                true
                            } else {
                                false
                            }
                        } else {
                            self.data_end();
                            true
                        }
                    }
                }
            };
            if restarted && pcm.len() == before {
                empty_restarts += 1;
                if empty_restarts > 1 {
                    break;
                }
            } else if pcm.len() != before {
                empty_restarts = 0;
            }
        }
        let end = self.exhausted && self.drained();
        Ok(Block { pcm, end })
    }

    /// No decoded bytes wait to be handed out.
    fn drained(&self) -> bool {
        match &self.kind {
            Kind::Pcm { .. } => true,
            Kind::Mp3 { pending, taken, .. } => *taken >= pending.len(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::super::wav::tests::{fmt_block, riff};
    use super::*;

    fn pcm_file(samples: &[u8], fact: Option<u32>) -> Vec<u8> {
        let mut chunks = vec![(b"fmt ", fmt_block(1, 1, 8000, 1, 8))];
        if let Some(f) = fact {
            chunks.push((b"fact", f.to_le_bytes().to_vec()));
        }
        chunks.push((b"data", samples.to_vec()));
        riff(&chunks)
    }

    #[test]
    fn pcm_archetypes_pass_through() {
        let d = decode_archetype(&pcm_file(&[1, 2, 3], None)).unwrap();
        assert_eq!(d.format, fmt8(1, 8, 8000));
        assert_eq!(d.samples, [1, 2, 3]);
        assert_eq!(
            fmt8(2, 16, 70000),
            PcmFormat {
                channels: 2,
                bytes_per_sample: 2,
                rate: 70000u32 as u16,
                block_align: 4
            }
        );
        let mut block = fmt_block(2, 1, 8000, 1, 4);
        block.extend_from_slice(&[0, 0]);
        let adpcm = riff(&[(b"fmt ", block), (b"data", vec![0])]);
        assert_eq!(
            decode_archetype(&adpcm).unwrap_err(),
            DecodeError::UnsupportedFormat(2)
        );
    }

    #[test]
    fn frame_headers() {
        // MPEG-2 Layer III, 32 kbit/s, 22050 Hz, mono, no padding.
        let h = FrameHeader::parse(&[0xFF, 0xF3, 0x40, 0xC0]).unwrap();
        assert_eq!(
            h,
            FrameHeader {
                size: 72_000 * 32 / 22050,
                samples: 576,
                rate: 22050,
                channels: 1
            }
        );
        // MPEG-2.5, 11025 Hz, 16 kbit/s, padded, stereo.
        let h = FrameHeader::parse(&[0xFF, 0xE3, 0x22, 0x00]).unwrap();
        assert_eq!(
            (h.rate, h.channels, h.size),
            (11025, 2, 72_000 * 16 / 11025 + 1)
        );
        // MPEG-1, 128 kbit/s, 44100 Hz.
        let h = FrameHeader::parse(&[0xFF, 0xFB, 0x90, 0x00]).unwrap();
        assert_eq!((h.size, h.samples), (417, 1152));
        // Layer II, free format, bad bitrate, reserved version.
        assert!(FrameHeader::parse(&[0xFF, 0xFD, 0x90, 0x00]).is_none());
        assert!(FrameHeader::parse(&[0xFF, 0xFB, 0x00, 0x00]).is_none());
        assert!(FrameHeader::parse(&[0xFF, 0xFB, 0xF0, 0x00]).is_none());
        assert!(FrameHeader::parse(&[0xFF, 0xEB, 0x90, 0x00]).is_none());
        assert!(FrameHeader::parse(&[0xFF, 0xFB]).is_none());
    }

    #[test]
    fn samples_round_and_clamp() {
        assert_eq!(sample_to_i16(0.0), 0);
        assert_eq!(sample_to_i16(1.0), 32767);
        assert_eq!(sample_to_i16(-1.0), -32768);
        assert_eq!(sample_to_i16(-2.0), -32768);
        assert_eq!(sample_to_i16(0.5 / 32768.0), 1);
        assert_eq!(sample_to_i16(f32::NAN), 0);
    }

    #[test]
    fn pcm_streams_loop_and_end() {
        let file: Arc<[u8]> = pcm_file(&[1, 2, 3, 4, 5], None).into();
        let mut once = StreamSource::open(file.clone(), false).unwrap();
        assert_eq!(once.format().bytes_per_sec, 8000);
        let b = once.read(3).unwrap();
        assert_eq!((b.pcm.as_slice(), b.end), (&[1, 2, 3][..], false));
        let b = once.read(10).unwrap();
        assert_eq!((b.pcm.as_slice(), b.end), (&[4, 5][..], true));
        assert_eq!(
            once.read(10).unwrap(),
            Block {
                pcm: vec![],
                end: true
            }
        );

        let mut looped = StreamSource::open(file, true).unwrap();
        let b = looped.read(12).unwrap();
        assert_eq!(b.pcm, [1, 2, 3, 4, 5, 1, 2, 3, 4, 5, 1, 2]);
        assert!(!b.end);
    }

    #[test]
    fn pcm_streams_stop_at_the_fact_count() {
        let file: Arc<[u8]> = pcm_file(&[1, 2, 3, 4, 5], Some(3)).into();
        let mut looped = StreamSource::open(file.clone(), true).unwrap();
        assert_eq!(looped.read(7).unwrap().pcm, [1, 2, 3, 1, 2, 3, 1]);
        let mut once = StreamSource::open(file, false).unwrap();
        assert_eq!(
            once.read(7).unwrap(),
            Block {
                pcm: vec![1, 2, 3],
                end: true
            }
        );
        // Nothing to play loops no further.
        let empty: Arc<[u8]> = pcm_file(&[], None).into();
        let mut e = StreamSource::open(empty, true).unwrap();
        assert!(e.read(4).unwrap().pcm.is_empty());
    }
}
