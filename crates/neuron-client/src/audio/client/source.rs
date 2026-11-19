//! The client's play keys (`Sound::Source` and `Sound::Source3D` of
//! `Client\cmn\Sound.cpp`, ported in
//! `CppSource/OutpostFrontier/Client/menu_sound_arch.cpp`): a source holds a
//! client archetype, its start tick, attenuation, pan and pitch, and for a
//! positioned source the position, velocity and cone. The mixer asks it the
//! questions of `ISoundSource` ([`SoundSource`]).
//!
//! The reference answers each question through the archetype and the
//! client's globals at the moment it is asked. The port takes those
//! answers once per call into the sound manager ([`SourceView`], refreshed
//! by [`super::ClientSound`]); nothing it holds changes inside one
//! manager call.

use neuron_core::num::trunc_to_i32_via_i64;

use super::config::SILENCE_DB;
use crate::audio::mixer::{ArchetypeId, NO_ARCHETYPE, SoundSource};

/// A client archetype: a `[Sound]` (its nickname id) or a line of a voice
/// profile (`Archetype::ISound`'s two implementations).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ArchRef {
    /// A `[Sound]` archetype by nickname id.
    Sound(u32),
    /// A voice line: the profile's id and the line's id.
    Voice {
        /// `CreateID` of the profile's nickname.
        profile: u32,
        /// `CreateID` of the line's `msg`.
        msg: u32,
    },
}

impl ArchRef {
    /// The dword after the archetype's table pointer (`ArchIdentity`): the
    /// nickname id of a `[Sound]`, the line id of a voice line.
    #[must_use]
    pub fn identity(self) -> u32 {
        match self {
            ArchRef::Sound(id) => id,
            ArchRef::Voice { msg, .. } => msg,
        }
    }
}

/// Bits of a source's flag byte (`+0x0c`).
pub mod source_flags {
    /// Looping.
    pub const LOOP: u8 = 0x01;
    /// The cockpit (interior) mix.
    pub const COCKPIT: u8 = 0x02;
    /// Played by the sound manager.
    pub const MANAGED: u8 = 0x04;
    /// Started (the start tick latched by slot 3).
    pub const STARTED: u8 = 0x08;
    /// The start tick is a clock value (latched from an offset).
    pub const TICK_LATCHED: u8 = 0x10;
}

/// The source categories (`+0x04`): 0 sfx, 1 interface, 2 and 5 voice,
/// 3 music, 4 ambient.
pub mod category {
    /// `audio_sfx`.
    pub const SFX: i32 = 0;
    /// `audio_interface`.
    pub const INTERFACE: i32 = 1;
    /// `audio_voice`.
    pub const VOICE: i32 = 2;
    /// `audio_music`.
    pub const MUSIC: i32 = 3;
    /// `audio_ambient`.
    pub const AMBIENT: i32 = 4;
    /// `audio_voice`, ducked.
    pub const VOICE_DUCKED: i32 = 5;
}

/// `kMixerNoScore`: a source that is not ranked.
pub const NO_SCORE: f32 = f32::MAX;

/// The `Sound::Source3D` block (`+0x30..+0x64`).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Spatial3d {
    /// Position [0..2], cone direction [3..5], velocity [6..8], cone inner
    /// [9], outer [10], outside volume [11].
    pub block: [f32; 12],
    /// The attached engine instance, -1 for none.
    pub attach: i32,
}

impl Default for Spatial3d {
    /// The constructor's block: zeros, the cone direction's z 1, the outer
    /// cone 360.
    fn default() -> Self {
        let mut block = [0.0; 12];
        block[5] = 1.0;
        block[10] = 360.0;
        Spatial3d { block, attach: -1 }
    }
}

