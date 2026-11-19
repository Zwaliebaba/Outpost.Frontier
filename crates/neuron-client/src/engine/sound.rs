//! The engine's sounds (work package P4.10, the client wiring): the
//! client's sound layer ([`crate::audio::client`]) over the sound manager's
//! mixer and the audio device, the sound tables and voice profiles of
//! `OutpostFrontier.ini`'s `[Data]`, the per-frame sound tick, and the
//! reverb of `[AudioProperties]` and of the scene scripts.
//!
//! [`Engine::play_sound`] is `PlayClientSoundArch`: a source of the
//! client's playing list, ranked and played by the frame's sound tick
//! (`TickClientSoundMixer`, then the mixer's `MixerTick`) like every sound
//! of the game.

use std::cell::{Cell, RefCell};
use std::rc::Rc;
use std::sync::Arc;

use neuron_core::id::create_id;
use neuron_core::vfs::{FileSystem, FsDesc, Host, UtfFileSystem};

use super::{Engine, lossy};
use crate::audio::client::config::{ListenerSetting, SoundOptions};
use crate::audio::client::source::ArchRef;
use crate::audio::client::{
    ClientSound, NoAttachments, PlayKey, SoundDevice, SoundEnv, SourceKey, WavFiles,
};
use crate::audio::listener::Listener;
use crate::audio::mixer::{Archetype, ArchetypeId, Emitter, VoiceBackend, VoiceId};
use crate::audio::reverb::{EaxState, reverb_parameters};
use crate::render::ff::device::Backend;
use crate::scene_script::entity::kind as entity_kind;
use crate::scene_script::playback::Player;
use crate::scene_script::playback::controls::{EntityControls, interface};

/// The client archetype of a sound: a `[Sound]` of the sound tables or a
/// line of a voice profile.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SoundId(pub ArchRef);

/// What [`Engine::sound_info`] reports of a sound's mixer archetype.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct SoundInfo {
    /// The sound.
    pub id: SoundId,
    /// Its length in milliseconds.
    pub duration_ms: u32,
    /// Its channels.
    pub channels: u16,
    /// Its sample rate.
    pub rate: u16,
    /// The mixer archetype's flags (`CreateSoundArchWav`'s).
    pub flags: u32,
}

/// What [`Engine::load_sound_tables`] loaded.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SoundTables {
    /// `[Sound]` archetypes.
    pub sounds: usize,
    /// Voice profiles.
    pub voices: usize,
}

/// The device of a manager without the audio device: every voice is made
/// and plays, and nothing is heard.
#[derive(Clone, Copy, Debug, Default)]
pub struct SilentDevice;

impl VoiceBackend for SilentDevice {
    fn create(&mut self, _: VoiceId, _: ArchetypeId, _: &Archetype) -> bool {
        true
    }
    fn release(&mut self, _: VoiceId) {}
    fn set_pan(&mut self, _: VoiceId, _: i32) {}
    fn set_volume(&mut self, _: VoiceId, _: i32) {}
    fn set_frequency(&mut self, _: VoiceId, _: u32) {}
    fn set_position(&mut self, _: VoiceId, _: u32) {}
    fn is_playing(&mut self, _: VoiceId) -> bool {
        true
    }
    fn play(&mut self, _: VoiceId, _: bool) -> bool {
        true
    }
    fn set_emitter(&mut self, _: VoiceId, _: &Listener, _: &Emitter) {}
}

impl SoundDevice for SilentDevice {
    fn set_samples(&mut self, _: ArchetypeId, _: Arc<[u8]>) {}
    fn remove_samples(&mut self, _: ArchetypeId) {}
}

#[cfg(windows)]
impl SoundDevice for crate::platform::audio::AudioDevice {
    fn set_samples(&mut self, archetype: ArchetypeId, samples: Arc<[u8]>) {
        crate::platform::audio::AudioDevice::set_samples(self, archetype, samples);
    }
    fn remove_samples(&mut self, archetype: ArchetypeId) {
        crate::platform::audio::AudioDevice::remove_samples(self, archetype);
    }
}

/// The wavs of the data: a file (`OpenWavFileSystem` through the data's
/// DOS file system) or a leaf of a `.utf` archive of the data (a voice
/// profile's).
pub struct DataWavs<'a>(pub Option<&'a Host>);

impl WavFiles for DataWavs<'_> {
    fn read(&self, file: &[u8], archive: Option<&[u8]>) -> Option<Vec<u8>> {
        let data = self.0?;
        let Some(archive) = archive else {
            return data.read_file(file).ok().map(|f| f.data);
        };
        let fs =
            UtfFileSystem::open(data, &FsDesc::read(archive).with_implementation(b"UTF")).ok()?;
        let mut leaf = fs.open_file(&FsDesc::read(file)).ok()?;
        let size = leaf.size().ok()? as usize;
        let mut buf = vec![0; size];
        let got = leaf.read(&mut buf).ok()?;
        buf.truncate(got);
        Some(buf)
    }
}

