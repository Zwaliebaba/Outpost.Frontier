//! The `pub::` functions Content calls (decision record 0007, point 9):
//! free functions on the server context, grouped by the C++ namespace.
//! Outputs are return values; a C++ result code is a [`PubError`].
//!
//! `pub::Market` lives in [`crate::bases`]. `pub::Reputation`, `Zone`,
//! `System`, `Phantom`, `Controller`, `Audio`, `Player` and `SpaceObj`
//! are here in full (`SpaceObj::Launch` without the launch action), with
//! the top-level getters ([`misc`]; `pub::Save` is open); `pub::AI`
//! arrives with the part that owns its state.

pub mod audio;
pub mod controller;
pub mod gf;
pub mod misc;
pub mod phantom;
pub mod player;
pub mod reputation;
pub mod space_obj;
pub mod system;
pub mod zone;

/// A `pub::` function's failure code, as Content tests it (-1 to -5 and
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
    /// -4 as the argument checks return it (an empty name, a zero id, a
    /// negative radius, a size that is not positive); the same code as
    /// [`PubError::NO_CONTROLLER`].
    pub const BAD_ARGUMENT: Self = Self(-4);
    /// -5: `pub::Phantom` while the phantom events are being delivered.
    pub const BUSY: Self = Self(-5);
}
