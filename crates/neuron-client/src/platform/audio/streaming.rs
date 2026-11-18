//! The streamer on the device: a [`Streamer`] of voice queues behind a
//! lock, refilled by a worker thread every 40 ms (`PumpStreams`), and the
//! client's stream calls (`CreateStream` to `SetPan`) under the same lock.
//!
//! A stream voice owns the blocks it queued until XAudio2 reports them
//! played (the voice's queued-buffer count), so the memory XAudio2 reads
//! stays valid; the voice is declared before its blocks, so destroying a
//! stream destroys the voice first.

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::thread::JoinHandle;
use std::time::Duration;

use windows::Win32::Media::Audio::XAudio2::XAUDIO2_MAX_QUEUED_BUFFERS;
use windows::Win32::System::Threading::{
    GetCurrentThread, SetThreadPriority, THREAD_PRIORITY_HIGHEST,
};

use super::{AudioError, Core, SourceVoice};
use crate::audio::decode::{DecodeError, StreamSource};
use crate::audio::mix::volume_to_amplitude;
use crate::audio::mixer::PcmFormat;
use crate::audio::spatial::pan_matrix;
use crate::audio::stream::{REFILL_PERIOD_MS, StreamError, StreamId, StreamVoice, Streamer};
use crate::audio::voice::QueueLevel;

/// Why a stream cannot be made.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum StreamCreateError {
    /// The file (`"StreamingSound: Error reading WAV file"`).
    Decode(DecodeError),
    /// The output format has no voice (`createsoundbuffer failed`).
    Voice(AudioError),
    /// The buffer length.
    Stream(StreamError),
}

impl std::fmt::Display for StreamCreateError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            StreamCreateError::Decode(e) => write!(f, "{e}"),
            StreamCreateError::Voice(e) => write!(f, "{e}"),
            StreamCreateError::Stream(e) => write!(f, "{e}"),
        }
    }
}

impl std::error::Error for StreamCreateError {}

/// A stream's voice: a source voice and the blocks it may still read.
pub(crate) struct DeviceStreamVoice {
    voice: SourceVoice,
    blocks: VecDeque<Box<[u8]>>,
    level: QueueLevel,
    block_align: u16,
    output_channels: u32,
    channel_mask: u32,
    volume: Option<i32>,
    pan: Option<i32>,
}

impl DeviceStreamVoice {
    /// Drops the blocks XAudio2 has finished with; returns the queued
    /// buffer count and the played-samples counter.
    fn reclaim(&mut self) -> (u32, u64) {
        let (queued, samples) = self.voice.state();
        while self.blocks.len() > queued as usize {
            self.blocks.pop_front();
        }
        (queued, samples)
    }
}

impl StreamVoice for DeviceStreamVoice {
    fn submit(&mut self, pcm: Vec<u8>, _last: bool) -> bool {
        let (queued, _) = self.reclaim();
        if pcm.is_empty() || queued >= XAUDIO2_MAX_QUEUED_BUFFERS {
            return false;
        }
        let block = pcm.into_boxed_slice();
        // SAFETY: the block's heap memory does not move when the box moves
        // into `blocks`, where it stays until the voice reports it played
        // (`reclaim`) or the voice, declared first, is destroyed.
        let ok = unsafe { self.voice.submit(&block, 0, None) }.is_ok();
        if ok {
            self.level.submitted += block.len() as u64;
            self.blocks.push_back(block);
        }
        ok
    }

    fn queued_bytes(&mut self) -> u32 {
        let (_, samples) = self.reclaim();
        let pending: Vec<usize> = self.blocks.iter().map(|b| b.len()).collect();
        self.level.queued(samples, self.block_align, &pending)
    }

    fn play(&mut self) -> bool {
        self.voice.start().is_ok()
    }

    fn stop(&mut self) {
        self.voice.stop();
    }

    fn set_volume(&mut self, volume: i32) {
        if self.volume != Some(volume) {
            self.volume = Some(volume);
            self.voice.set_volume(volume_to_amplitude(volume));
        }
    }

    fn set_pan(&mut self, pan: i32) {
        if self.pan != Some(pan) {
            self.pan = Some(pan);
            let matrix = pan_matrix(
                self.voice.channels,
                self.output_channels,
                self.channel_mask,
                pan,
            );
            self.voice.set_matrix(&matrix);
        }
    }
}

struct Shared {
    streamer: Mutex<Streamer<DeviceStreamVoice>>,
    stop: AtomicBool,
}

impl Shared {
    fn lock(&self) -> MutexGuard<'_, Streamer<DeviceStreamVoice>> {
        // A panic inside a pass leaves the streamer usable.
        self.streamer.lock().unwrap_or_else(PoisonError::into_inner)
    }
}

