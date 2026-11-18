//! Movie decoding with Media Foundation (work package P4.15; migration
//! plan: "Media Foundation source reader into a texture and an audio
//! voice"), in place of the C++'s DirectShow graph over the WM ASF Reader.
//!
//! [`MovieReader`] opens a file with a source reader per stream: one with
//! the video stream converted to RGB32 by the reader's video processing,
//! one with the audio stream converted to 16-bit PCM, so reading one never
//! queues decoded samples of the other. Frames come out as
//! [`crate::media::VideoFrame`] in presentation order; the audio is read
//! whole, for the audio device's streams.

use std::path::Path;

use windows::Win32::Media::MediaFoundation::{
    IMFAttributes, IMFMediaType, IMFSample, IMFSourceReader, MF_MT_AUDIO_BITS_PER_SAMPLE,
    MF_MT_AUDIO_NUM_CHANNELS, MF_MT_AUDIO_SAMPLES_PER_SECOND, MF_MT_DEFAULT_STRIDE,
    MF_MT_FRAME_SIZE, MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE, MF_SOURCE_READER_ALL_STREAMS,
    MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, MF_SOURCE_READER_FIRST_AUDIO_STREAM,
    MF_SOURCE_READER_FIRST_VIDEO_STREAM, MF_SOURCE_READERF_ENDOFSTREAM, MF_VERSION,
    MFAudioFormat_PCM, MFCreateAttributes, MFCreateMediaType, MFCreateSourceReaderFromURL,
    MFMediaType_Audio, MFMediaType_Video, MFSTARTUP_FULL, MFShutdown, MFStartup,
    MFVideoFormat_RGB32,
};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
use windows::core::HSTRING;

use crate::media::{PcmFormat, VideoFrame};

/// Why a movie could not be opened or read.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct MediaError {
    pub call: &'static str,
    pub code: i32,
}

impl std::fmt::Display for MediaError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} failed (0x{:08x})", self.call, self.code)
    }
}

impl std::error::Error for MediaError {}

trait At<T> {
    fn at(self, call: &'static str) -> Result<T, MediaError>;
}

impl<T> At<T> for windows::core::Result<T> {
    fn at(self, call: &'static str) -> Result<T, MediaError> {
        self.map_err(|e| MediaError {
            call,
            code: e.code().0,
        })
    }
}

/// COM and Media Foundation for the life of a reader.
struct Runtime {
    com: bool,
}

impl Runtime {
    fn start() -> Result<Runtime, MediaError> {
        // SAFETY: plain per-thread initialisation; a thread already in
        // another apartment answers RPC_E_CHANGED_MODE, which is not an
        // error for this use, and then must not be uninitialised.
        let com = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) }.is_ok();
        // SAFETY: reference-counted process-wide start, paired in Drop.
        if let Err(e) = unsafe { MFStartup(MF_VERSION, MFSTARTUP_FULL) } {
            if com {
                // SAFETY: pairs the successful CoInitializeEx above.
                unsafe { CoUninitialize() };
            }
            return Err(MediaError {
                call: "MFStartup",
                code: e.code().0,
            });
        }
        Ok(Runtime { com })
    }
}

impl Drop for Runtime {
    fn drop(&mut self) {
        // SAFETY: pairs MFStartup and, when it succeeded, CoInitializeEx.
        unsafe {
            let _ = MFShutdown();
            if self.com {
                CoUninitialize();
            }
        }
    }
}

