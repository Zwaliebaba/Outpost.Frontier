//! The mixer: archetypes, the attached-source list, the ranked voice cap
//! and the per-tick voice settings of `FSoundManager::MixerTick`
//! (`CppSource/SoundManager/SoundManager.cpp`).
//!
//! The client ranks its audible sources and hands the manager an ordered
//! list every frame (`ReplaceAttachedList`, [`Mixer::replace_attached`]).
//! The tick ([`Mixer::tick`]) walks that list: the first
//! [`Mixer::max_channels`] sources that are active, not finished and
//! started keep or get a voice; the rest lose theirs. Each voice then gets
//! its pan, frequency and volume, and voices that are not yet playing get
//! a start position and are played.
//!
//! The port has no sound buffers. A [`VoiceBackend`] stands for the device
//! (the XAudio2 source voices of a later increment, or a recorder in
//! tests); a [`SoundSource`] stands for the client's play key, asked the
//! same questions in the same order as the reference asks them.
//!
//! Voices of 3D archetypes hand their pose to the device
//! ([`VoiceBackend::set_emitter`]), which spatialises them with X3DAudio
//! in place of the reference's DirectSound 3D buffers (owner decision
//! O26, divergence D220); [`Spatial::Software`] gives them the
//! reference's software path instead, as the oracle harness does. Deleted
//! with decision T4: hardware and software placement (`MaybeMigrateHwSw`,
//! the location flags of `Play` and its retries; D222), the per-source
//! EAX environment (D223), and the notify thread (native loop regions
//! replace it).

// The reference's x87 comparisons send unordered operands (NaN) to one side;
// the port spells each outcome as the comparison that is true exactly when
// the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use super::listener::{Listener, Pose};
use super::mix::{
    DEFAULT_MAX_3D_PAN, clamp_pan_2d, elapsed_frames, frequency_for_pitch, pan_3d,
    pitch_to_multiplier, pitched_elapsed_ms, quake_distance, scaled_duration_ms,
    software_attenuation, volume_hundredths,
};

/// An archetype id: 1 for the first archetype, then counting up. The
/// client uses -1 for "no archetype"; no archetype ever has it.
pub type ArchetypeId = i32;

/// The archetype id meaning none.
pub const NO_ARCHETYPE: ArchetypeId = -1;

/// Archetype flags as the client passes them (`CreateArchetypeFromWav`).
pub mod archetype_flags {
    /// Silence beyond the maximum distance; for a 3D archetype also stop
    /// voices that move beyond it.
    pub const MUTE_AT_MAX_DISTANCE: u32 = 0x01;
    /// Frequency control (pitch bend).
    pub const FREQUENCY: u32 = 0x02;
    /// A 3D buffer.
    pub const SPATIAL: u32 = 0x08;
    /// Pan control. Dropped when [`SPATIAL`] is set too.
    pub const PAN: u32 = 0x10;
    /// Set by the manager with frequency control when
    /// `FORCE_FREQ_CONTROL_TO_SW` is on: software placement.
    pub const SOFTWARE: u32 = 0x20;
    /// Position notifications for a loop range inside the samples.
    pub const LOOP_NOTIFY: u32 = 0x40;
}

/// Buffer description flags (`DSBCAPS_*`) the archetype's buffer is
/// created with. Only the control bits matter to the port; the location
/// bits are computed for comparison with the reference.
pub mod buffer_flags {
    /// `LOCSOFTWARE`.
    pub const LOC_SOFTWARE: u32 = 0x8;
    /// `CTRL3D`.
    pub const CTRL_3D: u32 = 0x10;
    /// `CTRLFREQUENCY`.
    pub const CTRL_FREQUENCY: u32 = 0x20;
    /// `CTRLPAN`.
    pub const CTRL_PAN: u32 = 0x40;
    /// `CTRLVOLUME`.
    pub const CTRL_VOLUME: u32 = 0x80;
    /// `CTRLPOSITIONNOTIFY`.
    pub const CTRL_POSITION_NOTIFY: u32 = 0x100;
    /// `GETCURRENTPOSITION2`.
    pub const GET_CURRENT_POSITION2: u32 = 0x10000;
    /// `MUTE3DATMAXDISTANCE`.
    pub const MUTE_3D_AT_MAX_DISTANCE: u32 = 0x20000;
    /// `LOCDEFER`.
    pub const LOC_DEFER: u32 = 0x40000;
    /// The manager's default creation flags (`G_Ds3dCreateFlags`).
    pub const DEFAULT: u32 = LOC_DEFER | CTRL_VOLUME | CTRL_3D;
}

/// The sample format of an archetype: the four words the client passes
/// (`Fmt8`): channels, bytes per sample, rate, block align.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PcmFormat {
    /// Channels.
    pub channels: u16,
    /// Bytes per sample (`Field06`).
    pub bytes_per_sample: u16,
    /// Frames per second.
    pub rate: u16,
    /// Bytes per frame.
    pub block_align: u16,
}

/// Why an archetype cannot be made. The reference divides by these
/// unchecked (divergence D225).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ArchetypeError {
    /// A block align of zero.
    ZeroBlockAlign,
    /// A rate of zero.
    ZeroRate,
    /// No channels.
    ZeroChannels,
}

impl core::fmt::Display for ArchetypeError {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        f.write_str(match self {
            ArchetypeError::ZeroBlockAlign => "block align is zero",
            ArchetypeError::ZeroRate => "rate is zero",
            ArchetypeError::ZeroChannels => "channel count is zero",
        })
    }
}

impl std::error::Error for ArchetypeError {}

