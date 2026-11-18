//! The audio settings of the engine INI: `[AudioProperties]` and
//! `[SoundManager]` as `FSoundManager::StartupDevice` reads them
//! (`CppSource/SoundManager/SoundManager.cpp`), and the streamer's buffer
//! length (`FSoundStreamer::InitializeDevice`).
//!
//! Integers go through `ParseIniUnsigned` (the same reading as the
//! renderer's [`profile_number`]); floats through `ParseIniFloat`: the
//! leading run of `0123456789eE.+-`, cut there when it is not empty, then
//! the Universal C Runtime `atof` the reference links. A key whose value
//! is empty keeps its default, because `GetString` returns the length.
//!
//! Every key is read; the ones that only steered hardware placement, EAX
//! passthrough or the 3D algorithm are kept as read but have no effect in
//! the port (decision T4): `use2DHW`, `use3DHW`, `useNoHW`,
//! `createAll2DInSoftware`, `ALLOW_HW_TO_SW`, `ALLOW_SW_TO_HW`,
//! `3D_SW_Algorithm` and `deviceID`. The reference reads a key again from
//! the previous key's value when the profile parser cannot be obtained;
//! the port always has the INI it was given (divergence D228).

use neuron_core::ini::{EngineIni, EngineSection};

use crate::render::ff::defaults::profile_number;

use super::mix::DEFAULT_MAX_3D_PAN;

/// The `StartupDevice` flags the client passes (`NeuronRuntime`): 2D and 3D
/// hardware, right-handed coordinates and the LIGHT algorithm.
pub const CLIENT_STARTUP_FLAGS: u32 = 0x113;

/// Bits of the manager's flag word (`FSoundManager::Flags`).
pub mod startup_flags {
    /// `use2DHW`.
    pub const HARDWARE_2D: u32 = 0x1;
    /// `use3DHW`.
    pub const HARDWARE_3D: u32 = 0x2;
    /// `useEAX`.
    pub const EAX: u32 = 0x4;
    /// `rightHandedCoordinates`.
    pub const RIGHT_HANDED: u32 = 0x10;
    /// `createAll2DInSoftware`.
    pub const ALL_2D_IN_SOFTWARE: u32 = 0x20;
    /// `3D_SW_Algorithm = USER`.
    pub const ALGORITHM_USER: u32 = 0x40;
    /// `3D_SW_Algorithm = BASIC`.
    pub const ALGORITHM_BASIC: u32 = 0x80;
    /// `3D_SW_Algorithm = LIGHT`.
    pub const ALGORITHM_LIGHT: u32 = 0x100;
    /// `3D_SW_Algorithm = FULL`.
    pub const ALGORITHM_FULL: u32 = 0x200;
    /// `3D_SW_Algorithm = NONE` as a startup flag.
    pub const ALGORITHM_NONE: u32 = 0x400;
    /// The four algorithm bits `StartupDevice` replaces.
    pub const ALGORITHM_MASK: u32 = 0x3C0;
    /// `useNoHW` as a startup flag.
    pub const NO_HARDWARE: u32 = 0x800;
}

/// `3D_SW_Algorithm`. Read and kept; the port has no 3D algorithm option
/// (decision T4).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Algorithm {
    /// `FULL`: HRTF, full quality.
    Full,
    /// `LIGHT`: HRTF, light.
    Light,
    /// `BASIC`: no virtualisation.
    Basic,
    /// `USER`: the system's choice.
    User,
    /// `NONE`: 3D buffers created with pan control instead.
    None,
    /// Any other name: the flags keep no algorithm bit.
    Unknown,
}

/// The reverb settings of `[AudioProperties]` (EAX 1.0-style reverb).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ReverbSettings {
    /// `reverbBaseEnvironment` (default 0).
    pub environment: i32,
    /// `reverbMasterVolume` (default 0).
    pub volume: f32,
    /// `reverbDecayTime` (default 0.1).
    pub decay_time: f32,
    /// `reverbDamping` (default 1).
    pub damping: f32,
    /// `reverbMix`, truncated to an integer (default 0). The reference
    /// stores it and never reads it.
    pub mix: i32,
}

