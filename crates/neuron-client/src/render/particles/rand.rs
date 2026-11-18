//! The module's random numbers (`ParticleFxRand01` in
//! `particlefx_common.cpp`): a shift register with a call counter, both 0
//! when the module loads. The emitters and the turbulence field draw from
//! it in the reference's order.

/// `kParticleFxRandScale`, `1/255` as a float (`0x3B808081`).
const SCALE: f32 = f32::from_bits(0x3B80_8081);

/// The generator state: the register and the call counter.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ParticleRand {
    /// `G_ParticleFxRandState`.
    pub state: u32,
    /// `G_ParticleFxRandCounter`.
    pub counter: u32,
}

impl ParticleRand {
    /// A generator with the given register and counter.
    #[must_use]
    pub const fn new(state: u32, counter: u32) -> Self {
        Self { state, counter }
    }

    /// `ParticleFxRand01`: the next value in `[0, 1]`, an 8-bit integer times
    /// the float `1/255`, unrounded (the reference leaves it on the x87
    /// stack; the product is exact in `f64`).
    pub fn next_unit(&mut self) -> f64 {
        let state = self.state;
        let mixed = (((state & 0xFF) << 8) ^ (state & 0xFF00)) | ((self.counter ^ state) & 0xFF);
        let bit = ((state >> 14) & 1) ^ (state & 1);
        self.counter = self.counter.wrapping_add(1);
        let next = bit.wrapping_add(mixed.wrapping_mul(2));
        self.state = next;
        let byte = (next & 0xFF) ^ ((next >> 8) & 0xFF);
        f64::from(byte) * f64::from(SCALE)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn first_draws_from_the_load_state() {
        let mut rand = ParticleRand::default();
        // State 0, counter 0: everything is zero.
        assert_eq!(rand.next_unit(), 0.0);
        assert_eq!((rand.state, rand.counter), (0, 1));
        // Counter 1 enters the low byte: state 2, byte 2.
        assert_eq!(rand.next_unit(), 2.0 * f64::from(SCALE));
        assert_eq!(rand.state, 2);
    }
}