/// A sound archetype (`FSoundArchetype`): format, loop range, gain and
/// the buffer flags its voices get.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Archetype {
    /// The sample format.
    pub format: PcmFormat,
    /// Loop start, in frames.
    pub loop_start: u32,
    /// Loop end, in frames.
    pub loop_end: u32,
    /// `byte count / block align`.
    pub frames: u32,
    /// `frames * 1000 / rate`, the multiplication wrapping.
    pub duration_ms: u32,
    /// The gain in decibels, `-100..=0` (0 until [`Archetype::set_gain`]).
    pub gain: f32,
    /// The archetype flags after creation adjusted them.
    pub flags: u32,
    /// The buffer description flags (see [`buffer_flags`]).
    pub buffer_flags: u32,
}

impl Archetype {
    /// `FillFromPcm` without the buffer: frames, duration and the buffer
    /// flags from the archetype flags. `force_frequency_to_software` is
    /// the `FORCE_FREQ_CONTROL_TO_SW` switch (on unless the INI clears
    /// it).
    ///
    /// # Errors
    ///
    /// A zero block align, rate or channel count, which the reference
    /// divides by.
    pub fn from_pcm(
        format: PcmFormat,
        byte_count: u32,
        loop_start: u32,
        loop_end: u32,
        flags: u32,
        force_frequency_to_software: bool,
    ) -> Result<Archetype, ArchetypeError> {
        use archetype_flags as a;
        use buffer_flags as b;
        if format.block_align == 0 {
            return Err(ArchetypeError::ZeroBlockAlign);
        }
        if format.rate == 0 {
            return Err(ArchetypeError::ZeroRate);
        }
        if format.channels == 0 {
            return Err(ArchetypeError::ZeroChannels);
        }
        let frames = byte_count / u32::from(format.block_align);
        let duration_ms = frames.wrapping_mul(1000) / u32::from(format.rate);
        let mut flags = flags;
        let mut desc = b::DEFAULT;
        if flags & a::SPATIAL != 0 && flags & a::PAN != 0 {
            flags &= !a::PAN;
        }
        if flags & a::SPATIAL != 0 {
            desc = (desc & !b::CTRL_PAN) | b::CTRL_3D;
        } else if flags & a::PAN != 0 {
            desc = (desc & !b::CTRL_3D) | b::CTRL_PAN;
        }
        if flags & a::FREQUENCY != 0 {
            desc |= b::CTRL_FREQUENCY;
            if force_frequency_to_software {
                flags |= a::SOFTWARE;
            }
        }
        if flags & a::MUTE_AT_MAX_DISTANCE != 0 && desc & b::CTRL_3D != 0 {
            desc |= b::MUTE_3D_AT_MAX_DISTANCE;
        }
        if flags & a::LOOP_NOTIFY != 0 && (loop_start != 0 || loop_end < frames) {
            desc |= b::CTRL_POSITION_NOTIFY | b::GET_CURRENT_POSITION2;
        }
        if flags & a::SOFTWARE != 0 {
            desc = (desc & !b::LOC_DEFER) | b::LOC_SOFTWARE;
        }
        Ok(Archetype {
            format,
            loop_start,
            loop_end,
            frames,
            duration_ms,
            gain: 0.0,
            flags,
            buffer_flags: desc,
        })
    }

    /// `SetGain`: above 0 dB clamped to 0, below -100 dB to -100; a NaN is
    /// kept.
    pub fn set_gain(&mut self, gain: f32) {
        // `clamp` keeps a NaN, as the reference's two jumps do.
        self.gain = gain.clamp(-100.0, 0.0);
    }

    /// The archetype has frequency control (flag 2, which
    /// `ScaledDurationMs` tests).
    #[must_use]
    pub fn frequency_control(&self) -> bool {
        self.flags & archetype_flags::FREQUENCY != 0
    }
}

/// The client's play key (`ISoundSource`), as far as the mixer asks it.
/// The mixer calls these in the reference's order; [`SoundSource::restamp`]
/// and [`SoundSource::start_tick`] may change the source.
pub trait SoundSource {
    /// Slot 3: re-latch the start tick (called before the first start).
    fn restamp(&mut self);
    /// Slot 4: the tick the sound started on.
    fn start_tick(&mut self) -> u32;
    /// Slot 5.
    fn is_active(&self) -> bool;
    /// Slot 6: a positioned source.
    fn is_3d(&self) -> bool;
    /// Slot 7.
    fn is_looping(&self) -> bool;
    /// Slot 8: the archetype, or [`NO_ARCHETYPE`].
    fn archetype(&self) -> ArchetypeId;
    /// Slot 9: the attenuation in decibels; the value left in a float
    /// that starts at 0 (the result code is not tested).
    fn attenuation(&self) -> f32;
    /// Slot 10: the pitch, if the source has one.
    fn pitch(&self) -> Option<f32>;
    /// Slot 11: the pan of a 2D source.
    fn pan(&self) -> Option<i32>;
    /// Slot 12: the world position of a 3D source.
    fn position(&self) -> Option<[f32; 3]>;
    /// Slot 13.
    fn min_distance(&self) -> Option<f32>;
    /// Slot 14.
    fn max_distance(&self) -> Option<f32>;
}

/// The client's sources by key.
pub trait SourceTable<K> {
    /// The source of `key`. Every key the mixer holds must resolve.
    fn source(&mut self, key: K) -> &mut dyn SoundSource;
}

/// A voice of the device, numbered by the mixer from 1.
pub type VoiceId = u32;

