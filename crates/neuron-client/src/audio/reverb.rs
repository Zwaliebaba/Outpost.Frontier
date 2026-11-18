//! Reverb: the EAX preview settings of `FSoundManager::SetEaxPreview`
//! (`CppSource/SoundManager/SoundManager.cpp`) and their mapping, as data,
//! to the I3DL2 parameters an XAudio2 reverb takes (decision T4).
//!
//! Scene scripts set an environment, a volume, a decay time and a damping
//! (`reverbprops`), and `[AudioProperties]` gives the starting values. The
//! reference clamps them and, only when `useEAX` is on, hands them to the
//! sound card as an EAX 1.0-style reverb. The shipped configuration leaves
//! `useEAX` off; the port applies the reverb whatever `useEAX` says (owner
//! decision O26, divergence D224).
//!
//! The mapping is the port's (divergence D224): EAX environments 0 to 22
//! are the I3DL2 presets of the same names (`I3DL2_ENVIRONMENT_PRESET_*`
//! in the DirectX 8 SDK's `dsound.h`, which XAudio2 ships as
//! `XAUDIO2FX_I3DL2_PRESET_*`); 23 to 25 (drugged, dizzy, psychotic) have
//! no I3DL2 preset and take the generic one. The decay time replaces the
//! preset's, the damping is the high-frequency decay ratio, and the volume
//! sets the room level.

/// The environment count EAX 2.0 defines; larger ids become 0.
pub const ENVIRONMENT_COUNT: u32 = 26;

/// The reverb settings `SetEaxPreview` stores.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct EaxPreview {
    /// The environment, 0 to 25.
    pub environment: i32,
    /// The reverb volume, 0 to 1.
    pub volume: f32,
    /// The decay time in seconds, 0.1 to 100.
    pub decay_time: f32,
    /// The damping, 0 to 2.
    pub damping: f32,
}

/// One bound of `SetEaxPreview`: the float compared as a double with the
/// double bounds; below `lo` stores `lo`, above `hi` stores `hi`, NaN is
/// kept.
fn clamp_double(value: f32, lo: f64, hi: f64) -> f32 {
    let v = f64::from(value);
    if v < lo {
        lo as f32
    } else if v > hi {
        hi as f32
    } else {
        value
    }
}

impl EaxPreview {
    /// `SetEaxPreview`'s stores: an environment outside `0..26` (as an
    /// unsigned number) becomes 0, the others are clamped.
    #[must_use]
    pub fn clamped(environment: i32, volume: f32, decay_time: f32, damping: f32) -> EaxPreview {
        EaxPreview {
            environment: if environment.cast_unsigned() < ENVIRONMENT_COUNT {
                environment
            } else {
                0
            },
            volume: clamp_double(volume, 0.0, 1.0),
            decay_time: clamp_double(decay_time, 0.1, 100.0),
            damping: clamp_double(damping, 0.0, 2.0),
        }
    }
}

/// I3DL2 reverb parameters (`DSFXI3DL2Reverb`, `XAUDIO2FX_REVERB_I3DL2_PARAMETERS`
/// without the wet/dry mix).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct I3dl2Reverb {
    /// Room level, millibels.
    pub room: i32,
    /// Room high-frequency level, millibels.
    pub room_hf: i32,
    /// Room rolloff factor.
    pub room_rolloff_factor: f32,
    /// Decay time, seconds.
    pub decay_time: f32,
    /// High-frequency decay ratio.
    pub decay_hf_ratio: f32,
    /// Reflections level, millibels.
    pub reflections: i32,
    /// Reflections delay, seconds.
    pub reflections_delay: f32,
    /// Reverb level, millibels.
    pub reverb: i32,
    /// Reverb delay, seconds.
    pub reverb_delay: f32,
    /// Diffusion, percent.
    pub diffusion: f32,
    /// Density, percent.
    pub density: f32,
    /// High-frequency reference, hertz.
    pub hf_reference: f32,
}

// One argument per field, in the order of the SDK's preset macros.
#[allow(clippy::too_many_arguments)]
const fn preset(
    room: i32,
    room_hf: i32,
    room_rolloff_factor: f32,
    decay_time: f32,
    decay_hf_ratio: f32,
    reflections: i32,
    reflections_delay: f32,
    reverb: i32,
    reverb_delay: f32,
    diffusion: f32,
    density: f32,
    hf_reference: f32,
) -> I3dl2Reverb {
    I3dl2Reverb {
        room,
        room_hf,
        room_rolloff_factor,
        decay_time,
        decay_hf_ratio,
        reflections,
        reflections_delay,
        reverb,
        reverb_delay,
        diffusion,
        density,
        hf_reference,
    }
}

