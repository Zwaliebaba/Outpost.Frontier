//! The RIFF/WAVE walk of the two loaders: `WavLibLoad`, which
//! `CreateArchetypeFromWav` uses (`CppSource/SoundManager/SoundManager.cpp`),
//! and `FSoundStreamer::LoadWav` (`CppSource/SoundStreamer/SoundStreamer.cpp`).
//!
//! Both map the file, check the 12-byte `RIFF`/`WAVE` header and walk the
//! chunks that follow with no even-size padding: the cursor moves past the
//! 8-byte chunk header and then by the chunk size, a negative sum clamped to
//! 0 and a sum past the end clamped to the end. The last chunk of each kind
//! (`fmt `, `fact`, `trim`, `data`) wins; the streamer stops as soon as it
//! has seen `fmt `, `fact` and `data`. The `trim` chunk (encoder delay in
//! samples) and `fact` chunk (sample count) are the game's own MP3-in-WAV
//! convention.
//!
//! The reference reads the format block and the sample data through the
//! chunk headers without checking that they lie inside the mapping. The port
//! checks: a format block that ends past the file is an error, and the
//! sample data is cut at the end of the file (divergence D270).

/// `WAVE_FORMAT_PCM`.
pub const WAVE_FORMAT_PCM: u16 = 1;
/// `WAVE_FORMAT_MPEGLAYER3`, MPEG audio Layer III.
pub const WAVE_FORMAT_MPEGLAYER3: u16 = 0x55;

const RIFF: u32 = 0x4646_4952;
const WAVE: u32 = 0x4556_4157;
const FMT: u32 = 0x2074_6D66;
const FACT: u32 = 0x7463_6166;
const TRIM: u32 = 0x6D69_7274;
const DATA: u32 = 0x6174_6164;

/// Which loader's rules the walk follows.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Loader {
    /// `WavLibLoad`: walks to the end; PCM needs a 14-byte format block,
    /// other formats 18; a `trim` chunk counts only with exactly 4 bytes.
    Archetype,
    /// `LoadWav`: stops once `fmt `, `fact` and `data` were seen; PCM needs
    /// a 16-byte format block, other formats 18.
    Stream,
}

/// The `WAVEFORMATEX` fields both loaders read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct WaveFormat {
    /// `wFormatTag`.
    pub tag: u16,
    /// `nChannels`.
    pub channels: u16,
    /// `nSamplesPerSec`.
    pub rate: u32,
    /// `nAvgBytesPerSec`.
    pub avg_bytes_per_sec: u32,
    /// `nBlockAlign`.
    pub block_align: u16,
    /// `wBitsPerSample`.
    pub bits_per_sample: u16,
}

/// Why a file is not a WAV either loader accepts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WavError {
    /// Shorter than the 12-byte header, or not `RIFF`/`WAVE`.
    NotRiffWave,
    /// No `fmt ` or no `data` chunk.
    MissingChunk,
    /// The `fmt ` chunk is smaller than the loader requires.
    ShortFormat,
    /// The format block lies past the end of the file; the reference reads
    /// outside its mapping (divergence D270).
    FormatOutOfRange,
    /// The chunk walk returned to the start of the file a second time; the
    /// reference loops forever (divergence D270).
    ChunkLoop,
}

impl core::fmt::Display for WavError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(match self {
            WavError::NotRiffWave => "not a RIFF/WAVE file",
            WavError::MissingChunk => "no fmt or data chunk",
            WavError::ShortFormat => "fmt chunk too short",
            WavError::FormatOutOfRange => "fmt chunk past the end of the file",
            WavError::ChunkLoop => "chunk chain loops",
        })
    }
}

impl std::error::Error for WavError {}

/// A parsed WAV file: the format and where the samples are.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct WavFile {
    /// The format block.
    pub format: WaveFormat,
    /// The whole format chunk payload (at most its stated size, cut at the
    /// end of the file).
    pub format_range: core::ops::Range<usize>,
    /// The sample data: the `data` chunk payload, cut at the end of the
    /// file.
    pub data_range: core::ops::Range<usize>,
    /// The `data` chunk's stated size, as the reference uses it.
    pub data_size: u32,
    /// The `fact` sample count (a chunk of at least 4 bytes); a count of
    /// `0xFFFFFFFF`, the reference's "none", reads as `None`.
    pub fact_samples: Option<u32>,
    /// The `trim` sample count (a chunk of exactly 4 bytes).
    pub trim_samples: Option<u32>,
}