/// The device side of the mixer: one voice per instance, playing an
/// archetype. Each call stands for one buffer call of the reference.
pub trait VoiceBackend {
    /// `DuplicateSoundBuffer`: a voice for `archetype`; false when the
    /// device has none to give.
    fn create(&mut self, voice: VoiceId, archetype: ArchetypeId, data: &Archetype) -> bool;
    /// Stop and release the voice.
    fn release(&mut self, voice: VoiceId);
    /// `SetPan`, in hundredths of a decibel.
    fn set_pan(&mut self, voice: VoiceId, pan: i32);
    /// `SetVolume`, in hundredths of a decibel.
    fn set_volume(&mut self, voice: VoiceId, volume: i32);
    /// `SetFrequency`, in hertz.
    fn set_frequency(&mut self, voice: VoiceId, frequency: u32);
    /// `SetCurrentPosition`, in bytes.
    fn set_position(&mut self, voice: VoiceId, byte: u32);
    /// `GetStatus` & `PLAYING`.
    fn is_playing(&mut self, voice: VoiceId) -> bool;
    /// `Play`, looping or once; false when it failed.
    fn play(&mut self, voice: VoiceId, looping: bool) -> bool;
    /// The 3D settings of a voice of a 3D archetype under
    /// [`Spatial::Emitter`], each tick before its volume: the device
    /// derives distance attenuation, pan and Doppler from `listener` and
    /// `emitter` (X3DAudio, owner decision O26). Stands for the
    /// reference's 3D buffer calls (`SetPosition`, `SetMinDistance`,
    /// `SetMaxDistance` and the listener's deferred commit).
    fn set_emitter(&mut self, voice: VoiceId, listener: &Listener, emitter: &Emitter);
}

/// How voices of 3D archetypes are spatialised.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Spatial {
    /// The device spatialises them from [`VoiceBackend::set_emitter`]:
    /// the volume carries no distance attenuation and no pan is set
    /// (owner decision O26, divergence D220).
    #[default]
    Emitter,
    /// The reference's software path, as for a 3D buffer that offers no
    /// 3D interface (the oracle harness): distance attenuation in the
    /// volume, no pan, since such a buffer has no pan control.
    Software,
}

/// What [`VoiceBackend::set_emitter`] receives for one voice.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Emitter {
    /// The source position, Z scaled as the listener's.
    pub position: [f32; 3],
    /// The minimum distance: full volume inside it.
    pub min_distance: f32,
    /// The maximum distance: no further attenuation beyond it.
    pub max_distance: f32,
    /// `MUTE3DATMAXDISTANCE`.
    pub mute_at_max_distance: bool,
}

/// The capabilities a voice has, from its archetype's buffer flags.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct VoiceCaps {
    /// `CTRL3D`.
    pub spatial: bool,
    /// `CTRLPAN`.
    pub pan: bool,
    /// `CTRLFREQUENCY`.
    pub frequency: bool,
    /// `MUTE3DATMAXDISTANCE`.
    pub mute_at_max_distance: bool,
    /// `CTRLPOSITIONNOTIFY`: the loop range is inside the samples.
    pub loop_notify: bool,
}

impl VoiceCaps {
    /// The capabilities of a buffer made with `flags`.
    #[must_use]
    pub fn from_buffer_flags(flags: u32) -> VoiceCaps {
        use buffer_flags as b;
        VoiceCaps {
            spatial: flags & b::CTRL_3D != 0,
            pan: flags & b::CTRL_PAN != 0,
            frequency: flags & b::CTRL_FREQUENCY != 0,
            mute_at_max_distance: flags & b::MUTE_3D_AT_MAX_DISTANCE != 0,
            loop_notify: flags & b::CTRL_POSITION_NOTIFY != 0,
        }
    }
}

/// A playing instance (`FSoundInstanceNode`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Instance<K> {
    /// The source.
    pub key: K,
    /// The device voice.
    pub voice: VoiceId,
    /// The archetype's capabilities when the voice was made.
    pub caps: VoiceCaps,
    /// Status 0x20: frequency control is in use. Cleared for good when
    /// the source has no pitch at its first start.
    pub frequency_control: bool,
    /// Status 0x40: the voice was played.
    pub started: bool,
    /// The pose of a 3D source.
    pub pose: Pose,
    /// The loop range in bytes, when the buffer has position notification.
    pub loop_bytes: Option<(u32, u32)>,
    /// The start tick at the last start.
    pub last_start_tick: u32,
    /// The position in the attached list on the last tick.
    pub rank: u32,
    /// The pan last given to the device (a new voice has 0).
    pub pan: i32,
    /// The volume last given to the device (a new voice has 0).
    pub volume: i32,
}

/// What the software path sets for one instance (`MixerApplyInstance`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct VoiceSettings {
    /// The pan, when the voice takes one.
    pub pan: Option<i32>,
    /// The frequency, when the voice has frequency control and the source
    /// a pitch.
    pub frequency: Option<u32>,
    /// The volume, when the archetype resolves.
    pub volume: Option<i32>,
}

/// The listener-wide inputs of the software path.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct MixState<'a> {
    /// The listener.
    pub listener: &'a Listener,
    /// The master gain in decibels.
    pub master_gain: f32,
    /// `max3DPan`, in hundredths.
    pub max_3d_pan: f32,
}

/// `MixerApplyInstance` on the software path: attenuation and pan of a 3D
/// source from its pose (normalising `pose.direction` in place), the
/// client pan of a 2D source, the frequency for its pitch and the volume.
/// `pan_control` says whether the voice takes a pan. Without an archetype
/// there is no frequency and no volume; the reference reads through the
/// null archetype (divergence D226).
#[must_use]
pub fn software_settings(
    state: &MixState<'_>,
    source: &dyn SoundSource,
    archetype: Option<&Archetype>,
    pose: &mut Pose,
    pan_control: bool,
    frequency_control: bool,
) -> VoiceSettings {
    let listener = state.listener;
    let mut attenuation = source.attenuation();
    let mut pan = None;
    if source.is_3d() {
        let dist = quake_distance(pose.dist_sq);
        if pose.dist_sq > pose.min_dist_sq {
            attenuation = software_attenuation(
                pose.min_distance,
                dist,
                listener.distance_factor,
                listener.rolloff_factor,
                attenuation,
            );
        }
        if pan_control {
            pan = Some(pan_3d(
                &mut pose.direction,
                dist,
                listener.front,
                listener.up,
                state.max_3d_pan,
            ));
        }
    } else if pan_control {
        pan = source.pan().map(clamp_pan_2d);
    }
    voice_settings(
        state,
        source,
        archetype,
        attenuation,
        pan,
        frequency_control,
    )
}

