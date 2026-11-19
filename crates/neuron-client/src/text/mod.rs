//! Client text (work package P4.12): the fonts, their pages and the string
//! paints ([`font`]), rich-text display lists and their markup ([`rdl`]),
//! their execution, layout and drawing ([`layout`]), and the text of
//! number strings ([`number`]).
//!
//! The DirectWrite backend of the fonts is `platform::dwrite`.

pub mod font;
pub mod layout;
pub mod number;
pub mod rdl;
