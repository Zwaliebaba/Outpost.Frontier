//! The interface palette and the hover pulse (`nn_ui_pulse.cpp`).
//!
//! Controls under the mouse pulse between their colour, a grey and an
//! amber: a phase runs up to 0.3 seconds and wraps, flipping a direction
//! byte at each wrap, and the colour is blended in two halves of the
//! period. `Win3DUpdateAll` advances it with the frame time.

/// The HUD chrome colour, bytes R, G, B, A (`G_HudChromeColorRgba`).
pub const HUD_CHROME_RGBA: [u8; 4] = [0x87, 0xC3, 0xE0, 0xC0];

/// The NeuroNet palette as little-endian `0xAABBGGRR` words, by the
/// reference's addresses.
pub mod palette {
    /// `679bb0`: idle cyan.
    pub const IDLE: u32 = 0xFFE0_C387;
    /// `679bac`: green.
    pub const GREEN: u32 = 0xFF1D_BF3B;
    /// `679ba8`: red.
    pub const RED: u32 = 0xFF1D_1DBF;
    /// `679ba4`: pulse amber.
    pub const AMBER: u32 = 0xFF52_EAF5;
    /// `679b9c`.
    pub const PURPLE: u32 = 0xFFC2_0088;
    /// `679b98`.
    pub const BLUE: u32 = 0xFFE0_4848;
    /// `679b94`: grey.
    pub const GREY: u32 = 0xFF80_8080;
    /// `679b90`.
    pub const MINT: u32 = 0xFFC5_FBA8;
    /// `679b88`.
    pub const PINK: u32 = 0xFFFD_7EFF;
    /// `679b84`: white.
    pub const WHITE: u32 = 0xFFFF_FFFF;
    /// `679b80`.
    pub const ORANGE: u32 = 0xFFFF_5ACE;
}

/// The period of a pulse half, 0.3 seconds (a `float`).
const PERIOD: f32 = 0.3;
/// The mid grey of `NnUiPulseBlendArgb`.
const MID_GREY: u32 = 0xFFB9_B9B9;

/// `BlendByte`: `to + ftol((from - to) * t)`.
fn blend_byte(from: u8, to: u8, t: f32) -> u8 {
    let diff = i32::from(from) - i32::from(to);
    let delta = neuron_core::num::trunc_to_i32(f64::from(diff) * f64::from(t));
    (i32::from(to).wrapping_add(delta)) as u8
}

/// `BlendArgb`: per byte.
#[must_use]
pub fn blend_argb(from: u32, to: u32, t: f32) -> u32 {
    let mut out = 0;
    for shift in [0, 8, 16, 24] {
        let b = blend_byte((from >> shift) as u8, (to >> shift) as u8, t);
        out |= u32::from(b) << shift;
    }
    out
}

/// The pulse's phase and direction (`GPulsePhase`, `GPulseDir`); both
/// start at 0.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Pulse {
    pub phase: f32,
    pub direction: u8,
}

impl Pulse {
    /// `NnUiPulseReset`: direction 0, phase half the period.
    pub fn reset(&mut self) {
        self.direction = 0;
        self.phase = (f64::from(PERIOD) * 0.5) as f32;
    }

    /// `NnUiPulseAdvance`: the frame time added; past the period it wraps,
    /// flipping the direction each time.
    pub fn advance(&mut self, delta_seconds: f64) {
        let mut phase = f64::from(self.phase) + delta_seconds;
        self.phase = phase as f32;
        if !(phase > f64::from(PERIOD)) {
            return;
        }
        let mut direction = self.direction;
        loop {
            phase -= f64::from(PERIOD);
            direction = u8::from(direction == 0);
            if !(phase > f64::from(PERIOD)) {
                break;
            }
        }
        self.phase = phase as f32;
        self.direction = direction;
    }

    /// `NnUiPulsePhaseNorm`.
    #[must_use]
    pub fn phase_norm(&self) -> f32 {
        (f64::from(self.phase) / f64::from(PERIOD)) as f32
    }