/// The frequency for the source's pitch and the volume for `attenuation`,
/// the tail both paths share. Without an archetype there are neither.
fn voice_settings(
    state: &MixState<'_>,
    source: &dyn SoundSource,
    archetype: Option<&Archetype>,
    attenuation: f32,
    pan: Option<i32>,
    frequency_control: bool,
) -> VoiceSettings {
    let mut frequency = None;
    let mut volume = None;
    if let Some(archetype) = archetype {
        if frequency_control {
            frequency = source
                .pitch()
                .map(|pitch| frequency_for_pitch(pitch, archetype.format.rate));
        }
        volume = Some(volume_hundredths(
            archetype.gain,
            state.master_gain,
            attenuation,
        ));
    }
    VoiceSettings {
        pan,
        frequency,
        volume,
    }
}

/// The settings of a voice the device spatialises ([`Spatial::Emitter`]):
/// the frequency for its pitch and the volume with the client's
/// attenuation but no distance attenuation; no pan.
#[must_use]
pub fn emitter_settings(
    state: &MixState<'_>,
    source: &dyn SoundSource,
    archetype: Option<&Archetype>,
    frequency_control: bool,
) -> VoiceSettings {
    let attenuation = source.attenuation();
    voice_settings(
        state,
        source,
        archetype,
        attenuation,
        None,
        frequency_control,
    )
}

/// The clock inputs of [`start_offset`].
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct StartTiming {
    /// The tick of this mixer tick.
    pub now: u32,
    /// `NEVER_SET_START_POS`.
    pub never_set_start_pos: bool,
}

/// `MixerSetStartOffset`: whether to play the instance's voice, after
/// placing it where the source would be by now.
///
/// A voice not yet started re-latches the source's start tick
/// ([`SoundSource::restamp`]); a started one is only placed again when
/// the source's start tick changed. The position is the time since the
/// start (in the archetype's own time when the voice has frequency
/// control; a source without a pitch then loses frequency control for
/// good), wrapped into the loop range for a looping source, in bytes; a
/// position at or past the end plays nothing. A started voice is played
/// again only when it stopped.
pub fn start_offset<K>(
    timing: StartTiming,
    archetype: &dyn Fn(ArchetypeId) -> Option<Archetype>,
    instance: &mut Instance<K>,
    source: &mut dyn SoundSource,
    backend: &mut dyn VoiceBackend,
) -> bool {
    let started = instance.started;
    let tick = if started {
        let tick = source.start_tick();
        if instance.last_start_tick == tick {
            return false;
        }
        tick
    } else {
        source.restamp();
        source.start_tick()
    };
    instance.last_start_tick = tick;
    if timing.never_set_start_pos {
        return true;
    }
    let Some(archetype) = archetype(source.archetype()) else {
        return false;
    };
    let mut elapsed = timing.now.wrapping_sub(tick);
    if instance.frequency_control {
        let multiplier = match source.pitch() {
            Some(pitch) => pitch_to_multiplier(pitch),
            None => {
                instance.frequency_control = false;
                1.0
            }
        };
        elapsed = pitched_elapsed_ms(elapsed, multiplier);
    }
    let mut frames = elapsed_frames(elapsed, archetype.format.rate);
    if source.is_looping() && frames > archetype.loop_end {
        let start = archetype.loop_start;
        // The reference divides by the loop length unchecked (divergence
        // D225): an empty range keeps the loop start.
        frames = frames
            .wrapping_sub(start)
            .checked_rem(archetype.loop_end.wrapping_sub(start))
            .map_or(start, |r| r.wrapping_add(start));
    }
    let block = u32::from(archetype.format.block_align);
    let byte = block.wrapping_mul(frames);
    if archetype.frames.wrapping_mul(block) <= byte {
        return false;
    }
    backend.set_position(instance.voice, byte);
    if !started {
        return true;
    }
    !backend.is_playing(instance.voice)
}

/// The software mixer (`FSoundManager` without its device).
#[derive(Clone, Debug)]
pub struct Mixer<K> {
    archetypes: Vec<(ArchetypeId, Archetype)>,
    next_archetype: u32,
    attached: Vec<K>,
    instances: Vec<Instance<K>>,
    next_voice: VoiceId,
    now: u32,
    /// The listener.
    pub listener: Listener,
    /// `SetMasterGain`, in decibels.
    pub master_gain: f32,
    /// `SetMaxChannels` / `maxSoundChannels`: how many attached sources
    /// may hold a voice.
    pub max_channels: u32,
    /// `max3DPan` in hundredths.
    pub max_3d_pan: f32,
    /// `NEVER_SET_START_POS`: play from the start instead of from the
    /// time the source started.
    pub never_set_start_pos: bool,
    /// How voices of 3D archetypes are spatialised.
    pub spatial: Spatial,
}

impl<K> Default for Mixer<K> {
    fn default() -> Self {
        Mixer {
            archetypes: Vec::new(),
            next_archetype: 1,
            attached: Vec::new(),
            instances: Vec::new(),
            next_voice: 1,
            now: 0,
            listener: Listener::default(),
            master_gain: 0.0,
            max_channels: 0,
            max_3d_pan: DEFAULT_MAX_3D_PAN,
            never_set_start_pos: false,
            spatial: Spatial::Emitter,
        }
    }
}

impl<K: Copy + PartialEq> Mixer<K> {
    /// An empty mixer with the constructor's settings.
    #[must_use]
    pub fn new() -> Mixer<K> {
        Mixer::default()
    }