/// The audio settings after `StartupDevice` read the INI.
#[derive(Clone, Debug, PartialEq)]
pub struct AudioConfig {
    /// The manager's flag word.
    pub flags: u32,
    /// `[AudioProperties]` reverb values.
    pub reverb: ReverbSettings,
    /// `deviceID`, at most 0x28 bytes (no device selection in the port).
    pub device_id: Vec<u8>,
    /// `3D_SW_Algorithm`.
    pub algorithm: Algorithm,
    /// `speakerConfiguration`, only when the key has a value (default 2
    /// once present): the reference's speaker table index, 1 to 10.
    pub speaker_configuration: Option<u32>,
    /// `maxSoundChannels` (default: the channels already set, 0 at start).
    pub max_sound_channels: u32,
    /// `ALLOW_HW_TO_SW`, kept only with hardware enabled.
    pub allow_hw_to_sw: bool,
    /// `ALLOW_SW_TO_HW`, kept only with hardware enabled.
    pub allow_sw_to_hw: bool,
    /// `FORCE_FREQ_CONTROL_TO_SW` (default on).
    pub force_frequency_to_software: bool,
    /// `NEVER_SET_START_POS` (default off).
    pub never_set_start_pos: bool,
    /// `DISPLAY_EXTRA_DEBUG_INFO` (default off).
    pub display_extra_debug: bool,
    /// `max3DPan` times 100, as a float (default 100, so 10000).
    pub max_3d_pan: f32,
}

/// `ParseIniFloat`: the leading run of `0123456789eE.+-`, cut when not
/// empty, then `atof`, stored as a float.
#[must_use]
pub fn profile_float(value: &[u8]) -> f32 {
    const SPAN: &[u8] = b"0123456789eE.+-";
    let value = value.split(|&b| b == 0).next().unwrap_or_default();
    let span = value.iter().take_while(|b| SPAN.contains(b)).count();
    let text = if span != 0 { &value[..span] } else { value };
    ucrt_atof(text) as f32
}

/// The Universal C Runtime `atof`: white space, a sign, then `inf`,
/// `infinity`, `nan` (with an optional parenthesised tail), a hex float or
/// a decimal float, correctly rounded; no number gives +0 (the sign is
/// not applied).
#[must_use]
pub fn ucrt_atof(text: &[u8]) -> f64 {
    let byte = |at: usize| text.get(at).copied().unwrap_or(0);
    let mut at = 0;
    while matches!(byte(at), b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
        at += 1;
    }
    let negative = byte(at) == b'-';
    if matches!(byte(at), b'-' | b'+') {
        at += 1;
    }
    let rest = &text[at.min(text.len())..];
    let starts =
        |word: &[u8]| rest.len() >= word.len() && rest[..word.len()].eq_ignore_ascii_case(word);
    let magnitude = if starts(b"inf") {
        Some(f64::INFINITY)
    } else if starts(b"nan") {
        Some(f64::NAN)
    } else if rest.len() > 2 && rest[0] == b'0' && matches!(rest[1], b'x' | b'X') {
        // Without a hex digit the number is the "0" before the x.
        Some(hex_float(&rest[2..]).unwrap_or(0.0))
    } else {
        decimal_float(rest)
    };
    match magnitude {
        Some(m) if negative => -m,
        Some(m) => m,
        None => 0.0,
    }
}

