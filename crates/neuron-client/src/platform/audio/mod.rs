//! The XAudio2 device (work package P4.10, decisions T4 and O26): the
//! engine on the Windows default output device and its speaker layout,
//! source voices for the mixer's [`VoiceBackend`], X3DAudio for voices of
//! 3D archetypes, the reverb submix, and the streamer's worker over voice
//! queues.
//!
//! Lifetimes. Everything the engine owns lives in one `Core` behind an
//! [`Arc`]; every source voice holds a clone, so the mastering voice, the
//! submix and the engine are destroyed only after the last source voice.
//! A source voice is destroyed (`DestroyVoice`, which waits until the
//! audio thread no longer reads it) before the sample memory it plays is
//! released: the owning structures declare the voice before the samples.
//! [`AudioDevice`] drops the streamer's worker first, then the archetype
//! voices, then the core, then its COM initialisation.
//!
//! Threads. XAudio2 runs its own audio thread, and its methods may be
//! called from any thread outside its callbacks; the port registers no
//! callbacks and polls voice state instead. The mixer's calls come from
//! the client's thread; the streamer's refill pass runs on a worker every
//! 40 ms under the streamer's lock, which the client's stream calls take
//! too (the reference's critical section).
//!
//! The XAudio2 library is loaded at run time (`xaudio2_9.dll`, else
//! `xaudio2_8.dll`), so a machine without it fails [`AudioDevice::new`]
//! instead of failing to start; X3DAudio and the reverb come from the same
//! library. A missing output device fails `CreateMasteringVoice`
//! ([`AudioError::is_missing_device`]).

mod streaming;
mod voices;
mod x3daudio;

pub use streaming::{StreamCreateError, Streams};

use core::ffi::c_void;
use core::marker::PhantomData;
use core::mem::ManuallyDrop;
use std::fmt;
use std::sync::{Arc, OnceLock};

use windows::Win32::Foundation::E_POINTER;
use windows::Win32::Media::Audio::XAudio2::{
    IXAudio2, IXAudio2MasteringVoice, IXAudio2SourceVoice, IXAudio2SubmixVoice, IXAudio2Voice,
    XAUDIO2_BUFFER, XAUDIO2_COMMIT_NOW, XAUDIO2_DEFAULT_CHANNELS, XAUDIO2_DEFAULT_PROCESSOR,
    XAUDIO2_DEFAULT_SAMPLERATE, XAUDIO2_EFFECT_CHAIN, XAUDIO2_EFFECT_DESCRIPTOR,
    XAUDIO2_LOOP_INFINITE, XAUDIO2_MAX_AUDIO_CHANNELS, XAUDIO2_MAX_FREQ_RATIO,
    XAUDIO2_MAX_SAMPLE_RATE, XAUDIO2_MIN_SAMPLE_RATE, XAUDIO2_SEND_DESCRIPTOR, XAUDIO2_VOICE_SENDS,
    XAUDIO2_VOICE_STATE, XAUDIO2FX_REVERB_PARAMETERS,
};
use windows::Win32::Media::Audio::{AudioCategory_GameEffects, WAVE_FORMAT_PCM, WAVEFORMATEX};
use windows::Win32::System::Com::{COINIT_MULTITHREADED, CoInitializeEx, CoUninitialize};
use windows::Win32::System::LibraryLoader::{GetProcAddress, LoadLibraryW};
use windows::core::{HRESULT, IUnknown, Interface, PCSTR, PCWSTR, s, w};

use crate::audio::listener::Listener;
use crate::audio::mix::FREQUENCY_MAX;
use crate::audio::mixer::{Archetype, ArchetypeId, Emitter, PcmFormat, VoiceBackend, VoiceId};
use crate::audio::reverb::{I3dl2Reverb, NativeReverb, WET_DRY_MIX, native_reverb};
use crate::audio::spatial::SPEED_OF_SOUND;
use x3daudio::X3dAudio;

/// A failed XAudio2 call.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AudioError {
    /// The call.
    pub call: &'static str,
    /// Its `HRESULT`.
    pub code: i32,
}