/// The answers a source takes from outside itself, for one manager call:
/// its archetype's slots, the category's master and mode terms, the
/// manager's clock and, for a positioned source, its world position and
/// velocity.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SourceView {
    /// Slot 0 of the archetype: the mixer archetype.
    pub sm_arch: ArchetypeId,
    /// Slot 6: the archetype's attenuation in decibels.
    pub arch_attenuation: i32,
    /// Slot 13.
    pub pitch_bendable: bool,
    /// Slot 7.
    pub range_min: f32,
    /// Slot 8 times the perf distance scale.
    pub range_max: f32,
    /// The mixer archetype's length in milliseconds, when it has one.
    pub duration_ms: Option<u32>,
    /// The category's master volume (with the ducking for the ducked
    /// categories) in decibels, as `SourceCategoryDb` sums it.
    pub base_db: f64,
    /// The category's master without the ducking (`ComputeMixerScore`'s
    /// float load) and whether it adds the ducking.
    pub master_db: f32,
    /// The ducking applies.
    pub ducked: bool,
    /// The ducking's current decibels.
    pub duck_db: f32,
    /// The audio mode's term.
    pub mode_db: f32,
    /// The manager's clock (`GetTickCountSlot`).
    pub clock: u32,
    /// The world position of a positioned source.
    pub position: [f32; 3],
    /// The world velocity of a positioned source.
    pub velocity: [f32; 3],
}

impl Default for SourceView {
    fn default() -> Self {
        SourceView {
            sm_arch: NO_ARCHETYPE,
            arch_attenuation: 0,
            pitch_bendable: false,
            range_min: 0.0,
            range_max: 0.0,
            duration_ms: None,
            base_db: 0.0,
            master_db: 0.0,
            ducked: false,
            duck_db: 0.0,
            mode_db: 0.0,
            clock: 0,
            position: [0.0; 3],
            velocity: [0.0; 3],
        }
    }
}

/// A play key (`Sound::Source`, `Sound::Source3D`).
#[derive(Clone, Debug, PartialEq)]
pub struct ClientSource {
    /// The category (`+0x04`, see [`category`]).
    pub category: i32,
    /// The flag byte (see [`source_flags`]). The reference keeps the top
    /// three bits a pool block held; the port starts them at zero
    /// (divergence D527).
    pub flags: u8,
    /// The pitch in percent (`+0x10`).
    pub pitch: f32,
    /// The start tick, or the offset it latches from (`+0x14`).
    pub start_tick: i32,
    /// The archetype `Start` stored (the linked one).
    pub arch: Option<ArchRef>,
    /// The source's own attenuation in decibels (`+0x1c`).
    pub attenuation_db: f32,
    /// The pan in hundredths of a decibel (`+0x20`).
    pub pan: f32,
    /// The reference count (`+0x24`).
    pub(super) ref_count: i32,
    /// The mixer score (`+0x28`); higher ranks first.
    pub mixer_score: f32,
    /// In the playing list (`+0x2c` set).
    pub(super) playing: bool,
    /// The positioned block, for a `Sound::Source3D`.
    pub spatial: Option<Spatial3d>,
    /// The answers of this manager call.
    pub view: SourceView,
}

impl ClientSource {
    /// `FSoundSource(1)` or, with `spatial`, `FSpatialSource`.
    #[must_use]
    pub fn new(spatial: bool) -> ClientSource {
        ClientSource {
            category: 0,
            flags: source_flags::MANAGED,
            pitch: 0.0,
            start_tick: 0,
            arch: None,
            attenuation_db: 0.0,
            pan: 0.0,
            ref_count: 1,
            mixer_score: NO_SCORE,
            playing: false,
            spatial: spatial.then(Spatial3d::default),
            view: SourceView::default(),
        }
    }

    /// Slot 5: in the playing list.
    #[must_use]
    pub fn is_playing(&self) -> bool {
        self.playing
    }

    /// `SourceMarkStarted` (slot 3) at `clock`: once, latch an offset into
    /// a clock value and mark the source started.
    pub fn mark_started(&mut self, clock: u32) {
        if self.flags & source_flags::STARTED != 0 {
            return;
        }
        if self.flags & source_flags::MANAGED != 0 && self.flags & source_flags::TICK_LATCHED == 0 {
            self.start_tick = clock
                .wrapping_sub(self.start_tick.cast_unsigned())
                .cast_signed();
            self.flags |= source_flags::TICK_LATCHED;
        }
        self.flags |= source_flags::STARTED;
    }

    /// Slot 25 (`SetStartOffset`) at `clock`.
    pub fn set_start_offset(&mut self, offset: i32, clock: u32) {
        self.start_tick = if self.flags & source_flags::TICK_LATCHED != 0 {
            clock.wrapping_sub(offset.cast_unsigned()).cast_signed()
        } else {
            offset
        };
    }

    /// Slot 32 (`SetAttenuation`): clamped to [-96, 0]; a NaN is kept.
    pub fn set_attenuation(&mut self, db: f32) {
        // `clamp` keeps a NaN, as the reference's two jumps do.
        self.attenuation_db = db.clamp(SILENCE_DB, 0.0);
    }

