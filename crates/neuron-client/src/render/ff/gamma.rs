//! The gamma ramp of `IGammaControl` (`FDirectX8::SetGammaFunction`,
//! `SetGammaRamp`, `GetGammaRamp`, `SetGammaEnable` and `GetGammaEnable` in
//! `CppSource/DX8RENDERER/DX8RENDERER.cpp`; record 0004, point 6).
//!
//! The ramp is kept here; the Direct3D 12 backend applies it in the final
//! blit to the swap chain, where Direct3D 8 had the hardware ramp. The
//! enable bit only became the ramp's `D3DSGR_CALIBRATE` flag in the C++,
//! which changes nothing without a calibrator, so the ramp applies whether
//! it is set or not.
//!
//! The C++ refuses these calls outside `CreateBuffers`/`DestroyBuffers`
//! (or `Startup`/`Shutdown`) and when the device lacks the gamma caps; the
//! model has neither lifecycle nor caps, so the calls always act.

use neuron_core::num;

/// Channels of a ramp call: red, green, blue in bits 0 to 2.
pub mod channel {
    pub const RED: u32 = 1;
    pub const GREEN: u32 = 2;
    pub const BLUE: u32 = 4;
}

/// A `D3DGAMMARAMP`: 256 words per channel.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct GammaRamp {
    pub red: [u16; 256],
    pub green: [u16; 256],
    pub blue: [u16; 256],
}

impl Default for GammaRamp {
    /// The identity ramp a device starts with.
    fn default() -> Self {
        let identity = std::array::from_fn(|i| (i as u16) * 257);
        GammaRamp {
            red: identity,
            green: identity,
            blue: identity,
        }
    }
}

/// The gamma state of a device.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Gamma {
    pub ramp: GammaRamp,
    /// `SetGammaEnable`'s bit.
    pub enabled: bool,
}

/// The ramp word `SetGammaFunction` computes for entry `index`: in the
/// C++'s order, with each `float` store rounded (decision T11).
#[must_use]
pub fn function_word(index: u32, gamma: f32, brightness: f32, contrast: f32, bias: f32) -> u16 {
    let inv_gamma = (1.0 / f64::from(gamma)) as f32;
    let x = (f64::from(index) / 255.0) as f32;
    let value = (f64::from(brightness)
        + (f64::from(contrast) * f64::from(x) + f64::from(bias)).powf(f64::from(inv_gamma)))
        as f32;
    // The C++'s two comparisons; NaN fails both and is kept, as `clamp`
    // keeps it.
    let value = value.clamp(0.0, 1.0);
    // `(long)` then `(WORD)`: truncation, the low word kept.
    num::trunc_to_i32(f64::from(value) * f64::from(65535.0_f32)) as u16
}

impl Gamma {
    /// `SetGammaFunction(mask, gamma, brightness, contrast, bias)`: the
    /// masked channels get the curve.
    pub fn set_function(
        &mut self,
        mask: u32,
        gamma: f32,
        brightness: f32,
        contrast: f32,
        bias: f32,
    ) {
        for index in 0..256u32 {
            let word = function_word(index, gamma, brightness, contrast, bias);
            let i = index as usize;
            if mask & channel::RED != 0 {
                self.ramp.red[i] = word;
            }
            if mask & channel::GREEN != 0 {
                self.ramp.green[i] = word;
            }
            if mask & channel::BLUE != 0 {
                self.ramp.blue[i] = word;
            }
        }
    }

    /// `SetGammaRamp(mask, data)`: the same 256 words into each masked
    /// channel.
    pub fn set_ramp(&mut self, mask: u32, data: &[u16; 256]) {
        if mask & channel::RED != 0 {
            self.ramp.red = *data;
        }
        if mask & channel::GREEN != 0 {
            self.ramp.green = *data;
        }
        if mask & channel::BLUE != 0 {
            self.ramp.blue = *data;
        }
    }

    /// `GetGammaRamp(channel)`: one channel's words; `None` for any other
    /// value than 1, 2 or 4 (the C++'s "bad gamma ramp requested").
    #[must_use]
    pub fn ramp(&self, which: u32) -> Option<&[u16; 256]> {
        match which {
            channel::RED => Some(&self.ramp.red),
            channel::GREEN => Some(&self.ramp.green),
            channel::BLUE => Some(&self.ramp.blue),
            _ => None,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn the_client_curve_is_a_power_law() {
        // The client calls SetGammaFunction(7, gamma, 0, 1, 0).
        let mut g = Gamma::default();
        g.set_function(7, 1.0, 0.0, 1.0, 0.0);
        // Gamma 1: x * 65535 truncated, x stored as a float.
        for i in 0..256u32 {
            let x = (f64::from(i) / 255.0) as f32;
            let want = (f64::from(x) * 65535.0) as u16;
            assert_eq!(g.ramp.red[i as usize], want, "{i}");
        }
        assert_eq!(g.ramp.green[255], 65535);
        g.set_function(channel::RED, 2.2, 0.0, 1.0, 0.0);
        // (128/255)^(1/2.2) = 0.7310...; the other channels stay linear.
        let mid = g.ramp.red[128];
        assert!((47_850..47_950).contains(&mid), "{mid}");
        assert_eq!(g.ramp.green[128], g.ramp.blue[128]);
        assert_ne!(g.ramp.red[128], g.ramp.green[128]);
    }

    #[test]
    fn values_clamp_and_nan_gives_the_low_word_of_indefinite() {
        // Brightness pushes everything above 1.
        assert_eq!(function_word(0, 1.0, 2.0, 1.0, 0.0), 65535);
        // A negative base with a fractional exponent: NaN, kept by the
        // clamps; (long)NaN is 0x80000000, whose low word is 0.
        assert_eq!(function_word(10, 2.0, 0.0, -1.0, 0.0), 0);
        // Negative results clamp to 0.
        assert_eq!(function_word(10, 1.0, -1.0, 1.0, 0.0), 0);
    }

    #[test]
    fn ramps_copy_into_masked_channels_and_read_back() {
        let mut g = Gamma::default();
        assert_eq!(g.ramp.red[255], 65535);
        let data = [7u16; 256];
        g.set_ramp(channel::GREEN | channel::BLUE, &data);
        assert_eq!(g.ramp(channel::GREEN), Some(&data));
        assert_eq!(g.ramp(channel::RED).unwrap()[1], 257);
        assert_eq!(g.ramp(3), None);
    }
}