    /// `InsertArchetype`: the next id.
    pub fn insert_archetype(&mut self, archetype: Archetype) -> ArchetypeId {
        let id = self.next_archetype.cast_signed();
        self.next_archetype = self.next_archetype.wrapping_add(1);
        self.archetypes.push((id, archetype));
        id
    }

    /// `LookupArchetype`.
    #[must_use]
    pub fn archetype(&self, id: ArchetypeId) -> Option<&Archetype> {
        if id == NO_ARCHETYPE {
            return None;
        }
        self.archetypes
            .iter()
            .find(|(i, _)| *i == id)
            .map(|(_, a)| a)
    }

    /// The archetype for changes (`SetGain` through `QueryArchetype`).
    pub fn archetype_mut(&mut self, id: ArchetypeId) -> Option<&mut Archetype> {
        if id == NO_ARCHETYPE {
            return None;
        }
        self.archetypes
            .iter_mut()
            .find(|(i, _)| *i == id)
            .map(|(_, a)| a)
    }

    /// `DestroyArchetype`: removes it and the instances whose source
    /// plays it. False when there is no such archetype.
    pub fn destroy_archetype(
        &mut self,
        id: ArchetypeId,
        sources: &mut dyn SourceTable<K>,
        backend: &mut dyn VoiceBackend,
    ) -> bool {
        let Some(index) = self.archetypes.iter().position(|(i, _)| *i == id) else {
            return false;
        };
        self.archetypes.remove(index);
        let mut i = 0;
        while i < self.instances.len() {
            if sources.source(self.instances[i].key).archetype() == id {
                backend.release(self.instances.remove(i).voice);
            } else {
                i += 1;
            }
        }
        true
    }

    /// `AttachPlayKey`: insert before the `index`-th attached source, or
    /// at the end.
    pub fn attach(&mut self, key: K, index: usize) {
        let index = index.min(self.attached.len());
        self.attached.insert(index, key);
    }

    /// `DetachPlayKey`: the first instance of the key and every attached
    /// entry of it.
    pub fn detach(&mut self, key: K, backend: &mut dyn VoiceBackend) {
        if let Some(i) = self.instances.iter().position(|i| i.key == key) {
            backend.release(self.instances.remove(i).voice);
        }
        self.attached.retain(|k| *k != key);
    }

    /// `ReplaceAttachedList`: the ranked list for the next tick. An empty
    /// list also releases every instance at once.
    pub fn replace_attached(&mut self, keys: &[K], backend: &mut dyn VoiceBackend) {
        self.attached.clear();
        if keys.is_empty() {
            for instance in self.instances.drain(..) {
                backend.release(instance.voice);
            }
            return;
        }
        self.attached.extend_from_slice(keys);
    }

    /// The attached sources in rank order.
    #[must_use]
    pub fn attached(&self) -> &[K] {
        &self.attached
    }

    /// The instances in list order.
    #[must_use]
    pub fn instances(&self) -> &[Instance<K>] {
        &self.instances
    }

    /// The first instance of `key` (`FindInstanceByKey`).
    #[must_use]
    pub fn instance(&self, key: K) -> Option<&Instance<K>> {
        self.instances.iter().find(|i| i.key == key)
    }

    /// The tick of the last [`Mixer::tick`].
    #[must_use]
    pub fn now(&self) -> u32 {
        self.now
    }

    fn release_at(&mut self, index: usize, backend: &mut dyn VoiceBackend) {
        backend.release(self.instances.remove(index).voice);
    }

    /// `OneShotAlreadyExpired` at tick `now`: a one-shot whose
    /// (pitch-scaled) length has passed, `now > length + start` with the
    /// sum wrapping. A missing archetype counts as expired.
    pub fn one_shot_expired(&self, now: u32, source: &mut dyn SoundSource) -> bool {
        if source.is_looping() {
            return false;
        }
        let Some(archetype) = self.archetype(source.archetype()) else {
            return true;
        };
        let frequency_control = archetype.frequency_control();
        let pitch = if frequency_control {
            source.pitch()
        } else {
            None
        };
        let duration = scaled_duration_ms(archetype.duration_ms, frequency_control, pitch);
        let start = source.start_tick();
        now > duration.wrapping_add(start)
    }

    /// `MixerAllowCreate`: 2D sources always; 3D sources need an archetype
    /// and, when it asks for it, to be in range.
    pub fn allow_create(&self, source: &dyn SoundSource) -> bool {
        if !source.is_3d() {
            return true;
        }
        let Some(archetype) = self.archetype(source.archetype()) else {
            return false;
        };
        if archetype.flags & archetype_flags::MUTE_AT_MAX_DISTANCE == 0 {
            return true;
        }
        self.listener
            .in_creation_range(source.position(), source.max_distance())
    }

    /// `CreateInstanceFromKey`.
    fn create_instance(
        &mut self,
        key: K,
        source: &dyn SoundSource,
        backend: &mut dyn VoiceBackend,
    ) -> Option<Instance<K>> {
        let id = source.archetype();
        let archetype = *self.archetype(id)?;
        let voice = self.next_voice;
        if !backend.create(voice, id, &archetype) {
            return None;
        }
        self.next_voice = self.next_voice.wrapping_add(1);
        let caps = VoiceCaps::from_buffer_flags(archetype.buffer_flags);
        let block = u32::from(archetype.format.block_align);
        let loop_bytes = caps.loop_notify.then(|| {
            (
                archetype.loop_start.wrapping_mul(block),
                archetype.loop_end.wrapping_mul(block),
            )
        });
        let pose = if source.is_3d() {
            self.listener.pose(
                source.position(),
                source.min_distance(),
                source.max_distance(),
            )
        } else {
            Pose::default()
        };
        Some(Instance {
            key,
            voice,
            caps,
            frequency_control: caps.frequency,
            started: false,
            pose,
            loop_bytes,
            last_start_tick: 0,
            rank: 0,
            pan: 0,
            volume: 0,
        })
    }

