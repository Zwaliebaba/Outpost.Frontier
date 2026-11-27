//! The client's sound layer (work package P4.10, the client wiring):
//! `Client\cmn\SoundArch.cpp`, `Sound.cpp` and `Voice.cpp`, ported in
//! `CppSource/OutpostFrontier/Client/menu_sound_arch.cpp`,
//! `voice_profiles.cpp` and `sound_config.cpp`, over the mixer of
//! [`crate::audio::mixer`].
//!
//! - [`arch`]: the `[Sound]` archetypes of the sound tables, their
//!   level-of-detail links, use counts and mixer archetypes.
//! - [`voice`]: the voice profiles and their lines, whose wavs live in the
//!   profiles' `.utf` archives.
//! - [`source`]: the play keys the mixer ranks and plays.
//! - [`rank`]: the VC6 sort the tick ranks them with.
//! - [`config`]: `audio\SoundCFG.ini` and the options the sound code reads.
//!
//! [`ClientSound`] holds them with the playing list, the category masters
//! and the ducking state. [`ClientSound::play`] is `PlayClientSoundArch`;
//! [`ClientSound::tick`] is `TickClientSoundMixer`, which ranks the
//! audible sources, hands the ranked list to the mixer
//! ([`Mixer::replace_attached`]) and runs the mixer's tick
//! ([`Mixer::tick`]), then retires the sources that finished.
//!
//! - [`streamed`]: the streamed sources over the streamer, and the
//!   background music and ambience with their fades (P5.5).
//!
//! Not ported here (the game's, P5.5): the thruster sound and the
//! equipment's emitters, and the single-player pause of every source.

// The reference's x87 comparisons send unordered operands (NaN) to one side;
// the port spells each outcome as the comparison that is true exactly when
// the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

pub mod arch;
pub mod config;
pub mod rank;
pub mod source;
pub mod streamed;
pub mod voice;

use std::sync::Arc;

use neuron_core::rng::CrtRand;

use self::arch::{SoundArchTable, WavFactory, WavRequest};
use self::config::{SoundConfig, SoundOptions, db_blend, linear_to_option_db};
use self::source::{ArchRef, ClientSource, NO_SCORE, category, source_flags};
use self::streamed::ClientStreamer;
use self::voice::VoiceTable;
use super::decode::decode_archetype;
use super::mixer::{Archetype, ArchetypeId, Mixer, SourceTable, VoiceBackend};

/// The device side of the client's sounds: the mixer's voices, and the
/// samples of each mixer archetype.
pub trait SoundDevice: VoiceBackend {
    /// The decoded samples of a mixer archetype (`FillFromPcm`'s upload).
    fn set_samples(&mut self, archetype: ArchetypeId, samples: Arc<[u8]>);
    /// The archetype is gone.
    fn remove_samples(&mut self, archetype: ArchetypeId);
    /// The streamer (`NeuronRuntime::Streamer`); none without one, as on a
    /// client without sound.
    fn streamer(&mut self) -> Option<&mut dyn ClientStreamer> {
        None
    }
}

/// Where wavs come from: a file of the data, or a leaf of an archive of the
/// data (a voice profile's `.utf`).
pub trait WavFiles {
    /// The bytes of `file`, inside `archive` when there is one.
    fn read(&self, file: &[u8], archive: Option<&[u8]>) -> Option<Vec<u8>>;
}

/// What one call into the client's sounds works with: the mixer, the
/// device, the data, the options and the manager's clock.
pub struct SoundEnv<'a> {
    /// The sound manager's mixer.
    pub mixer: &'a mut Mixer<u32>,
    /// Its device.
    pub device: &'a mut dyn SoundDevice,
    /// The wavs.
    pub files: &'a dyn WavFiles,
    /// The options.
    pub options: SoundOptions,
    /// The manager's clock in milliseconds (`GetTickCount`).
    pub now: u32,
}

/// A play key's handle: a source the caller holds a reference on.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct SourceKey(pub u32);

/// The play key of `PlayClientSoundArch`.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct PlayKey {
    /// `key[0]`: the start offset in milliseconds (how long ago the sound
    /// started).
    pub start: u32,
    /// `key[1]`'s low byte (see [`key_flags`]).
    pub flags: u8,
    /// `key[2..5]`: the world position of a positioned source.
    pub position: [f32; 3],
    /// `key[5]`: the source's attenuation, as given.
    pub attenuation: f32,
}

