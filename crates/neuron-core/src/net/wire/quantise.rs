//! The lossy field encodings of the C++ codecs, bit-exact (Tier A).
//!
//! Every quantiser in `RemoteCommon/` is x87 code of one shape: `FLD` the
//! `f32`, `FMUL` by a `double` constant (some first by a `float`), then
//! `FISTP qword` under the truncating rounding mode and keep the low byte
//! or word. The expanders `FILD` the sign- or zero-extended integer,
//! `FMUL` by a separate reciprocal constant and `FSTP` an `f32`. Under the
//! 53-bit precision of decision T11 each x87 step rounds exactly like the
//! `f64` operation here; the conversion to an integer is
//! [`trunc_to_i64`], whose "integer indefinite" result for NaN and out of
//! range values has a zero low byte and word, as on x86.
//!
//! | Encoding | C++ | Range |
//! | --- | --- | --- |
//! | [`quantise_unit`] / [`expand_unit`] | `RpcServerQuantize127`, `RpcUpdateQuantize` | quaternion components, throttle, attitude: signed byte × 127 |
//! | [`quantise_fraction`] / [`expand_fraction`] | `RpcServerQuantize255` | equipment and collision-group health: unsigned byte × 255 |
//! | [`quantise_tenths`] / [`expand_tenths`] | `RpcServerSendCommQuantize` | `SENDCOMM` +0x70: × 0.1f × 255, back × 2/51 |
//! | [`quantise_hull`] / [`expand_hull`] | `RpcServerQuantizeHull` | hit points as a fraction of the archetype's maximum × 255 |
//! | [`fire_tier`], [`put_fire_component`], [`get_fire_component`] | `RpcFirePickTier`, `RpcFirePutComponent`, `RpcFireGetComponent` | `FIREWEAPON` target: byte, word or full `f32` per axis |

use super::{WireError, WireReader, WireWriter};
use crate::num::{trunc_to_i32_via_i64, trunc_to_i64};

/// Scale of the signed unit byte (PE `06c0df40`).
pub const UNIT_SCALE: f64 = 127.0;
/// Its expander, a separate constant (`080402814020803f`).
pub const UNIT_INVERSE: f64 = 1.0 / 127.0;
/// Scale of the unsigned fraction byte (RS `06c0e270`).
pub const FRACTION_SCALE: f64 = 255.0;
/// Its expander (`101010101010703f`).
pub const FRACTION_INVERSE: f64 = 1.0 / 255.0;
/// The `float` factor applied before [`FRACTION_SCALE`] by
/// [`quantise_tenths`] (RS `06c0e278`, `cdcccc3d`).
pub const TENTH: f32 = 0.1;
/// The expander of [`quantise_tenths`], `2/51` (`141414141414a43f`).
pub const TENTHS_INVERSE: f64 = 2.0 / 51.0;
/// A target component below this magnitude takes the byte tier.
pub const FIRE_SMALL_LIMIT: f64 = 64.0;
/// A target component below this magnitude takes the word tier.
pub const FIRE_MEDIUM_LIMIT: f64 = 8192.0;
/// Pre-scale of the byte tier, `1/64` (`0000803c`).
pub const FIRE_SMALL_PRESCALE: f32 = 1.0 / 64.0;
/// Pre-scale of the word tier, `1/8192` (`00000039`).
pub const FIRE_MEDIUM_PRESCALE: f32 = 1.0 / 8192.0;
/// Scale of the byte tier.
pub const FIRE_SMALL_SCALE: f64 = 127.0;
/// Scale of the word tier.
pub const FIRE_MEDIUM_SCALE: f64 = 32767.0;
/// Expander of the byte tier, `64/127` (`080402814020e03f`).
pub const FIRE_SMALL_INVERSE: f64 = 64.0 / 127.0;
/// Expander of the word tier, `8192/32767` (`800040002000d03f`).
pub const FIRE_MEDIUM_INVERSE: f64 = 8192.0 / 32767.0;