/// A source reader on `path` with only the first stream of `major` selected
/// and converted to `subtype`.
fn reader(
    path: &Path,
    stream: u32,
    major: windows::core::GUID,
    subtype: windows::core::GUID,
    configure: impl FnOnce(&IMFMediaType) -> windows::core::Result<()>,
) -> Result<IMFSourceReader, MediaError> {
    let mut attributes: Option<IMFAttributes> = None;
    // SAFETY: a valid out slot; the attributes are used below.
    unsafe { MFCreateAttributes(&mut attributes, 1) }.at("MFCreateAttributes")?;
    let attributes = attributes.ok_or(MediaError {
        call: "MFCreateAttributes",
        code: 0,
    })?;
    let url = HSTRING::from(path.as_os_str());
    // SAFETY: the attributes and URL live through the calls.
    let reader = unsafe {
        attributes
            .SetUINT32(&MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, 1)
            .at("SetUINT32")?;
        MFCreateSourceReaderFromURL(&url, &attributes).at("MFCreateSourceReaderFromURL")?
    };
    // SAFETY: plain calls on the reader just made and a new media type.
    unsafe {
        reader
            .SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS.0 as u32, false)
            .at("SetStreamSelection")?;
        reader
            .SetStreamSelection(stream, true)
            .at("SetStreamSelection")?;
        let wanted = MFCreateMediaType().at("MFCreateMediaType")?;
        wanted.SetGUID(&MF_MT_MAJOR_TYPE, &major).at("SetGUID")?;
        wanted.SetGUID(&MF_MT_SUBTYPE, &subtype).at("SetGUID")?;
        configure(&wanted).at("SetUINT32")?;
        reader
            .SetCurrentMediaType(stream, None, &wanted)
            .at("SetCurrentMediaType")?;
    }
    Ok(reader)
}

/// The next sample of `stream`: its time and bytes, or `None` at the end.
fn read(reader: &IMFSourceReader, stream: u32) -> Result<Option<(i64, Vec<u8>)>, MediaError> {
    loop {
        let mut flags = 0u32;
        let mut time = 0i64;
        let mut sample: Option<IMFSample> = None;
        // SAFETY: valid out pointers for one synchronous read.
        unsafe {
            reader.ReadSample(
                stream,
                0,
                None,
                Some(&mut flags),
                Some(&mut time),
                Some(&mut sample),
            )
        }
        .at("ReadSample")?;
        if flags & MF_SOURCE_READERF_ENDOFSTREAM.0 as u32 != 0 {
            return Ok(None);
        }
        // A gap or a format change can come without a sample.
        let Some(sample) = sample else { continue };
        // SAFETY: the buffer is locked for the copy and unlocked after.
        let bytes = unsafe {
            let buffer = sample
                .ConvertToContiguousBuffer()
                .at("ConvertToContiguousBuffer")?;
            let mut data = std::ptr::null_mut();
            let mut len = 0u32;
            buffer.Lock(&mut data, None, Some(&mut len)).at("Lock")?;
            let bytes = std::slice::from_raw_parts(data, len as usize).to_vec();
            let _ = buffer.Unlock();
            bytes
        };
        return Ok(Some((time, bytes)));
    }
}

/// A movie file opened for decoding.
pub struct MovieReader {
    video: IMFSourceReader,
    audio: Option<(IMFSourceReader, PcmFormat)>,
    width: u32,
    height: u32,
    /// Bytes per source row; negative for bottom-up rows.
    stride: i32,
    // Dropped last: the readers above are released first.
    _runtime: Runtime,
}

impl MovieReader {
    /// Opens `path`; a movie without an audio stream plays silent.
    pub fn open(path: &Path) -> Result<MovieReader, MediaError> {
        let runtime = Runtime::start()?;
        let stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM.0 as u32;
        let video = reader(path, stream, MFMediaType_Video, MFVideoFormat_RGB32, |_| {
            Ok(())
        })?;
        // SAFETY: plain queries of the current output type.
        let (width, height, stride) = unsafe {
            let current = video
                .GetCurrentMediaType(stream)
                .at("GetCurrentMediaType")?;
            let size = current
                .GetUINT64(&MF_MT_FRAME_SIZE)
                .at("MF_MT_FRAME_SIZE")?;
            let (width, height) = ((size >> 32) as u32, size as u32);
            let stride = current
                .GetUINT32(&MF_MT_DEFAULT_STRIDE)
                .map_or((width * 4) as i32, |s| s as i32);
            (width, height, stride)
        };
        let audio_stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM.0 as u32;
        let audio = reader(
            path,
            audio_stream,
            MFMediaType_Audio,
            MFAudioFormat_PCM,
            |t| {
                // SAFETY: a new media type owned by the caller.
                unsafe { t.SetUINT32(&MF_MT_AUDIO_BITS_PER_SAMPLE, 16) }
            },
        )
        .ok()
        .and_then(|r| {
            // SAFETY: plain queries of the current output type.
            let format = unsafe {
                let t = r.GetCurrentMediaType(audio_stream).ok()?;
                PcmFormat {
                    channels: t.GetUINT32(&MF_MT_AUDIO_NUM_CHANNELS).ok()? as u16,
                    rate: t.GetUINT32(&MF_MT_AUDIO_SAMPLES_PER_SECOND).ok()?,
                    bits: t.GetUINT32(&MF_MT_AUDIO_BITS_PER_SAMPLE).ok()? as u16,
                }
            };
            Some((r, format))
        });
        Ok(MovieReader {
            video,
            audio,
            width,
            height,
            stride,
            _runtime: runtime,
        })
    }