/// Bits of [`PlayKey::flags`].
pub mod key_flags {
    /// Looping.
    pub const LOOP: u8 = 0x01;
    /// A positioned source (`Sound::Source3D`).
    pub const SPATIAL: u8 = 0x02;
    /// A streamed source (`Sound::StreamedSource`).
    pub const STREAMED: u8 = 0x04;
    /// The cockpit mix.
    pub const COCKPIT: u8 = 0x08;
}

/// The camera the tick takes the listener from, when there is one.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ListenerPose {
    /// The camera's position (slot 3).
    pub position: [f32; 3],
    /// The camera's orientation (slot 4), rows of a 3x3.
    pub orientation: [f32; 9],
    /// The listener object's velocity, when there is one.
    pub velocity: Option<[f32; 3]>,
}

/// The engine instances attached sources follow (`GetInstanceTransform`
/// and the user data's velocity): world points from an instance's frame
/// and back, and the instance's velocity.
pub trait Attachments {
    /// A point of `instance`'s frame in the world.
    fn to_world(&self, instance: i32, local: [f32; 3]) -> [f32; 3];
    /// A world point in `instance`'s frame.
    fn to_local(&self, instance: i32, world: [f32; 3]) -> [f32; 3];
    /// The velocity of `instance`'s object (zero without one).
    fn velocity(&self, instance: i32) -> [f32; 3];
}

/// No engine instances: attached points stay where they are.
#[derive(Clone, Copy, Debug, Default)]
pub struct NoAttachments;

impl Attachments for NoAttachments {
    fn to_world(&self, _: i32, local: [f32; 3]) -> [f32; 3] {
        local
    }
    fn to_local(&self, _: i32, world: [f32; 3]) -> [f32; 3] {
        world
    }
    fn velocity(&self, _: i32) -> [f32; 3] {
        [0.0; 3]
    }
}

/// `kMixerMaxEntries`: at most this many sources are ranked.
pub const MAX_RANKED: usize = 0x80;

/// The client's sounds.
#[derive(Clone, Debug)]
pub struct ClientSound {
    /// The `[Sound]` archetypes.
    pub sounds: SoundArchTable,
    /// The voice profiles.
    pub voices: VoiceTable,
    /// `audio\SoundCFG.ini` and the audio mode.
    pub config: SoundConfig,
    /// The C runtime's `rand` the random pitch draws from (divergence D523).
    pub rand: CrtRand,
    slots: Slots,
    free: Vec<u32>,
    playing: Vec<u32>,
    ranked: Vec<u32>,
    alive: i32,
    /// The category masters in decibels: sfx, interface, voice, ambient,
    /// music (the `audio_*` options, 0 dB until the game sets them).
    pub masters: CategoryMasters,
    duck_target: f32,
    duck_current: f32,
    duck_refs: i32,
    duck_timer: f32,
    listener: [f32; 3],
    background_music: Option<SourceKey>,
    background_ambient: Option<SourceKey>,
}

/// The `audio_*` masters in decibels.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct CategoryMasters {
    /// `audio_sfx`.
    pub sfx: f32,
    /// `audio_interface`.
    pub interface: f32,
    /// `audio_voice` (categories 2 and 5).
    pub voice: f32,
    /// `audio_ambient`.
    pub ambient: f32,
    /// `audio_music`.
    pub music: f32,
}

impl Default for ClientSound {
    fn default() -> Self {
        ClientSound::new()
    }
}

/// The wav factory of one call: the mixer archetypes made from the data
/// and destroyed with their voices.
struct Wavs<'a, 'b> {
    env: &'a mut SoundEnv<'b>,
    slots: &'a mut Slots,
}

/// The sources by key, as the mixer asks for them.
#[derive(Clone, Debug, Default)]
struct Slots(Vec<Option<ClientSource>>);

impl SourceTable<u32> for Slots {
    fn source(&mut self, key: u32) -> &mut dyn crate::audio::mixer::SoundSource {
        self.0[key as usize]
            .as_mut()
            .unwrap_or_else(|| panic!("the mixer holds a freed source {key}"))
    }
}