impl AudioError {
    /// The mastering voice could not be made: no audio output device (as
    /// on a build machine without an audio endpoint).
    #[must_use]
    pub fn is_missing_device(&self) -> bool {
        self.call == "CreateMasteringVoice"
    }

    /// The XAudio2 library is not on this machine.
    #[must_use]
    pub fn is_missing_library(&self) -> bool {
        self.call == "LoadLibraryW"
    }
}

impl fmt::Display for AudioError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{} failed with 0x{:08x}", self.call, self.code)
    }
}

impl std::error::Error for AudioError {}

trait Context<T> {
    fn at(self, call: &'static str) -> Result<T, AudioError>;
}

impl<T> Context<T> for windows::core::Result<T> {
    fn at(self, call: &'static str) -> Result<T, AudioError> {
        self.map_err(|e| AudioError {
            call,
            code: e.code().0,
        })
    }
}

impl Context<()> for HRESULT {
    fn at(self, call: &'static str) -> Result<(), AudioError> {
        self.ok().at(call)
    }
}

/// `NTDDI_WIN10`, the version `XAudio2Create` passes on Windows 10.
const NTDDI_WIN10: u32 = 0x0A00_0000;

type CreateWithVersionFn = unsafe extern "system" fn(*mut *mut c_void, u32, u32, u32) -> HRESULT;
type CreateFn = unsafe extern "system" fn(*mut *mut c_void, u32, u32) -> HRESULT;
type CreateReverbFn = unsafe extern "system" fn(*mut *mut c_void) -> HRESULT;
type ExportFn = unsafe extern "system" fn() -> isize;

#[derive(Clone, Copy)]
enum Create {
    /// `XAudio2CreateWithVersionInfo` (XAudio 2.9).
    WithVersion(CreateWithVersionFn),
    /// `XAudio2Create` (XAudio 2.8).
    Plain(CreateFn),
}

/// The exports of the XAudio2 library.
#[derive(Clone, Copy)]
struct Library {
    create: Create,
    create_reverb: Option<CreateReverbFn>,
    x3d_initialize: Option<x3daudio::InitializeFn>,
    x3d_calculate: Option<x3daudio::CalculateFn>,
}

/// The library, loaded once and kept for the process (as an import
/// would be).
fn library() -> Result<Library, AudioError> {
    static LIBRARY: OnceLock<Result<Library, AudioError>> = OnceLock::new();
    LIBRARY.get_or_init(load_library).clone()
}

fn load_library() -> Result<Library, AudioError> {
    let mut code = 0;
    for (dll, version) in [(w!("xaudio2_9.dll"), true), (w!("xaudio2_8.dll"), false)] {
        // SAFETY: loads a system library by name; it is never freed, so the
        // function pointers taken from it stay valid for the process.
        let module = match unsafe { LoadLibraryW(dll) } {
            Ok(module) => module,
            Err(e) => {
                code = e.code().0;
                continue;
            }
        };
        let export = |name: PCSTR| -> Option<ExportFn> {
            // SAFETY: a lookup in the module just loaded.
            unsafe { GetProcAddress(module, name) }
        };
        let create = if version {
            export(s!("XAudio2CreateWithVersionInfo")).map(|f| {
                // SAFETY: xaudio2.h declares XAudio2CreateWithVersionInfo
                // with this signature.
                Create::WithVersion(unsafe {
                    core::mem::transmute::<ExportFn, CreateWithVersionFn>(f)
                })
            })
        } else {
            export(s!("XAudio2Create")).map(|f| {
                // SAFETY: XAudio 2.8 exports XAudio2Create with this signature.
                Create::Plain(unsafe { core::mem::transmute::<ExportFn, CreateFn>(f) })
            })
        };
        let Some(create) = create else {
            continue;
        };
        let create_reverb = export(s!("CreateAudioReverb")).map(|f| {
            // SAFETY: xaudio2fx.h declares CreateAudioReverb with this signature.
            unsafe { core::mem::transmute::<ExportFn, CreateReverbFn>(f) }
        });
        let x3d_initialize = export(s!("X3DAudioInitialize")).map(|f| {
            // SAFETY: x3daudio.h declares X3DAudioInitialize with this signature.
            unsafe { core::mem::transmute::<ExportFn, x3daudio::InitializeFn>(f) }
        });
        let x3d_calculate = export(s!("X3DAudioCalculate")).map(|f| {
            // SAFETY: x3daudio.h declares X3DAudioCalculate with this signature.
            unsafe { core::mem::transmute::<ExportFn, x3daudio::CalculateFn>(f) }
        });
        return Ok(Library {
            create,
            create_reverb,
            x3d_initialize,
            x3d_calculate,
        });
    }
    Err(AudioError {
        call: "LoadLibraryW",
        code,
    })
}

/// COM initialised on this thread for the device's lifetime (XAudio2
/// needs it). Not `Send`: it is undone on the thread that did it.
struct ComGuard {
    uninitialize: bool,
    _thread: PhantomData<*const ()>,
}

impl ComGuard {
    fn new() -> ComGuard {
        // SAFETY: plain initialisation; S_OK and S_FALSE are balanced by
        // CoUninitialize in Drop, a thread already in another apartment
        // (RPC_E_CHANGED_MODE) keeps it and is not uninitialised.
        let hr = unsafe { CoInitializeEx(None, COINIT_MULTITHREADED) };
        ComGuard {
            uninitialize: hr.is_ok(),
            _thread: PhantomData,
        }
    }
}

impl Drop for ComGuard {
    fn drop(&mut self) {
        if self.uninitialize {
            // SAFETY: balances the successful CoInitializeEx of `new` on the
            // same thread (the guard is not Send).
            unsafe { CoUninitialize() };
        }
    }
}

/// The engine, the mastering voice and the reverb submix.
pub(crate) struct Core {
    xaudio2: IXAudio2,
    master: IXAudio2MasteringVoice,
    reverb: Option<IXAudio2SubmixVoice>,
    channel_mask: u32,
    output_channels: u32,
    sample_rate: u32,
    x3d: Option<X3dAudio>,
}

// SAFETY: XAudio2's engine and voice methods may be called from any thread
// (they take the engine's own locks); the port never calls them from an
// XAudio2 callback. The raw voice pointers are only destroyed in Drop, after
// every source voice (which holds an Arc of the core) is gone.
unsafe impl Send for Core {}
// SAFETY: as above; shared use only calls XAudio2 methods.
unsafe impl Sync for Core {}

impl Drop for Core {
    fn drop(&mut self) {
        // SAFETY: no source voice is left (each holds an Arc of the core), so
        // nothing sends to the submix or the mastering voice any more.
        unsafe {
            if let Some(reverb) = self.reverb.take() {
                reverb.DestroyVoice();
            }
            self.master.DestroyVoice();
            self.xaudio2.StopEngine();
        }
    }
}

impl Core {
    fn new() -> Result<Core, AudioError> {
        let library = library()?;
        let mut raw = core::ptr::null_mut();
        // SAFETY: the out pointer is valid; the function comes from the library.
        let hr = unsafe {
            match library.create {
                Create::WithVersion(f) => {
                    f(&raw mut raw, 0, XAUDIO2_DEFAULT_PROCESSOR, NTDDI_WIN10)
                }
                Create::Plain(f) => f(&raw mut raw, 0, XAUDIO2_DEFAULT_PROCESSOR),
            }
        };
        hr.at("XAudio2Create")?;
        if raw.is_null() {
            return Err(AudioError {
                call: "XAudio2Create",
                code: E_POINTER.0,
            });
        }
        // SAFETY: a successful create returns one reference, which this takes.
        let xaudio2 = unsafe { IXAudio2::from_raw(raw) };
        let mut master = None;
        // SAFETY: the default device (no id) with its own channel count and
        // rate; the out slot is valid.
        unsafe {
            xaudio2.CreateMasteringVoice(
                &raw mut master,
                XAUDIO2_DEFAULT_CHANNELS,
                XAUDIO2_DEFAULT_SAMPLERATE,
                0,
                PCWSTR::null(),
                None,
                AudioCategory_GameEffects,
            )
        }
        .at("CreateMasteringVoice")?;
        let master = master.ok_or(AudioError {
            call: "CreateMasteringVoice",
            code: E_POINTER.0,
        })?;
        // SAFETY: plain queries on the voice just made.
        let details = unsafe { master.GetVoiceDetails() };
        // SAFETY: as above.
        let channel_mask = unsafe { master.GetChannelMask() }.unwrap_or(0);
        let mut core = Core {
            xaudio2,
            master,
            reverb: None,
            channel_mask,
            output_channels: details.InputChannels.max(1),
            sample_rate: details.InputSampleRate,
            x3d: None,
        };
        core.x3d = library.x3d_initialize.zip(library.x3d_calculate).and_then(
            |(initialize, calculate)| {
                X3dAudio::new(
                    initialize,
                    calculate,
                    channel_mask,
                    core.output_channels,
                    SPEED_OF_SOUND,
                )
            },
        );
        core.reverb = library.create_reverb.and_then(|f| core.reverb_submix(f));
        Ok(core)
    }