/// The maximum hit points `Archetype::GetArchMaxHitPts` reports for an
/// archetype it does not know.
pub const UNKNOWN_ARCHETYPE_MAX_HIT_POINTS: i32 = 1;

fn low_byte(value: i64) -> u8 {
    value.to_le_bytes()[0]
}

fn low_word(value: i64) -> u16 {
    let [a, b, ..] = value.to_le_bytes();
    u16::from_le_bytes([a, b])
}

fn signed(byte: u8) -> i8 {
    i8::from_le_bytes([byte])
}

/// `value × 127`, truncated, low byte. Values in `[-1, 1]` give the signed
/// bytes `-127..=127`.
#[must_use]
pub fn quantise_unit(value: f32) -> u8 {
    low_byte(trunc_to_i64(f64::from(value) * UNIT_SCALE))
}

/// The byte as a signed integer × `1/127`, rounded to `f32`.
#[must_use]
pub fn expand_unit(byte: u8) -> f32 {
    (f64::from(signed(byte)) * UNIT_INVERSE) as f32
}

/// `value × 255`, truncated, low byte.
#[must_use]
pub fn quantise_fraction(value: f32) -> u8 {
    low_byte(trunc_to_i64(f64::from(value) * FRACTION_SCALE))
}

/// The byte, zero-extended, × `1/255`, rounded to `f32`.
#[must_use]
pub fn expand_fraction(byte: u8) -> f32 {
    (f64::from(byte) * FRACTION_INVERSE) as f32
}

/// `value × 0.1f × 255`, truncated, low byte: `[0, 10]` in 255 steps.
#[must_use]
pub fn quantise_tenths(value: f32) -> u8 {
    low_byte(trunc_to_i64(
        f64::from(value) * f64::from(TENTH) * FRACTION_SCALE,
    ))
}

/// The byte, zero-extended, × `2/51`, rounded to `f32`.
#[must_use]
pub fn expand_tenths(byte: u8) -> f32 {
    (f64::from(byte) * TENTHS_INVERSE) as f32
}

/// The hull byte of `CREATESHIP`/`CREATESOLAR`: the hit points rounded to
/// `f32`, divided by the archetype's maximum, × 255, truncated, low byte.
/// A zero maximum gives an infinity or NaN and so the byte 0.
#[must_use]
pub fn quantise_hull(hit_points: i32, max_hit_points: i32) -> u8 {
    let points = hit_points as f32;
    low_byte(trunc_to_i64(
        f64::from(points) / f64::from(max_hit_points) * FRACTION_SCALE,
    ))
}

/// The hit points a hull byte stands for: the maximum × (byte × `1/255`),
/// truncated to 64 bits, low 32 bits.
#[must_use]
pub fn expand_hull(byte: u8, max_hit_points: i32) -> i32 {
    trunc_to_i32_via_i64(f64::from(max_hit_points) * (f64::from(byte) * FRACTION_INVERSE))
}

/// How one `FIREWEAPON` target component travels.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum FireTier {
    /// The `f32` itself.
    Full,
    /// One byte, `|value| < 64`.
    Small,
    /// One word, `|value| < 8192`.
    Medium,
}

impl FireTier {
    /// The two flag bits of the tier: 1 for [`FireTier::Small`], 2 for
    /// [`FireTier::Medium`].
    #[must_use]
    pub const fn bits(self) -> u8 {
        match self {
            Self::Full => 0,
            Self::Small => 1,
            Self::Medium => 2,
        }
    }

    /// The tier two flag bits select. The decoder tests the small bit
    /// first, so both bits set read as [`FireTier::Small`].
    #[must_use]
    pub const fn from_bits(bits: u8) -> Self {
        if bits & 1 != 0 {
            Self::Small
        } else if bits & 2 != 0 {
            Self::Medium
        } else {
            Self::Full
        }
    }
}

/// The tier the encoder picks for a target component. `medium` is false
/// for `z`, which has only the byte tier. NaN compares unordered and
/// takes the full tier.
#[must_use]
pub fn fire_tier(value: f32, medium: bool) -> FireTier {
    let magnitude = f64::from(value.abs());
    if magnitude < FIRE_SMALL_LIMIT {
        FireTier::Small
    } else if medium && magnitude < FIRE_MEDIUM_LIMIT {
        FireTier::Medium
    } else {
        FireTier::Full
    }
}