/// The longest decimal float at the start of `text`, correctly rounded;
/// `None` without a digit.
fn decimal_float(text: &[u8]) -> Option<f64> {
    let digits = |from: usize| {
        text[from..]
            .iter()
            .take_while(|b| b.is_ascii_digit())
            .count()
    };
    let int = digits(0);
    let mut end = int;
    let mut frac = 0;
    if text.get(end) == Some(&b'.') {
        frac = digits(end + 1);
        end += 1 + frac;
    }
    if int + frac == 0 {
        return None;
    }
    if matches!(text.get(end), Some(b'e' | b'E')) {
        let mut exp = end + 1;
        if matches!(text.get(exp), Some(b'+' | b'-')) {
            exp += 1;
        }
        let n = digits(exp);
        if n != 0 {
            end = exp + n;
        }
    }
    let mut number = String::with_capacity(end + 1);
    if int == 0 {
        number.push('0');
    }
    number.extend(text[..end].iter().map(|&b| char::from(b)));
    number.parse().ok()
}

/// A hex float after its `0x`: hex digits, an optional point and digits,
/// an optional binary exponent; `None` without a digit (the reference
/// then reads the `0`).
fn hex_float(text: &[u8]) -> Option<f64> {
    let mut at = 0;
    let mut mantissa: u64 = 0;
    let mut exponent: i64 = 0;
    let mut sticky = false;
    let mut any = false;
    let mut point = false;
    loop {
        let b = text.get(at).copied().unwrap_or(0);
        if b == b'.' && !point {
            point = true;
            at += 1;
            continue;
        }
        let Some(d) = char::from(b).to_digit(16) else {
            break;
        };
        any = true;
        if mantissa >> 59 == 0 {
            mantissa = mantissa << 4 | u64::from(d);
            if point {
                exponent -= 4;
            }
        } else {
            sticky |= d != 0;
            if !point {
                exponent += 4;
            }
        }
        at += 1;
    }
    if !any {
        return None;
    }
    if matches!(text.get(at), Some(b'p' | b'P')) {
        let mut e = at + 1;
        let negative = text.get(e) == Some(&b'-');
        if matches!(text.get(e), Some(b'+' | b'-')) {
            e += 1;
        }
        let start = e;
        let mut value: i64 = 0;
        while let Some(d) = text.get(e).filter(|b| b.is_ascii_digit()) {
            value = (value * 10 + i64::from(d - b'0')).min(1 << 20);
            e += 1;
        }
        if e > start {
            exponent += if negative { -value } else { value };
        }
    }
    Some(scale_binary(mantissa, sticky, exponent))
}

/// `(mantissa + sticky) * 2^exponent` rounded to nearest, ties to even.
fn scale_binary(mantissa: u64, sticky: bool, exponent: i64) -> f64 {
    if mantissa == 0 {
        return 0.0;
    }
    let shift = mantissa.leading_zeros();
    let m = mantissa << shift;
    // The value lies in [2^top, 2^(top + 1)).
    let mut top = exponent + 63 - i64::from(shift);
    if top > 1023 {
        return f64::INFINITY;
    }
    // Significant bits kept: 53 for a normal result, fewer for a subnormal.
    let keep = if top >= -1022 { 53 } else { 53 - (-1022 - top) };
    if keep <= 0 {
        // At most half the smallest subnormal: exactly half is a tie to 0.
        let above_half = top == -1075 && (m << 1 != 0 || sticky);
        return if above_half { f64::from_bits(1) } else { 0.0 };
    }
    let drop = 64 - keep as u32;
    let mut kept = m >> drop;
    let rest = m << (64 - drop);
    let half = 1u64 << 63;
    if rest > half || (rest == half && (sticky || kept & 1 == 1)) {
        kept += 1;
    }
    if top < -1022 {
        // A subnormal is kept * 2^-1074; a carry into bit 52 is the
        // smallest normal, which the same bits spell.
        return f64::from_bits(kept);
    }
    if kept == 1 << 53 {
        kept >>= 1;
        top += 1;
        if top > 1023 {
            return f64::INFINITY;
        }
    }
    f64::from_bits(((top + 1023) as u64) << 52 | (kept & ((1 << 52) - 1)))
}

