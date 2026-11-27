//! The game's calls into the client's sounds (P5.5): the background music
//! and ambience, their fades, the ducking, stop-all and the master fade
//! (`menu_sound_arch.cpp`, `sound_config.cpp`), behind [`SoundCalls`] so
//! the game's code runs over the client's sound layer ([`SoundLayer`]) and
//! the oracle can log each call ([`Logged`], `audio.jsonl`).

use neuron_client::audio::client::source::ArchRef;
use neuron_client::audio::client::streamed::Background;
use neuron_client::audio::client::{ClientSound, SoundEnv};

use crate::oracle::audio::AudioLog;

/// The client's sound calls the game makes.
pub trait SoundCalls {
    /// `SetClientBackgroundMusic`.
    fn set_background_music(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool;
    /// `SetClientBackgroundAmbient`.
    fn set_background_ambient(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool;
    /// `FadeClientBackgroundMusic`.
    fn fade_background_music(&mut self, target_db: f32, rate: f32);
    /// `FadeClientBackgroundAmbient`.
    fn fade_background_ambient(&mut self, target_db: f32, rate: f32);
    /// `PushClientSoundDucking`.
    fn push_ducking(&mut self);
    /// `PopClientSoundDucking`.
    fn pop_ducking(&mut self);
    /// `StopAllClientSoundsExceptArch` with the `[Sound]` of `keep` (0:
    /// none, `StopAllClientSounds`).
    fn stop_all_except(&mut self, keep: u32);
    /// `SnapClientAudioFade`.
    fn snap_audio_fade(&mut self, value: f32);
    /// `SetClientAudioFade`.
    fn set_audio_fade(&mut self, target: f32, seconds: f32);
    /// `GetClientDuckingConfig`'s `music_fade_time`.
    fn music_fade_time(&self) -> f32;
    /// `BindClientBackgroundMusic` and its source's `IsFinished`: false
    /// without a background music source.
    fn background_music_finished(&mut self) -> bool;
}

/// The calls where nothing plays (a client without its sound layer, as in
/// the tests): the backgrounds are not set, the rest does nothing, and the
/// music's fade time is the given one.
#[derive(Clone, Copy, Debug, Default)]
pub struct Silent {
    /// What `music_fade_time` answers.
    pub music_fade_time: f32,
}

impl SoundCalls for Silent {
    fn set_background_music(&mut self, _: u32, _: u32, _: f32) -> bool {
        false
    }
    fn set_background_ambient(&mut self, _: u32, _: u32, _: f32) -> bool {
        false
    }
    fn fade_background_music(&mut self, _: f32, _: f32) {}
    fn fade_background_ambient(&mut self, _: f32, _: f32) {}
    fn push_ducking(&mut self) {}
    fn pop_ducking(&mut self) {}
    fn stop_all_except(&mut self, _: u32) {}
    fn snap_audio_fade(&mut self, _: f32) {}
    fn set_audio_fade(&mut self, _: f32, _: f32) {}
    fn music_fade_time(&self) -> f32 {
        self.music_fade_time
    }
    fn background_music_finished(&mut self) -> bool {
        false
    }
}

/// The client's sound layer for one call.
pub struct SoundLayer<'a, 'b> {
    /// The client's sounds.
    pub sound: &'a mut ClientSound,
    /// The mixer, device, data and clock.
    pub env: SoundEnv<'b>,
}

impl SoundCalls for SoundLayer<'_, '_> {
    fn set_background_music(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool {
        self.sound
            .set_background(&mut self.env, Background::Music, nickname_id, flags, fade)
    }
    fn set_background_ambient(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool {
        self.sound
            .set_background(&mut self.env, Background::Ambient, nickname_id, flags, fade)
    }
    fn fade_background_music(&mut self, target_db: f32, rate: f32) {
        self.sound
            .fade_background(Background::Music, target_db, rate);
    }
    fn fade_background_ambient(&mut self, target_db: f32, rate: f32) {
        self.sound
            .fade_background(Background::Ambient, target_db, rate);
    }
    fn push_ducking(&mut self) {
        self.sound.push_ducking();
    }
    fn pop_ducking(&mut self) {
        self.sound.pop_ducking();
    }
    fn stop_all_except(&mut self, keep: u32) {
        let keep = (keep != 0).then_some(ArchRef::Sound(keep));
        self.sound.stop_all_except(&mut self.env, keep);
    }
    fn snap_audio_fade(&mut self, value: f32) {
        self.sound.fade.snap(value);
    }
    fn set_audio_fade(&mut self, target: f32, seconds: f32) {
        let (_, down_time) = self.sound.config.ducking();
        self.sound.fade.set(target, seconds, down_time);
    }
    fn music_fade_time(&self) -> f32 {
        self.sound.config.music_fade_time
    }
    fn background_music_finished(&mut self) -> bool {
        self.sound
            .background_finished(&mut self.env, Background::Music)
    }
}

/// The calls of `inner`, each logged as it returns.
pub struct Logged<'a> {
    /// The calls' target.
    pub inner: &'a mut dyn SoundCalls,
    /// `audio.jsonl`.
    pub log: &'a mut AudioLog,
    /// The frame of the records.
    pub frame: u32,
}

impl SoundCalls for Logged<'_> {
    fn set_background_music(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool {
        let r = self.inner.set_background_music(nickname_id, flags, fade);
        self.log.call(
            self.frame,
            "SetClientBackgroundMusic",
            &[nickname_id, flags],
            &[fade],
            Some(r),
        );
        r
    }
    fn set_background_ambient(&mut self, nickname_id: u32, flags: u32, fade: f32) -> bool {
        let r = self.inner.set_background_ambient(nickname_id, flags, fade);
        self.log.call(
            self.frame,
            "SetClientBackgroundAmbient",
            &[nickname_id, flags],
            &[fade],
            Some(r),
        );
        r
    }
    fn fade_background_music(&mut self, target_db: f32, rate: f32) {
        self.inner.fade_background_music(target_db, rate);
        let m = "FadeClientBackgroundMusic";
        self.log.call(self.frame, m, &[], &[target_db, rate], None);
    }
    fn fade_background_ambient(&mut self, target_db: f32, rate: f32) {
        self.inner.fade_background_ambient(target_db, rate);
        let m = "FadeClientBackgroundAmbient";
        self.log.call(self.frame, m, &[], &[target_db, rate], None);
    }
    fn push_ducking(&mut self) {
        self.inner.push_ducking();
        let m = "PushClientSoundDucking";
        self.log.call(self.frame, m, &[], &[], None);
    }
    fn pop_ducking(&mut self) {
        self.inner.pop_ducking();
        let m = "PopClientSoundDucking";
        self.log.call(self.frame, m, &[], &[], None);
    }
    fn stop_all_except(&mut self, keep: u32) {
        self.inner.stop_all_except(keep);
        let m = "StopAllClientSoundsExceptArch";
        self.log.call(self.frame, m, &[keep], &[], None);
    }
    fn snap_audio_fade(&mut self, value: f32) {
        self.inner.snap_audio_fade(value);
        let m = "SnapClientAudioFade";
        self.log.call(self.frame, m, &[], &[value], None);
    }
    fn set_audio_fade(&mut self, target: f32, seconds: f32) {
        // The reference writes the record first: the arguments as given.
        let m = "SetClientAudioFade";
        self.log.call(self.frame, m, &[], &[target, seconds], None);
        self.inner.set_audio_fade(target, seconds);
    }
    fn music_fade_time(&self) -> f32 {
        self.inner.music_fade_time()
    }
    /// Not a logged call: the C++ hook logs the calls that change the sound.
    fn background_music_finished(&mut self) -> bool {
        self.inner.background_music_finished()
    }
}