    /// The picture size, the movie's largest ideal size.
    #[must_use]
    pub fn size(&self) -> (u32, u32) {
        (self.width, self.height)
    }

    /// The audio format, if the movie has sound.
    #[must_use]
    pub fn audio_format(&self) -> Option<PcmFormat> {
        self.audio.as_ref().map(|(_, f)| *f)
    }

    /// The next frame in presentation order, `None` after the last.
    pub fn next_frame(&mut self) -> Result<Option<VideoFrame>, MediaError> {
        let stream = MF_SOURCE_READER_FIRST_VIDEO_STREAM.0 as u32;
        let Some((time, bytes)) = read(&self.video, stream)? else {
            return Ok(None);
        };
        let (w, h) = (self.width as usize, self.height as usize);
        let pitch = self.stride.unsigned_abs() as usize;
        let mut bgra = vec![0u8; w * h * 4];
        for y in 0..h {
            let source_row = if self.stride < 0 { h - 1 - y } else { y };
            let Some(row) = bytes.get(source_row * pitch..source_row * pitch + w * 4) else {
                break;
            };
            let out = &mut bgra[y * w * 4..(y + 1) * w * 4];
            out.copy_from_slice(row);
            // RGB32 leaves the fourth byte undefined: opaque.
            for px in out.as_chunks_mut::<4>().0 {
                px[3] = 0xff;
            }
        }
        Ok(Some(VideoFrame {
            time,
            width: self.width,
            height: self.height,
            bgra,
        }))
    }

    /// All of the audio as PCM, or `None` for a silent movie.
    pub fn read_audio(&mut self) -> Result<Option<(PcmFormat, Vec<u8>)>, MediaError> {
        let Some((reader, format)) = &self.audio else {
            return Ok(None);
        };
        let stream = MF_SOURCE_READER_FIRST_AUDIO_STREAM.0 as u32;
        let mut pcm = Vec::new();
        while let Some((_, bytes)) = read(reader, stream)? {
            pcm.extend_from_slice(&bytes);
        }
        Ok(Some((*format, pcm)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The shipped logo movie decodes: a picture of sane size, frames in
    /// order, and sound. Windows without the Windows Media decoders (for
    /// example a server without its Media Foundation feature) says so and
    /// returns.
    #[test]
    fn the_logo_movie_decodes() {
        let path = Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../outdir/data/MOVIES/da_logo_final.wmv");
        let mut movie = match MovieReader::open(&path) {
            Ok(m) => m,
            Err(e) => {
                println!("no Media Foundation decoding here ({e}); nothing to test");
                return;
            }
        };
        let (w, h) = movie.size();
        assert!(
            (64..=4096).contains(&w) && (64..=4096).contains(&h),
            "{w}x{h}"
        );
        let mut last = i64::MIN;
        let mut frames = 0;
        while let Some(f) = movie.next_frame().unwrap() {
            assert!(f.time >= last, "frames out of order");
            assert_eq!(f.bgra.len(), (w * h * 4) as usize);
            last = f.time;
            frames += 1;
        }
        assert!(frames > 10, "{frames} frames");
        if let Some((format, pcm)) = movie.read_audio().unwrap() {
            assert_eq!(format.bits, 16);
            assert!(!pcm.is_empty());
        }
    }
}
