//! The client's audio configuration: `audio\SoundCFG.ini`'s `[CFG]`
//! (`LoadClientSoundCfgIni`, `CppSource/OutpostFrontier/Client/sound_config.cpp`),
//! the audio mode, and the options the sound code reads from the game's
//! option tables (`GetGameOption`, the perf options), which the game sets
//! (P5) and the engine holds at their first-run values (divergence D524).

// The reference's x87 comparisons send unordered operands (NaN) to one side;
// the port spells each outcome as the comparison that is true exactly when
// the reference takes that side, which needs negated comparisons.
#![allow(clippy::neg_cmp_op_on_partial_ord)]

use neuron_core::ini::IniReader;

/// The `[CFG]` values, with the reference's initial values for the keys a
/// file omits.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SoundConfig {
    /// `music_fade_time`.
    pub music_fade_time: f32,
    /// `cross_fade_silence`.
    pub cross_fade_silence: f32,
    /// `ducking_rtc_down_by`.
    pub rtc_down_by: f32,
    /// `ducking_rtc_down_time`.
    pub rtc_down_time: f32,
    /// `ducking_rtc_up_time`.
    pub rtc_up_time: f32,
    /// `ducking_spaceflight_down_by`.
    pub space_down_by: f32,
    /// `ducking_spaceflight_down_time`.
    pub space_down_time: f32,
    /// `ducking_spaceflight_up_time`.
    pub space_up_time: f32,
    /// `ducking_comm_down_by`.
    pub comm_down_by: f32,
    /// `ducking_comm_down_time`.
    pub comm_down_time: f32,
    /// `ducking_comm_up_time`.
    pub comm_up_time: f32,
    /// `spaceflight_dialogue_pan_range`.
    pub dialogue_pan_range: f32,
    /// `spaceflight_dialogue_pan_factor`.
    pub dialogue_pan_factor: f32,
    /// `COCKPIT_ATTENUATION`.
    pub cockpit_attenuation: f32,
    /// The audio mode (`SetClientAudioMode`; 3 selects the RTC ducking pair).
    pub audio_mode: i32,
}

impl Default for SoundConfig {
    fn default() -> Self {
        SoundConfig {
            music_fade_time: 3.0,
            cross_fade_silence: -96.0,
            rtc_down_by: -10.0,
            rtc_down_time: 0.25,
            rtc_up_time: 1.0,
            space_down_by: -6.0,
            space_down_time: 0.2,
            space_up_time: 0.75,
            comm_down_by: -6.0,
            comm_down_time: 0.25,
            comm_up_time: 0.25,
            dialogue_pan_range: 90.0,
            dialogue_pan_factor: 20.0,
            cockpit_attenuation: 0.0,
            audio_mode: 0,
        }
    }
}

/// What `[CFG]` hands the sound manager: `ear_doppler_factor` and
/// `ear_rolloff_factor`, applied in file order.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ListenerSetting {
    /// `SetDopplerFactor`.
    Doppler(f32),
    /// `SetRolloffFactor`.
    Rolloff(f32),
}