/// The reverb the sound manager keeps, shared with the scene scripts'
/// controls; `dirty` until the device has it.
#[derive(Debug)]
pub struct SharedReverb {
    /// `EaxEnv` to `EaxDamp`.
    pub state: RefCell<EaxState>,
    /// Changed since the device last took it.
    pub dirty: Cell<bool>,
}

/// The scene root's `IReverbControl` (`FSceneScript_Scene::SetReverb` and
/// `GetReverb`): the sound manager's `SetEaxPreview` and `GetEaxPreview`.
/// Its fog facet is the game's room fog (P5.9) and is not answered.
pub struct SceneReverb(pub Rc<SharedReverb>);

impl EntityControls for SceneReverb {
    fn interfaces(&self) -> u32 {
        interface::REVERB
    }

    fn set_reverb(&mut self, environment: u32, volume: f32, decay: f32, damping: f32) {
        self.0
            .state
            .borrow_mut()
            .set(environment.cast_signed(), volume, decay, damping);
        self.0.dirty.set(true);
    }

    fn get_reverb(&mut self, out: &mut [u32; 4]) {
        *out = self.0.state.borrow().words();
    }
}

/// Attaches the scene's reverb control to every scene entity of a loaded
/// script, as the game attaches its scene object (`FSceneScript_Scene`;
/// divergence D526).
pub fn attach_scene_reverb(player: &mut Player, reverb: &Rc<SharedReverb>) {
    let scenes: Vec<i32> = player
        .entity_handles()
        .filter(|&h| player.entity_owner_type(h) == Some(entity_kind::SCENE))
        .collect();
    for handle in scenes {
        player.attach(handle, Box::new(SceneReverb(Rc::clone(reverb))));
    }
}

/// The engine's sound state.
#[derive(Debug)]
pub struct EngineSound {
    /// The client's sounds.
    pub client: ClientSound,
    /// The perf options the sound code reads and `FORCE_FREQ_CONTROL_TO_SW`
    /// (the game options' `[AUDIO]` and `PITCH_BEND_SOUNDS` come from
    /// [`Engine::options`] at each call).
    pub options: SoundOptions,
    /// The reverb.
    pub reverb: Rc<SharedReverb>,
    /// The manager's clock: the frames' time in milliseconds (divergence
    /// D520).
    pub clock_ms: f64,
    pub(super) silent: SilentDevice,
    pub(super) sound_files: Vec<Vec<u8>>,
    pub(super) voice_files: Vec<Vec<u8>>,
}

impl EngineSound {
    /// The state `StartupDevice` leaves: the `[AudioProperties]` reverb
    /// stored and handed to `SetEaxPreview`.
    #[must_use]
    pub fn new(
        reverb: &crate::audio::config::ReverbSettings,
        force_frequency_to_software: bool,
        sound_files: Vec<Vec<u8>>,
        voice_files: Vec<Vec<u8>>,
    ) -> EngineSound {
        let mut state = EaxState::from_ini(reverb, true);
        state.startup();
        EngineSound {
            client: ClientSound::new(),
            options: SoundOptions {
                force_frequency_to_software,
                ..SoundOptions::default()
            },
            reverb: Rc::new(SharedReverb {
                state: RefCell::new(state),
                dirty: Cell::new(true),
            }),
            clock_ms: 0.0,
            silent: SilentDevice,
            sound_files,
            voice_files,
        }
    }
}

/// Splits the engine into the client's sounds and the environment one
/// call into them works with.
macro_rules! sound_env {
    ($engine:expr) => {{
        let options = $engine.current_sound_options();
        let now = $engine.sound_now();
        let data = $engine.data.as_ref();
        #[cfg(windows)]
        let device: &mut dyn SoundDevice = match $engine.audio_device.as_mut() {
            Some(d) => d,
            None => &mut $engine.sound.silent,
        };
        #[cfg(not(windows))]
        let device: &mut dyn SoundDevice = &mut $engine.sound.silent;
        (
            &mut $engine.sound.client,
            &mut $engine.mixer,
            device,
            DataWavs(data),
            options,
            now,
        )
    }};
}