/// The I3DL2 presets for EAX environments 0 to 22, in EAX order
/// (`CppSource/DX8/include/dsound.h`, `I3DL2_ENVIRONMENT_PRESET_GENERIC`
/// to `_UNDERWATER`).
pub const I3DL2_PRESETS: [I3dl2Reverb; 23] = [
    preset(
        -1000, -100, 0.0, 1.49, 0.83, -2602, 0.007, 200, 0.011, 100.0, 100.0, 5000.0,
    ), // generic
    preset(
        -1000, -6000, 0.0, 0.17, 0.10, -1204, 0.001, 207, 0.002, 100.0, 100.0, 5000.0,
    ), // padded cell
    preset(
        -1000, -454, 0.0, 0.40, 0.83, -1646, 0.002, 53, 0.003, 100.0, 100.0, 5000.0,
    ), // room
    preset(
        -1000, -1200, 0.0, 1.49, 0.54, -370, 0.007, 1030, 0.011, 100.0, 60.0, 5000.0,
    ), // bathroom
    preset(
        -1000, -6000, 0.0, 0.50, 0.10, -1376, 0.003, -1104, 0.004, 100.0, 100.0, 5000.0,
    ), // living room
    preset(
        -1000, -300, 0.0, 2.31, 0.64, -711, 0.012, 83, 0.017, 100.0, 100.0, 5000.0,
    ), // stone room
    preset(
        -1000, -476, 0.0, 4.32, 0.59, -789, 0.020, -289, 0.030, 100.0, 100.0, 5000.0,
    ), // auditorium
    preset(
        -1000, -500, 0.0, 3.92, 0.70, -1230, 0.020, -2, 0.029, 100.0, 100.0, 5000.0,
    ), // concert hall
    preset(
        -1000, 0, 0.0, 2.91, 1.30, -602, 0.015, -302, 0.022, 100.0, 100.0, 5000.0,
    ), // cave
    preset(
        -1000, -698, 0.0, 7.24, 0.33, -1166, 0.020, 16, 0.030, 100.0, 100.0, 5000.0,
    ), // arena
    preset(
        -1000, -1000, 0.0, 10.05, 0.23, -602, 0.020, 198, 0.030, 100.0, 100.0, 5000.0,
    ), // hangar
    preset(
        -1000, -4000, 0.0, 0.30, 0.10, -1831, 0.002, -1630, 0.030, 100.0, 100.0, 5000.0,
    ), // carpeted hallway
    preset(
        -1000, -300, 0.0, 1.49, 0.59, -1219, 0.007, 441, 0.011, 100.0, 100.0, 5000.0,
    ), // hallway
    preset(
        -1000, -237, 0.0, 2.70, 0.79, -1214, 0.013, 395, 0.020, 100.0, 100.0, 5000.0,
    ), // stone corridor
    preset(
        -1000, -270, 0.0, 1.49, 0.86, -1204, 0.007, -4, 0.011, 100.0, 100.0, 5000.0,
    ), // alley
    preset(
        -1000, -3300, 0.0, 1.49, 0.54, -2560, 0.162, -613, 0.088, 79.0, 100.0, 5000.0,
    ), // forest
    preset(
        -1000, -800, 0.0, 1.49, 0.67, -2273, 0.007, -2217, 0.011, 50.0, 100.0, 5000.0,
    ), // city
    preset(
        -1000, -2500, 0.0, 1.49, 0.21, -2780, 0.300, -2014, 0.100, 27.0, 100.0, 5000.0,
    ), // mountains
    preset(
        -1000, -1000, 0.0, 1.49, 0.83, -10000, 0.061, 500, 0.025, 100.0, 100.0, 5000.0,
    ), // quarry
    preset(
        -1000, -2000, 0.0, 1.49, 0.50, -2466, 0.179, -2514, 0.100, 21.0, 100.0, 5000.0,
    ), // plain
    preset(
        -1000, 0, 0.0, 1.65, 1.50, -1363, 0.008, -1153, 0.012, 100.0, 100.0, 5000.0,
    ), // parking lot
    preset(
        -1000, -1000, 0.0, 2.81, 0.14, 429, 0.014, 648, 0.021, 80.0, 60.0, 5000.0,
    ), // sewer pipe
    preset(
        -1000, -4000, 0.0, 1.49, 0.10, -449, 0.007, 1700, 0.011, 100.0, 100.0, 5000.0,
    ), // underwater
];