    /// The reverb submix: one channel in, the built-in reverb on it with
    /// one output channel, into the mastering voice. None when the effect
    /// or the voice cannot be made (no reverb then).
    fn reverb_submix(&self, create: CreateReverbFn) -> Option<IXAudio2SubmixVoice> {
        let mut raw = core::ptr::null_mut();
        // SAFETY: the out pointer is valid.
        unsafe { create(&raw mut raw) }.ok().ok()?;
        if raw.is_null() {
            return None;
        }
        // SAFETY: the effect comes with one reference, which this takes.
        let effect = unsafe { IUnknown::from_raw(raw) };
        let mut descriptor = XAUDIO2_EFFECT_DESCRIPTOR {
            pEffect: ManuallyDrop::new(Some(effect)),
            InitialState: true.into(),
            OutputChannels: 1,
        };
        let chain = XAUDIO2_EFFECT_CHAIN {
            EffectCount: 1,
            pEffectDescriptors: &raw mut descriptor,
        };
        // The reverb runs at 20 to 48 kHz.
        let rate = self.sample_rate.clamp(20_000, 48_000);
        let mut submix = None;
        // SAFETY: the chain and its descriptor outlive the call, which takes
        // its own reference to the effect.
        let created = unsafe {
            self.xaudio2.CreateSubmixVoice(
                &raw mut submix,
                1,
                rate,
                0,
                0,
                None,
                Some(&raw const chain),
            )
        };
        drop(ManuallyDrop::into_inner(descriptor.pEffect));
        created.ok()?;
        submix
    }