impl<B: Backend + 'static> Engine<B> {
    fn current_sound_options(&self) -> SoundOptions {
        use super::options::OPTION_COUNT;
        const AUDIO: u32 = 0x2D;
        const PITCH_BEND_SOUNDS: u32 = 0x34;
        const _: () = assert!((PITCH_BEND_SOUNDS as usize) < OPTION_COUNT);
        SoundOptions {
            audio_group: self.options.get(AUDIO) != 0,
            pitch_bend_sounds: self.options.get(PITCH_BEND_SOUNDS) != 0,
            ..self.sound.options
        }
    }

    /// The manager's clock as `GetTickCount` gives it: milliseconds,
    /// wrapping at 2^32.
    fn sound_now(&self) -> u32 {
        neuron_core::num::trunc_to_i32_via_i64(self.sound.clock_ms).cast_unsigned()
    }

    /// The sound tables (`sounds =`), the voice profiles (`voices =`) of
    /// `OutpostFrontier.ini`'s `[Data]` and `audio\SoundCFG.ini`, as the
    /// client's data load reads them (`LoadClientIniData`). A file that does
    /// not load is a notice.
    pub fn load_sound_tables(&mut self) -> SoundTables {
        let mut out = SoundTables::default();
        let Some(data) = self.data.clone() else {
            return out;
        };
        for path in self.sound.sound_files.clone() {
            let Ok(file) = data.read_file(&path) else {
                self.notices.push(format!("{}: not loaded", lossy(&path)));
                continue;
            };
            let (client, mixer, device, wavs, options, now) = sound_env!(self);
            let mut env = SoundEnv {
                mixer,
                device,
                files: &wavs,
                options,
                now,
            };
            out.sounds += client.load_sounds(&mut env, &file.data, false);
        }
        for path in self.sound.voice_files.clone() {
            let Ok(file) = data.read_file(&path) else {
                self.notices.push(format!("{}: not loaded", lossy(&path)));
                continue;
            };
            let exists = |p: &[u8]| matches!(data.attributes(p), Ok(Some(_)));
            out.voices += self.sound.client.load_voices(&file.data, &exists);
        }
        match data.read_file(b"audio\\SoundCFG.ini") {
            Ok(file) => {
                for setting in self.sound.client.config.load(&file.data) {
                    match setting {
                        ListenerSetting::Doppler(v) => self.mixer.listener.doppler_factor = v,
                        ListenerSetting::Rolloff(v) => self.mixer.listener.rolloff_factor = v,
                    }
                }
            }
            Err(_) => self
                .notices
                .push("audio\\SoundCFG.ini: not loaded".to_owned()),
        }
        out
    }

    /// A `[Sound]` of the sound tables by nickname.
    #[must_use]
    pub fn sound(&self, nickname: &[u8]) -> Option<SoundId> {
        self.sound.client.sound(nickname).map(SoundId)
    }

    /// A line of a voice profile: the line `msg` names, or the profile's
    /// first line (in id order) without one.
    #[must_use]
    pub fn voice_line(&self, profile: &[u8], msg: Option<&[u8]>) -> Option<SoundId> {
        let profile_id = create_id(profile);
        let p = self.sound.client.voices.get(profile_id)?;
        let msg_id = match msg {
            Some(m) => create_id(m),
            None => *p.messages.keys().next()?,
        };
        self.sound
            .client
            .voice_line(profile_id, msg_id)
            .map(SoundId)
    }

    /// Plays a sound once, flat and centred (`PlayClientSoundArch` without
    /// a play key): a source joins the client's playing list, and the
    /// frame's sound tick ranks it and plays it through the mixer. False
    /// when its archetype has no wav to play (or is a streamed one, which
    /// is not ported yet).
    pub fn play_sound(&mut self, sound: SoundId) -> bool {
        self.play_sound_with(sound, None, false).0
    }

    /// `PlayClientSoundArch` with a play key, and a handle on the source
    /// when `want_handle` asks for it (release it with
    /// [`Engine::release_sound_source`]).
    pub fn play_sound_with(
        &mut self,
        sound: SoundId,
        key: Option<&PlayKey>,
        want_handle: bool,
    ) -> (bool, Option<SourceKey>) {
        let (client, mixer, device, wavs, options, now) = sound_env!(self);
        let mut env = SoundEnv {
            mixer,
            device,
            files: &wavs,
            options,
            now,
        };
        client.play(&mut env, sound.0, key, want_handle)
    }

    /// Releases a handle [`Engine::play_sound_with`] gave.
    pub fn release_sound_source(&mut self, key: SourceKey) {
        let (client, mixer, device, wavs, options, now) = sound_env!(self);
        let mut env = SoundEnv {
            mixer,
            device,
            files: &wavs,
            options,
            now,
        };
        client.release(&mut env, key);
    }

    /// The mixer archetype of a sound while it has one (from its first
    /// play until its last use ends).
    #[must_use]
    pub fn sound_info(&self, sound: SoundId) -> Option<SoundInfo> {
        let sm = match sound.0 {
            ArchRef::Sound(id) => {
                let arch = self.sound.client.sounds.get(id)?;
                let linked = arch.linked?;
                self.sound.client.sounds.get(linked)?.sm_arch_id
            }
            ArchRef::Voice { profile, msg } => {
                self.sound.client.voices.message(profile, msg)?.sm_arch_id
            }
        };
        let a = self.mixer.archetype(sm)?;
        Some(SoundInfo {
            id: sound,
            duration_ms: a.duration_ms,
            channels: a.format.channels,
            rate: a.format.rate,
            flags: a.flags,
        })
    }

    /// The audio device started (on Windows; a silent device stands for it
    /// otherwise).
    #[must_use]
    pub fn has_audio_device(&self) -> bool {
        #[cfg(windows)]
        {
            self.audio_device.is_some()
        }
        #[cfg(not(windows))]
        {
            false
        }
    }

    /// The sources of the client's playing list.
    #[must_use]
    pub fn sounds_playing(&self) -> usize {
        self.sound.client.playing().count()
    }

    /// The client's sounds.
    #[must_use]
    pub fn client_sound(&self) -> &ClientSound {
        &self.sound.client
    }

    /// The reverb the sound manager holds (`GetEaxPreview`).
    #[must_use]
    pub fn reverb(&self) -> EaxState {
        *self.sound.reverb.state.borrow()
    }

    /// `SetEaxPreview`: the reverb the scene scripts set, from code.
    pub fn set_reverb(&mut self, environment: i32, volume: f32, decay: f32, damping: f32) {
        self.sound
            .reverb
            .state
            .borrow_mut()
            .set(environment, volume, decay, damping);
        self.sound.reverb.dirty.set(true);
        self.apply_reverb();
    }

    /// Hands a changed reverb to the device, as the I3DL2 reverb of its
    /// preview (divergence D224).
    pub(super) fn apply_reverb(&mut self) {
        if !self.sound.reverb.dirty.replace(false) {
            return;
        }
        let preview = self.sound.reverb.state.borrow().preview;
        let reverb = reverb_parameters(&preview);
        #[cfg(windows)]
        if let Some(device) = self.audio_device.as_ref()
            && let Err(e) = device.set_reverb(Some(&reverb))
        {
            self.notices.push(format!("the reverb was not set: {e}"));
        }
        #[cfg(not(windows))]
        let _ = reverb;
    }

    /// The frame's sound step: the manager's clock moves by the frame's
    /// time (divergence D520), the client's sound tick runs
    /// (`TickClientSoundMixer` without a camera: the manager keeps its
    /// listener, divergence D521), and a reverb the scene scripts changed
    /// goes to the device.
    pub(super) fn tick_sounds(&mut self, dt: f64) {
        self.sound.clock_ms += dt * 1000.0;
        #[allow(clippy::cast_possible_truncation)]
        let dt = dt as f32;
        let (client, mixer, device, wavs, options, now) = sound_env!(self);
        let mut env = SoundEnv {
            mixer,
            device,
            files: &wavs,
            options,
            now,
        };
        client.tick(&mut env, None, dt, &NoAttachments);
        self.apply_reverb();
    }

    /// The teardown of the client's sounds (`StopAllClientSounds`,
    /// `ShutdownClientSoundArch`, `ClearClientVoices`).
    pub(super) fn shutdown_sounds(&mut self) {
        let (client, mixer, device, wavs, options, now) = sound_env!(self);
        let mut env = SoundEnv {
            mixer,
            device,
            files: &wavs,
            options,
            now,
        };
        client.stop_all_except(&mut env, None);
        client.shutdown(&mut env);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audio::config::ReverbSettings;

    #[test]
    fn the_scene_reverb_is_the_managers_preview() {
        let ini = ReverbSettings {
            environment: 40,
            volume: 2.0,
            decay_time: 0.01,
            damping: 1.0,
            mix: 0,
        };
        let sound = EngineSound::new(&ini, true, Vec::new(), Vec::new());
        // StartupDevice's SetEaxPreview clamped the INI values.
        let p = sound.reverb.state.borrow().preview;
        assert_eq!((p.environment, p.volume, p.decay_time), (0, 1.0, 0.1));
        sound.reverb.dirty.set(false);
        let mut control = SceneReverb(Rc::clone(&sound.reverb));
        assert_eq!(control.interfaces(), interface::REVERB);
        control.set_reverb(12, 0.5, 300.0, -1.0);
        assert!(sound.reverb.dirty.get());
        let mut words = [0; 4];
        control.get_reverb(&mut words);
        assert_eq!(
            words,
            [12, 0.5f32.to_bits(), 100.0f32.to_bits(), 0.0f32.to_bits()]
        );
    }
}
