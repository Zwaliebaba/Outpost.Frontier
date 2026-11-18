//! The scalar arithmetic of the software mixer: distance, attenuation,
//! pan, pitch, frequency and volume, ported from the file-local helpers
//! and `MixerApplyInstance` of `CppSource/SoundManager/SoundManager.cpp`.
//!
//! The reference is x87 code. Each function evaluates in `f64` and rounds
//! to `f32` exactly where the C++ stores a `float` (decision T11).
//! [`software_attenuation`] (the reference's `fyl2x`) and
//! [`pitch_to_multiplier`] (the CRT `pow`) go through the Rust standard
//! library and are Tier B; everything else reproduces the reference bit
//! for bit.
//!
//! Units are the reference's: volumes and pans in hundredths of a
//! decibel (`-10000..=0` and `-10000..=10000`), attenuations and gains in
//! decibels, pitch in hundredths of an octave (`-100..=100`), frequencies
//! in hertz. The last section converts those to the linear factors an
//! XAudio2 voice takes.

use neuron_core::num::trunc_to_i32_via_i64;

/// The quietest volume, in hundredths of a decibel (`DSBVOLUME_MIN`).
pub const VOLUME_MIN: i32 = -10000;
/// The full volume (`DSBVOLUME_MAX`).
pub const VOLUME_MAX: i32 = 0;
/// The pan limit on either side (`DSBPAN_RIGHT`).
pub const PAN_LIMIT: i32 = 10000;
/// The lowest frequency the mixer sets (`DSBFREQUENCY_MIN`).
pub const FREQUENCY_MIN: u32 = 100;
/// The highest frequency the mixer sets (`DSBFREQUENCY_MAX`).
pub const FREQUENCY_MAX: u32 = 100_000;
/// The default minimum distance of a 3D source.
pub const DEFAULT_MIN_DISTANCE: f32 = 10.0;
/// The default maximum distance of a 3D source.
pub const DEFAULT_MAX_DISTANCE: f32 = 500.0;
/// The default of `max3DPan` scaled to hundredths (`Max3dPan` before
/// `StartupDevice` reads the key).
pub const DEFAULT_MAX_3D_PAN: f32 = 10000.0;

/// `Ftol32`: MSVCRT `_ftol`, truncation to a 64-bit integer of which the
/// caller keeps the low 32 bits.
#[must_use]
pub fn ftol32(value: f64) -> u32 {
    trunc_to_i32_via_i64(value).cast_unsigned()
}

/// `QuakeDistance`: the distance from its square through the bit-level
/// inverse square root with one Newton step,
/// `((1.5 - y*y*x*0.5) * y) * x` evaluated on the stack and stored once.
#[must_use]
pub fn quake_distance(dist_sq: f32) -> f32 {
    let bits = dist_sq.to_bits().cast_signed();
    let y = f32::from_bits(0x5F37_59DF_i32.wrapping_sub(bits >> 1).cast_unsigned());
    let y = f64::from(y);
    let x = f64::from(dist_sq);
    let half = ((y * y) * x) * f64::from(0.5_f32);
    (((f64::from(1.5_f32) - half) * y) * x) as f32
}

/// `SoftwareAttenuation`: the client attenuation plus the distance law
/// `10 * log10(min / (min + rolloff * (dist * distance_factor - min)))`,
/// in decibels. The reference calls it only beyond the minimum distance
/// and does not guard the logarithm's argument (a zero or negative ratio
/// gives `-inf` or NaN here as there). Tier B: the reference takes the
/// logarithm with `fyl2x`.
#[must_use]
pub fn software_attenuation(
    min_distance: f32,
    dist: f32,
    distance_factor: f32,
    rolloff_factor: f32,
    client_attenuation: f32,
) -> f32 {
    let min = f64::from(min_distance);
    let spread = (f64::from(dist) * f64::from(distance_factor) - min) * f64::from(rolloff_factor);
    let ratio = min / (spread + min);
    (ratio.log10() * 10.0 + f64::from(client_attenuation)) as f32
}