impl WavFactory for Wavs<'_, '_> {
    fn create_wav(&mut self, request: &WavRequest<'_>) -> i32 {
        create_sound_arch_wav(self.env, request)
    }

    fn destroy_wav(&mut self, id: i32) {
        let SoundEnv { mixer, device, .. } = &mut *self.env;
        mixer.destroy_archetype(id, self.slots, &mut **device);
        device.remove_samples(id);
    }
}

/// `CreateSoundArchWav`: the archetype's flags (pan control for a 2D
/// archetype or without 3D sounds, else a 3D buffer; frequency control for
/// a pitch-bendable one when the options allow it), the wav decoded whole
/// (`FillFromWav`) into a mixer archetype with the whole sound as its loop
/// range, its samples handed to the device. -1 when the wav is missing or
/// does not decode.
pub fn create_sound_arch_wav(env: &mut SoundEnv<'_>, request: &WavRequest<'_>) -> i32 {
    let Some(file) = request.file else {
        return -1;
    };
    let Some(bytes) = env.files.read(file, request.archive) else {
        return -1;
    };
    let flags = wav_flags(request.is_2d, request.pitch_bendable, &env.options);
    let Ok(decoded) = decode_archetype(&bytes) else {
        return -1;
    };
    let byte_count = decoded.byte_count();
    let Some(frames) = byte_count.checked_div(u32::from(decoded.format.block_align)) else {
        return -1;
    };
    let Ok(archetype) = Archetype::from_pcm(
        decoded.format,
        byte_count,
        0,
        frames,
        flags,
        env.options.force_frequency_to_software,
    ) else {
        return -1;
    };
    let id = env.mixer.insert_archetype(archetype);
    env.device.set_samples(id, decoded.samples.into());
    id
}

/// The flags `CreateSoundArchWav` asks for: 0x11 (pan control, silent
/// beyond the maximum distance) for a 2D archetype or when `USE_3D_SOUNDS`
/// is below 1, else 9 (a 3D buffer); with 2 (frequency control) for a
/// pitch-bendable archetype when `[AUDIO]` and `PITCH_BEND_SOUNDS` are set.
#[must_use]
pub fn wav_flags(is_2d: bool, pitch_bendable: bool, options: &SoundOptions) -> u32 {
    let no_3d = options.use_3d_sounds < 1.0;
    let mut flags = if is_2d || no_3d { 0x11 } else { 9 };
    if pitch_bendable && options.audio_group && options.pitch_bend_sounds {
        flags |= 2;
    }
    flags
}

impl ClientSound {
    /// No archetypes, profiles or sources.
    #[must_use]
    pub fn new() -> ClientSound {
        ClientSound {
            sounds: SoundArchTable::new(),
            voices: VoiceTable::new(),
            config: SoundConfig::default(),
            rand: CrtRand::default(),
            slots: Slots::default(),
            free: Vec::new(),
            playing: Vec::new(),
            ranked: Vec::new(),
            alive: 0,
            masters: CategoryMasters::default(),
            duck_target: 0.0,
            duck_current: 0.0,
            duck_refs: 0,
            duck_timer: -1.0,
            listener: [0.0; 3],
            background_music: None,
            background_ambient: None,
        }
    }

    // --- the archetypes ---

    /// `LoadClientSoundArchIni`.
    pub fn load_sounds(&mut self, env: &mut SoundEnv<'_>, bytes: &[u8], force: bool) -> usize {
        let Self { sounds, slots, .. } = self;
        sounds.load_ini(bytes, force, &mut Wavs { env, slots })
    }

    /// `LoadClientVoicesIni`.
    pub fn load_voices(&mut self, bytes: &[u8], archive_exists: &dyn Fn(&[u8]) -> bool) -> usize {
        self.voices.load_ini(bytes, archive_exists)
    }

    /// A `[Sound]` archetype by nickname.
    #[must_use]
    pub fn sound(&self, nickname: &[u8]) -> Option<ArchRef> {
        self.sounds.lookup_name(nickname).map(ArchRef::Sound)
    }

    /// A line of a voice profile, by the profile's and the line's names
    /// (`CreateID` of each).
    #[must_use]
    pub fn voice_line(&self, profile: u32, msg: u32) -> Option<ArchRef> {
        self.voices
            .message(profile, msg)
            .map(|_| ArchRef::Voice { profile, msg })
    }