    /// The mastering voice as a send target.
    fn master_voice(&self) -> IXAudio2Voice {
        (*self.master).clone()
    }
}

/// The format XAudio2 is given for an archetype's PCM.
fn wave_format(format: PcmFormat) -> Option<WAVEFORMATEX> {
    let channels = u32::from(format.channels);
    let rate = u32::from(format.rate);
    let bits = format.bytes_per_sample.checked_mul(8)?;
    let valid = (1..=XAUDIO2_MAX_AUDIO_CHANNELS).contains(&channels)
        && (XAUDIO2_MIN_SAMPLE_RATE..=XAUDIO2_MAX_SAMPLE_RATE).contains(&rate)
        && matches!(bits, 8 | 16 | 24 | 32)
        && format.block_align == format.bytes_per_sample * format.channels;
    valid.then(|| WAVEFORMATEX {
        wFormatTag: WAVE_FORMAT_PCM as u16,
        nChannels: format.channels,
        nSamplesPerSec: rate,
        nAvgBytesPerSec: rate * u32::from(format.block_align),
        nBlockAlign: format.block_align,
        wBitsPerSample: bits,
        cbSize: 0,
    })
}

/// The highest frequency ratio a voice of `rate` hertz needs: the mixer's
/// frequency limit, twice over for Doppler, within XAudio2's limits.
fn max_ratio(rate: u32) -> f32 {
    let ratio = 2.0 * f64::from(FREQUENCY_MAX) / f64::from(rate.max(1));
    (ratio as f32).clamp(2.0, XAUDIO2_MAX_FREQ_RATIO)
}

/// A source voice. Dropping it destroys the voice (which waits until the
/// audio thread no longer reads its buffers).
pub(crate) struct SourceVoice {
    voice: IXAudio2SourceVoice,
    channels: u32,
    core: Arc<Core>,
}

// SAFETY: see Core; the voice's methods may be called from any thread.
unsafe impl Send for SourceVoice {}

impl Drop for SourceVoice {
    fn drop(&mut self) {
        // SAFETY: the voice was made by CreateSourceVoice and is destroyed
        // once; the core it sends to outlives it (the Arc).
        unsafe { self.voice.DestroyVoice() };
    }
}

impl SourceVoice {
    /// A stopped voice of `format`, sending to the mastering voice and, when
    /// `reverb` and the device has one, to the reverb submix.
    fn new(core: &Arc<Core>, format: PcmFormat, reverb: bool) -> Result<SourceVoice, AudioError> {
        let invalid = AudioError {
            call: "CreateSourceVoice",
            code: windows::Win32::Foundation::E_INVALIDARG.0,
        };
        let wave = wave_format(format).ok_or(invalid)?;
        let mut sends = vec![XAUDIO2_SEND_DESCRIPTOR {
            Flags: 0,
            pOutputVoice: ManuallyDrop::new(Some(core.master_voice())),
        }];
        if reverb && let Some(submix) = &core.reverb {
            sends.push(XAUDIO2_SEND_DESCRIPTOR {
                Flags: 0,
                pOutputVoice: ManuallyDrop::new(Some((**submix).clone())),
            });
        }
        let send_list = XAUDIO2_VOICE_SENDS {
            SendCount: sends.len() as u32,
            pSends: sends.as_mut_ptr(),
        };
        let mut voice = None;
        // SAFETY: the format and the send list outlive the call; no callback.
        unsafe {
            core.xaudio2.CreateSourceVoice(
                &raw mut voice,
                &raw const wave,
                0,
                max_ratio(wave.nSamplesPerSec),
                None,
                Some(&raw const send_list),
                None,
            )
        }
        .at("CreateSourceVoice")?;
        let voice = voice.ok_or(AudioError {
            call: "CreateSourceVoice",
            code: E_POINTER.0,
        })?;
        Ok(SourceVoice {
            voice,
            channels: u32::from(format.channels),
            core: core.clone(),
        })
    }