    /// `MixerApplyInstance` for the instance at `index`.
    fn apply(&mut self, index: usize, source: &dyn SoundSource, backend: &mut dyn VoiceBackend) {
        let state = MixState {
            listener: &self.listener,
            master_gain: self.master_gain,
            max_3d_pan: self.max_3d_pan,
        };
        let archetype = self.archetype(source.archetype());
        let instance = &self.instances[index];
        let mut pose = instance.pose;
        let emitter = self.spatial == Spatial::Emitter && instance.caps.spatial;
        let settings = if emitter {
            emitter_settings(&state, source, archetype, instance.frequency_control)
        } else {
            software_settings(
                &state,
                source,
                archetype,
                &mut pose,
                instance.caps.pan,
                instance.frequency_control,
            )
        };
        let instance = &mut self.instances[index];
        instance.pose = pose;
        if emitter {
            backend.set_emitter(
                instance.voice,
                &self.listener,
                &Emitter {
                    position: pose.position,
                    min_distance: pose.min_distance,
                    max_distance: pose.max_distance,
                    mute_at_max_distance: instance.caps.mute_at_max_distance,
                },
            );
        }
        if let Some(pan) = settings.pan
            && pan != instance.pan
        {
            backend.set_pan(instance.voice, pan);
            instance.pan = pan;
        }
        if let Some(frequency) = settings.frequency {
            backend.set_frequency(instance.voice, frequency);
        }
        if let Some(volume) = settings.volume
            && volume != instance.volume
        {
            backend.set_volume(instance.voice, volume);
            instance.volume = volume;
        }
    }

    /// `MixerSetStartOffset` for the instance at `index`.
    fn set_start_offset(
        &mut self,
        index: usize,
        source: &mut dyn SoundSource,
        backend: &mut dyn VoiceBackend,
    ) -> bool {
        let timing = StartTiming {
            now: self.now,
            never_set_start_pos: self.never_set_start_pos,
        };
        let archetypes = &self.archetypes;
        let lookup = |id: ArchetypeId| {
            archetypes
                .iter()
                .find(|(i, _)| *i == id && id != NO_ARCHETYPE)
                .map(|(_, a)| *a)
        };
        start_offset(timing, &lookup, &mut self.instances[index], source, backend)
    }

