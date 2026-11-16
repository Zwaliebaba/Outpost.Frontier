//! The C runtime's `rand` (work package P1.1, decision T11).
//!
//! The reference calls `srand` and `rand` from the C runtime, whose state is
//! per thread. Here the generator is a value its users own and pass on.

/// The largest value [`CrtRand::rand`] returns (`RAND_MAX`).
pub const RAND_MAX: i32 = 0x7FFF;

/// The C runtime's linear congruential generator.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CrtRand {
    state: u32,
}

impl Default for CrtRand {
    /// The state of a thread that never called `srand`.
    fn default() -> Self {
        Self { state: 1 }
    }
}

impl CrtRand {
    /// `srand(seed)` on a new generator.
    #[must_use]
    pub fn with_seed(seed: u32) -> Self {
        Self { state: seed }
    }

    /// `srand(seed)`.
    pub fn seed(&mut self, seed: u32) {
        self.state = seed;
    }

    /// `rand()`: 0..=`RAND_MAX`.
    pub fn rand(&mut self) -> i32 {
        self.state = self.state.wrapping_mul(214_013).wrapping_add(2_531_011);
        i32::from(((self.state >> 16) & 0x7FFF) as u16)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_sequence_matches_the_crt() {
        let mut rand = CrtRand::default();
        let first: Vec<i32> = (0..5).map(|_| rand.rand()).collect();
        assert_eq!(first, [41, 18467, 6334, 26500, 19169]);
    }

    #[test]
    fn seeding_restarts_the_sequence() {
        let mut a = CrtRand::with_seed(7);
        let mut b = CrtRand::default();
        b.seed(7);
        assert_eq!(a.rand(), b.rand());
    }
}
