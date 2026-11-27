//! `Sound::StreamedSource` (`menu_sound_arch.cpp`, work package P5.5): a
//! source that plays through the streamer (`IStreamer2`) instead of the
//! sound manager, with its fade, and the two background slots that hold
//! one each, the music and the ambience (`WriteBackgroundSource`,
//! `FadeBackgroundSource`).
//!
//! A streamed source is not managed (flag 4 clear): the mixer never ranks
//! it, its archetype is not acquired, and its volume goes straight to its
//! stream (`StreamedApplyVolume`) at each `Update`. Without a streamer
//! (the reference's `NeuronRuntime::Streamer` null, a client without
//! sound) it never gets a stream, so it never starts: `Start` answers
//! false and the background slots stay empty.

use neuron_core::num::trunc_to_i32_via_i64;

use super::config::{SILENCE_DB, db_blend};
use super::source::{ArchRef, ClientSource, category, source_flags};
use super::{ClientSound, PlayKey, SoundEnv, SourceKey, key_flags};
use crate::audio::stream::StreamId;

/// `IStreamer2` as the client's sounds call it.
pub trait ClientStreamer {
    /// Slot 4, `CreateStream`: a stream of a file's bytes (the client's
    /// sounds read it from the data), looping or not (mode 2 or 0), not
    /// started. None for a handle of 0 or -1.
    fn create(&mut self, file: Vec<u8>, looping: bool) -> Option<StreamId>;
    /// Slot 5, `DestroyStream`.
    fn destroy(&mut self, stream: StreamId);
    /// Slot 6, `StopStream`.
    fn stop(&mut self, stream: StreamId);
    /// Slot 7, `StartStream`.
    fn start(&mut self, stream: StreamId);
    /// Slot 8, `SetVolume`, in hundredths of a decibel.
    fn set_volume(&mut self, stream: StreamId, volume: i32);
    /// Slot 13, `SetPan`, in hundredths of a decibel.
    fn set_pan(&mut self, stream: StreamId, pan: i32);
    /// Slot 10, `QueryStatus`: 0 for none, 4 for finished.
    fn status(&mut self, stream: StreamId) -> u32;
}

/// The streamed block of a source (`+0x30..+0x3c`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Streamed {
    /// The stream (`+0x30`), none for 0 or -1.
    pub stream: Option<StreamId>,
    /// The decibels the fade runs to (`+0x34`).
    pub fade_target: f32,
    /// Seconds left of the fade (`+0x38`), -1 for none.
    pub fade_time: f32,
    /// The background slot let go of this source (`+0x3c`).
    pub fade_out: bool,
}

impl Default for Streamed {
    fn default() -> Self {
        Streamed {
            stream: None,
            fade_target: 0.0,
            fade_time: -1.0,
            fade_out: false,
        }
    }
}

/// A background slot: the music (category 3) or the ambience (4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Background {
    /// `G_BackgroundMusic`.
    Music,
    /// `G_BackgroundAmbient`.
    Ambient,
}

impl Background {
    fn category(self) -> i32 {
        match self {
            Background::Music => category::MUSIC,
            Background::Ambient => category::AMBIENT,
        }
    }
}

/// `StreamedApplyVolume`'s value: the sum in hundredths of a decibel as a
/// float, -9999 below -9600, 0 above 0, otherwise `ftol`.
#[must_use]
pub fn stream_volume(sum_db: f64) -> i32 {
    let hundredths = (sum_db * 100.0) as f32;
    if hundredths < -9600.0 {
        -9999
    } else if hundredths > 0.0 {
        0
    } else {
        trunc_to_i32_via_i64(f64::from(hundredths))
    }
}

impl ClientSource {
    /// `FStreamedSource`: not managed, no stream, no fade.
    #[must_use]
    pub fn new_streamed() -> ClientSource {
        let mut source = ClientSource::new(false);
        source.flags &= !source_flags::MANAGED;
        source.streamed = Some(Streamed::default());
        source
    }

    /// Slot 40: faded out, the slot having let go of it and its decibels
    /// at or below the target.
    #[must_use]
    pub fn faded_out(&self) -> bool {
        self.streamed
            .is_some_and(|s| s.fade_out && self.attenuation_db <= s.fade_target)
    }
}