fn value<'a>(section: Option<&'a EngineSection>, key: &str) -> Option<&'a [u8]> {
    // GetString copies at most 0x3FF bytes and returns the length; an empty
    // value counts as absent.
    let v = section?.value(key.as_bytes())?;
    let v = &v[..v.len().min(0x3FF)];
    let v = v.split(|&b| b == 0).next().unwrap_or_default();
    (!v.is_empty()).then_some(v)
}

fn int(section: Option<&EngineSection>, key: &str, default: u32) -> u32 {
    value(section, key).map_or(default, profile_number)
}

fn float(section: Option<&EngineSection>, key: &str, default: f32) -> f32 {
    value(section, key).map_or(default, profile_float)
}

impl AudioConfig {
    /// `StartupDevice`'s INI reads for the startup flags `flags`
    /// ([`CLIENT_STARTUP_FLAGS`] for the client). The algorithm comes from
    /// the startup flags unless the INI names one.
    #[must_use]
    pub fn read(ini: &EngineIni, flags: u32) -> AudioConfig {
        use startup_flags as f;
        let audio = ini.section(b"AudioProperties");
        let manager = ini.section(b"SoundManager");
        let mut flags = flags;
        if flags & f::NO_HARDWARE != 0 {
            flags &= !(f::HARDWARE_2D | f::HARDWARE_3D);
        }
        if int(audio, "useEAX", flags & f::EAX) != 0 {
            flags |= f::EAX;
        } else {
            flags &= !f::EAX;
        }
        let reverb = ReverbSettings {
            environment: int(audio, "reverbBaseEnvironment", 0).cast_signed(),
            volume: float(audio, "reverbMasterVolume", 0.0),
            decay_time: float(audio, "reverbDecayTime", 0.1),
            damping: float(audio, "reverbDamping", 1.0),
            mix: neuron_core::num::trunc_to_i32(f64::from(float(audio, "reverbMix", 0.0))),
        };
        let mut device_id = value(manager, "deviceID")
            .unwrap_or(b"{00000000-0000-0000-0000-000000000000}")
            .to_vec();
        device_id.truncate(0x28);

        // The reference also tests the masked bits for 0x400 (NONE), which
        // the mask excludes, so NONE never comes from the startup flags.
        let default_algorithm: &[u8] = match flags & f::ALGORITHM_MASK {
            0x200 => b"FULL",
            0x80 => b"BASIC",
            0x100 => b"LIGHT",
            _ => b"USER",
        };
        let mut name = value(manager, "3D_SW_Algorithm")
            .unwrap_or(default_algorithm)
            .to_vec();
        name.truncate(0x1F);
        flags &= !f::ALGORITHM_MASK;
        let algorithm = if name.eq_ignore_ascii_case(b"FULL") {
            flags |= f::ALGORITHM_FULL;
            Algorithm::Full
        } else if name.eq_ignore_ascii_case(b"LIGHT") {
            flags |= f::ALGORITHM_LIGHT;
            Algorithm::Light
        } else if name.eq_ignore_ascii_case(b"BASIC") {
            flags |= f::ALGORITHM_BASIC;
            Algorithm::Basic
        } else if name.eq_ignore_ascii_case(b"USER") {
            flags |= f::ALGORITHM_USER;
            Algorithm::User
        } else if name.eq_ignore_ascii_case(b"NONE") {
            Algorithm::None
        } else {
            Algorithm::Unknown
        };

        if int(manager, "useNoHW", flags & f::NO_HARDWARE) != 0 {
            flags &= !(f::HARDWARE_2D | f::HARDWARE_3D);
        }
        if int(manager, "rightHandedCoordinates", flags & f::RIGHT_HANDED) != 0 {
            flags |= f::RIGHT_HANDED;
        } else {
            flags &= !f::RIGHT_HANDED;
        }
        if int(
            manager,
            "createAll2DInSoftware",
            flags & f::ALL_2D_IN_SOFTWARE,
        ) != 0
        {
            flags |= f::ALL_2D_IN_SOFTWARE;
        } else {
            flags &= !f::ALL_2D_IN_SOFTWARE;
        }
        let speaker_configuration =
            value(manager, "speakerConfiguration").map(|_| int(manager, "speakerConfiguration", 2));
        let max_sound_channels = int(manager, "maxSoundChannels", 0);
        for (key, bit) in [("use2DHW", f::HARDWARE_2D), ("use3DHW", f::HARDWARE_3D)] {
            if int(manager, key, flags & bit) != 0 {
                flags |= bit;
            } else {
                flags &= !bit;
            }
        }
        let hardware = flags & (f::HARDWARE_2D | f::HARDWARE_3D) != 0;
        let allow_hw_to_sw = int(manager, "ALLOW_HW_TO_SW", 0) != 0 && hardware;
        let allow_sw_to_hw = int(manager, "ALLOW_SW_TO_HW", 0) != 0 && hardware;
        let max_3d_pan = int(manager, "max3DPan", 100).wrapping_mul(100);
        AudioConfig {
            flags,
            reverb,
            device_id,
            algorithm,
            speaker_configuration,
            max_sound_channels,
            allow_hw_to_sw,
            allow_sw_to_hw,
            force_frequency_to_software: int(manager, "FORCE_FREQ_CONTROL_TO_SW", 1) != 0,
            never_set_start_pos: int(manager, "NEVER_SET_START_POS", 0) != 0,
            display_extra_debug: int(manager, "DISPLAY_EXTRA_DEBUG_INFO", 0) != 0,
            max_3d_pan: max_3d_pan as f32,
        }
    }