/// `value × 1/64f × 127`, truncated, low byte.
#[must_use]
pub fn quantise_fire_small(value: f32) -> u8 {
    low_byte(trunc_to_i64(
        f64::from(value) * f64::from(FIRE_SMALL_PRESCALE) * FIRE_SMALL_SCALE,
    ))
}

/// `value × 1/8192f × 32767`, truncated, low word.
#[must_use]
pub fn quantise_fire_medium(value: f32) -> u16 {
    low_word(trunc_to_i64(
        f64::from(value) * f64::from(FIRE_MEDIUM_PRESCALE) * FIRE_MEDIUM_SCALE,
    ))
}

/// The byte as a signed integer × `64/127`, rounded to `f32`.
#[must_use]
pub fn expand_fire_small(byte: u8) -> f32 {
    (f64::from(signed(byte)) * FIRE_SMALL_INVERSE) as f32
}

/// The word as a signed integer × `8192/32767`, rounded to `f32`.
#[must_use]
pub fn expand_fire_medium(word: u16) -> f32 {
    (f64::from(i16::from_le_bytes(word.to_le_bytes())) * FIRE_MEDIUM_INVERSE) as f32
}

/// Writes one target component in `tier`; the full tier moves the float
/// through the x87 stack ([`WireWriter::put_f32_x87`]).
///
/// # Errors
/// [`WireError::Overflow`] when the field passes the writer's limit.
pub fn put_fire_component(
    writer: &mut WireWriter,
    value: f32,
    tier: FireTier,
) -> Result<(), WireError> {
    match tier {
        FireTier::Small => writer.put_u8(quantise_fire_small(value)),
        FireTier::Medium => writer.put_u16(quantise_fire_medium(value)),
        FireTier::Full => writer.put_f32_x87(value),
    }
}