impl ClientSound {
    /// The source in a background slot.
    #[must_use]
    pub fn background(&self, which: Background) -> Option<SourceKey> {
        match which {
            Background::Music => self.background_music,
            Background::Ambient => self.background_ambient,
        }
    }

    fn background_slot(&mut self, which: Background) -> &mut Option<SourceKey> {
        match which {
            Background::Music => &mut self.background_music,
            Background::Ambient => &mut self.background_ambient,
        }
    }

    fn streamed_mut(&mut self, key: u32) -> Option<&mut Streamed> {
        self.slots.0[key as usize]
            .as_mut()
            .and_then(|s| s.streamed.as_mut())
    }

    /// The archetype's own decibels (slot 6), 0 without it.
    fn arch_db(&self, arch: ArchRef) -> i32 {
        match arch {
            ArchRef::Sound(id) => self.sounds.get(id).map_or(0, |a| a.attenuation()),
            ArchRef::Voice { profile, msg } => self
                .voices
                .message(profile, msg)
                .map_or(0, |m| i32::from(m.attenuation)),
        }
    }

    /// `SourceCategoryDb`: the master with the ducking where it applies,
    /// and the mode's term.
    fn category_db(&self, source: &ClientSource) -> (f64, f64) {
        let (master, ducked, mode) = self.category_terms(source);
        let base = if ducked {
            f64::from(master) + f64::from(self.duck_current)
        } else {
            f64::from(master)
        };
        (base, f64::from(mode))
    }

    /// Slot 9, `GetAttenuation`: the archetype's and the source's decibels
    /// with the category's, clamped to [-96, 0]; 0 without an archetype.
    #[must_use]
    pub fn total_attenuation(&self, key: SourceKey) -> f32 {
        let Some(source) = self.source(key) else {
            return 0.0;
        };
        let Some(arch) = source.arch else {
            return 0.0;
        };
        let own = f64::from(self.arch_db(arch)) + f64::from(source.attenuation_db);
        let (base, mode) = self.category_db(source);
        let total = own + (base + mode);
        if total > 0.0 {
            0.0
        } else if total < f64::from(SILENCE_DB) {
            SILENCE_DB
        } else {
            total as f32
        }
    }

    /// `StreamedApplyVolume`: the source's sum (the category's, the
    /// archetype's and its own decibels, not clamped) to its stream.
    pub(super) fn apply_stream_volume(&mut self, env: &mut SoundEnv<'_>, key: u32) {
        let Some(source) = self.slots.0[key as usize].as_ref() else {
            return;
        };
        let Some(stream) = source.streamed.and_then(|s| s.stream) else {
            return;
        };
        let Some(arch) = source.arch else {
            return;
        };
        let own = f64::from(source.attenuation_db);
        let arch_db = f64::from(self.arch_db(arch));
        let (base, mode) = self.category_db(source);
        let volume = stream_volume(((base + mode) + arch_db) + own);
        if let Some(streamer) = env.device.streamer() {
            streamer.set_volume(stream, volume);
        }
    }

    /// Slot 23 (`AltStart`), the stream opened: true without a streamer;
    /// otherwise false when the stream does not open ("Failed to start
    /// streamer sound").
    fn open_stream(&mut self, env: &mut SoundEnv<'_>, key: u32, arch: ArchRef) -> bool {
        if env.device.streamer().is_none() {
            return true;
        }
        let looping = self.slot(key).flags & source_flags::LOOP != 0;
        let file = match arch {
            ArchRef::Sound(id) => self.sounds.get(id).and_then(|a| a.file.clone()),
            ArchRef::Voice { .. } => None,
        };
        let bytes = file.and_then(|f| env.files.read(&f, None));
        let stream = bytes.and_then(|b| env.device.streamer()?.create(b, looping));
        if let Some(s) = self.streamed_mut(key) {
            s.stream = stream;
        }
        stream.is_some()
    }