    /// `MixerTick` at tick `now` (milliseconds, the client's clock).
    pub fn tick(
        &mut self,
        now: u32,
        sources: &mut dyn SourceTable<K>,
        backend: &mut dyn VoiceBackend,
    ) {
        self.now = now;
        let mut i = 0;
        while i < self.instances.len() {
            if self.attached.contains(&self.instances[i].key) {
                i += 1;
            } else {
                self.release_at(i, backend);
            }
        }

        let attached = self.attached.clone();
        let mut created = 0u32;
        let mut pending = Vec::new();
        for (rank, &key) in attached.iter().enumerate() {
            let source = sources.source(key);
            let skip = created >= self.max_channels
                || !source.is_active()
                || self.one_shot_expired(now, source)
                || now < source.start_tick();
            let existing = self.instances.iter().position(|i| i.key == key);
            if skip {
                if let Some(index) = existing {
                    self.release_at(index, backend);
                }
                continue;
            }
            if let Some(index) = existing {
                if source.is_3d() {
                    self.instances[index].pose = self.listener.pose(
                        source.position(),
                        source.min_distance(),
                        source.max_distance(),
                    );
                }
                let instance = &self.instances[index];
                if source.is_3d()
                    && instance.caps.mute_at_max_distance
                    && !(instance.pose.dist_sq < instance.pose.max_dist_sq)
                {
                    self.release_at(index, backend);
                } else {
                    created += 1;
                    self.instances[index].rank = rank as u32;
                }
            } else if self.allow_create(source)
                && let Some(mut instance) = self.create_instance(key, source, backend)
            {
                created += 1;
                instance.rank = rank as u32;
                pending.push(instance);
            }
        }
        self.instances.append(&mut pending);

        for index in 0..self.instances.len() {
            let source = sources.source(self.instances[index].key);
            self.apply(index, source, backend);
        }

        for index in 0..self.instances.len() {
            let source = sources.source(self.instances[index].key);
            let looping = source.is_looping();
            if self.set_start_offset(index, source, backend)
                && backend.play(self.instances[index].voice, looping)
            {
                self.instances[index].started = true;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Clone, Debug)]
    struct Src {
        active: bool,
        start: u32,
        looping: bool,
        spatial: bool,
        arch: ArchetypeId,
        atten: f32,
        pitch: Option<f32>,
        pan: Option<i32>,
        pos: Option<[f32; 3]>,
        max: Option<f32>,
        restamps: u32,
    }

    impl Src {
        fn flat(arch: ArchetypeId) -> Src {
            Src {
                active: true,
                start: 0,
                looping: true,
                spatial: false,
                arch,
                atten: 0.0,
                pitch: None,
                pan: Some(0),
                pos: None,
                max: None,
                restamps: 0,
            }
        }
    }

    impl SoundSource for Src {
        fn restamp(&mut self) {
            self.restamps += 1;
        }
        fn start_tick(&mut self) -> u32 {
            self.start
        }
        fn is_active(&self) -> bool {
            self.active
        }
        fn is_3d(&self) -> bool {
            self.spatial
        }
        fn is_looping(&self) -> bool {
            self.looping
        }
        fn archetype(&self) -> ArchetypeId {
            self.arch
        }
        fn attenuation(&self) -> f32 {
            self.atten
        }
        fn pitch(&self) -> Option<f32> {
            self.pitch
        }
        fn pan(&self) -> Option<i32> {
            self.pan
        }
        fn position(&self) -> Option<[f32; 3]> {
            self.pos
        }
        fn min_distance(&self) -> Option<f32> {
            Some(10.0)
        }
        fn max_distance(&self) -> Option<f32> {
            self.max
        }
    }

    struct Table(Vec<Src>);

    impl SourceTable<usize> for Table {
        fn source(&mut self, key: usize) -> &mut dyn SoundSource {
            &mut self.0[key]
        }
    }

    #[derive(Default)]
    struct Log(Vec<String>, Vec<VoiceId>);

    impl VoiceBackend for Log {
        fn create(&mut self, voice: VoiceId, archetype: ArchetypeId, _: &Archetype) -> bool {
            self.0.push(format!("create {voice} {archetype}"));
            true
        }
        fn release(&mut self, voice: VoiceId) {
            self.0.push(format!("release {voice}"));
        }
        fn set_pan(&mut self, voice: VoiceId, pan: i32) {
            self.0.push(format!("pan {voice} {pan}"));
        }
        fn set_volume(&mut self, voice: VoiceId, volume: i32) {
            self.0.push(format!("volume {voice} {volume}"));
        }
        fn set_frequency(&mut self, voice: VoiceId, frequency: u32) {
            self.0.push(format!("freq {voice} {frequency}"));
        }
        fn set_position(&mut self, voice: VoiceId, byte: u32) {
            self.0.push(format!("pos {voice} {byte}"));
        }
        fn set_emitter(&mut self, voice: VoiceId, _: &Listener, emitter: &Emitter) {
            let [x, y, z] = emitter.position;
            self.0.push(format!("emitter {voice} {x} {y} {z}"));
        }
        fn is_playing(&mut self, voice: VoiceId) -> bool {
            !self.1.contains(&voice)
        }
        fn play(&mut self, voice: VoiceId, looping: bool) -> bool {
            self.0.push(format!("play {voice} {looping}"));
            true
        }
    }

    fn pcm(flags: u32) -> Archetype {
        let format = PcmFormat {
            channels: 1,
            bytes_per_sample: 2,
            rate: 22050,
            block_align: 2,
        };
        Archetype::from_pcm(format, 44100, 0, 22050, flags, false).unwrap()
    }

    #[test]
    fn archetype_flags_follow_fill_from_pcm() {
        let a = pcm(0x11);
        assert_eq!((a.frames, a.duration_ms), (22050, 1000));
        assert_eq!(a.buffer_flags, 0x40000 | 0x80 | 0x40);
        let a = pcm(0x09);
        assert_eq!(a.buffer_flags, 0x40000 | 0x80 | 0x10 | 0x20000);
        // 3D and pan: pan is dropped.
        let a = pcm(0x18);
        assert_eq!(a.flags, 0x08);
        let format = a.format;
        let a = Archetype::from_pcm(format, 44100, 0, 22050, 0x13, true).unwrap();
        assert_eq!(a.flags, 0x33);
        assert_eq!(a.buffer_flags, 0x08 | 0x80 | 0x40 | 0x20);
        // Loop notification only for a range inside the samples.
        assert_eq!(pcm(0x40).buffer_flags & 0x100, 0);
        let a = Archetype::from_pcm(format, 44100, 10, 22050, 0x40, false).unwrap();
        assert_eq!(a.buffer_flags & 0x10100, 0x10100);
        assert_eq!(
            Archetype::from_pcm(PcmFormat::default(), 4, 0, 0, 0, false),
            Err(ArchetypeError::ZeroBlockAlign)
        );
        let mut a = pcm(0);
        a.set_gain(3.0);
        assert_eq!(a.gain, 0.0);
        a.set_gain(-300.0);
        assert_eq!(a.gain, -100.0);
    }

    #[test]
    fn the_cap_plays_the_first_sources_in_rank_order() {
        let mut mixer = Mixer::new();
        let arch = mixer.insert_archetype(pcm(0x11));
        mixer.max_channels = 2;
        let mut table = Table(vec![Src::flat(arch); 4]);
        table.0[1].active = false;
        let mut log = Log::default();
        mixer.replace_attached(&[0, 1, 2, 3], &mut log);
        mixer.tick(100, &mut table, &mut log);
        // Source 1 is inactive; 0 and 2 take the two channels.
        let keys: Vec<usize> = mixer.instances().iter().map(|i| i.key).collect();
        assert_eq!(keys, [0, 2]);
        assert_eq!(mixer.instance(2).unwrap().rank, 2);
        assert_eq!(table.0[0].restamps, 1);
        // Started 100 ms ago: 2205 frames of 2 bytes.
        assert!(log.0.contains(&"pos 1 4410".to_owned()));
        assert!(log.0.contains(&"play 1 true".to_owned()));
        // Source 3 moves to the front: 0 loses its voice.
        log.0.clear();
        mixer.replace_attached(&[3, 2, 0], &mut log);
        mixer.tick(200, &mut table, &mut log);
        let keys: Vec<usize> = mixer.instances().iter().map(|i| i.key).collect();
        assert_eq!(keys, [2, 3]);
        assert_eq!(log.0[..2], ["create 3 1", "release 1"]);
        // Voice 2 keeps playing: same start tick, no new position.
        assert!(!log.0.iter().any(|l| l.starts_with("pos 2")));
        // An empty list releases everything at once.
        mixer.replace_attached(&[], &mut log);
        assert!(mixer.instances().is_empty());
    }

    #[test]
    fn one_shots_expire_and_future_starts_wait() {
        let mut mixer = Mixer::new();
        let arch = mixer.insert_archetype(pcm(0x11));
        mixer.max_channels = 8;
        let mut table = Table(vec![Src::flat(arch); 3]);
        table.0[0].looping = false;
        table.0[0].start = 0; // 1000 ms long, now 1001: expired.
        table.0[1].looping = false;
        table.0[1].start = 1; // ends exactly at 1001: not expired.
        table.0[2].start = 2000; // starts later.
        let mut log = Log::default();
        mixer.replace_attached(&[0, 1, 2], &mut log);
        mixer.tick(1001, &mut table, &mut log);
        let keys: Vec<usize> = mixer.instances().iter().map(|i| i.key).collect();
        assert_eq!(keys, [1]);
        // 1000 ms in: 22050 frames is the end, so nothing is played.
        assert!(!log.0.iter().any(|l| l.starts_with("play")));
    }

    #[test]
    fn spatial_sources_attenuate_pan_and_cull() {
        let mut mixer = Mixer::new();
        let pan_arch = mixer.insert_archetype(pcm(0x11));
        let spatial = mixer.insert_archetype(pcm(0x09));
        mixer.max_channels = 8;
        let mut near = Src::flat(pan_arch);
        near.spatial = true;
        near.pos = Some([20.0, 0.0, 0.0]);
        let mut far = near.clone();
        far.arch = spatial;
        far.pos = Some([0.0, 0.0, 400.0]);
        far.max = Some(300.0);
        let mut table = Table(vec![near, far]);
        let mut log = Log::default();
        mixer.replace_attached(&[0, 1], &mut log);
        mixer.tick(0, &mut table, &mut log);
        // The far one is out of its archetype's range: not created.
        assert_eq!(mixer.instances().len(), 1);
        // Right of the listener (right = up x front = +x), at about twice
        // the minimum distance.
        let i = mixer.instance(0).unwrap();
        let dist = quake_distance(400.0);
        let x = (1.0 / f64::from(dist) * 20.0) as f32;
        assert_eq!(
            i.pan,
            crate::audio::mix::ftol32(f64::from(x) * 10000.0).cast_signed()
        );
        assert_eq!(i.pan, 10015);
        let atten = software_attenuation(10.0, dist, 1.0, 1.0, 0.0);
        assert_eq!(i.volume, volume_hundredths(0.0, 0.0, atten));
        assert_eq!(i.volume, -300);
        // Bring the far one in, then move it out: a 3D archetype's voice is
        // stopped beyond its maximum distance.
        table.0[1].pos = Some([0.0, 0.0, 100.0]);
        mixer.tick(10, &mut table, &mut log);
        assert!(mixer.instance(1).is_some());
        assert_eq!(mixer.instance(1).unwrap().pan, 0);
        table.0[1].pos = Some([0.0, 0.0, 300.0]);
        mixer.tick(20, &mut table, &mut log);
        assert!(mixer.instance(1).is_none());
        // A 3D archetype's voice is the device's to spatialise: its pose
        // goes to the emitter, its volume has no distance attenuation and
        // it gets no pan.
        table.0[1].pos = Some([50.0, 0.0, 0.0]);
        log.0.clear();
        mixer.tick(30, &mut table, &mut log);
        let far = mixer.instance(1).unwrap().voice;
        assert!(log.0.contains(&format!("emitter {far} 50 0 0")));
        assert!(!log.0.iter().any(|l| l.starts_with(&format!("pan {far} "))));
        assert_eq!(mixer.instance(1).unwrap().volume, 0);
        // The software path, as the oracle harness runs it: distance
        // attenuation in the volume, still no pan (a 3D buffer has no pan
        // control), no emitter.
        mixer.spatial = Spatial::Software;
        log.0.clear();
        mixer.tick(40, &mut table, &mut log);
        assert!(!log.0.iter().any(|l| l.starts_with("emitter")));
        assert!(!log.0.iter().any(|l| l.starts_with(&format!("pan {far} "))));
        let atten = software_attenuation(10.0, quake_distance(2500.0), 1.0, 1.0, 0.0);
        assert_eq!(
            mixer.instance(1).unwrap().volume,
            volume_hundredths(0.0, 0.0, atten)
        );
    }

    #[test]
    fn pitch_sets_frequency_and_its_absence_disables_it() {
        let mut mixer = Mixer::new();
        let arch = mixer.insert_archetype(pcm(0x13));
        mixer.max_channels = 1;
        let mut table = Table(vec![Src::flat(arch)]);
        let mut log = Log::default();
        mixer.replace_attached(&[0], &mut log);
        mixer.tick(0, &mut table, &mut log);
        // No pitch at the first start: frequency control is off for good.
        assert!(!mixer.instance(0).unwrap().frequency_control);
        table.0[0].pitch = Some(-100.0);
        log.0.clear();
        mixer.tick(10, &mut table, &mut log);
        assert!(!log.0.iter().any(|l| l.starts_with("freq")));
        // A new instance with a pitch gets its frequency every tick.
        mixer.replace_attached(&[], &mut log);
        mixer.replace_attached(&[0], &mut log);
        log.0.clear();
        mixer.tick(20, &mut table, &mut log);
        assert!(log.0.contains(&"freq 2 11025".to_owned()));
        // Started 20 ms ago at half speed: 10 ms of samples.
        assert!(log.0.contains(&"pos 2 440".to_owned()));
    }

    #[test]
    fn restarted_sources_reposition_and_replay_when_stopped() {
        let mut mixer = Mixer::new();
        let arch = mixer.insert_archetype(pcm(0x11));
        mixer.max_channels = 1;
        let mut table = Table(vec![Src::flat(arch)]);
        let mut log = Log::default();
        mixer.replace_attached(&[0], &mut log);
        mixer.tick(0, &mut table, &mut log);
        table.0[0].start = 50;
        log.0.clear();
        mixer.tick(60, &mut table, &mut log);
        // Repositioned, but still playing: no second play.
        assert_eq!(log.0, ["pos 1 440"]);
        table.0[0].start = 55;
        log.1.push(1);
        log.0.clear();
        mixer.tick(60, &mut table, &mut log);
        assert_eq!(log.0, ["pos 1 220", "play 1 true"]);
    }
}