impl SoundConfig {
    /// `LoadClientSoundCfgIni`: the `[CFG]` keys over the current values,
    /// and the listener settings the file gives.
    pub fn load(&mut self, bytes: &[u8]) -> Vec<ListenerSetting> {
        let mut out = Vec::new();
        let mut reader = IniReader::new();
        if !reader.open_memory(bytes) {
            return out;
        }
        while reader.read_header() {
            if !reader.is_header(b"CFG") {
                continue;
            }
            while reader.read_value() {
                let slot = if reader.is_value(b"COCKPIT_ATTENUATION") {
                    &mut self.cockpit_attenuation
                } else if reader.is_value(b"ducking_rtc_down_by") {
                    &mut self.rtc_down_by
                } else if reader.is_value(b"ducking_rtc_down_time") {
                    &mut self.rtc_down_time
                } else if reader.is_value(b"ducking_rtc_up_time") {
                    &mut self.rtc_up_time
                } else if reader.is_value(b"ducking_spaceflight_down_by") {
                    &mut self.space_down_by
                } else if reader.is_value(b"ducking_spaceflight_down_time") {
                    &mut self.space_down_time
                } else if reader.is_value(b"ducking_spaceflight_up_time") {
                    &mut self.space_up_time
                } else if reader.is_value(b"ducking_comm_down_by") {
                    &mut self.comm_down_by
                } else if reader.is_value(b"ducking_comm_down_time") {
                    &mut self.comm_down_time
                } else if reader.is_value(b"ducking_comm_up_time") {
                    &mut self.comm_up_time
                } else if reader.is_value(b"music_fade_time") {
                    &mut self.music_fade_time
                } else if reader.is_value(b"cross_fade_silence") {
                    &mut self.cross_fade_silence
                } else if reader.is_value(b"spaceflight_dialogue_pan_range") {
                    &mut self.dialogue_pan_range
                } else if reader.is_value(b"spaceflight_dialogue_pan_factor") {
                    &mut self.dialogue_pan_factor
                } else if reader.is_value(b"ear_doppler_factor") {
                    out.push(ListenerSetting::Doppler(reader.value_float(0) as f32));
                    continue;
                } else if reader.is_value(b"ear_rolloff_factor") {
                    out.push(ListenerSetting::Rolloff(reader.value_float(0) as f32));
                    continue;
                } else {
                    continue;
                };
                *slot = reader.value_float(0) as f32;
            }
        }
        out
    }

    /// `GetClientDuckingConfig`: the ducking down-by and down-time of the
    /// audio mode (the RTC pair in mode 3).
    #[must_use]
    pub fn ducking(&self) -> (f32, f32) {
        if self.audio_mode == 3 {
            (self.rtc_down_by, self.rtc_down_time)
        } else {
            (self.space_down_by, self.space_down_time)
        }
    }
}

/// The game and perf options the sound code reads.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct SoundOptions {
    /// `GetGameOption(kGameOptAudioGroup)` ("[AUDIO]") is set.
    pub audio_group: bool,
    /// `GetGameOption(kGameOptPitchBendSounds)` is set.
    pub pitch_bend_sounds: bool,
    /// Perf slot 2, `sound_max_dist_scale`.
    pub max_distance_scale: f32,
    /// Perf slot 4, `USE_3D_SOUNDS`: below 1 every archetype is made with
    /// pan control.
    pub use_3d_sounds: f32,
    /// `max_submitted_sounds`.
    pub max_submitted_sounds: i32,
    /// `FORCE_FREQ_CONTROL_TO_SW` of `[SoundManager]`.
    pub force_frequency_to_software: bool,
}

impl Default for SoundOptions {
    /// The first-run values: the options' defaults and `optlist.ini`'s seeds
    /// of the perf options (100 % for the distance scale and 3D sounds, 32
    /// submitted sounds).
    fn default() -> Self {
        SoundOptions {
            audio_group: true,
            pitch_bend_sounds: true,
            max_distance_scale: 1.0,
            use_3d_sounds: 1.0,
            max_submitted_sounds: 32,
            force_frequency_to_software: true,
        }
    }
}

/// The silence floor every decibel path clamps to.
pub const SILENCE_DB: f32 = -96.0;

/// `LinearToOptionDb`: an `audio_*` slider (0 to 1) as `10 * log2(x)`
/// decibels, -96 at or below `2^-9.6`.
#[must_use]
pub fn linear_to_option_db(linear: f32) -> f32 {
    if !(linear > 0.001_288_6) {
        return SILENCE_DB;
    }
    (f64::from(linear).ln() * 14.426_950_454_711_914) as f32
}

/// The client's master fade (`GAudioFade`): the scene scripts' sounds and
/// the fade a room's entry snaps and raises (`SnapClientAudioFade`,
/// `SetClientAudioFade`, `TickClientAudioFade`).
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct AudioFade {
    /// `+0x10`.
    pub current: f32,
    /// The last reported (`+0x14`).
    pub last: f32,
    /// `+0x18`.
    pub target: f32,
    /// Decibels per second (`+0x1c`).
    pub rate: f32,
    /// `+0x20`.
    pub dirty: bool,
}

impl AudioFade {
    /// `SnapClientAudioFade`: at `value` at once; the last reported value
    /// is left.
    pub fn snap(&mut self, value: f32) {
        self.current = value;
        self.target = value;
        self.rate = 0.0;
        self.dirty = true;
    }