/// The streams of a device and their worker.
pub struct Streams {
    shared: Arc<Shared>,
    worker: Option<JoinHandle<()>>,
    core: Arc<Core>,
}

impl std::fmt::Debug for Streams {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        let (active, pending) = self.shared.lock().handles();
        f.debug_struct("Streams")
            .field("active", &active)
            .field("pending", &pending)
            .finish_non_exhaustive()
    }
}

/// The worker (`WorkerThreadProc` and `PumpStreams`): a pass, then 40 ms.
fn run(shared: &Shared) {
    // SAFETY: raises this thread's own priority, as the reference does; a
    // failure leaves it as it was.
    let _ = unsafe { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) };
    while !shared.stop.load(Ordering::Acquire) {
        shared.lock().pass();
        std::thread::park_timeout(Duration::from_millis(u64::from(REFILL_PERIOD_MS)));
    }
}

impl Streams {
    pub(crate) fn new(core: &Arc<Core>, seconds: f32) -> Result<Streams, AudioError> {
        let shared = Arc::new(Shared {
            streamer: Mutex::new(Streamer::new(seconds)),
            stop: AtomicBool::new(false),
        });
        let worker_shared = shared.clone();
        let worker = std::thread::Builder::new()
            .name("audio-streamer".into())
            .spawn(move || run(&worker_shared))
            .map_err(|e| AudioError {
                call: "CreateThread",
                code: e.raw_os_error().unwrap_or(-1),
            })?;
        Ok(Streams {
            shared,
            worker: Some(worker),
            core: core.clone(),
        })
    }

    /// `CreateStream` of a WAV file's bytes: looping or not, started at
    /// once or not.
    ///
    /// # Errors
    ///
    /// See [`StreamCreateError`].
    pub fn create(
        &self,
        bytes: Arc<[u8]>,
        looping: bool,
        start: bool,
    ) -> Result<StreamId, StreamCreateError> {
        let source = StreamSource::open(bytes, looping).map_err(StreamCreateError::Decode)?;
        let f = source.format();
        let format = PcmFormat {
            channels: f.channels,
            bytes_per_sample: f.bits_per_sample / 8,
            rate: u16::try_from(f.rate).unwrap_or(u16::MAX),
            block_align: f.block_align,
        };
        if u32::from(format.rate) != f.rate {
            return Err(StreamCreateError::Voice(AudioError {
                call: "CreateSourceVoice",
                code: windows::Win32::Foundation::E_INVALIDARG.0,
            }));
        }
        let voice =
            SourceVoice::new(&self.core, format, false).map_err(StreamCreateError::Voice)?;
        let voice = DeviceStreamVoice {
            voice,
            blocks: VecDeque::new(),
            level: QueueLevel::default(),
            block_align: f.block_align,
            output_channels: self.core.output_channels,
            channel_mask: self.core.channel_mask,
            volume: None,
            pan: None,
        };
        self.shared
            .lock()
            .create(source, voice, start)
            .map_err(StreamCreateError::Stream)
    }

    /// `DestroyStream`; false for an unknown handle.
    pub fn destroy(&self, id: StreamId) -> bool {
        // The voice is destroyed outside the lock.
        let voice = self.shared.lock().destroy(id);
        voice.is_some()
    }

    /// `StartStream`.
    pub fn start(&self, id: StreamId) -> bool {
        self.shared.lock().start(id)
    }

    /// `StopStream`.
    pub fn stop(&self, id: StreamId) -> bool {
        self.shared.lock().stop(id)
    }

    /// `SetVolume`, hundredths of a decibel.
    pub fn set_volume(&self, id: StreamId, volume: i32) -> bool {
        self.shared.lock().set_volume(id, volume)
    }

    /// `SetPan`, hundredths of a decibel.
    pub fn set_pan(&self, id: StreamId, pan: i32) -> bool {
        self.shared.lock().set_pan(id, pan)
    }

    /// `QueryStatus`.
    #[must_use]
    pub fn status(&self, id: StreamId) -> u32 {
        self.shared.lock().status(id)
    }

    /// One refill pass now, as the worker makes every 40 ms.
    pub fn pass(&self) {
        self.shared.lock().pass();
    }
}

impl Drop for Streams {
    /// `ShutdownDevice`: the worker stopped and joined, every stream
    /// stopped and its voice destroyed.
    fn drop(&mut self) {
        self.shared.stop.store(true, Ordering::Release);
        if let Some(worker) = self.worker.take() {
            worker.thread().unpark();
            let _ = worker.join();
        }
        let voices = self.shared.lock().shutdown();
        drop(voices);
    }
}