/// Reads one target component in `tier`; the full tier copies the bits.
///
/// # Errors
/// [`WireError::Underrun`] when the field is cut short.
pub fn get_fire_component(reader: &mut WireReader<'_>, tier: FireTier) -> Result<f32, WireError> {
    match tier {
        FireTier::Small => reader.get_u8().map(expand_fire_small),
        FireTier::Medium => reader.get_u16().map(expand_fire_medium),
        FireTier::Full => reader.get_f32(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn le(bits: u64) -> u64 {
        u64::from_be_bytes(bits.to_le_bytes())
    }

    #[test]
    fn constants_match_the_reference_image() {
        // The byte strings the C++ comments quote from the PE image.
        assert_eq!(UNIT_INVERSE.to_bits(), le(0x0804_0281_4020_803f));
        assert_eq!(FIRE_SMALL_INVERSE.to_bits(), le(0x0804_0281_4020_e03f));
        assert_eq!(FIRE_MEDIUM_INVERSE.to_bits(), le(0x8000_4000_2000_d03f));
        assert_eq!(FRACTION_INVERSE.to_bits(), le(0x1010_1010_1010_703f));
        assert_eq!(TENTHS_INVERSE.to_bits(), le(0x1414_1414_1414_a43f));
        assert_eq!(TENTH.to_bits(), 0x3dcc_cccd);
        assert_eq!(FIRE_SMALL_PRESCALE.to_bits(), 0x3c80_0000);
        assert_eq!(FIRE_MEDIUM_PRESCALE.to_bits(), 0x3900_0000);
    }

    #[test]
    fn unit_byte() {
        assert_eq!(quantise_unit(1.0), 127);
        assert_eq!(quantise_unit(-1.0), 0x81);
        assert_eq!(quantise_unit(0.999), 126);
        assert_eq!(quantise_unit(-0.0), 0);
        assert_eq!(quantise_unit(2.0), 254);
        assert_eq!(quantise_unit(f32::NAN), 0);
        assert_eq!(quantise_unit(f32::INFINITY), 0);
        assert_eq!(quantise_unit(1e30), 0);
        assert_eq!(expand_unit(127), 1.0);
        assert_eq!(expand_unit(0x81), -1.0);
        assert_eq!(expand_unit(0x80), (-128.0_f64 / 127.0) as f32);
    }

    #[test]
    fn fraction_and_tenths() {
        assert_eq!(quantise_fraction(1.0), 255);
        assert_eq!(quantise_fraction(0.5), 127);
        assert_eq!(quantise_fraction(-0.5), 0x81);
        assert_eq!(expand_fraction(255), 1.0);
        assert_eq!(quantise_tenths(10.0), 255);
        assert_eq!(quantise_tenths(1.0), 25);
        assert_eq!(expand_tenths(255), 10.0);
        assert_eq!(expand_tenths(51), 2.0);
    }

    #[test]
    fn hull_byte() {
        assert_eq!(quantise_hull(500, 1000), 127);
        assert_eq!(quantise_hull(1000, 1000), 255);
        assert_eq!(quantise_hull(1, 0), 0);
        assert_eq!(quantise_hull(0, 0), 0);
        assert_eq!(quantise_hull(2, 1), 254);
        // 16 777 217 rounds to 16 777 216 as an f32 before the divide.
        assert_eq!(quantise_hull(16_777_217, 16_777_216), 255);
        assert_eq!(expand_hull(255, 1000), 1000);
        assert_eq!(expand_hull(127, 1000), 498);
        assert_eq!(expand_hull(0, 1000), 0);
        assert_eq!(expand_hull(255, i32::MIN), i32::MIN);
    }

    #[test]
    fn fire_tiers() {
        assert_eq!(fire_tier(63.99, true), FireTier::Small);
        assert_eq!(fire_tier(-64.0, true), FireTier::Medium);
        assert_eq!(fire_tier(64.0, false), FireTier::Full);
        assert_eq!(fire_tier(8191.0, true), FireTier::Medium);
        assert_eq!(fire_tier(8192.0, true), FireTier::Full);
        assert_eq!(fire_tier(f32::NAN, true), FireTier::Full);
        assert_eq!(fire_tier(-0.0, false), FireTier::Small);
        for tier in [FireTier::Full, FireTier::Small, FireTier::Medium] {
            assert_eq!(FireTier::from_bits(tier.bits()), tier);
        }
        assert_eq!(FireTier::from_bits(3), FireTier::Small);
    }

    #[test]
    fn fire_components() {
        assert_eq!(quantise_fire_small(63.9), 126);
        assert_eq!(quantise_fire_small(-64.0), 0x81);
        assert_eq!(quantise_fire_medium(8191.0), 32763);
        assert_eq!(quantise_fire_medium(-8192.0), 0x8001);
        assert_eq!(expand_fire_small(127), 64.0);
        assert_eq!(expand_fire_medium(0x7fff), 8192.0);
        let mut writer = WireWriter::new();
        put_fire_component(&mut writer, 32.0, FireTier::Small).unwrap();
        put_fire_component(&mut writer, 4096.0, FireTier::Medium).unwrap();
        put_fire_component(&mut writer, f32::from_bits(0x7f80_0001), FireTier::Full).unwrap();
        let bytes = writer.into_bytes();
        assert_eq!(bytes, [63, 0xff, 0x3f, 1, 0, 0xc0, 0x7f]);
        let mut reader = WireReader::new(&bytes);
        let x = get_fire_component(&mut reader, FireTier::Small).unwrap();
        let y = get_fire_component(&mut reader, FireTier::Medium).unwrap();
        let z = get_fire_component(&mut reader, FireTier::Full).unwrap();
        assert_eq!(x, (63.0 * FIRE_SMALL_INVERSE) as f32);
        assert_eq!(y, (16383.0 * FIRE_MEDIUM_INVERSE) as f32);
        assert_eq!(z.to_bits(), 0x7fc0_0001);
        assert!(get_fire_component(&mut reader, FireTier::Small).is_err());
    }
}