    /// Slot 3 of an archetype.
    pub fn acquire(&mut self, env: &mut SoundEnv<'_>, arch: ArchRef) -> bool {
        let Self {
            sounds,
            voices,
            slots,
            ..
        } = self;
        let mut wavs = Wavs { env, slots };
        match arch {
            ArchRef::Sound(id) => sounds.acquire(id, &mut wavs),
            ArchRef::Voice { profile, msg } => voices.acquire(profile, msg, &mut wavs),
        }
    }

    /// Slot 4 of an archetype.
    pub fn release_arch(&mut self, env: &mut SoundEnv<'_>, arch: ArchRef) -> i32 {
        let Self {
            sounds,
            voices,
            slots,
            ..
        } = self;
        let mut wavs = Wavs { env, slots };
        match arch {
            ArchRef::Sound(id) => sounds.release(id, &mut wavs),
            ArchRef::Voice { profile, msg } => voices.release(profile, msg, &mut wavs),
        }
    }

    /// Slot 0 of an archetype: its mixer archetype, -1 without one.
    pub fn sm_arch_id(&mut self, env: &mut SoundEnv<'_>, arch: ArchRef) -> ArchetypeId {
        let Self {
            sounds,
            slots,
            voices,
            ..
        } = self;
        match arch {
            ArchRef::Sound(id) => sounds.sm_arch_id(id, &mut Wavs { env, slots }),
            ArchRef::Voice { profile, msg } => {
                voices.message(profile, msg).map_or(-1, |m| m.sm_arch_id)
            }
        }
    }

    /// `PreloadClientSoundArchesByCategory`.
    pub fn preload_category(&mut self, env: &mut SoundEnv<'_>, mask: u32) {
        let Self { sounds, slots, .. } = self;
        sounds.preload_category(mask, &mut Wavs { env, slots });
    }

    /// `UnloadClientSoundArchesByCategory`.
    pub fn unload_category(&mut self, env: &mut SoundEnv<'_>, mask: u32) {
        let Self { sounds, slots, .. } = self;
        sounds.unload_category(mask, &mut Wavs { env, slots });
    }

    /// `SetClientSoundLod`.
    pub fn set_lod(&mut self, env: &mut SoundEnv<'_>, lod: i32) {
        let Self { sounds, slots, .. } = self;
        sounds.set_lod(lod, &mut Wavs { env, slots });
    }

    /// `ShutdownClientSoundArch` and `ClearClientVoices`.
    pub fn shutdown(&mut self, env: &mut SoundEnv<'_>) {
        let Self {
            sounds,
            slots,
            voices,
            ..
        } = self;
        sounds.shutdown(&mut Wavs { env, slots });
        voices.clear();
    }

    /// The `audio_*` option callbacks: a 0..1 slider for a category (0 sfx,
    /// 1 interface, 2 music, 3 ambient, 4 voice).
    pub fn set_category_volume(&mut self, which: u32, linear: f32) {
        let db = linear_to_option_db(linear);
        match which {
            0 => self.masters.sfx = db,
            1 => self.masters.interface = db,
            2 => self.masters.music = db,
            3 => self.masters.ambient = db,
            _ => self.masters.voice = db,
        }
    }

    // --- the sources ---

    /// The source of a key.
    #[must_use]
    pub fn source(&self, key: SourceKey) -> Option<&ClientSource> {
        self.slots.0.get(key.0 as usize)?.as_ref()
    }

    /// The source of a key, for changes through its setters.
    pub fn source_mut(&mut self, key: SourceKey) -> Option<&mut ClientSource> {
        self.slots.0.get_mut(key.0 as usize)?.as_mut()
    }

