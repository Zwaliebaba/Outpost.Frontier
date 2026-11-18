//! Client text (work package P4.12): the fonts, their pages and the string
//! paints ([`font`]), rich-text display lists and their markup ([`rdl`]),
//! and the text of number strings ([`number`]).
//!
//! The DirectWrite backend of the fonts is `platform::dwrite`. Rich-text
//! layout and execution come with the rest of P4.12.

pub mod font;
pub mod number;
pub mod rdl;