    /// Queues `data` (whole frames of the voice's format), from frame
    /// `play_begin`, looping `loop_region` (begin, length in frames; length
    /// 0 to the end) forever when given.
    ///
    /// # Safety
    ///
    /// `data` must stay valid and unchanged until the voice no longer holds
    /// the buffer: it has played it, it was flushed and the voice reports
    /// no queued buffer, or the voice was dropped.
    unsafe fn submit(
        &self,
        data: &[u8],
        play_begin: u32,
        loop_region: Option<(u32, u32)>,
    ) -> Result<(), AudioError> {
        let (loop_begin, loop_length, loop_count) = match loop_region {
            Some((begin, length)) => (begin, length, XAUDIO2_LOOP_INFINITE),
            None => (0, 0, 0),
        };
        let buffer = XAUDIO2_BUFFER {
            Flags: 0,
            AudioBytes: u32::try_from(data.len()).unwrap_or(u32::MAX),
            pAudioData: data.as_ptr(),
            PlayBegin: play_begin,
            PlayLength: 0,
            LoopBegin: loop_begin,
            LoopLength: loop_length,
            LoopCount: loop_count,
            pContext: core::ptr::null_mut(),
        };
        // SAFETY: the buffer description is copied by the call; the caller
        // keeps the data alive as documented.
        unsafe { self.voice.SubmitSourceBuffer(&raw const buffer, None) }.at("SubmitSourceBuffer")
    }

    fn start(&self) -> Result<(), AudioError> {
        // SAFETY: a plain call on a live voice.
        unsafe { self.voice.Start(0, XAUDIO2_COMMIT_NOW) }.at("Start")
    }

    fn stop(&self) {
        // SAFETY: as above. A failure leaves the voice as it was.
        let _ = unsafe { self.voice.Stop(0, XAUDIO2_COMMIT_NOW) };
    }

    fn flush(&self) {
        // SAFETY: as above.
        let _ = unsafe { self.voice.FlushSourceBuffers() };
    }