    /// Whether `useEAX` is on: only then does the reference apply reverb.
    /// The port applies it either way (owner decision O26, divergence
    /// D224); the flag is kept for the client.
    #[must_use]
    pub fn use_eax(&self) -> bool {
        self.flags & startup_flags::EAX != 0
    }

    /// Whether coordinates are right-handed (the listener's Z scale is -1).
    #[must_use]
    pub fn right_handed(&self) -> bool {
        self.flags & startup_flags::RIGHT_HANDED != 0
    }
}

impl Default for AudioConfig {
    /// The settings with no INI keys and the client's startup flags.
    fn default() -> Self {
        AudioConfig::read(&EngineIni::new(), CLIENT_STARTUP_FLAGS)
    }
}

/// The streamer's buffer length in seconds (`FStreamerInitDesc::Gain`; the
/// client passes 0, which means 1).
#[must_use]
pub fn stream_buffer_seconds(requested: f32) -> f32 {
    if requested == 0.0 { 1.0 } else { requested }
}

/// [`DEFAULT_MAX_3D_PAN`] is what `max3DPan`'s default of 100 gives.
const _: () = assert!(DEFAULT_MAX_3D_PAN == 10000.0);

#[cfg(test)]
mod tests {
    use super::*;
    use neuron_core::ini::NoIncludes;

    fn ini(text: &str) -> EngineIni {
        let mut ini = EngineIni::new();
        ini.load_buffer(text.as_bytes(), &mut NoIncludes).unwrap();
        ini
    }