/// `PitchToMultiplier`: `2^(pitch / 100)` with the pitch clamped to
/// `-100..=100` first (a NaN pitch is not clamped). The product with
/// `0.01f` stays a double, as the result does. Tier B (CRT `pow`).
#[must_use]
pub fn pitch_to_multiplier(pitch: f32) -> f64 {
    // `clamp` keeps a NaN, as the reference's two jumps do.
    let pitch = pitch.clamp(-100.0, 100.0);
    2.0_f64.powf(f64::from(pitch) * f64::from(0.01_f32))
}

/// The frequency `MixerApplyInstance` sets for a pitch: the archetype's
/// rate times [`pitch_to_multiplier`], truncated, then clamped to
/// [`FREQUENCY_MIN`]`..=`[`FREQUENCY_MAX`] as an unsigned value.
#[must_use]
pub fn frequency_for_pitch(pitch: f32, rate: u16) -> u32 {
    let frequency = ftol32(pitch_to_multiplier(pitch) * f64::from(rate));
    frequency.clamp(FREQUENCY_MIN, FREQUENCY_MAX)
}

/// The volume `MixerApplyInstance` sets: the archetype gain, the master
/// gain and the attenuation summed in that order and times 100 on the
/// stack, clamped to `-10000..=0` (a NaN passes both tests), then
/// truncated.
#[must_use]
pub fn volume_hundredths(archetype_gain: f32, master_gain: f32, attenuation: f32) -> i32 {
    let volume =
        ((f64::from(archetype_gain) + f64::from(master_gain)) + f64::from(attenuation)) * 100.0;
    let volume = if volume > 0.0 {
        0.0
    } else if volume < f64::from(VOLUME_MIN) {
        f64::from(VOLUME_MIN)
    } else {
        volume
    };
    ftol32(volume).cast_signed()
}

/// The pan of a 2D source: the client's value clamped to `-10000..=10000`.
#[must_use]
pub fn clamp_pan_2d(pan: i32) -> i32 {
    pan.clamp(-PAN_LIMIT, PAN_LIMIT)
}

/// The listener's right vector as the software path computes it,
/// `up x front` on the stack (not rounded).
#[must_use]
pub fn listener_right(front: [f32; 3], up: [f32; 3]) -> [f64; 3] {
    let [fx, fy, fz] = front.map(f64::from);
    let [ux, uy, uz] = up.map(f64::from);
    [uy * fz - uz * fy, fx * uz - fz * ux, fy * ux - fx * uy]
}

/// The pan of a 3D source on the software path. `direction` (listener to
/// source) is scaled by `1 / dist` and stored back as floats, as the
/// reference stores it into the instance; the pan is
/// `((dz*rz + dy*ry) + dx*rx) * max_pan`, truncated with no clamp.
#[must_use]
pub fn pan_3d(
    direction: &mut [f32; 3],
    dist: f32,
    front: [f32; 3],
    up: [f32; 3],
    max_3d_pan: f32,
) -> i32 {
    let [rx, ry, rz] = listener_right(front, up);
    let inv = 1.0 / f64::from(dist);
    for d in direction.iter_mut() {
        *d = (inv * f64::from(*d)) as f32;
    }
    let [dx, dy, dz] = direction.map(f64::from);
    let dot = (dz * rz + dy * ry) + dx * rx;
    ftol32(dot * f64::from(max_3d_pan)).cast_signed()
}

/// `ScaledDurationMs`: an archetype's length in milliseconds at the
/// source's pitch. Unchanged without frequency control, when the source
/// has no pitch, or for a pitch of exactly zero (a NaN pitch goes on);
/// otherwise `float(duration) / multiplier`, truncated.
#[must_use]
pub fn scaled_duration_ms(duration_ms: u32, frequency_control: bool, pitch: Option<f32>) -> u32 {
    if !frequency_control {
        return duration_ms;
    }
    match pitch {
        Some(pitch) if pitch != 0.0 => {
            let duration = duration_ms as f32;
            ftol32(f64::from(duration) / pitch_to_multiplier(pitch))
        }
        _ => duration_ms,
    }
}