    /// The playing sources in start order.
    pub fn playing(&self) -> impl Iterator<Item = SourceKey> + '_ {
        self.playing.iter().map(|&k| SourceKey(k))
    }

    /// The sources the last tick ranked, best first.
    pub fn ranked(&self) -> impl Iterator<Item = SourceKey> + '_ {
        self.ranked.iter().map(|&k| SourceKey(k))
    }

    /// The live sources (`G_AliveSoundSourceCount`).
    #[must_use]
    pub fn alive(&self) -> i32 {
        self.alive
    }

    /// The ducking's current decibels (`GetClientSoundDuckingDb`).
    #[must_use]
    pub fn ducking_db(&self) -> f32 {
        self.duck_current
    }

    fn slot(&mut self, key: u32) -> &mut ClientSource {
        self.slots.0[key as usize]
            .as_mut()
            .unwrap_or_else(|| panic!("no source {key}"))
    }

    fn alloc(&mut self, source: ClientSource) -> u32 {
        self.alive += 1;
        if let Some(key) = self.free.pop() {
            self.slots.0[key as usize] = Some(source);
            key
        } else {
            self.slots.0.push(Some(source));
            u32::try_from(self.slots.0.len() - 1).unwrap_or(u32::MAX)
        }
    }

    /// The category terms of `SourceCategoryDb` and `ComputeMixerScore`:
    /// the master as a float, whether the ducking applies, and the mode.
    fn category_terms(&self, source: &ClientSource) -> (f32, bool, f32) {
        let mode = self.config.audio_mode;
        let cockpit = source.flags & source_flags::COCKPIT != 0;
        let mode_db = |source_mode: bool| -> f32 {
            if !source_mode {
                return 0.0;
            }
            if mode == 2 && !cockpit {
                self.config.cockpit_attenuation
            } else if mode == 1 && cockpit {
                -1e10
            } else {
                0.0
            }
        };
        let m = &self.masters;
        match source.category {
            category::SFX => (m.sfx, true, mode_db(true)),
            category::INTERFACE => {
                if mode == 1 {
                    (0.0, false, -1e10)
                } else {
                    (m.interface, false, 0.0)
                }
            }
            category::VOICE => (m.voice, false, 0.0),
            category::MUSIC => (m.music, true, 0.0),
            category::AMBIENT => (m.ambient, true, mode_db(true)),
            category::VOICE_DUCKED => (m.voice, true, 0.0),
            _ => (0.0, false, 0.0),
        }
    }

    /// Takes a source's answers from its archetype, the categories and the
    /// clock for this call.
    fn refresh(&mut self, env: &mut SoundEnv<'_>, key: u32, attachments: &dyn Attachments) {
        let Some(source) = self.slots.0[key as usize].clone() else {
            return;
        };
        let mut view = source.view;
        view.clock = env.now;
        let (master, ducked, mode) = self.category_terms(&source);
        view.master_db = master;
        view.ducked = ducked;
        view.duck_db = self.duck_current;
        view.mode_db = mode;
        view.base_db = if ducked {
            f64::from(master) + f64::from(self.duck_current)
        } else {
            f64::from(master)
        };
        if let Some(arch) = source.arch {
            // A streamed source plays no mixer archetype.
            if source.streamed.is_none() {
                view.sm_arch = self.sm_arch_id(env, arch);
            }
            let (atten, bendable, min, max) = match arch {
                ArchRef::Sound(id) => self.sounds.get(id).map_or((0, false, 0.0, 0.0), |a| {
                    (
                        a.attenuation(),
                        a.pitch_bendable(),
                        a.range_min,
                        a.range_max,
                    )
                }),
                ArchRef::Voice { profile, msg } => self
                    .voices
                    .message(profile, msg)
                    .map_or((0, false, -1.0, -1.0), |m| {
                        (i32::from(m.attenuation), false, -1.0, -1.0)
                    }),
            };
            view.arch_attenuation = atten;
            view.pitch_bendable = bendable;
            view.range_min = min;
            view.range_max = (f64::from(max) * f64::from(env.options.max_distance_scale)) as f32;
            view.duration_ms = env.mixer.archetype(view.sm_arch).map(|a| a.duration_ms);
        }
        if let Some(s) = source.spatial {
            let local = [s.block[0], s.block[1], s.block[2]];
            if s.attach == -1 {
                view.position = local;
                view.velocity = [s.block[6], s.block[7], s.block[8]];
            } else {
                view.position = attachments.to_world(s.attach, local);
                view.velocity = attachments.velocity(s.attach);
            }
        }
        self.slot(key).view = view;
    }

    /// `AddRef`.
    pub fn add_ref(&mut self, key: SourceKey) {
        let s = self.slot(key.0);
        s.ref_count += 1;
    }

    /// `Release`: at zero the source is torn down (stopped when it still
    /// plays) and freed. The remaining count.
    pub fn release(&mut self, env: &mut SoundEnv<'_>, key: SourceKey) -> i32 {
        let s = self.slot(key.0);
        s.ref_count -= 1;
        let remaining = s.ref_count;
        if remaining == 0 {
            if self.slot(key.0).playing {
                self.stop_linked(env, key.0);
            }
            self.alive -= 1;
            self.slots.0[key.0 as usize] = None;
            self.free.push(key.0);
        }
        remaining
    }

    /// `SourceStopLinked`: the archetype released and the key detached
    /// from the mixer (for a managed source), out of the playing list, the
    /// list's reference dropped.
    fn stop_linked(&mut self, env: &mut SoundEnv<'_>, key: u32) {
        let source = self.slot(key);
        let managed = source.flags & source_flags::MANAGED != 0;
        let arch = source.arch;
        if managed {
            if let Some(arch) = arch {
                self.release_arch(env, arch);
            }
            env.mixer.detach(key, &mut *env.device);
        }
        self.playing.retain(|&k| k != key);
        self.slot(key).playing = false;
        // The list's reference: the count is above zero here unless a
        // teardown is under way, which only takes it below zero.
        let s = self.slot(key);
        s.ref_count -= 1;
        if s.ref_count == 0 {
            self.alive -= 1;
            self.slots.0[key as usize] = None;
            self.free.push(key);
        }
    }

    /// Slot 24 (`Stop`): a playing source is stopped; an idle one is left.
    /// A streamed source also loses its stream, playing or not.
    pub fn stop(&mut self, env: &mut SoundEnv<'_>, key: SourceKey) {
        if self.source(key).is_some_and(|s| s.streamed.is_some()) {
            self.stop_streamed(env, key.0);
        } else if self.source(key).is_some_and(ClientSource::is_playing) {
            self.stop_linked(env, key.0);
        }
    }

    /// Slot 25 at the manager's clock. A streamed source cannot be moved
    /// (the reference's "Tried to restart streaming sound").
    pub fn set_start_offset(&mut self, env: &SoundEnv<'_>, key: SourceKey, offset: i32) {
        let now = env.now;
        let source = self.slot(key.0);
        if source.streamed.is_none() {
            source.set_start_offset(offset, now);
        }
    }

    /// Slot 22 (`SourceStart`): a playing source of the same archetype
    /// only moves its start offset; another archetype stops it first. The
    /// archetype's link becomes the source's archetype, its type the
    /// category; a managed source acquires the archetype (false when that
    /// fails) and joins the playing list.
    pub fn start(
        &mut self,
        env: &mut SoundEnv<'_>,
        key: SourceKey,
        arch: ArchRef,
        extra: i32,
    ) -> bool {
        let source = self.slot(key.0);
        if source.playing {
            if source.arch.map(ArchRef::identity) == Some(arch.identity()) {
                self.set_start_offset(env, key, extra);
                return true;
            }
            self.stop(env, key);
        }
        let linked = match arch {
            ArchRef::Sound(id) => {
                let Self { sounds, slots, .. } = self;
                sounds
                    .linked(id, &mut Wavs { env, slots })
                    .map(ArchRef::Sound)
            }
            voice @ ArchRef::Voice { .. } => Some(voice),
        };
        self.slot(key.0).arch = linked;
        let Some(linked) = linked else {
            return false;
        };
        let kind = match linked {
            ArchRef::Sound(id) => self.sounds.get(id).map_or(0, |a| a.source_type()),
            ArchRef::Voice { .. } => 2,
        };
        let source = self.slot(key.0);
        source.category = kind.cast_signed();
        source.start_tick = extra;
        source.flags &= 0xE7;
        if source.flags & source_flags::MANAGED != 0 && !self.acquire(env, linked) {
            return false;
        }
        let source = self.slot(key.0);
        source.ref_count += 1;
        source.playing = true;
        self.playing.push(key.0);
        self.refresh(env, key.0, &NoAttachments);
        true
    }

    /// `PlayClientSoundArch`: a source for `arch` (positioned when the key
    /// asks for it), the key's loop and cockpit bits, attenuation and
    /// position, the archetype's random pitch, then [`Self::start`] with the
    /// key's offset. With `want_handle` the caller gets a reference on the
    /// source. A streamer archetype or a streamed key makes a streamed
    /// source ([`streamed`]), which starts only with a stream.
    pub fn play(
        &mut self,
        env: &mut SoundEnv<'_>,
        arch: ArchRef,
        key: Option<&PlayKey>,
        want_handle: bool,
    ) -> (bool, Option<SourceKey>) {
        let streamer = match arch {
            ArchRef::Sound(id) => self
                .sounds
                .get(id)
                .is_some_and(arch::SoundArch::is_streamer),
            ArchRef::Voice { .. } => false,
        };
        let flags = key.map_or(0, |k| k.flags);
        let streamed = streamer || flags & key_flags::STREAMED != 0;
        let mut source = if streamed {
            ClientSource::new_streamed()
        } else {
            ClientSource::new(flags & key_flags::SPATIAL != 0)
        };
        if let Some(k) = key {
            let mut bits = source.flags;
            bits = (((k.flags >> 2) ^ bits) & 2) ^ bits;
            bits = ((k.flags ^ bits) & 1) ^ bits;
            source.flags = bits;
            source.attenuation_db = k.attenuation;
            source.set_position(k.position, &|_, w| w);
        }
        source.pitch = match arch {
            ArchRef::Sound(id) => {
                let Self {
                    sounds,
                    slots,
                    rand,
                    ..
                } = self;
                sounds.random_pitch(id, rand, &mut Wavs { env, slots })
            }
            ArchRef::Voice { .. } => 0.0,
        };
        let id = SourceKey(self.alloc(source));
        let extra = key.map_or(0, |k| k.start.cast_signed());
        let ok = if streamed {
            self.start_streamed(env, id.0, arch, extra)
        } else {
            self.start(env, id, arch, extra)
        };
        let handle = if ok && want_handle {
            self.add_ref(id);
            Some(id)
        } else {
            None
        };
        self.release(env, id);
        (ok, handle)
    }

    /// `PlayClientSoundArchIfIdle`: nothing when a playing source plays
    /// `arch`.
    pub fn play_if_idle(
        &mut self,
        env: &mut SoundEnv<'_>,
        arch: ArchRef,
        key: Option<&PlayKey>,
        want_handle: bool,
    ) -> (bool, Option<SourceKey>) {
        if self.count_playing(arch) != 0 {
            return (false, None);
        }
        self.play(env, arch, key, want_handle)
    }

    /// `CountClientSoundArchSources`.
    #[must_use]
    pub fn count_playing(&self, arch: ArchRef) -> usize {
        self.playing
            .iter()
            .filter(|&&k| self.slots.0[k as usize].as_ref().and_then(|s| s.arch) == Some(arch))
            .count()
    }

    /// `StopClientSoundArch`: every playing source of `arch`.
    pub fn stop_arch(&mut self, env: &mut SoundEnv<'_>, arch: ArchRef) {
        for key in self.playing.clone() {
            if self.slots.0[key as usize].as_ref().and_then(|s| s.arch) == Some(arch) {
                self.stop(env, SourceKey(key));
            }
        }
    }

    /// `StopAllClientSoundsExceptArch`: the background music and ambience
    /// stopped and let go, every playing source not of `keep` stopped, the
    /// mixer ticked, the ducking reset.
    pub fn stop_all_except(&mut self, env: &mut SoundEnv<'_>, keep: Option<ArchRef>) {
        self.stop_backgrounds(env);
        for key in self.playing.clone() {
            if self.slots.0[key as usize].as_ref().and_then(|s| s.arch) != keep {
                self.stop(env, SourceKey(key));
            }
        }
        self.refresh_all(env, &NoAttachments);
        let SoundEnv {
            mixer, device, now, ..
        } = env;
        mixer.tick(*now, &mut self.slots, &mut **device);
        self.duck_target = 0.0;
        self.duck_current = 0.0;
        self.duck_refs = 0;
        self.duck_timer = -1.0;
    }

    /// `PushClientSoundDucking`: the first push fades the mix down by the
    /// spaceflight ducking.
    pub fn push_ducking(&mut self) {
        if self.duck_refs == 0 {
            self.duck_timer = self.config.space_down_time;
            self.duck_target = self.config.space_down_by;
        }
        self.duck_refs += 1;
    }

    /// `PopClientSoundDucking`: the last pop fades it back up.
    pub fn pop_ducking(&mut self) {
        self.duck_refs -= 1;
        if self.duck_refs == 0 {
            self.duck_target = 0.0;
            self.duck_timer = self.config.space_up_time;
        }
    }

    fn refresh_all(&mut self, env: &mut SoundEnv<'_>, attachments: &dyn Attachments) {
        for key in self.playing.clone() {
            self.refresh(env, key, attachments);
        }
    }

    /// `TickClientSoundMixer` with the manager's clock at `env.now`: the
    /// ducking fade; the listener from the camera (or the manager's own);
    /// the voice budget; the audible sources from the last started back,
    /// scored (`Update`) and ranked; the ranked list, cut to the budget,
    /// handed to the mixer and the mixer ticked; then every source that
    /// finished stopped.
    pub fn tick(
        &mut self,
        env: &mut SoundEnv<'_>,
        camera: Option<&ListenerPose>,
        dt: f32,
        attachments: &dyn Attachments,
    ) {
        self.ranked.clear();
        // (1) The ducking fade.
        if !(self.duck_timer < 0.0) {
            let timer = f64::from(self.duck_timer) - f64::from(dt);
            self.duck_timer = timer as f32;
            if !(timer > 0.0) {
                self.duck_timer = -1.0;
                self.duck_current = self.duck_target;
            } else {
                self.duck_current = db_blend(
                    self.duck_current,
                    self.duck_target,
                    dt,
                    f64::from(self.duck_timer),
                ) as f32;
            }
        }
        // (2) The listener.
        if let Some(c) = camera {
            self.listener = c.position;
            env.mixer
                .listener
                .set_orientation_from_matrix(&c.orientation);
            env.mixer.listener.set_position(self.listener);
            if let Some(v) = c.velocity {
                env.mixer.listener.set_velocity(v);
            }
        } else {
            self.listener = env.mixer.listener.get_position();
        }
        // (3) The voice budget.
        let mut budget = MAX_RANKED;
        if env.options.audio_group {
            let submitted = env.options.max_submitted_sounds;
            budget = if submitted.cast_unsigned() as usize >= MAX_RANKED {
                MAX_RANKED
            } else if submitted < 8 {
                8
            } else {
                submitted.cast_unsigned() as usize
            };
        }
        // (4) The audible sources, from the last started back.
        self.refresh_all(env, attachments);
        let listener = self.listener;
        for key in self.playing.clone().into_iter().rev() {
            let source = self.slot(key);
            source.integrate(dt);
            if source.spatial.is_some_and(|s| s.attach != -1) {
                // The integrated offset moves the world position.
                self.refresh(env, key, attachments);
            }
            let full = self.ranked.len() >= MAX_RANKED;
            let source = self.slot(key);
            source.compute_score(listener);
            if source.streamed.is_some() {
                // Unmanaged: never ranked.
                self.update_streamed(env, key, dt);
                continue;
            }
            if source.is_finished() {
                continue;
            }
            if source.flags & source_flags::MANAGED == 0 {
                continue;
            }
            if !(source.mixer_score == NO_SCORE) && !full {
                self.ranked.push(key);
            } else if source.flags & source_flags::STARTED == 0 {
                let now = env.now;
                source.mark_started(now);
            }
        }
        // (5) Rank, cut, latch, hand to the mixer and tick it.
        let slots = &self.slots;
        let score = |k: u32| {
            slots.0[k as usize]
                .as_ref()
                .map_or(NO_SCORE, |s| s.mixer_score)
        };
        let mut ranked = std::mem::take(&mut self.ranked);
        rank::sort_ranked(&mut ranked, &score);
        ranked.truncate(budget);
        for &key in &ranked {
            let now = env.now;
            let s = self.slot(key);
            if s.flags & source_flags::STARTED == 0 {
                s.mark_started(now);
            }
        }
        {
            let SoundEnv {
                mixer, device, now, ..
            } = env;
            mixer.replace_attached(&ranked, &mut **device);
            mixer.tick(*now, &mut self.slots, &mut **device);
        }
        self.ranked = ranked;
        // (6) Retire what finished, from the front.
        for key in self.playing.clone() {
            let finished = match self.slots.0[key as usize].as_ref() {
                None => false,
                Some(s) if s.streamed.is_some() => self.streamed_finished(env, key),
                Some(s) => s.is_finished(),
            };
            if finished {
                self.stop(env, SourceKey(key));
            }
        }
    }
}

#[cfg(test)]
mod tests;
