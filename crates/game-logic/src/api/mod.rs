//! The `pub::` functions Content calls (decision record 0007, point 9):
//! free functions on the server context, grouped by the C++ namespace.
//! Outputs are return values; a C++ result code is a [`PubError`].
//!
//! `pub::Market` lives in [`crate::bases`]. The other namespaces arrive
//! with the parts of P3.3 that own their state.

pub mod gf;
pub mod space_obj;

/// A `pub::` function's failure code, as Content tests it (-1 to -4 and
/// the others the C++ returns).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PubError(pub i32);

impl PubError {
    /// -1: the C++'s general failure.
    pub const FAILED: Self = Self(-1);
    /// -2: an invalid id.
    pub const BAD_ID: Self = Self(-2);
    /// -3: not found.
    pub const NOT_FOUND: Self = Self(-3);
    /// -4: no such controller.
    pub const NO_CONTROLLER: Self = Self(-4);
}