/// The start position, in frames, of a voice whose source started
/// `elapsed_ms` ago: `elapsed * 0.001f * rate`, truncated
/// (`MixerSetStartOffset`).
#[must_use]
pub fn elapsed_frames(elapsed_ms: u32, rate: u16) -> u32 {
    ftol32((f64::from(elapsed_ms) * f64::from(0.001_f32)) * f64::from(rate))
}

/// The elapsed time of a pitched voice in the archetype's own time,
/// `elapsed * multiplier`, truncated (`MixerSetStartOffset`).
#[must_use]
pub fn pitched_elapsed_ms(elapsed_ms: u32, multiplier: f64) -> u32 {
    ftol32(f64::from(elapsed_ms) * multiplier)
}

// --- Device units ---------------------------------------------------------
//
// The reference handed these numbers to its device, which interpreted them
// as documented for DirectSound buffers: a volume and a pan in hundredths
// of a decibel of attenuation, with the minimum volume silent, and a
// frequency in hertz. XAudio2 takes linear amplitudes and a frequency
// ratio; these convert. They are not reference arithmetic (divergence
// D229).

/// The linear amplitude of a volume in hundredths of a decibel; the
/// minimum volume is silence.
#[must_use]
pub fn volume_to_amplitude(volume: i32) -> f32 {
    if volume <= VOLUME_MIN {
        0.0
    } else {
        10.0_f64.powf(f64::from(volume.min(VOLUME_MAX)) / 2000.0) as f32
    }
}

/// The left and right amplitudes of a pan: a positive pan attenuates the
/// left channel by `pan` hundredths of a decibel, a negative one the
/// right; the limits silence that channel.
#[must_use]
pub fn pan_to_amplitudes(pan: i32) -> [f32; 2] {
    let pan = clamp_pan_2d(pan);
    let side = |attenuation: i32| {
        if attenuation >= PAN_LIMIT {
            0.0
        } else {
            10.0_f64.powf(-f64::from(attenuation) / 2000.0) as f32
        }
    };
    if pan > 0 {
        [side(pan), 1.0]
    } else {
        [1.0, side(-pan)]
    }
}