/// The I3DL2 limits of the decay time and the high-frequency ratio.
const DECAY_TIME_RANGE: (f32, f32) = (0.1, 20.0);
const DECAY_HF_RATIO_RANGE: (f32, f32) = (0.1, 2.0);
/// The quietest I3DL2 level, millibels.
const LEVEL_MIN: i32 = -10000;

/// The I3DL2 reverb for a preview (divergence D224): the environment's
/// preset with the decay time replaced, the damping as the
/// high-frequency decay ratio (both within the I3DL2 ranges), and the
/// room level raised or lowered by the volume in decibels
/// (`2000 * log10(volume)` millibels; 0 is silent).
#[must_use]
pub fn reverb_parameters(preview: &EaxPreview) -> I3dl2Reverb {
    let index = usize::try_from(preview.environment).unwrap_or(0);
    let mut reverb = I3DL2_PRESETS
        .get(index)
        .copied()
        .unwrap_or(I3DL2_PRESETS[0]);
    let clamp = |v: f32, (lo, hi): (f32, f32)| if v.is_nan() { lo } else { v.clamp(lo, hi) };
    reverb.decay_time = clamp(preview.decay_time, DECAY_TIME_RANGE);
    reverb.decay_hf_ratio = clamp(preview.damping, DECAY_HF_RATIO_RANGE);
    reverb.room = if preview.volume > 0.0 {
        let offset = (2000.0 * f64::from(preview.volume).log10()).round();
        let level = f64::from(reverb.room) + offset;
        level.clamp(f64::from(LEVEL_MIN), 0.0) as i32
    } else {
        LEVEL_MIN
    };
    reverb
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn preview_clamps_as_the_reference() {
        let p = EaxPreview::clamped(26, -1.0, 0.05, 3.0);
        assert_eq!(p, EaxPreview::clamped(0, 0.0, 0.1, 2.0));
        assert_eq!(p.decay_time, 0.1_f32);
        assert_eq!(EaxPreview::clamped(-1, 2.0, 200.0, -1.0).environment, 0);
        let p = EaxPreview::clamped(25, 0.5, 1.5, 1.0);
        assert_eq!(
            (p.environment, p.volume, p.decay_time, p.damping),
            (25, 0.5, 1.5, 1.0)
        );
        // The bound is the double 0.1: 0.1f is above it and kept, the float
        // just below it is raised to 0.1f.
        let below = f32::from_bits(0.1_f32.to_bits() - 1);
        assert_eq!(EaxPreview::clamped(0, 0.0, below, 0.0).decay_time, 0.1_f32);
        assert!(EaxPreview::clamped(0, f32::NAN, 1.0, 1.0).volume.is_nan());
    }

    #[test]
    fn presets_map_by_environment() {
        let r = reverb_parameters(&EaxPreview::clamped(10, 1.0, 5.0, 0.5));
        assert_eq!(r.room, -1000);
        assert_eq!(r.room_hf, -1000);
        assert_eq!((r.decay_time, r.decay_hf_ratio), (5.0, 0.5));
        assert_eq!(r.reverb, 198);
        // Half volume is about -6 dB on the room level; silence at 0.
        assert_eq!(
            reverb_parameters(&EaxPreview::clamped(0, 0.5, 1.0, 1.0)).room,
            -1602
        );
        assert_eq!(
            reverb_parameters(&EaxPreview::clamped(0, 0.0, 1.0, 1.0)).room,
            -10000
        );
        // No I3DL2 preset for the last three: generic.
        let generic = reverb_parameters(&EaxPreview::clamped(0, 1.0, 1.0, 0.0));
        assert_eq!(
            reverb_parameters(&EaxPreview::clamped(24, 1.0, 1.0, 0.0)),
            generic
        );
        assert_eq!(generic.decay_hf_ratio, 0.1);
        assert_eq!(
            reverb_parameters(&EaxPreview::clamped(0, 1.0, 100.0, 1.0)).decay_time,
            20.0
        );
    }
}