    /// The blend factor and which half of the period: `2 norm` in the
    /// first half, `2 (norm - 0.5)` in the second.
    fn half(&self) -> (bool, f32) {
        let norm = self.phase_norm();
        if norm < 0.5 {
            (true, (f64::from(norm) + f64::from(norm)) as f32)
        } else {
            let d = f64::from(norm) - 0.5;
            (false, (d + d) as f32)
        }
    }

    /// `NnUiPulseBlendArgb`: the palette pulse between the mid grey, the
    /// idle cyan and the amber.
    #[must_use]
    pub fn blend(&self) -> u32 {
        let (first, t) = self.half();
        let (from, to) = match (self.direction == 0, first) {
            (true, true) => (MID_GREY, palette::IDLE),
            (true, false) => (palette::AMBER, MID_GREY),
            (false, true) => (MID_GREY, palette::AMBER),
            (false, false) => (palette::IDLE, MID_GREY),
        };
        blend_argb(from, to, t)
    }

    /// `NnUiPulseBlendFromArgb`: the same pulse around `base` in place of
    /// the idle colour, with a grey from the base and amber midpoints in
    /// place of the mid grey.
    #[must_use]
    pub fn blend_from(&self, base: u32) -> u32 {
        let mut mid_sum = 0i32;
        for shift in [0, 8, 16] {
            let base_byte = ((base >> shift) & 0xFF) as i32;
            let hot = ((palette::AMBER >> shift) & 0xFF) as i32;
            let half = neuron_core::num::trunc_to_i32(f64::from(hot - base_byte) * -0.5);
            mid_sum += i32::from((base_byte - half) as u8);
        }
        let grey = ((mid_sum / 3) & 0xFF) as u32;
        let dim = grey | (grey << 8) | (grey << 16) | 0xFF00_0000;
        let (first, t) = self.half();
        let (from, to) = match (self.direction == 0, first) {
            (true, true) => (dim, base),
            (true, false) => (palette::AMBER, dim),
            (false, true) => (dim, palette::AMBER),
            (false, false) => (base, dim),
        };
        blend_argb(from, to, t)
    }
}

/// `NnReputationColorArgb`: red at or below -0.6, green at or above 0.6,
/// white between.
#[must_use]
pub fn reputation_color(attitude: f32) -> u32 {
    if attitude <= -0.6 {
        palette::RED
    } else if attitude <= -0.2 {
        palette::WHITE
    } else if attitude >= 0.6 {
        palette::GREEN
    } else {
        palette::WHITE
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_phase_wraps_and_flips() {
        let mut p = Pulse::default();
        p.reset();
        assert_eq!(p.phase, 0.15);
        p.advance(0.1);
        assert_eq!(p.direction, 0);
        p.advance(0.1);
        assert_eq!(p.direction, 1);
        assert!((p.phase - 0.05).abs() < 1e-6);
        p.advance(0.7);
        assert_eq!(p.direction, 1);
    }

    #[test]
    fn blends() {
        let mut p = Pulse::default();
        // Norm 0: the first half at t = 0 is the target, the idle colour.
        assert_eq!(p.blend(), palette::IDLE);
        assert_eq!(p.blend_from(palette::GREEN), palette::GREEN);
        // The middle (0.15f is exactly half of 0.3f): the second half at
        // t = 0, the grey.
        p.phase = 0.15;
        assert_eq!(p.blend(), MID_GREY);
        // Just before: t close to 1, each byte a truncated step short of
        // the grey.
        p.phase = 0.149_999;
        assert_eq!(p.blend(), 0xFFBA_BAB8);
        p.direction = 1;
        p.phase = 0.29;
        assert_eq!(p.blend() >> 24, 0xFF);
        assert_eq!(blend_argb(0xFF00_00FF, 0xFF00_0000, 0.5), 0xFF00_007F);
        assert_eq!(reputation_color(-0.7), palette::RED);
        assert_eq!(reputation_color(0.0), palette::WHITE);
        assert_eq!(reputation_color(0.6), palette::GREEN);
    }
}
