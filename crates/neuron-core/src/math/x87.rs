//! The 53-bit x87 model the kernels are written in; see "Precision" in
//! the module documentation of `math`.
//!
//! `FLD dword` widens an `f32` exactly ([`ext`]); every x87 operation is
//! then an `f64` operation; `FSTP dword` and a `/fp:precise` assignment to
//! `float` round once ([`round`], [`Words::st`]).
//!
//! The kernels address their operands as words in memory, as the assembly
//! does, so a call whose output overlaps an input sees each stored word in
//! the reads that follow it, exactly like the reference.

/// `FLD dword`: an `f32` on the 53-bit stack.
#[inline]
pub(super) fn ext(value: f32) -> f64 {
    f64::from(value)
}

/// `FSTP dword` or a `/fp:precise` assignment to `float`.
#[inline]
pub(super) fn round(value: f64) -> f32 {
    value as f32
}

/// The words of one or more value types laid out in memory. Operands are
/// word offsets into it; outputs may overlap inputs.
pub(super) struct Words<'a>(pub(super) &'a mut [f32]);

impl Words<'_> {
    /// `FLD dword [index]`.
    #[inline]
    pub(super) fn ld(&self, index: usize) -> f64 {
        ext(self.0[index])
    }

    /// `FSTP dword [index]`.
    #[inline]
    pub(super) fn st(&mut self, index: usize, value: f64) {
        self.0[index] = round(value);
    }
}
