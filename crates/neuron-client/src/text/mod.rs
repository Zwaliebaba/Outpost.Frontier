//! Client text (work package P4.12): rich-text display lists and their
//! markup ([`rdl`]), and the text of number strings ([`number`]).
//!
//! The font code (the DirectWrite rasteriser, the glyph atlas and the font
//! configuration) and text layout come with the rest of P4.12.

pub mod number;
pub mod rdl;