    /// Slot 33 (`SetPan`): clamped to [-10000, 10000].
    pub fn set_pan(&mut self, pan: f32) {
        self.pan = pan.clamp(-10000.0, 10000.0);
    }

    /// Slot 38 (`SetPitch`): clamped to [-100, 100].
    pub fn set_pitch(&mut self, pitch: f32) {
        self.pitch = pitch.clamp(-100.0, 100.0);
    }

    /// Slot 34 (`SetLooping`).
    pub fn set_looping(&mut self, looping: bool) {
        self.flags = (self.flags & !source_flags::LOOP) | u8::from(looping);
    }

    /// Slot 35 of a positioned source: the world position (an attached
    /// source keeps it in its instance's frame, which `to_local` makes);
    /// nothing for a flat one.
    pub fn set_position(&mut self, world: [f32; 3], to_local: &dyn Fn(i32, [f32; 3]) -> [f32; 3]) {
        if let Some(s) = self.spatial.as_mut() {
            let p = if s.attach == -1 {
                world
            } else {
                to_local(s.attach, world)
            };
            s.block[..3].copy_from_slice(&p);
        }
    }

    /// Slot 36: the velocity of a positioned source.
    pub fn set_velocity(&mut self, velocity: [f32; 3]) {
        if let Some(s) = self.spatial.as_mut() {
            s.block[6..9].copy_from_slice(&velocity);
        }
    }

    /// Slot 37 (`SetPose`): the attached instance and the transform's
    /// position.
    pub fn set_pose(&mut self, attach: i32, position: [f32; 3]) {
        if let Some(s) = self.spatial.as_mut() {
            s.attach = attach;
            s.block[..3].copy_from_slice(&position);
        }
    }

    /// `SetClientSoundSourceCone`.
    pub fn set_cone(&mut self, inner: f32, outer: f32) {
        if let Some(s) = self.spatial.as_mut() {
            s.block[9] = inner;
            s.block[10] = outer;
        }
    }

    /// Slot 9 (`GetAttenuation`): the archetype's and the source's decibels
    /// plus the category's master and mode, clamped to [-96, 0]; 0 without
    /// an archetype.
    #[must_use]
    pub fn volume_db(&self) -> f32 {
        if self.arch.is_none() {
            return 0.0;
        }
        let v = &self.view;
        let total = (f64::from(v.arch_attenuation) + f64::from(self.attenuation_db))
            + (v.base_db + f64::from(v.mode_db));
        if total > 0.0 {
            0.0
        } else if total < f64::from(SILENCE_DB) {
            SILENCE_DB
        } else {
            total as f32
        }
    }

    /// Slot 10 (`GetPitch`): the pitch of a pitch-bendable archetype when
    /// it is not 0.
    #[must_use]
    pub fn pitch_value(&self) -> Option<f32> {
        (self.arch.is_some() && self.view.pitch_bendable && !(self.pitch == 0.0))
            .then_some(self.pitch)
    }

    /// Slot 30 (`SourceDurationMs`): the mixer archetype's length, divided
    /// by `2^(pitch/100)` for a pitched source; 0 without one.
    #[must_use]
    pub fn duration_ms(&self) -> u32 {
        if self.arch.is_none() || self.view.sm_arch == NO_ARCHETYPE {
            return 0;
        }
        let Some(mut result) = self.view.duration_ms else {
            return 0;
        };
        if let Some(pitch) = self.pitch_value() {
            let pitch = pitch.clamp(-100.0, 100.0);
            let adjusted = f64::from(result) / 2.0_f64.powf(f64::from(pitch) * f64::from(0.01_f32));
            result = trunc_to_i32_via_i64(adjusted).cast_unsigned();
        }
        result
    }

    /// Slot 29 (`SourceIsFinished`): a one-shot the manager played whose
    /// start tick plus its length is behind the manager's clock. A looping
    /// source never finishes; a source without an archetype or mixer
    /// archetype has.
    #[must_use]
    pub fn is_finished(&self) -> bool {
        if self.flags & source_flags::LOOP != 0 {
            return false;
        }
        if self.arch.is_some() {
            if self.flags & source_flags::STARTED == 0 || self.flags & source_flags::MANAGED == 0 {
                return false;
            }
            if self.view.sm_arch != NO_ARCHETYPE {
                let end = self
                    .start_tick
                    .cast_unsigned()
                    .wrapping_add(self.duration_ms());
                return end < self.view.clock;
            }
        }
        true
    }

