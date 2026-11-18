//! Windows FFI (decision T14, record 0004 point 19): the only place in the
//! crate that may use unsafe code. Each module wraps the interfaces it
//! calls in safe types; every unsafe block says why it is sound.
#![allow(unsafe_code)]

pub mod d3d12;
pub mod dpapi;
pub mod known_folder;
pub mod registry;
pub mod window;