fn dword(bytes: &[u8], at: usize) -> Option<u32> {
    let b = bytes.get(at..at.checked_add(4)?)?;
    Some(u32::from_le_bytes([b[0], b[1], b[2], b[3]]))
}

fn word(bytes: &[u8], at: usize) -> Option<u16> {
    let b = bytes.get(at..at.checked_add(2)?)?;
    Some(u16::from_le_bytes([b[0], b[1]]))
}

/// One chunk header: where it starts and its stated size.
#[derive(Clone, Copy)]
struct Chunk {
    at: usize,
    size: u32,
}

impl WavFile {
    /// Walks `bytes` with `loader`'s rules.
    ///
    /// # Errors
    ///
    /// See [`WavError`].
    pub fn parse(bytes: &[u8], loader: Loader) -> Result<WavFile, WavError> {
        let len = bytes.len();
        if dword(bytes, 0) != Some(RIFF) || dword(bytes, 8) != Some(WAVE) {
            return Err(WavError::NotRiffWave);
        }
        // The cursor is a 32-bit value in the reference; mapped sizes fit.
        let size = u32::try_from(len).unwrap_or(u32::MAX);
        let mut cursor: u32 = 12;
        let (mut fmt, mut fact, mut trim, mut data) = (None, None, None, None);
        let mut rewound = false;
        while let Some(id) = (cursor as usize)
            .checked_add(8)
            .filter(|end| *end <= len)
            .and_then(|_| dword(bytes, cursor as usize))
        {
            let at = cursor as usize;
            let chunk_size = dword(bytes, at + 4).unwrap_or(0);
            cursor += 8;
            let chunk = Some(Chunk {
                at,
                size: chunk_size,
            });
            match id {
                FACT => fact = chunk,
                TRIM => trim = chunk,
                FMT => fmt = chunk,
                DATA => data = chunk,
                _ => {}
            }
            // `long Next = Size + Cursor`: negative clamps to 0, past the
            // mapping to its size (a signed comparison).
            let next = chunk_size.wrapping_add(cursor).cast_signed();
            cursor = if next < 0 {
                if rewound {
                    return Err(WavError::ChunkLoop);
                }
                rewound = true;
                0
            } else if next > size.cast_signed() {
                size
            } else {
                next.cast_unsigned()
            };
            if loader == Loader::Stream && fmt.is_some() && fact.is_some() && data.is_some() {
                break;
            }
        }
        let (Some(fmt), Some(data)) = (fmt, data) else {
            return Err(WavError::MissingChunk);
        };
        let at = fmt.at + 8;
        let field = |offset: usize| word(bytes, at + offset).ok_or(WavError::FormatOutOfRange);
        let tag = field(0)?;
        let minimum = match (loader, tag) {
            (Loader::Archetype, WAVE_FORMAT_PCM) => 0xE,
            (Loader::Stream, WAVE_FORMAT_PCM) => 0x10,
            _ => 0x12,
        };
        if fmt.size < minimum {
            return Err(WavError::ShortFormat);
        }
        let long = |offset: usize| dword(bytes, at + offset).ok_or(WavError::FormatOutOfRange);
        let format = WaveFormat {
            tag,
            channels: field(2)?,
            rate: long(4)?,
            avg_bytes_per_sec: long(8)?,
            block_align: field(12)?,
            // WavLib reads the bits of a 14-byte PCM block past its end;
            // the port needs them inside the file.
            bits_per_sample: field(14)?,
        };
        let payload = |chunk: Chunk| {
            let start = (chunk.at + 8).min(len);
            let end = start.saturating_add(chunk.size as usize).min(len);
            start..end
        };
        let fact_samples = fact
            .filter(|c| c.size >= 4)
            .and_then(|c| dword(bytes, c.at + 8))
            .filter(|n| *n != u32::MAX);
        let trim_samples = trim
            .filter(|c| c.size == 4)
            .and_then(|c| dword(bytes, c.at + 8));
        Ok(WavFile {
            format,
            format_range: payload(fmt),
            data_range: payload(data),
            data_size: data.size,
            fact_samples,
            trim_samples,
        })
    }
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    /// A RIFF/WAVE file from (id, payload) chunks, sizes as given.
    pub(crate) fn riff(chunks: &[(&[u8; 4], Vec<u8>)]) -> Vec<u8> {
        let mut body = b"WAVE".to_vec();
        for (id, payload) in chunks {
            body.extend_from_slice(*id);
            body.extend_from_slice(&(payload.len() as u32).to_le_bytes());
            body.extend_from_slice(payload);
        }
        let mut file = b"RIFF".to_vec();
        file.extend_from_slice(&(body.len() as u32).to_le_bytes());
        file.extend_from_slice(&body);
        file
    }