    /// `StreamedRestart`: with a streamer and a stream, `SourceStart` and on
    /// success the volume and `StartStream` ("Bad streamer status" when it
    /// reports none). Whether the source has a stream.
    fn restart_stream(
        &mut self,
        env: &mut SoundEnv<'_>,
        key: u32,
        arch: ArchRef,
        extra: i32,
    ) -> bool {
        let stream = self.slots.0[key as usize]
            .as_ref()
            .and_then(|s| s.streamed)
            .and_then(|s| s.stream);
        if env.device.streamer().is_some()
            && let Some(stream) = stream
            && self.start(env, SourceKey(key), arch, extra)
        {
            self.apply_stream_volume(env, key);
            if let Some(streamer) = env.device.streamer() {
                streamer.start(stream);
                // A zero status is the reference's warning only.
                let _ = streamer.status(stream);
            }
        }
        stream.is_some()
    }

    /// `RestartClientStreamedSound`: [`Self::restart_stream`] on a source a
    /// script started.
    pub fn restart_streamed(
        &mut self,
        env: &mut SoundEnv<'_>,
        key: SourceKey,
        arch: ArchRef,
        offset: i32,
    ) -> bool {
        self.restart_stream(env, key.0, arch, offset)
    }

    /// Slot 22 of a streamed source: the stream opened, then
    /// [`Self::restart_stream`]. Whether it has a stream.
    pub(super) fn start_streamed(
        &mut self,
        env: &mut SoundEnv<'_>,
        key: u32,
        arch: ArchRef,
        extra: i32,
    ) -> bool {
        self.open_stream(env, key, arch);
        self.restart_stream(env, key, arch, extra)
    }

    /// Slot 24 of a streamed source: the stream stopped and destroyed, the
    /// fade cleared, then the common stop of a playing source.
    pub(super) fn stop_streamed(&mut self, env: &mut SoundEnv<'_>, key: u32) {
        let Some(streamed) = self.streamed_mut(key) else {
            return;
        };
        let stream = streamed.stream.take();
        streamed.fade_target = 0.0;
        streamed.fade_time = -1.0;
        if let Some(stream) = stream
            && let Some(streamer) = env.device.streamer()
        {
            streamer.stop(stream);
            streamer.destroy(stream);
        }
        if self.slot(key).playing {
            self.stop_linked(env, key);
        }
    }

    /// `BindClientBackgroundMusic` (or the ambient slot's), then slot 29
    /// (`IsFinished`) of its source: false with no source in the slot.
    pub fn background_finished(&mut self, env: &mut SoundEnv<'_>, which: Background) -> bool {
        let Some(key) = self.background(which) else {
            return false;
        };
        match self.slots.0.get(key.0 as usize).and_then(Option::as_ref) {
            None => false,
            Some(s) if s.streamed.is_some() => self.streamed_finished(env, key.0),
            Some(s) => s.is_finished(),
        }
    }

    /// Slot 29 of a streamed source: finished without a stream, faded
    /// out, or with a stream status of 0 or 4; otherwise the base test.
    pub(super) fn streamed_finished(&mut self, env: &mut SoundEnv<'_>, key: u32) -> bool {
        let source = self.slot(key);
        let Some(stream) = source.streamed.and_then(|s| s.stream) else {
            return true;
        };
        if source.faded_out() {
            return true;
        }
        let status = env.device.streamer().map_or(0, |s| s.status(stream));
        if status == 4 || status == 0 {
            return true;
        }
        self.slot(key).is_finished()
    }

    /// Slot 42 (`Update`) of a streamed source after its score: false when
    /// it finished; otherwise the fade's step and the volume to the
    /// stream.
    pub(super) fn update_streamed(&mut self, env: &mut SoundEnv<'_>, key: u32, dt: f32) -> bool {
        if self.streamed_finished(env, key) {
            return false;
        }
        let source = self.slot(key);
        let from = source.attenuation_db;
        let Some(streamed) = source.streamed.as_mut() else {
            return false;
        };
        let to = streamed.fade_target;
        if !(streamed.fade_time < 0.0) {
            let remaining = f64::from(streamed.fade_time) - f64::from(dt);
            streamed.fade_time = remaining as f32;
            let result = if !(remaining > 0.0) {
                streamed.fade_time = -1.0;
                f64::from(to)
            } else {
                db_blend(from, to, dt, remaining)
            };
            source.attenuation_db = if result < f64::from(SILENCE_DB) {
                SILENCE_DB
            } else if result > 0.0 {
                0.0
            } else {
                result as f32
            };
        }
        self.apply_stream_volume(env, key);
        true
    }