/// The frequency ratio of a voice playing an archetype of `rate` hertz at
/// `frequency` hertz.
#[must_use]
pub fn frequency_ratio(frequency: u32, rate: u16) -> f32 {
    if rate == 0 {
        1.0
    } else {
        (f64::from(frequency) / f64::from(rate)) as f32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn distance_is_close_to_the_square_root() {
        for dist_sq in [1.0_f32, 2.0, 100.0, 12345.0, 1.0e6] {
            let d = quake_distance(dist_sq);
            assert!(
                (d - dist_sq.sqrt()).abs() / dist_sq.sqrt() < 2.0e-3,
                "{dist_sq}: {d}"
            );
        }
        // Hand-evaluated: x = 4, y = bits 0x5F3759DF - 0x20400000 = 0x3EF759DF.
        let y = f64::from(f32::from_bits(0x3EF7_59DF));
        let want = (((1.5 - ((y * y) * 4.0) * 0.5) * y) * 4.0) as f32;
        assert_eq!(quake_distance(4.0).to_bits(), want.to_bits());
    }

    #[test]
    fn attenuation_follows_the_ten_log_law() {
        // At twice the minimum distance with unit factors: 10*log10(1/2).
        let a = software_attenuation(10.0, 20.0, 1.0, 1.0, 0.0);
        assert_eq!(a, (10.0 * 0.5_f64.log10()) as f32);
        // The client attenuation is added before the float store.
        let b = software_attenuation(10.0, 20.0, 1.0, 1.0, -3.0);
        assert_eq!(b, (10.0 * 0.5_f64.log10() - 3.0) as f32);
        // No guard: a ratio of zero is -inf.
        assert_eq!(
            software_attenuation(0.0, 5.0, 1.0, 1.0, 0.0),
            f32::NEG_INFINITY
        );
    }

    #[test]
    fn pitch_is_clamped_to_an_octave() {
        assert_eq!(pitch_to_multiplier(0.0), 1.0);
        assert_eq!(pitch_to_multiplier(250.0), pitch_to_multiplier(100.0));
        assert_eq!(pitch_to_multiplier(-250.0), pitch_to_multiplier(-100.0));
        assert!(pitch_to_multiplier(f32::NAN).is_nan());
        // 0.01f is slightly below 0.01, so +100 is slightly below an octave.
        assert!(pitch_to_multiplier(100.0) < 2.0);
        assert_eq!(frequency_for_pitch(0.0, 22050), 22050);
        assert_eq!(frequency_for_pitch(100.0, 60000), FREQUENCY_MAX);
        assert_eq!(frequency_for_pitch(-100.0, 150), FREQUENCY_MIN);
        // A NaN multiplier truncates to the indefinite value, whose low word is 0.
        assert_eq!(frequency_for_pitch(f32::NAN, 22050), FREQUENCY_MIN);
    }

    #[test]
    fn volume_is_clamped_in_hundredths() {
        assert_eq!(volume_hundredths(0.0, 0.0, 0.0), 0);
        assert_eq!(volume_hundredths(-3.0, -1.5, -0.25), -475);
        assert_eq!(volume_hundredths(5.0, 0.0, 0.0), 0);
        assert_eq!(volume_hundredths(-60.0, -60.0, 0.0), VOLUME_MIN);
        assert_eq!(volume_hundredths(f32::NAN, 0.0, 0.0), 0);
        // Truncation toward zero: -0.019 dB is -1.9 hundredths.
        assert_eq!(volume_hundredths(-0.019, 0.0, 0.0), -1);
    }

    #[test]
    fn pans() {
        assert_eq!(clamp_pan_2d(-20000), -PAN_LIMIT);
        assert_eq!(clamp_pan_2d(123), 123);
        // Listener facing +z with +y up: right is up x front = +x.
        let mut d = [3.0, 0.0, 4.0];
        let pan = pan_3d(&mut d, 5.0, [0.0, 0.0, 1.0], [0.0, 1.0, 0.0], 10000.0);
        assert_eq!(d, [0.6, 0.0, 0.8]);
        assert_eq!(pan, 6000);
        let mut d = [-5.0, 0.0, 0.0];
        assert_eq!(
            pan_3d(&mut d, 5.0, [0.0, 0.0, 1.0], [0.0, 1.0, 0.0], 1500.0),
            -1500
        );
    }

    #[test]
    fn durations_and_offsets() {
        assert_eq!(scaled_duration_ms(1000, false, Some(100.0)), 1000);
        assert_eq!(scaled_duration_ms(1000, true, None), 1000);
        assert_eq!(scaled_duration_ms(1000, true, Some(0.0)), 1000);
        assert_eq!(scaled_duration_ms(1000, true, Some(-100.0)), 1999);
        assert_eq!(scaled_duration_ms(1000, true, Some(f32::NAN)), 0);
        // 0.001f is slightly above 1/1000.
        assert_eq!(elapsed_frames(1000, 22050), 22050);
        assert_eq!(elapsed_frames(1, 44100), 44);
        assert_eq!(pitched_elapsed_ms(1000, 0.5), 500);
    }

    #[test]
    fn device_units() {
        assert_eq!(volume_to_amplitude(0), 1.0);
        assert_eq!(volume_to_amplitude(VOLUME_MIN), 0.0);
        assert!((volume_to_amplitude(-2000) - 0.1).abs() < 1e-7);
        assert_eq!(pan_to_amplitudes(0), [1.0, 1.0]);
        assert_eq!(pan_to_amplitudes(PAN_LIMIT), [0.0, 1.0]);
        assert_eq!(pan_to_amplitudes(-PAN_LIMIT), [1.0, 0.0]);
        let [l, r] = pan_to_amplitudes(2000);
        assert!((l - 0.1).abs() < 1e-7 && r == 1.0);
        assert_eq!(frequency_ratio(44100, 22050), 2.0);
        assert_eq!(frequency_ratio(44100, 0), 1.0);
    }
}