    /// Slot 31 (`ComputeMixerScore`) against the listener at `listener`.
    /// A source the categories silence (at or below -96 dB) is not ranked;
    /// a flat source scores 100 unless its own attenuation is at the floor;
    /// a positioned one `10 * log10(min / distance)`, unranked beyond its
    /// maximum distance.
    pub fn compute_score(&mut self, listener: [f32; 3]) {
        if self.arch.is_some() {
            let v = &self.view;
            let combined = (f64::from(v.arch_attenuation) + f64::from(self.attenuation_db)) as f32;
            let mut sum = f64::from(v.master_db);
            if v.ducked {
                sum += f64::from(v.duck_db);
            }
            sum = sum + f64::from(v.mode_db) + f64::from(combined);
            if sum <= f64::from(SILENCE_DB) {
                self.mixer_score = NO_SCORE;
                return;
            }
        }
        if self.spatial.is_none() {
            self.mixer_score = if self.attenuation_db <= SILENCE_DB {
                NO_SCORE
            } else {
                100.0
            };
            return;
        }
        let p = self.view.position;
        let dx = f64::from(listener[0]) - f64::from(p[0]);
        let dy = f64::from(listener[1]) - f64::from(p[1]);
        let dz = f64::from(listener[2]) - f64::from(p[2]);
        let dist_sq = (dz * dz + dy * dy + dx * dx) as f32;
        if self.arch.is_some() {
            let max = f64::from(self.view.range_max);
            if max * max < f64::from(dist_sq) {
                self.mixer_score = NO_SCORE;
                return;
            }
        }
        let min = if self.arch.is_some() {
            self.view.range_min
        } else {
            1.0
        };
        let bits = dist_sq.to_bits();
        let guess =
            f32::from_bits(0x5F37_59DF_u32.wrapping_sub((bits >> 1) | (bits & 0x8000_0000)));
        let g = f64::from(guess);
        let inv = (1.5 - ((g * g) * f64::from(dist_sq)) * 0.5) * g;
        self.mixer_score = ((inv * f64::from(min)).log10() * 10.0) as f32;
    }

    /// `Sound::Source3D::Update`'s integration: an attached source moves
    /// its offset by its velocity over `dt` (only the z step is stored as a
    /// float before it is added).
    pub fn integrate(&mut self, dt: f32) {
        if let Some(s) = self.spatial.as_mut()
            && s.attach != -1
        {
            let b = &mut s.block;
            let dt = f64::from(dt);
            let z_step = (dt * f64::from(b[8])) as f32;
            b[0] = (dt * f64::from(b[6]) + f64::from(b[0])) as f32;
            b[1] = (dt * f64::from(b[7]) + f64::from(b[1])) as f32;
            b[2] = (f64::from(z_step) + f64::from(b[2])) as f32;
        }
    }
}

impl SoundSource for ClientSource {
    fn restamp(&mut self) {
        self.mark_started(self.view.clock);
    }

    fn start_tick(&mut self) -> u32 {
        if self.flags & source_flags::TICK_LATCHED == 0 {
            self.mark_started(self.view.clock);
        }
        self.start_tick.cast_unsigned()
    }

    fn is_active(&self) -> bool {
        self.playing
    }

    fn is_3d(&self) -> bool {
        self.spatial.is_some()
    }

    fn is_looping(&self) -> bool {
        self.flags & source_flags::LOOP != 0
    }

    fn archetype(&self) -> ArchetypeId {
        self.view.sm_arch
    }

    fn attenuation(&self) -> f32 {
        self.volume_db()
    }

    fn pitch(&self) -> Option<f32> {
        self.pitch_value()
    }

    fn pan(&self) -> Option<i32> {
        Some(trunc_to_i32_via_i64(f64::from(self.pan)))
    }

    fn position(&self) -> Option<[f32; 3]> {
        self.spatial.map(|_| self.view.position)
    }

    fn min_distance(&self) -> Option<f32> {
        self.arch.map(|_| self.view.range_min)
    }

    fn max_distance(&self) -> Option<f32> {
        self.arch.map(|_| self.view.range_max)
    }

    fn velocity(&self) -> Option<[f32; 3]> {
        self.spatial.map(|_| self.view.velocity)
    }
}