    /// Slot 32 of a streamed source: the decibels clamped to [-96, 0] as
    /// both its own and the fade's target, the fade time 0, the volume to
    /// the stream.
    pub fn set_streamed_attenuation(&mut self, env: &mut SoundEnv<'_>, key: SourceKey, db: f32) {
        // A NaN passes, as through the reference's two tests.
        let value = db.clamp(SILENCE_DB, 0.0);
        let source = self.slot(key.0);
        source.attenuation_db = value;
        if let Some(s) = source.streamed.as_mut() {
            s.fade_target = value;
            s.fade_time = 0.0;
        }
        self.apply_stream_volume(env, key.0);
    }

    /// `WriteBackgroundSource` (`SetClientBackgroundMusic` and
    /// `SetClientBackgroundAmbient`). The fade is `music_fade_time` unless
    /// `fade` is not below 0. A slot already playing the archetype of a
    /// non-empty id keeps it (true); otherwise its source fades out to
    /// `cross_fade_silence` and is let go. An empty id answers false;
    /// otherwise a streamed source of the archetype (looping with flag bit
    /// 0) starts at the silence and fades in to 0 dB.
    pub fn set_background(
        &mut self,
        env: &mut SoundEnv<'_>,
        which: Background,
        nickname_id: u32,
        flags: u32,
        fade: f32,
    ) -> bool {
        let silence = self.config.cross_fade_silence;
        let fade_time = if fade >= 0.0 {
            fade
        } else {
            self.config.music_fade_time
        };
        if let Some(current) = self.background(which) {
            if nickname_id != 0 {
                let arch = self.sounds.lookup(nickname_id).map(ArchRef::Sound);
                if arch.is_some() && self.source(current).and_then(|s| s.arch) == arch {
                    return true;
                }
            }
            if let Some(s) = self.streamed_mut(current.0) {
                s.fade_target = silence;
                s.fade_time = fade_time;
                s.fade_out = true;
            }
            self.release(env, current);
            *self.background_slot(which) = None;
        }
        if nickname_id == 0 {
            return false;
        }
        let Some(arch) = self.sounds.lookup(nickname_id) else {
            return false;
        };
        let key = PlayKey {
            start: 0,
            flags: key_flags::STREAMED | u8::from(flags & 1 != 0),
            position: [0.0; 3],
            attenuation: silence,
        };
        let (ok, handle) = self.play(env, ArchRef::Sound(arch), Some(&key), true);
        let Some(handle) = handle.filter(|_| ok) else {
            return false;
        };
        *self.background_slot(which) = Some(handle);
        let source = self.slot(handle.0);
        source.category = which.category();
        if let Some(s) = source.streamed.as_mut() {
            s.fade_target = 0.0;
            s.fade_time = fade_time;
        }
        true
    }

    /// `FadeBackgroundSource` (`FadeClientBackgroundMusic` and
    /// `FadeClientBackgroundAmbient`): a target above 1e8 is the mode's
    /// ducking down-by; a rate above 1e8 takes the mode's down-time as the
    /// fade's time; otherwise the time is the distance from the source's
    /// total attenuation over the rate. Nothing for an empty slot.
    pub fn fade_background(&mut self, which: Background, target_db: f32, rate: f32) {
        let Some(key) = self.background(which) else {
            return;
        };
        let (down_by, down_time) = self.config.ducking();
        let target = if f64::from(target_db) > 100_000_000.0 {
            down_by
        } else {
            target_db
        };
        if f64::from(rate) > 100_000_000.0 {
            if let Some(s) = self.streamed_mut(key.0) {
                s.fade_time = down_time;
                s.fade_target = target;
            }
            return;
        }
        let current = self.total_attenuation(key);
        if let Some(s) = self.streamed_mut(key.0) {
            s.fade_target = target;
            s.fade_time = ((f64::from(target) - f64::from(current)) / f64::from(rate)).abs() as f32;
        }
    }

    /// The background part of `StopAllClientSoundsExceptArch`: each slot's
    /// source stopped, let go and the slot cleared, the music first.
    pub(super) fn stop_backgrounds(&mut self, env: &mut SoundEnv<'_>) {
        for which in [Background::Music, Background::Ambient] {
            if let Some(key) = self.background_slot(which).take() {
                self.stop(env, key);
                self.release(env, key);
            }
        }
    }
}