    #[test]
    fn shipped_section() {
        let shipped = std::fs::read(concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../outdir/NeuronComponents.ini"
        ))
        .unwrap();
        let mut parsed = EngineIni::new();
        parsed.load_buffer(&shipped, &mut NoIncludes).unwrap();
        let c = AudioConfig::read(&parsed, CLIENT_STARTUP_FLAGS);
        assert_eq!(c.algorithm, Algorithm::Light);
        assert_eq!(c.max_sound_channels, 24);
        assert_eq!(c.max_3d_pan, 1500.0);
        assert!(!c.force_frequency_to_software);
        assert!(c.right_handed());
        assert!(!c.use_eax());
        assert_eq!(c.speaker_configuration, None);
        assert_eq!(c.flags, 0x113 | 0x20);
        assert_eq!(c.reverb.decay_time, 0.1);
    }

    #[test]
    fn defaults_and_overrides() {
        let c = AudioConfig::default();
        assert_eq!(c.flags, 0x113);
        assert_eq!(c.max_3d_pan, DEFAULT_MAX_3D_PAN);
        assert!(c.force_frequency_to_software);
        assert_eq!(c.device_id, b"{00000000-0000-0000-0000-000000000000}");
        let c = AudioConfig::read(
            &ini(
                "[AudioProperties]\nuseEAX = yes\nreverbBaseEnvironment = 0x10\n\
                  reverbMasterVolume = 0.5\nreverbMix = -2.75\nreverbDamping =\n\
                  [SoundManager]\n3D_SW_Algorithm = none\nuseNoHW = 1\n\
                  rightHandedCoordinates = false\nspeakerConfiguration = 7\nmax3DPan = 15x\n",
            ),
            CLIENT_STARTUP_FLAGS,
        );
        assert!(c.use_eax());
        assert_eq!(c.reverb.environment, 16);
        assert_eq!(c.reverb.volume, 0.5);
        assert_eq!(c.reverb.mix, -2);
        assert_eq!(c.reverb.damping, 1.0);
        assert_eq!(c.algorithm, Algorithm::None);
        // No algorithm bit, no hardware, left-handed.
        assert_eq!(c.flags, 0x04);
        assert_eq!(c.speaker_configuration, Some(7));
        // "15x" holds an x: read as hex.
        assert_eq!(c.max_3d_pan, 2100.0);
    }

    #[test]
    fn floats_read_as_the_runtime_reads_them() {
        assert_eq!(profile_float(b"1.5"), 1.5);
        assert_eq!(profile_float(b"2.5dB"), 2.5);
        assert_eq!(profile_float(b".5"), 0.5);
        assert_eq!(profile_float(b"5."), 5.0);
        assert_eq!(profile_float(b"1e3"), 1000.0);
        assert_eq!(profile_float(b"1e"), 1.0);
        assert_eq!(profile_float(b"1e+"), 1.0);
        assert_eq!(profile_float(b"-.25e1x"), -2.5);
        assert_eq!(profile_float(b"+-1"), 0.0);
        assert_eq!(profile_float(b"."), 0.0);
        assert_eq!(profile_float(b"abc"), 0.0);
        assert_eq!(profile_float(b"0x10"), 0.0);
        assert_eq!(profile_float(b"inf"), f32::INFINITY);
        assert_eq!(profile_float(b"Infinity"), f32::INFINITY);
        // The span stops after the sign, and atof("-") converts nothing: +0.
        assert_eq!(profile_float(b"-Infinity").to_bits(), 0);
        assert_eq!(ucrt_atof(b"-0").to_bits(), (-0.0_f64).to_bits());
        assert_eq!(ucrt_atof(b"-0xg").to_bits(), (-0.0_f64).to_bits());
        assert!(profile_float(b"nan(1)").is_nan());
        assert_eq!(profile_float(b"1e400"), f32::INFINITY);
        assert_eq!(profile_float(b" 0x1.8p1"), 3.0);
        assert_eq!(ucrt_atof(b"0x1p-1074"), f64::from_bits(1));
        assert_eq!(ucrt_atof(b"0x1p-1076"), 0.0);
        assert_eq!(ucrt_atof(b"0x1.8p-1075"), f64::from_bits(1));
        assert_eq!(ucrt_atof(b"0x1p1024"), f64::INFINITY);
        assert_eq!(ucrt_atof(b"0x1.fffffffffffff8p0"), 2.0);
        assert_eq!(
            ucrt_atof(b"0x1.fffffffffffff7p0"),
            f64::from_bits(0x3FFF_FFFF_FFFF_FFFF)
        );
        assert_eq!(ucrt_atof(b"0xg"), 0.0);
        assert_eq!(stream_buffer_seconds(0.0), 1.0);
        assert_eq!(stream_buffer_seconds(0.5), 0.5);
    }
}
