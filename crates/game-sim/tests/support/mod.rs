//! Helpers shared by the oracle and property tests of `game-sim`. The C++
//! harness (`CppSource/Oracle/oracle_protocol*.cpp`) writes the same lines
//! from the same synthetic values.
#![allow(dead_code)]

pub mod fill;

use std::fmt::Write as _;
use std::fs;
use std::path::Path;

/// Compares `lines` with the committed suite `tests/vectors/<name>`, or
/// writes it when `OUTPOST_BLESS_VECTORS` is set.
pub fn check_suite(name: &str, lines: &[String]) {
    let path = Path::new(env!("CARGO_MANIFEST_DIR"))
        .join("tests/vectors")
        .join(name);
    let rendered: String = lines.iter().map(|l| format!("{l}\n")).collect();
    if std::env::var_os("OUTPOST_BLESS_VECTORS").is_some() {
        fs::write(&path, rendered).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
        return;
    }
    let committed = fs::read_to_string(&path).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
    let committed: Vec<&str> = committed.lines().collect();
    for (index, (want, got)) in committed.iter().zip(lines).enumerate() {
        assert_eq!(*want, got, "{name} line {}", index + 1);
    }
    assert_eq!(committed.len(), lines.len(), "{name}: line count");
}

/// Lower-case hex of `bytes`.
pub fn hex(bytes: &[u8]) -> String {
    bytes.iter().fold(String::new(), |mut out, b| {
        let _ = write!(out, "{b:02x}");
        out
    })
}

/// Lower-case hex of UTF-16 units, four digits each.
pub fn wide_hex(units: &[u16]) -> String {
    units.iter().fold(String::new(), |mut out, u| {
        let _ = write!(out, "{u:04x}");
        out
    })
}

/// The bytes a hex string spells.
pub fn unhex(text: &str) -> Vec<u8> {
    (0..text.len())
        .step_by(2)
        .map(|i| u8::from_str_radix(&text[i..i + 2], 16).unwrap_or_else(|e| panic!("{text}: {e}")))
        .collect()
}

/// The synthetic value source both sides share: Marsaglia's 32-bit
/// xorshift (13, 17, 5). `OracleGen` in `oracle_protocol.h` is the same.
#[derive(Clone, Debug)]
pub struct Gen {
    state: u32,
}

impl Gen {
    /// The generator of case `case` of packet `id`.
    pub fn for_case(id: u32, case: u32) -> Self {
        let seed = id.wrapping_mul(2_654_435_761) ^ case.wrapping_add(1).wrapping_mul(0x9e37_79b9);
        Self::new(seed)
    }

    /// A generator from a seed; 0 becomes 1.
    pub fn new(seed: u32) -> Self {
        Self {
            state: if seed == 0 { 1 } else { seed },
        }
    }

    /// The next value.
    pub fn u32(&mut self) -> u32 {
        let mut x = self.state;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        self.state = x;
        x
    }

    /// The low byte of the next value.
    pub fn u8(&mut self) -> u8 {
        self.u32().to_le_bytes()[0]
    }

    /// The low word of the next value.
    pub fn u16(&mut self) -> u16 {
        let [a, b, ..] = self.u32().to_le_bytes();
        u16::from_le_bytes([a, b])
    }

    /// The next value as an `i32`.
    pub fn i32(&mut self) -> i32 {
        i32::from_le_bytes(self.u32().to_le_bytes())
    }

    /// A finite float: the next value's bits, with exponent bit 30
    /// cleared when the exponent is all ones.
    pub fn f32(&mut self) -> f32 {
        let mut bits = self.u32();
        if bits & 0x7f80_0000 == 0x7f80_0000 {
            bits &= 0xbfff_ffff;
        }
        f32::from_bits(bits)
    }

    /// A unit value: `(next % 257 - 128) / 128`, exact.
    pub fn unit(&mut self) -> f32 {
        let step = i32::try_from(self.u32() % 257).unwrap_or(0) - 128;
        step as f32 * 0.007_812_5
    }

    /// A printable byte, never 0: `0x21 + next % 0x5e`.
    pub fn text_byte(&mut self) -> u8 {
        0x21 + u8::try_from(self.u32() % 0x5e).unwrap_or(0)
    }

    /// A letter-range unit, never 0: `0x41 + next % 58` (`A` to `z`).
    pub fn text_unit(&mut self) -> u16 {
        0x41 + u16::try_from(self.u32() % 58).unwrap_or(0)
    }

    /// `count` bytes.
    pub fn bytes(&mut self, count: usize) -> Vec<u8> {
        (0..count).map(|_| self.u8()).collect()
    }

    /// `count` words.
    pub fn words(&mut self, count: usize) -> Vec<u16> {
        (0..count).map(|_| self.u16()).collect()
    }

    /// `count` dwords.
    pub fn dwords(&mut self, count: usize) -> Vec<u32> {
        (0..count).map(|_| self.u32()).collect()
    }

    /// A vector of three floats.
    pub fn vector(&mut self) -> [f32; 3] {
        [self.f32(), self.f32(), self.f32()]
    }

    /// A quaternion of four floats, `w`, `x`, `y`, `z`.
    pub fn quaternion(&mut self) -> [f32; 4] {
        [self.f32(), self.f32(), self.f32(), self.f32()]
    }

    /// Four unit values, `w`, `x`, `y`, `z`.
    pub fn unit_quaternion(&mut self) -> [f32; 4] {
        [self.unit(), self.unit(), self.unit(), self.unit()]
    }
}

#[test]
fn generator_reference_values() {
    // The first values of seed 1, as the C++ generator produces them.
    let mut g = Gen::new(1);
    assert_eq!(g.u32(), 270_369);
    assert_eq!(g.u32(), 67_634_689);
}