    /// Buffers queued (the playing one included) and samples played.
    fn state(&self) -> (u32, u64) {
        let mut state = XAUDIO2_VOICE_STATE::default();
        // SAFETY: the out structure is valid.
        unsafe { self.voice.GetState(&raw mut state, 0) };
        (state.BuffersQueued, state.SamplesPlayed)
    }

    fn set_volume(&self, amplitude: f32) {
        // SAFETY: a plain call on a live voice.
        let _ = unsafe { self.voice.SetVolume(amplitude, XAUDIO2_COMMIT_NOW) };
    }

    fn set_ratio(&self, ratio: f32) {
        // SAFETY: as above; the ratio is within the voice's maximum.
        let _ = unsafe { self.voice.SetFrequencyRatio(ratio, XAUDIO2_COMMIT_NOW) };
    }

    /// The matrix to the mastering voice (`destination * sources + source`).
    fn set_matrix(&self, matrix: &[f32]) {
        let outputs = self.core.output_channels;
        if matrix.len() != (self.channels * outputs) as usize {
            return;
        }
        let master = self.core.master_voice();
        // SAFETY: the matrix has sources * destinations entries.
        let _ = unsafe {
            self.voice.SetOutputMatrix(
                &master,
                self.channels,
                outputs,
                matrix.as_ptr(),
                XAUDIO2_COMMIT_NOW,
            )
        };
    }
}

/// The audio device: the engine, the archetype voices the mixer drives and
/// the streams.
pub struct AudioDevice {
    streams: Streams,
    voices: voices::ArchetypeVoices,
    core: Arc<Core>,
    _com: ComGuard,
}

impl fmt::Debug for AudioDevice {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("AudioDevice")
            .field("output_channels", &self.core.output_channels)
            .field("channel_mask", &self.core.channel_mask)
            .field("sample_rate", &self.core.sample_rate)
            .finish_non_exhaustive()
    }
}

impl AudioDevice {
    /// The engine on the default output device, its reverb off, and the
    /// streamer's worker with streams of `stream_seconds`
    /// ([`crate::audio::config::stream_buffer_seconds`]).
    ///
    /// # Errors
    ///
    /// No XAudio2 library, no output device
    /// ([`AudioError::is_missing_device`]), or no worker thread.
    pub fn new(stream_seconds: f32) -> Result<AudioDevice, AudioError> {
        let com = ComGuard::new();
        let core = Arc::new(Core::new()?);
        let streams = Streams::new(&core, stream_seconds)?;
        let device = AudioDevice {
            streams,
            voices: voices::ArchetypeVoices::new(&core),
            core,
            _com: com,
        };
        device.set_reverb(None)?;
        Ok(device)
    }

    /// Output channels of the mastering voice.
    #[must_use]
    pub fn output_channels(&self) -> u32 {
        self.core.output_channels
    }

    /// The speaker layout of the output (`SPEAKER_*` bits).
    #[must_use]
    pub fn channel_mask(&self) -> u32 {
        self.core.channel_mask
    }

    /// The output sample rate.
    #[must_use]
    pub fn sample_rate(&self) -> u32 {
        self.core.sample_rate
    }

    /// Whether X3DAudio spatialises 3D voices (else they play centred with
    /// the distance curve's gain).
    #[must_use]
    pub fn has_x3daudio(&self) -> bool {
        self.core.x3d.is_some()
    }

    /// Whether the reverb submix exists.
    #[must_use]
    pub fn has_reverb(&self) -> bool {
        self.core.reverb.is_some()
    }

    /// The environmental reverb (`audio::reverb::reverb_parameters` of the
    /// EAX settings), or none. Without a reverb submix this does nothing.
    ///
    /// # Errors
    ///
    /// XAudio2 refused the parameters or the effect state.
    pub fn set_reverb(&self, reverb: Option<&I3dl2Reverb>) -> Result<(), AudioError> {
        let Some(submix) = &self.core.reverb else {
            return Ok(());
        };
        let Some(reverb) = reverb else {
            // SAFETY: effect 0 exists on the submix.
            return unsafe { submix.DisableEffect(0, XAUDIO2_COMMIT_NOW) }.at("DisableEffect");
        };
        let native = raw_reverb(&native_reverb(reverb, WET_DRY_MIX, true));
        // SAFETY: the parameters are the reverb's own structure, of its size.
        unsafe {
            submix.SetEffectParameters(
                0,
                (&raw const native).cast(),
                core::mem::size_of::<XAUDIO2FX_REVERB_PARAMETERS>() as u32,
                XAUDIO2_COMMIT_NOW,
            )
        }
        .at("SetEffectParameters")?;
        // SAFETY: effect 0 exists on the submix.
        unsafe { submix.EnableEffect(0, XAUDIO2_COMMIT_NOW) }.at("EnableEffect")
    }