    pub(crate) fn fmt_block(tag: u16, channels: u16, rate: u32, block: u16, bits: u16) -> Vec<u8> {
        let mut v = Vec::new();
        v.extend_from_slice(&tag.to_le_bytes());
        v.extend_from_slice(&channels.to_le_bytes());
        v.extend_from_slice(&rate.to_le_bytes());
        v.extend_from_slice(&(rate * u32::from(block)).to_le_bytes());
        v.extend_from_slice(&block.to_le_bytes());
        v.extend_from_slice(&bits.to_le_bytes());
        v
    }

    #[test]
    fn pcm_chunks() {
        let file = riff(&[
            (b"fmt ", fmt_block(1, 1, 22050, 2, 16)),
            (b"LIST", vec![0; 6]),
            (b"data", vec![1, 2, 3, 4]),
        ]);
        let wav = WavFile::parse(&file, Loader::Archetype).unwrap();
        assert_eq!(wav.format.rate, 22050);
        assert_eq!(&file[wav.data_range.clone()], &[1, 2, 3, 4]);
        assert_eq!((wav.fact_samples, wav.trim_samples), (None, None));
    }

    #[test]
    fn last_chunk_wins_and_the_streamer_stops_early() {
        let file = riff(&[
            (b"fmt ", fmt_block(1, 1, 8000, 1, 8)),
            (b"fact", 7u32.to_le_bytes().to_vec()),
            (b"data", vec![9]),
            (b"data", vec![5, 6]),
            (b"trim", 3u32.to_le_bytes().to_vec()),
        ]);
        let a = WavFile::parse(&file, Loader::Archetype).unwrap();
        assert_eq!(&file[a.data_range.clone()], &[5, 6]);
        assert_eq!((a.fact_samples, a.trim_samples), (Some(7), Some(3)));
        let s = WavFile::parse(&file, Loader::Stream).unwrap();
        assert_eq!(&file[s.data_range.clone()], &[9]);
        assert_eq!(s.trim_samples, None);
    }

    #[test]
    fn format_sizes_and_errors() {
        let short = fmt_block(1, 1, 8000, 1, 8);
        // A 14-byte PCM block: WavLib takes it (and reads the bits from the
        // next chunk's header), the streamer does not.
        let file = riff(&[(b"fmt ", short[..14].to_vec()), (b"data", vec![0; 4])]);
        let wav = WavFile::parse(&file, Loader::Archetype).unwrap();
        assert_eq!(wav.format.bits_per_sample, u16::from_le_bytes(*b"da"));
        assert_eq!(
            WavFile::parse(&file, Loader::Stream).unwrap_err(),
            WavError::ShortFormat
        );
        assert_eq!(
            WavFile::parse(b"RIFX\0\0\0\0WAVE", Loader::Archetype).unwrap_err(),
            WavError::NotRiffWave
        );
        let mp3 = riff(&[
            (b"fmt ", fmt_block(0x55, 1, 22050, 1, 0)),
            (b"data", vec![]),
        ]);
        assert_eq!(
            WavFile::parse(&mp3, Loader::Archetype).unwrap_err(),
            WavError::ShortFormat
        );
    }

    #[test]
    fn data_is_cut_at_the_end_of_the_file() {
        let mut file = riff(&[
            (b"fmt ", fmt_block(1, 1, 8000, 1, 8)),
            (b"data", vec![1, 2]),
        ]);
        let at = file.len() - 6;
        file[at..at + 4].copy_from_slice(&100u32.to_le_bytes());
        let wav = WavFile::parse(&file, Loader::Archetype).unwrap();
        assert_eq!(wav.data_size, 100);
        assert_eq!(&file[wav.data_range], &[1, 2]);
    }

    #[test]
    fn a_rewinding_chain_is_an_error() {
        // A chunk whose size sends the signed cursor negative rewinds to 0,
        // where the RIFF header reads as a chunk of the same kind.
        let mut file = riff(&[(b"junk", vec![0; 4])]);
        file[16..20].copy_from_slice(&0x8000_0000u32.to_le_bytes());
        file[4..8].copy_from_slice(&0x8000_0000u32.to_le_bytes());
        assert_eq!(
            WavFile::parse(&file, Loader::Archetype).unwrap_err(),
            WavError::ChunkLoop
        );
    }
}