    /// `SetClientAudioFade`: to `target` over `seconds` (below 0, the
    /// mode's ducking down-time `down_time`); not above 0, at once (a rate
    /// of 1e6).
    pub fn set(&mut self, target: f32, seconds: f32, down_time: f32) {
        let seconds = if seconds < 0.0 { down_time } else { seconds };
        self.target = target;
        self.rate = if !(seconds > 0.0) {
            1_000_000.0
        } else {
            ((f64::from(target) - f64::from(self.current)).abs() / f64::from(seconds)) as f32
        };
    }

    /// `SetClientAudioFadeTargetRate`.
    pub fn set_target_rate(&mut self, target: f32, rate: f32) {
        self.target = target;
        self.rate = rate;
    }

    /// `TickClientAudioFade`: a step of `dt` toward the target, clamped at
    /// it. Whether the value changed since the last tick or was snapped.
    pub fn tick(&mut self, dt: f32) -> bool {
        if self.current != self.target && self.rate != 0.0 {
            let step = f64::from(dt) * f64::from(self.rate);
            if self.target < self.current {
                self.current = (f64::from(self.current) - step) as f32;
                if self.current < self.target {
                    self.current = self.target;
                }
            } else if self.target > self.current {
                self.current = (step + f64::from(self.current)) as f32;
                if self.target < self.current {
                    self.current = self.target;
                }
            }
        }
        let changed = self.current != self.last;
        let was_dirty = self.dirty;
        self.last = self.current;
        self.dirty = false;
        changed || was_dirty
    }
}

/// The power-ratio fade step of the ducking fade and the streamed fade
/// (`DbBlend`): both ends as power ratios, moved `dt / (remaining + dt)` of
/// the way, back to decibels; -96 at or below `10^-9.6`. The result stays
/// on the reference's FPU, so it is a double.
#[must_use]
pub fn db_blend(from_db: f32, to_db: f32, dt: f32, remaining: f64) -> f64 {
    let k = f64::from(0.230_258_51_f32);
    let from = (f64::from(from_db) * k).exp();
    let to = (f64::from(to_db) * k).exp();
    let value = (to - from) * (f64::from(dt) / (remaining + f64::from(dt))) + from;
    // The float 10^-9.6 (0x2f8a17b5).
    if !(value > f64::from(f32::from_bits(0x2F8A_17B5))) {
        return f64::from(SILENCE_DB);
    }
    value.log10() * 10.0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_cfg_keys_and_the_listener_settings() {
        let mut c = SoundConfig::default();
        let set = c.load(
            b"[CFG]\near_doppler_factor = 0.1\nducking_spaceflight_down_by = -9\n\
              cockpit_attenuation = -2\n[reverb]\nmusic_fade_time = 9\n",
        );
        assert_eq!(set, [ListenerSetting::Doppler(0.1)]);
        assert_eq!(c.space_down_by, -9.0);
        assert_eq!(c.cockpit_attenuation, -2.0);
        assert_eq!(c.music_fade_time, 3.0);
        assert_eq!(c.ducking(), (-9.0, 0.2));
    }

    #[test]
    fn the_master_fade() {
        let mut f = AudioFade::default();
        f.snap(-96.0);
        f.set(0.0, 3.0, 0.5);
        assert_eq!(f.rate, 32.0);
        assert!(f.tick(1.0));
        assert_eq!(f.current, -64.0);
        assert!(f.tick(3.0));
        assert_eq!(f.current, 0.0);
        assert!(!f.tick(1.0));
        // Below 0 s the down-time; not above 0 at once.
        f.set(-10.0, -1.0, 0.5);
        assert_eq!(f.rate, 20.0);
        f.set(-10.0, 0.0, 0.5);
        assert_eq!(f.rate, 1e6);
    }

    #[test]
    fn sliders_and_fades() {
        assert_eq!(linear_to_option_db(1.0), 0.0);
        assert_eq!(linear_to_option_db(0.0), -96.0);
        assert!((linear_to_option_db(0.5) + 10.0).abs() < 1e-5);
        assert_eq!(db_blend(0.0, -96.0, 1.0, 0.0), -96.0);
        let half = db_blend(0.0, -6.0, 0.5, 0.5);
        assert!(half < 0.0 && half > -6.0);
    }
}