    /// Hands the device the samples of an archetype (the bytes the
    /// archetype was made from), which its voices play.
    pub fn set_samples(&mut self, archetype: ArchetypeId, samples: Arc<[u8]>) {
        self.voices.set_samples(archetype, samples);
    }

    /// Forgets an archetype's samples; voices still playing them keep
    /// them until released.
    pub fn remove_samples(&mut self, archetype: ArchetypeId) {
        self.voices.remove_samples(archetype);
    }

    /// Voices created and not released.
    #[must_use]
    pub fn live_voices(&self) -> usize {
        self.voices.live()
    }

    /// The streams.
    #[must_use]
    pub fn streams(&self) -> &Streams {
        &self.streams
    }

    /// The streams, for the client's streamed sources.
    pub fn streams_mut(&mut self) -> &mut Streams {
        &mut self.streams
    }
}

impl VoiceBackend for AudioDevice {
    fn create(&mut self, voice: VoiceId, archetype: ArchetypeId, data: &Archetype) -> bool {
        self.voices.create(voice, archetype, data)
    }
    fn release(&mut self, voice: VoiceId) {
        self.voices.release(voice);
    }
    fn set_pan(&mut self, voice: VoiceId, pan: i32) {
        self.voices.set_pan(voice, pan);
    }
    fn set_volume(&mut self, voice: VoiceId, volume: i32) {
        self.voices.set_volume(voice, volume);
    }
    fn set_frequency(&mut self, voice: VoiceId, frequency: u32) {
        self.voices.set_frequency(voice, frequency);
    }
    fn set_position(&mut self, voice: VoiceId, byte: u32) {
        self.voices.set_position(voice, byte);
    }
    fn is_playing(&mut self, voice: VoiceId) -> bool {
        self.voices.is_playing(voice)
    }
    fn play(&mut self, voice: VoiceId, looping: bool) -> bool {
        self.voices.play(voice, looping)
    }
    fn set_emitter(&mut self, voice: VoiceId, listener: &Listener, emitter: &Emitter) {
        self.voices.set_emitter(voice, listener, emitter);
    }
}

/// The native reverb parameters as XAudio2's structure.
fn raw_reverb(n: &NativeReverb) -> XAUDIO2FX_REVERB_PARAMETERS {
    XAUDIO2FX_REVERB_PARAMETERS {
        WetDryMix: n.wet_dry_mix,
        ReflectionsDelay: n.reflections_delay,
        ReverbDelay: n.reverb_delay,
        RearDelay: n.rear_delay,
        SideDelay: n.side_delay,
        PositionLeft: n.position_left,
        PositionRight: n.position_right,
        PositionMatrixLeft: n.position_matrix_left,
        PositionMatrixRight: n.position_matrix_right,
        EarlyDiffusion: n.early_diffusion,
        LateDiffusion: n.late_diffusion,
        LowEQGain: n.low_eq_gain,
        LowEQCutoff: n.low_eq_cutoff,
        HighEQGain: n.high_eq_gain,
        HighEQCutoff: n.high_eq_cutoff,
        RoomFilterFreq: n.room_filter_freq,
        RoomFilterMain: n.room_filter_main,
        RoomFilterHF: n.room_filter_hf,
        ReflectionsGain: n.reflections_gain,
        ReverbGain: n.reverb_gain,
        DecayTime: n.decay_time,
        Density: n.density,
        RoomSize: n.room_size,
        DisableLateField: n.disable_late_field.into(),
    }
}

#[cfg(test)]
mod tests;
